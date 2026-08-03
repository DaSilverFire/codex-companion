#pragma once

#include "mobile/relay/RelayConnection.h"
#include "mobile/security/PairingRecordStore.h"

#include <QFuture>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

#include <map>
#include <memory>
#include <optional>

namespace companion {

class RelayEndpointManager final
    : public QObject {
    Q_OBJECT

public:
    explicit RelayEndpointManager(
        QString localEndpointId,
        RelayConnectionTiming timing = {},
        QObject* parent = nullptr);
    ~RelayEndpointManager() override;

    RelayEndpointManager(
        const RelayEndpointManager&) =
        delete;
    RelayEndpointManager& operator=(
        const RelayEndpointManager&) =
        delete;

    void synchronize(
        const QVector<PairingRecord>& records,
        const std::optional<QUrl>&
            configuredUrl);
    QFuture<Result<void>> send(
        const QString& deviceId,
        const EncryptedEnvelope& envelope);
    QFuture<void> stopAll();

    qsizetype endpointCount() const
        noexcept;

signals:
    void endpointStateChanged(
        QString deviceId,
        companion::RelayConnectionState
            state);
    void envelopeReceived(
        QString deviceId,
        companion::EncryptedEnvelope
            envelope);
    void failureOccurred(
        QString deviceId,
        companion::CompanionError error);

private:
    struct Endpoint final {
        ~Endpoint();

        QUrl url;
        QByteArray secret;
        RelayConnection* connection =
            nullptr;
    };

    void removeEndpoint(
        const QString& deviceId);
    void clearEndpoints();

    QString localEndpointId_;
    RelayConnectionTiming timing_;
    std::map<
        QString,
        std::unique_ptr<Endpoint>>
        endpoints_;
};

} // namespace companion
