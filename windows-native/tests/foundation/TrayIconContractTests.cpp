#include "platform/windows/TrayIconHost.h"
#include "platform/windows/TrayWindowPlacement.h"

#include <QSignalSpy>
#include <memory>
#include <QtTest>

class TrayIconContractTests final : public QObject {
    Q_OBJECT

private slots:
    void menuOrderMatchesMacCompanionContract()
    {
        QCOMPARE(companion::TrayIconHost::commandOrder(), QStringList({
            QStringLiteral("toggle-pet"),
            QStringLiteral("toggle-companion-menu"),
            QStringLiteral("separator"),
            QStringLiteral("show-processes"),
            QStringLiteral("show-chat"),
            QStringLiteral("show-settings"),
            QStringLiteral("separator"),
            QStringLiteral("quit"),
        }));
    }

    void myDockFinderDisablesVersionFourCallbackPacking()
    {
        QVERIFY(!companion::TrayIconHost::shouldUseNotifyVersion4(true));
        QVERIFY(companion::TrayIconHost::shouldUseNotifyVersion4(false));
    }

    void pairedMouseMessagesCollapseToOneActivation()
    {
        companion::TrayActivationDeduplicator deduplicator(175);
        QVERIFY(deduplicator.accept(1000));
        QVERIFY(!deduplicator.accept(1080));
        QVERIFY(deduplicator.accept(1200));
    }

    void callbackPackingMatchesNotifyVersion()
    {
        const LPARAM packed = MAKELPARAM(WM_CONTEXTMENU, 42);
        QCOMPARE(companion::TrayIconHost::callbackEventFromMessage(packed, true),
                 static_cast<UINT>(WM_CONTEXTMENU));
        QCOMPARE(companion::TrayIconHost::callbackEventFromMessage(WM_RBUTTONUP, false),
                 static_cast<UINT>(WM_RBUTTONUP));
    }

    void nativeMouseActivationMapsIntoScaledSecondaryScreen()
    {
        const companion::TrayMonitorCoordinateSpace secondary{
            QRect(1920, 0, 3840, 2160),
            QRect(1920, 0, 2560, 1440),
            1.5,
        };

        const QPoint mapped =
            companion::TrayIconHost::activationPointFromNative(
                POINT{5580, 2115},
                secondary);

        QCOMPARE(mapped, QPoint(4360, 1410));
        QVERIFY(secondary.qtGeometry.contains(mapped));
        QCOMPARE(
            companion::TrayWindowPlacement::nearAnchor(
                mapped,
                QRect(1920, 0, 2560, 1400),
                QSize(392, 520),
                12),
            QPoint(3968, 878));
    }

    void nativeKeyboardActivationPreservesPrimaryScreenCoordinates()
    {
        const companion::TrayMonitorCoordinateSpace primary{
            QRect(0, 0, 1920, 1080),
            QRect(0, 0, 1920, 1080),
            1.0,
        };

        const QPoint mapped =
            companion::TrayIconHost::activationPointFromNative(
                POINT{1850, 1040},
                primary);

        QCOMPARE(mapped, QPoint(1850, 1040));
        QVERIFY(primary.qtGeometry.contains(mapped));
        QCOMPARE(
            companion::TrayWindowPlacement::nearAnchor(
                mapped,
                QRect(0, 0, 1920, 1040),
                QSize(392, 520),
                12),
            QPoint(1458, 508));
    }

    void menuEntriesFilterUnavailableSurfacesAndCollapseSeparators()
    {
        companion::TrayRouteState state;

        QCOMPARE(companion::TrayIconHost::menuEntryKeysForState(state), QStringList({
            QStringLiteral("show-settings"),
            QStringLiteral("separator"),
            QStringLiteral("quit"),
        }));
        QCOMPARE(companion::TrayIconHost::menuEntryLabelsForState(state), QStringList({
            QStringLiteral("Settings"),
            QStringLiteral("Quit Codex Companion"),
        }));
    }

    void menuLabelsReflectVisibleRouteState()
    {
        companion::TrayRouteState state;
        state.petRegistered = true;
        state.petVisible = true;
        state.companionMenuRegistered = true;
        state.companionMenuVisible = false;
        state.processesRegistered = true;
        state.chatRegistered = true;
        state.settingsVisible = true;

        QCOMPARE(companion::TrayIconHost::menuEntryKeysForState(state), QStringList({
            QStringLiteral("toggle-pet"),
            QStringLiteral("toggle-companion-menu"),
            QStringLiteral("separator"),
            QStringLiteral("show-processes"),
            QStringLiteral("show-chat"),
            QStringLiteral("show-settings"),
            QStringLiteral("separator"),
            QStringLiteral("quit"),
        }));
        QCOMPARE(companion::TrayIconHost::menuEntryLabelsForState(state), QStringList({
            QStringLiteral("Hide Pet"),
            QStringLiteral("Show Companion Menu"),
            QStringLiteral("Codex Processes"),
            QStringLiteral("Local Chat"),
            QStringLiteral("Settings"),
            QStringLiteral("Quit Codex Companion"),
        }));
    }

    void taskbarCreatedRestoreFailureEmitsRuntimeError()
    {
        struct NativeState final {
            int addCalls = 0;
            bool failRestoredAdd = false;
        };
        auto state = std::make_shared<NativeState>();
        companion::TrayIconHost host(
            [state](DWORD message, NOTIFYICONDATAW*) {
                if (message == NIM_ADD) {
                    ++state->addCalls;
                    return !state->failRestoredAdd;
                }
                return true;
            });
        QSignalSpy errorSpy(&host, &companion::TrayIconHost::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        QVERIFY(host.show(reinterpret_cast<HICON>(1), QStringLiteral("Test")).hasValue());
        QCOMPARE(state->addCalls, 1);

        state->failRestoredAdd = true;
        const auto restored = host.restoreAfterTaskbarCreated();

        QVERIFY(!restored.hasValue());
        QCOMPARE(restored.error().code, QStringLiteral("tray.icon-add-failed"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("tray.icon-add-failed"));
    }

    void throwingRouteStateProviderEmitsTypedRefreshError()
    {
        companion::TrayIconHost host([](DWORD, NOTIFYICONDATAW*) {
            return true;
        });
        host.setRouteStateProvider([]() -> companion::TrayRouteState {
            throw std::bad_alloc();
        });
        QSignalSpy errorSpy(&host, &companion::TrayIconHost::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        const auto refreshed = host.refreshRouteStateForMenu();

        QVERIFY(!refreshed.hasValue());
        QCOMPARE(refreshed.error().code, QStringLiteral("tray.route-state-failed"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("tray.route-state-failed"));
    }

    void settingsCommandAlwaysRequestsOpenAndFocus()
    {
        companion::TrayIconHost host([](DWORD, NOTIFYICONDATAW*) {
            return true;
        });
        QSignalSpy showSpy(&host, &companion::TrayIconHost::showSettingsRequested);
        QSignalSpy hideSpy(&host, &companion::TrayIconHost::hideSettingsRequested);
        QVERIFY(showSpy.isValid());
        QVERIFY(hideSpy.isValid());

        companion::TrayRouteState hiddenState;
        hiddenState.settingsVisible = false;
        host.setRouteState(hiddenState);
        host.invokeCommand(companion::TrayIconHost::Command::ShowSettings);

        QCOMPARE(showSpy.count(), 1);
        QCOMPARE(hideSpy.count(), 0);

        companion::TrayRouteState visibleState;
        visibleState.settingsVisible = true;
        host.setRouteState(visibleState);
        host.invokeCommand(companion::TrayIconHost::Command::ShowSettings);

        QCOMPARE(showSpy.count(), 2);
        QCOMPARE(hideSpy.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TrayIconContractTests)
#include "TrayIconContractTests.moc"
