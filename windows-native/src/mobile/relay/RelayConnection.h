#pragma once

#include "core/Result.h"
#include "mobile/relay/RelayModels.h"
#include "mobile/security/SecurityModels.h"

#include <QByteArrayView>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

QT_BEGIN_NAMESPACE
class QTimer;
class QWebSocket;
QT_END_NAMESPACE

namespace companion {

enum class RelayConnectionState {
    Stopped,
    Connecting,
    Registered,
    WaitingToReconnect,
};

enum class RelaySenderMode {
    PairedExact,
    BootstrapUnknown,
};

struct RelayConnectionTiming final {
    int packetResultTimeoutMilliseconds =
        5'000;
    int pingIntervalMilliseconds =
        20'000;
    QVector<int>
        reconnectDelayMilliseconds{
            1'000,
            2'000,
            4'000,
            8'000,
            16'000,
            30'000,
        };
    std::function<QString()>
        packetIdGenerator;
};

class RelayConnection final
    : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype
        kMaximumMessageBytes =
            1'048'576;

    RelayConnection(
        QUrl url,
        QString channelId,
        QString endpointId,
        QString remoteSenderId,
        RelayConnectionTiming timing = {},
        QObject* parent = nullptr);
    RelayConnection(
        QUrl url,
        QString channelId,
        QString endpointId,
        RelaySenderMode senderMode,
        RelayConnectionTiming timing = {},
        QObject* parent = nullptr);
    ~RelayConnection() override;

    RelayConnection(
        const RelayConnection&) = delete;
    RelayConnection& operator=(
        const RelayConnection&) = delete;

    void start();
    QFuture<Result<void>> stop();
    QFuture<Result<void>> send(
        const EncryptedEnvelope& envelope);

    RelayConnectionState state()
        const noexcept;
    const QUrl& effectiveUrl()
        const noexcept;
    bool registrationAcknowledged()
        const noexcept;

    static Result<void>
    validateFinalMessageSize(
        QByteArrayView bytes);

signals:
    void stateChanged(
        companion::RelayConnectionState state);
    void envelopeReceived(
        companion::EncryptedEnvelope envelope);
    void failureOccurred(
        companion::CompanionError error);
    void reconnectScheduled(
        int delayMilliseconds);
    void transportPingSent();

private:
    RelayConnection(
        QUrl url,
        QString channelId,
        QString endpointId,
        QString remoteSenderId,
        RelaySenderMode senderMode,
        RelayConnectionTiming timing,
        QObject* parent);

    struct PendingSend final {
        std::shared_ptr<
            QPromise<Result<void>>>
            promise;
        QPointer<QTimer> timeout;
    };

    void connectNow();
    void destroySocket();
    Result<void> sendWire(
        const RelayWireMessage& message);
    void handlePayload(
        QByteArray bytes,
        quint64 generation);
    Result<void> handleWire(
        const RelayWireMessage& message);
    void handleFailure(
        CompanionError error,
        quint64 generation);
    void scheduleReconnect();
    int currentReconnectDelay() const;
    bool resolvePending(
        const QString& packetId,
        Result<void> result);
    void failPending(
        const CompanionError& error);
    void publish(
        RelayConnectionState state);
    bool isCurrentGeneration(
        quint64 generation) const noexcept;
    quint64 advanceGeneration() noexcept;
    QString nextPacketId() const;

    QUrl effectiveUrl_;
    QString channelId_;
    QString endpointId_;
    QString remoteSenderId_;
    RelaySenderMode senderMode_ =
        RelaySenderMode::PairedExact;
    RelayConnectionTiming timing_;
    std::optional<CompanionError>
        configurationError_;
    QWebSocket* socket_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QTimer* pingTimer_ = nullptr;
    QHash<QString, PendingSend>
        pendingSends_;
    RelayConnectionState state_ =
        RelayConnectionState::Stopped;
    bool shouldRun_ = false;
    bool registrationAcknowledged_ =
        false;
    int reconnectAttempt_ = 0;
    quint64 generation_ = 0;
};

} // namespace companion

Q_DECLARE_METATYPE(
    companion::RelayConnectionState)
