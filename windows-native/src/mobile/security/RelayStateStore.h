#pragma once

#include "core/Result.h"
#include "mobile/security/SecretProtector.h"

#include <QHash>
#include <QMutex>
#include <QString>

#include <optional>

namespace companion {

class RelayStateStore final {
public:
    RelayStateStore(
        QString filePath,
        const SecretProtector& protector);

    RelayStateStore(
        const RelayStateStore&) = delete;
    RelayStateStore& operator=(
        const RelayStateStore&) = delete;

    static QString defaultFilePath();
    static QByteArray protectionEntropy();

    Result<quint64> nextOutbound(
        const QString& channelId,
        const QString& senderId);

    Result<bool> acceptInbound(
        const QString& channelId,
        const QString& senderId,
        quint64 sequence);

    Result<void> eraseChannel(
        const QString& channelId);

private:
    struct Entry final {
        quint64 nextOutbound = 1;
        quint64 highestInbound = 0;
    };

    using SenderState =
        QHash<QString, Entry>;
    using State =
        QHash<QString, SenderState>;

    void load();
    Result<void> persist(
        const State& state) const;

    QString filePath_;
    const SecretProtector* protector_ =
        nullptr;
    mutable QMutex mutex_;
    State state_;
    std::optional<CompanionError>
        loadError_;
};

} // namespace companion
