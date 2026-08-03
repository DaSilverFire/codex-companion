#pragma once

#include "core/Result.h"

#include <QObject>
#include <memory>

class QLocalServer;
class QLocalSocket;

namespace companion {

class SingleInstanceGate final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceGate(QString instanceName, QObject* parent = nullptr);
    ~SingleInstanceGate() override;

    Result<void> startPrimary();
    Result<void> sendActivation();

signals:
    void activationRequested();

private:
    struct MutexHandle;

    void handleNewConnection();
    void handleSocketReadyRead(QLocalSocket* socket);
    void closeServer();

    QString instanceName_;
    QString activationServerName_;
    std::unique_ptr<MutexHandle> mutex_;
    QLocalServer* activationServer_;
    bool ownsActivationServer_ = false;
};

} // namespace companion
