#include "platform/windows/SingleInstanceGate.h"

#include <QByteArray>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

namespace {

using companion::CompanionError;
using companion::Result;

const QString kAlreadyRunningCode = QStringLiteral("app.already-running");
const QString kAlreadyRunningMessage =
    QStringLiteral("Codex Companion is already running.");
const QByteArray kActivationPayload("activate-settings\n");
constexpr int kActivationTimeoutMs = 2000;

CompanionError alreadyRunningError()
{
    return {
        kAlreadyRunningCode,
        kAlreadyRunningMessage,
        false,
        {},
    };
}

CompanionError instanceError(
    QString code,
    QString message,
    const QString& instanceName)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {{QStringLiteral("instanceName"), instanceName}},
    };
}

bool waitForSocketConnected(QLocalSocket& socket, int timeoutMs)
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

bool waitForSocketBytesWritten(
    QLocalSocket& socket,
    qint64 expectedBytes,
    int timeoutMs)
{
    if (expectedBytes <= 0 || socket.bytesToWrite() == 0) {
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

} // namespace

namespace companion {

struct SingleInstanceGate::MutexHandle final {
    explicit MutexHandle(HANDLE nativeHandle) : nativeHandle(nativeHandle) {}

    ~MutexHandle()
    {
        if (nativeHandle != nullptr) {
            CloseHandle(nativeHandle);
        }
    }

    HANDLE nativeHandle = nullptr;
};

SingleInstanceGate::SingleInstanceGate(QString instanceName, QObject* parent)
    : QObject(parent),
      instanceName_(std::move(instanceName)),
      activationServerName_(instanceName_ + QStringLiteral(".activation")),
      activationServer_(new QLocalServer(this))
{
    connect(activationServer_,
            &QLocalServer::newConnection,
            this,
            &SingleInstanceGate::handleNewConnection);
}

SingleInstanceGate::~SingleInstanceGate()
{
    closeServer();
}

Result<void> SingleInstanceGate::startPrimary()
{
    if (mutex_) {
        return Result<void>::success();
    }

    const HANDLE rawHandle =
        CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(instanceName_.utf16()));
    if (rawHandle == nullptr) {
        return Result<void>::failure(instanceError(
            QStringLiteral("app.instance-mutex-create-failed"),
            QStringLiteral("Failed to create the single-instance mutex."),
            instanceName_));
    }

    std::unique_ptr<MutexHandle> ownedMutex =
        std::make_unique<MutexHandle>(rawHandle);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<void>::failure(alreadyRunningError());
    }

    closeServer();
    QLocalServer::removeServer(activationServerName_);
    activationServer_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!activationServer_->listen(activationServerName_)) {
        return Result<void>::failure(instanceError(
            QStringLiteral("app.activation-listen-failed"),
            QStringLiteral("Failed to listen for activation requests."),
            activationServerName_));
    }

    mutex_ = std::move(ownedMutex);
    ownsActivationServer_ = true;
    return Result<void>::success();
}

Result<void> SingleInstanceGate::sendActivation()
{
    QLocalSocket socket;
    socket.connectToServer(activationServerName_);
    if (!waitForSocketConnected(socket, kActivationTimeoutMs)) {
        const QString socketMessage = socket.errorString().isEmpty()
            ? QStringLiteral("Failed to connect to the running companion.")
            : QStringLiteral("Failed to connect to the running companion: %1.")
                  .arg(socket.errorString());
        return Result<void>::failure(instanceError(
            QStringLiteral("app.activation-connect-failed"),
            socketMessage,
            activationServerName_));
    }

    const qint64 bytesWritten = socket.write(kActivationPayload);
    if (bytesWritten != kActivationPayload.size()) {
        const QString socketMessage = socket.errorString().isEmpty()
            ? QStringLiteral("Failed to deliver the activation request.")
            : QStringLiteral("Failed to deliver the activation request: %1.")
                  .arg(socket.errorString());
        return Result<void>::failure(instanceError(
            QStringLiteral("app.activation-write-failed"),
            socketMessage,
            activationServerName_));
    }
    if (!waitForSocketBytesWritten(
            socket, kActivationPayload.size(), kActivationTimeoutMs)) {
        const QString socketMessage = socket.errorString().isEmpty()
            ? QStringLiteral("Failed to deliver the activation request.")
            : QStringLiteral("Failed to deliver the activation request: %1.")
                  .arg(socket.errorString());
        return Result<void>::failure(instanceError(
            QStringLiteral("app.activation-write-failed"),
            socketMessage,
            activationServerName_));
    }

    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(2000);
    }
    return Result<void>::success();
}

void SingleInstanceGate::handleNewConnection()
{
    while (QLocalSocket* socket = activationServer_->nextPendingConnection()) {
        connect(socket,
                &QLocalSocket::readyRead,
                this,
                [this, socket]() { handleSocketReadyRead(socket); });
        connect(socket,
                &QLocalSocket::disconnected,
                socket,
                &QObject::deleteLater);
        handleSocketReadyRead(socket);
    }
}

void SingleInstanceGate::handleSocketReadyRead(QLocalSocket* socket)
{
    if (socket == nullptr || !socket->canReadLine()) {
        return;
    }

    const QByteArray payload = socket->readLine();
    if (payload == kActivationPayload) {
        emit activationRequested();
    }
    socket->disconnectFromServer();
}

void SingleInstanceGate::closeServer()
{
    if (!ownsActivationServer_) {
        return;
    }

    if (activationServer_->isListening()) {
        activationServer_->close();
    }
    QLocalServer::removeServer(activationServerName_);
    ownsActivationServer_ = false;
}

} // namespace companion
