#pragma once

#include "core/Result.h"
#include "mobile/security/SecurityModels.h"

#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace companion {

class PairingCoordinator;
class RelayStateStore;

struct RelayPairingBootstrapOffer final {
    static constexpr int currentVersion = 1;

    int version = currentVersion;
    QUrl relayUrl;
    QString hostDeviceId;
    QString hostDisplayName;
    QString pairingCode;
    qint64 expiresAtMilliseconds = 0;
    QByteArray bootstrapSecret;

    Result<QString> pairingLink() const;
    static Result<RelayPairingBootstrapOffer>
    parse(QStringView link);

    friend bool operator==(
        const RelayPairingBootstrapOffer&,
        const RelayPairingBootstrapOffer&) =
        default;
};

class RelayPairingBootstrapEndpoint
    : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~RelayPairingBootstrapEndpoint()
        override = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual QFuture<Result<void>> send(
        const EncryptedEnvelope& envelope) =
        0;

signals:
    void envelopeReceived(
        companion::EncryptedEnvelope
            envelope);
    void failureOccurred(
        companion::CompanionError error);
};

using RelayPairingBootstrapEndpointFactory =
    std::function<
        std::unique_ptr<
            RelayPairingBootstrapEndpoint>(
            QUrl url,
            QString channelId,
            QString endpointId)>;
using RelayPairingBootstrapClock =
    std::function<QDateTime()>;
using RelayPairingBootstrapSecretGenerator =
    std::function<Result<QByteArray>()>;
using RelayPairingBootstrapSecretEraser =
    std::function<void(QByteArray&)>;

struct RelayPairingBootstrapDependencies final {
    RelayPairingBootstrapEndpointFactory
        endpointFactory;
    RelayPairingBootstrapClock clock;
    RelayPairingBootstrapSecretGenerator
        secretGenerator;
    RelayPairingBootstrapSecretEraser
        secretEraser;
};

class RelayPairingBootstrap final
    : public QObject {
    Q_OBJECT

public:
    RelayPairingBootstrap(
        PairingCoordinator& pairingCoordinator,
        RelayStateStore& relayStateStore,
        QString hostDeviceId,
        QString hostDisplayName,
        RelayPairingBootstrapDependencies
            dependencies = {},
        QObject* parent = nullptr);
    ~RelayPairingBootstrap() override;

    RelayPairingBootstrap(
        const RelayPairingBootstrap&) =
        delete;
    RelayPairingBootstrap& operator=(
        const RelayPairingBootstrap&) =
        delete;

    void setRelayUrl(
        std::optional<QUrl> relayUrl);
    std::optional<QUrl> relayUrl() const;

    Result<RelayPairingBootstrapOffer>
    beginPairing(
        std::chrono::seconds validFor =
            std::chrono::seconds(300));
    void cancelPairing();

    std::optional<
        RelayPairingBootstrapOffer>
    activeOffer() const;
    std::optional<CompanionError>
    lastError() const;

signals:
    void stateChanged();

private:
    struct ActiveState;

    void processEnvelope(
        const std::shared_ptr<
            ActiveState>& state,
        const EncryptedEnvelope& envelope);
    void finishSend(
        const std::shared_ptr<
            ActiveState>& state,
        bool pairingSucceeded,
        Result<void> result);
    void complete(
        const std::shared_ptr<
            ActiveState>& state);
    void expire(
        const std::shared_ptr<
            ActiveState>& state);
    void recordFailure(
        const std::shared_ptr<
            ActiveState>& state,
        CompanionError error);
    void terminate(
        const std::shared_ptr<
            ActiveState>& state,
        bool cancelPairing);

    PairingCoordinator*
        pairingCoordinator_ = nullptr;
    RelayStateStore* relayStateStore_ =
        nullptr;
    QString hostDeviceId_;
    QString hostDisplayName_;
    RelayPairingBootstrapDependencies
        dependencies_;
    std::optional<QUrl> relayUrl_;
    std::shared_ptr<ActiveState> active_;
    std::optional<CompanionError>
        lastError_;
};

} // namespace companion
