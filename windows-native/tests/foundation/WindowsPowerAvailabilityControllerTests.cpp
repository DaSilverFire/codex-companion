#include "platform/windows/mobile/WindowsPowerAvailabilityController.h"

#include <QtTest>

namespace {

class FakePowerRequestApi final
    : public companion::IWindowsPowerRequestApi {
public:
    companion::Result<
        companion::WindowsPowerRequestHandle>
    create(const QString& reason) override
    {
        ++createCalls;
        lastReason = reason;
        if (failCreate) {
            return companion::Result<
                companion::
                    WindowsPowerRequestHandle>::
                failure({
                    QStringLiteral(
                        "test.power-create-failed"),
                    QStringLiteral(
                        "Injected power request creation failure."),
                    false,
                    {},
                });
        }
        return companion::Result<
            companion::
                WindowsPowerRequestHandle>::
            success(handle);
    }

    companion::Result<void> setSystemRequired(
        companion::WindowsPowerRequestHandle
            requestedHandle) override
    {
        ++setCalls;
        if (requestedHandle != handle) {
            return companion::Result<void>::
                failure({
                    QStringLiteral(
                        "test.power-handle-mismatch"),
                    QStringLiteral(
                        "The power request handle did not match."),
                    false,
                    {},
                });
        }
        if (failSet) {
            return companion::Result<void>::
                failure({
                    QStringLiteral(
                        "test.power-set-failed"),
                    QStringLiteral(
                        "Injected power request activation failure."),
                    false,
                    {},
                });
        }
        return companion::Result<void>::
            success();
    }

    companion::Result<void> clearSystemRequired(
        companion::WindowsPowerRequestHandle
            requestedHandle) override
    {
        ++clearCalls;
        if (requestedHandle != handle) {
            return companion::Result<void>::
                failure({
                    QStringLiteral(
                        "test.power-handle-mismatch"),
                    QStringLiteral(
                        "The power request handle did not match."),
                    false,
                    {},
                });
        }
        if (failClear) {
            return companion::Result<void>::
                failure({
                    QStringLiteral(
                        "test.power-clear-failed"),
                    QStringLiteral(
                        "Injected power request release failure."),
                    false,
                    {},
                });
        }
        return companion::Result<void>::
            success();
    }

    void close(
        companion::WindowsPowerRequestHandle
            requestedHandle) noexcept override
    {
        ++closeCalls;
        lastClosedHandle =
            requestedHandle;
    }

    companion::WindowsPowerRequestHandle
        handle =
            reinterpret_cast<void*>(0x1234);
    QString lastReason;
    companion::WindowsPowerRequestHandle
        lastClosedHandle = nullptr;
    int createCalls = 0;
    int setCalls = 0;
    int clearCalls = 0;
    int closeCalls = 0;
    bool failCreate = false;
    bool failSet = false;
    bool failClear = false;
};

} // namespace

class WindowsPowerAvailabilityControllerTests final
    : public QObject {
    Q_OBJECT

private slots:
    void activationIsIdempotentAndReleaseClosesTheRequest()
    {
        FakePowerRequestApi api;
        companion::
            WindowsPowerAvailabilityController
                controller(&api);

        QVERIFY(
            controller.setAvailable(true)
                .hasValue());
        QVERIFY(controller.isAvailable());
        QVERIFY(
            controller.setAvailable(true)
                .hasValue());
        QCOMPARE(api.createCalls, 1);
        QCOMPARE(api.setCalls, 1);
        QCOMPARE(
            api.lastReason,
            QStringLiteral(
                "Keep Codex Companion mobile access available while the display is off."));

        QVERIFY(
            controller.setAvailable(false)
                .hasValue());
        QVERIFY(!controller.isAvailable());
        QCOMPARE(api.clearCalls, 1);
        QCOMPARE(api.closeCalls, 1);
        QCOMPARE(
            api.lastClosedHandle,
            api.handle);
        QVERIFY(
            controller.setAvailable(false)
                .hasValue());
        QCOMPARE(api.clearCalls, 1);
    }

    void activationFailureDoesNotLeakOrClaimAvailability()
    {
        FakePowerRequestApi api;
        api.failSet = true;
        companion::
            WindowsPowerAvailabilityController
                controller(&api);

        const auto activated =
            controller.setAvailable(true);

        QVERIFY(!activated.hasValue());
        QVERIFY(!controller.isAvailable());
        QCOMPARE(api.createCalls, 1);
        QCOMPARE(api.setCalls, 1);
        QCOMPARE(api.clearCalls, 0);
        QCOMPARE(api.closeCalls, 1);
    }

    void clearFailureKeepsTheLiveRequestForRetry()
    {
        FakePowerRequestApi api;
        companion::
            WindowsPowerAvailabilityController
                controller(&api);
        QVERIFY(
            controller.setAvailable(true)
                .hasValue());
        api.failClear = true;

        const auto released =
            controller.setAvailable(false);

        QVERIFY(!released.hasValue());
        QVERIFY(controller.isAvailable());
        QCOMPARE(api.closeCalls, 0);

        api.failClear = false;
        QVERIFY(
            controller.setAvailable(false)
                .hasValue());
        QVERIFY(!controller.isAvailable());
        QCOMPARE(api.clearCalls, 2);
        QCOMPARE(api.closeCalls, 1);
    }
};

QTEST_GUILESS_MAIN(
    WindowsPowerAvailabilityControllerTests)
#include "WindowsPowerAvailabilityControllerTests.moc"
