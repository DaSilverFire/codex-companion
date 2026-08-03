#include "platform/windows/SingleInstanceGate.h"

#include <QEventLoop>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTimer>
#include <QUuid>
#include <QtTest>

namespace {

constexpr int kTimeoutMs = 2000;

QString uniqueInstanceName()
{
    return QStringLiteral("CodexCompanion.Test.") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool waitForConnected(QLocalSocket& socket, int timeoutMs)
{
    if (socket.state() == QLocalSocket::ConnectedState) {
        return true;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&socket, &QLocalSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&socket,
                     &QLocalSocket::errorOccurred,
                     &loop,
                     &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);

    while (socket.state() == QLocalSocket::ConnectingState && timer.isActive()) {
        loop.exec();
    }

    return socket.state() == QLocalSocket::ConnectedState;
}

bool waitForBytesWritten(QLocalSocket& socket, int timeoutMs)
{
    if (socket.bytesToWrite() == 0) {
        return true;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&socket,
                     &QLocalSocket::bytesWritten,
                     &loop,
                     [&loop, &socket]() {
                         if (socket.bytesToWrite() == 0) {
                             loop.quit();
                         }
                     });
    QObject::connect(&socket,
                     &QLocalSocket::errorOccurred,
                     &loop,
                     &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);

    while (socket.bytesToWrite() > 0 &&
           socket.state() == QLocalSocket::ConnectedState &&
           timer.isActive()) {
        loop.exec();
    }

    return socket.bytesToWrite() == 0;
}

bool waitForDisconnected(QLocalSocket& socket, int timeoutMs)
{
    if (socket.state() == QLocalSocket::UnconnectedState) {
        return true;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&socket,
                     &QLocalSocket::disconnected,
                     &loop,
                     &QEventLoop::quit);
    QObject::connect(&socket,
                     &QLocalSocket::errorOccurred,
                     &loop,
                     &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);

    while (socket.state() != QLocalSocket::UnconnectedState && timer.isActive()) {
        loop.exec();
    }

    return socket.state() == QLocalSocket::UnconnectedState;
}

} // namespace

class SingleInstanceGateTests final : public QObject {
    Q_OBJECT

private slots:
    void secondStartReturnsExactAlreadyRunningError()
    {
        const QString name = uniqueInstanceName();
        companion::SingleInstanceGate primary(name);
        companion::SingleInstanceGate secondary(name);

        QVERIFY(primary.startPrimary().hasValue());

        const auto secondStart = secondary.startPrimary();

        QVERIFY(!secondStart.hasValue());
        QCOMPARE(secondStart.error().code, QStringLiteral("app.already-running"));
        QCOMPARE(secondStart.error().message,
                 QStringLiteral("Codex Companion is already running."));
        QVERIFY(!secondStart.error().retryable);
        QVERIFY(secondStart.error().context.isEmpty());
    }

    void secondGateSignalsThePrimary()
    {
        const QString name = uniqueInstanceName();
        companion::SingleInstanceGate primary(name);
        companion::SingleInstanceGate secondary(name);
        QSignalSpy activationSpy(
            &primary,
            &companion::SingleInstanceGate::activationRequested);

        QVERIFY(primary.startPrimary().hasValue());
        QVERIFY(!secondary.startPrimary().hasValue());
        const auto activated = secondary.sendActivation();
        const QByteArray activationMessage =
            activated.error().message.toLocal8Bit();
        QVERIFY2(activated.hasValue(), activationMessage.constData());
        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 1, 2000);
    }

    void nonExactPayloadDoesNotSignalThePrimary()
    {
        const QString name = uniqueInstanceName();
        companion::SingleInstanceGate primary(name);
        QSignalSpy activationSpy(
            &primary,
            &companion::SingleInstanceGate::activationRequested);

        QVERIFY(primary.startPrimary().hasValue());

        QLocalSocket socket;
        socket.connectToServer(name + QStringLiteral(".activation"));
        QVERIFY(waitForConnected(socket, kTimeoutMs));
        QCOMPARE(socket.write("activate-settings-now\n"), qint64(22));
        QVERIFY(waitForBytesWritten(socket, kTimeoutMs));
        QVERIFY(waitForDisconnected(socket, kTimeoutMs));

        QTest::qWait(200);
        QCOMPARE(activationSpy.count(), 0);
    }

    void destroyingPrimaryReleasesEndpointForReplacementPrimary()
    {
        const QString name = uniqueInstanceName();

        {
            companion::SingleInstanceGate primary(name);
            QVERIFY(primary.startPrimary().hasValue());
        }

        companion::SingleInstanceGate replacement(name);

        QVERIFY(replacement.startPrimary().hasValue());
    }

    void repeatedSecondaryLaunchesKeepPrimaryEndpointAvailable()
    {
        const QString name = uniqueInstanceName();
        companion::SingleInstanceGate primary(name);
        QSignalSpy activationSpy(
            &primary,
            &companion::SingleInstanceGate::activationRequested);

        QVERIFY(primary.startPrimary().hasValue());

        {
            companion::SingleInstanceGate secondary(name);
            QVERIFY(!secondary.startPrimary().hasValue());
            const auto activated = secondary.sendActivation();
            const QByteArray activationMessage =
                activated.error().message.toLocal8Bit();
            QVERIFY2(activated.hasValue(), activationMessage.constData());
        }
        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 1, 2000);

        {
            companion::SingleInstanceGate tertiary(name);
            QVERIFY(!tertiary.startPrimary().hasValue());
            const auto activated = tertiary.sendActivation();
            const QByteArray activationMessage =
                activated.error().message.toLocal8Bit();
            QVERIFY2(activated.hasValue(), activationMessage.constData());
        }
        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 2, 2000);
    }
};

QTEST_GUILESS_MAIN(SingleInstanceGateTests)
#include "SingleInstanceGateTests.moc"
