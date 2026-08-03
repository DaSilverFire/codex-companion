#include "mobile/relay/RelayEndpointManager.h"

#include "mobile/security/BridgeSecurity.h"
#include "platform/windows/security/WindowsCrypto.h"

#include <QPromise>
#include <QSet>

#include <utility>

namespace companion {
namespace {

QFuture<void> readyVoidFuture()
{
    QPromise<void> promise;
    promise.start();
    QFuture<void> future =
        promise.future();
    promise.finish();
    return future;
}

QFuture<Result<void>> readyResultFuture(
    Result<void> result)
{
    QPromise<Result<void>> promise;
    promise.start();
    QFuture<Result<void>> future =
        promise.future();
    promise.addResult(
        std::move(result));
    promise.finish();
    return future;
}

CompanionError endpointUnavailableError()
{
    return {
        QStringLiteral(
            "relay.endpoint_unavailable"),
        QStringLiteral(
            "The paired mobile relay endpoint is unavailable."),
        true,
        {},
    };
}

QString normalizedUrl(const QUrl& url)
{
    return url.toString(
        QUrl::FullyEncoded);
}

} // namespace

RelayEndpointManager::Endpoint::~Endpoint()
{
    WindowsCrypto::secureZero(secret);
}

RelayEndpointManager::
RelayEndpointManager(
    QString localEndpointId,
    RelayConnectionTiming timing,
    QObject* parent)
    : QObject(parent),
      localEndpointId_(
          std::move(localEndpointId)),
      timing_(std::move(timing))
{
}

RelayEndpointManager::
~RelayEndpointManager()
{
    clearEndpoints();
}

void RelayEndpointManager::synchronize(
    const QVector<PairingRecord>& records,
    const std::optional<QUrl>&
        configuredUrl)
{
    if (!configuredUrl.has_value()
        || localEndpointId_.isEmpty()) {
        clearEndpoints();
        return;
    }

    QSet<QString> desiredIds;
    for (const PairingRecord& record :
         records) {
        if (record.deviceId.isEmpty()
            || record.secret.size() != 32) {
            continue;
        }
        desiredIds.insert(record.deviceId);
        auto existing =
            endpoints_.find(record.deviceId);
        const bool unchanged =
            existing != endpoints_.end()
            && normalizedUrl(
                   existing->second->url)
                == normalizedUrl(
                    *configuredUrl)
            && WindowsCrypto::
                constantTimeEquals(
                    existing->second->secret,
                    record.secret);
        if (unchanged) {
            continue;
        }
        if (existing != endpoints_.end()) {
            removeEndpoint(
                record.deviceId);
        }

        const auto channel =
            BridgeSecurity::channelId(
                record.secret);
        if (!channel.hasValue()) {
            emit failureOccurred(
                record.deviceId,
                channel.error());
            continue;
        }

        auto endpoint =
            std::make_unique<Endpoint>();
        endpoint->url = *configuredUrl;
        endpoint->secret = record.secret;
        endpoint->connection =
            new RelayConnection(
                *configuredUrl,
                channel.value(),
                localEndpointId_,
                record.deviceId,
                timing_,
                this);
        RelayConnection* connection =
            endpoint->connection;
        const QString deviceId =
            record.deviceId;
        QObject::connect(
            connection,
            &RelayConnection::stateChanged,
            this,
            [this, deviceId](
                RelayConnectionState state) {
                emit endpointStateChanged(
                    deviceId,
                    state);
            });
        QObject::connect(
            connection,
            &RelayConnection::
                envelopeReceived,
            this,
            [this, deviceId](
                EncryptedEnvelope envelope) {
                emit envelopeReceived(
                    deviceId,
                    std::move(envelope));
            });
        QObject::connect(
            connection,
            &RelayConnection::
                failureOccurred,
            this,
            [this, deviceId](
                CompanionError error) {
                emit failureOccurred(
                    deviceId,
                    std::move(error));
            });
        endpoints_.insert_or_assign(
            deviceId,
            std::move(endpoint));
        connection->start();
    }

    QVector<QString> removed;
    for (const auto& [deviceId, endpoint] :
         endpoints_) {
        Q_UNUSED(endpoint);
        if (!desiredIds.contains(deviceId)) {
            removed.append(deviceId);
        }
    }
    for (const QString& deviceId :
         removed) {
        removeEndpoint(deviceId);
    }
}

QFuture<Result<void>>
RelayEndpointManager::send(
    const QString& deviceId,
    const EncryptedEnvelope& envelope)
{
    const auto iterator =
        endpoints_.find(deviceId);
    if (iterator == endpoints_.end()
        || iterator->second == nullptr
        || iterator->second->connection
            == nullptr) {
        return readyResultFuture(
            Result<void>::failure(
                endpointUnavailableError()));
    }
    return iterator->second
        ->connection->send(envelope);
}

QFuture<void>
RelayEndpointManager::stopAll()
{
    clearEndpoints();
    return readyVoidFuture();
}

qsizetype
RelayEndpointManager::endpointCount()
    const noexcept
{
    return qsizetype(
        endpoints_.size());
}

void RelayEndpointManager::removeEndpoint(
    const QString& deviceId)
{
    const auto iterator =
        endpoints_.find(deviceId);
    if (iterator == endpoints_.end()) {
        return;
    }
    RelayConnection* connection =
        iterator->second->connection;
    if (connection != nullptr) {
        connection->stop();
        connection->deleteLater();
        iterator->second->connection =
            nullptr;
    }
    endpoints_.erase(iterator);
}

void RelayEndpointManager::clearEndpoints()
{
    while (!endpoints_.empty()) {
        removeEndpoint(
            endpoints_.begin()->first);
    }
}

} // namespace companion
