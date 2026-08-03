#include "ui/WindowBackdropState.h"

#include <QSignalSpy>
#include <QtTest>

class WindowBackdropStateTests final : public QObject {
    Q_OBJECT

private slots:
    void tracksEffectiveModesIndependentlyByWindowRole()
    {
        companion::WindowBackdropState state;
        QSignalSpy changedSpy(
            &state,
            &companion::WindowBackdropState::effectiveBackdropModesChanged);
        QVERIFY(changedSpy.isValid());

        QCOMPARE(state.settingsEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(state.companionMenuEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(state.modelPickerEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(state.goalEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(state.usageEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(state.attentionEffectiveMode(), QStringLiteral("mica"));

        state.setEffectiveMode(
            companion::WindowRole::CompanionMenu,
            companion::BackdropMode::SolidBlack);

        QCOMPARE(
            state.companionMenuEffectiveMode(),
            QStringLiteral("solid-black"));
        QCOMPARE(state.settingsEffectiveMode(), QStringLiteral("mica"));
        QCOMPARE(changedSpy.count(), 1);

        state.setEffectiveMode(
            companion::WindowRole::Usage,
            companion::BackdropMode::WindowsGlass);

        QCOMPARE(
            state.usageEffectiveMode(),
            QStringLiteral("windows-glass"));
        QCOMPARE(
            state.companionMenuEffectiveMode(),
            QStringLiteral("solid-black"));
        QCOMPARE(changedSpy.count(), 2);

        state.setEffectiveMode(
            companion::WindowRole::Usage,
            companion::BackdropMode::WindowsGlass);
        QCOMPARE(changedSpy.count(), 2);
    }
};

QTEST_MAIN(WindowBackdropStateTests)
#include "WindowBackdropStateTests.moc"
