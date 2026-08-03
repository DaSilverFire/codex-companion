#pragma once

#include "core/Result.h"
#include "mobile/security/SecretProtector.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct PairingRecord final {
    PairingRecord() = default;
    PairingRecord(
        QString deviceId,
        QString displayName,
        QByteArray secret,
        QDateTime pairedAt,
        std::optional<QString>
            relayUrlString =
                std::nullopt);
    PairingRecord(
        const PairingRecord& source);
    PairingRecord(
        PairingRecord&& source) noexcept;
    PairingRecord& operator=(
        const PairingRecord& source);
    PairingRecord& operator=(
        PairingRecord&& source) noexcept;
    ~PairingRecord();

    QString deviceId;
    QString displayName;
    QByteArray secret;
    QDateTime pairedAt;
    std::optional<QString> relayUrlString;

    friend bool operator==(
        const PairingRecord&,
        const PairingRecord&) = default;
};

class PairingRecordStore final {
public:
    PairingRecordStore(
        QString filePath,
        const SecretProtector& protector);
    ~PairingRecordStore();

    PairingRecordStore(
        const PairingRecordStore&) = delete;
    PairingRecordStore& operator=(
        const PairingRecordStore&) = delete;

    static QString defaultFilePath();
    static QByteArray protectionEntropy();

    std::optional<CompanionError>
    loadError() const;

    std::optional<PairingRecord> record(
        const QString& deviceId) const;
    QVector<PairingRecord> records() const;

    Result<void> save(
        const PairingRecord& record);
    Result<void> remove(
        const QString& deviceId);

private:
    using RecordMap =
        QHash<QString, PairingRecord>;

    void load();
    Result<void> persist(
        const RecordMap& records) const;

    QString filePath_;
    const SecretProtector* protector_ =
        nullptr;
    mutable QMutex mutex_;
    RecordMap recordsById_;
    std::optional<CompanionError> loadError_;
};

} // namespace companion
