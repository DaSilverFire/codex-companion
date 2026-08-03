#include "mobile/security/PairingRecordStore.h"

#include "platform/windows/security/WindowsCrypto.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <utility>

namespace companion {
namespace {

constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;
constexpr qsizetype kPairingSecretBytes =
    32;
constexpr qint64 kMaximumExactJsonInteger =
    (qint64(1) << 53) - 1;

CompanionError storeError(
    QString code,
    QString message,
    const QString& path,
    QString causeCode = {})
{
    QVariantMap context{
        {QStringLiteral("path"), path},
    };
    if (!causeCode.isEmpty()) {
        context.insert(
            QStringLiteral("causeCode"),
            std::move(causeCode));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

PairingRecord deepCopy(
    const PairingRecord& source)
{
    return PairingRecord(source);
}

QHash<QString, PairingRecord>
deepCopyRecords(
    const QHash<QString, PairingRecord>& source)
{
    QHash<QString, PairingRecord> result;
    result.reserve(source.size());
    for (auto iterator = source.cbegin();
         iterator != source.cend();
         ++iterator) {
        result.insert(
            iterator.key(),
            deepCopy(iterator.value()));
    }
    return result;
}

void clearSecrets(
    QHash<QString, PairingRecord>& records)
{
    for (auto iterator = records.begin();
         iterator != records.end();
         ++iterator) {
        WindowsCrypto::secureZero(
            iterator->secret);
    }
    records.clear();
}

bool isValidRecord(
    const PairingRecord& record)
{
    return !record.deviceId.isEmpty()
        && !record.displayName.isEmpty()
        && record.secret.size()
            == kPairingSecretBytes
        && record.pairedAt.isValid();
}

} // namespace

PairingRecord::PairingRecord(
    QString deviceIdValue,
    QString displayNameValue,
    QByteArray secretValue,
    QDateTime pairedAtValue,
    std::optional<QString>
        relayUrlStringValue)
    : deviceId(
          std::move(deviceIdValue)),
      displayName(
          std::move(displayNameValue)),
      secret(
          std::move(secretValue)),
      pairedAt(
          std::move(pairedAtValue)),
      relayUrlString(
          std::move(relayUrlStringValue))
{
}

PairingRecord::PairingRecord(
    const PairingRecord& source)
    : deviceId(source.deviceId),
      displayName(source.displayName),
      secret(
          source.secret.constData(),
          source.secret.size()),
      pairedAt(source.pairedAt),
      relayUrlString(
          source.relayUrlString)
{
}

PairingRecord::PairingRecord(
    PairingRecord&& source) noexcept
    : deviceId(
          std::move(source.deviceId)),
      displayName(
          std::move(source.displayName)),
      secret(
          std::move(source.secret)),
      pairedAt(
          std::move(source.pairedAt)),
      relayUrlString(
          std::move(
              source.relayUrlString))
{
}

PairingRecord& PairingRecord::operator=(
    const PairingRecord& source)
{
    if (this == &source) {
        return *this;
    }
    PairingRecord replacement(source);
    return *this =
        std::move(replacement);
}

PairingRecord& PairingRecord::operator=(
    PairingRecord&& source) noexcept
{
    if (this == &source) {
        return *this;
    }
    WindowsCrypto::secureZero(secret);
    deviceId =
        std::move(source.deviceId);
    displayName =
        std::move(source.displayName);
    secret =
        std::move(source.secret);
    pairedAt =
        std::move(source.pairedAt);
    relayUrlString =
        std::move(
            source.relayUrlString);
    return *this;
}

PairingRecord::~PairingRecord()
{
    WindowsCrypto::secureZero(secret);
}

PairingRecordStore::PairingRecordStore(
    QString filePath,
    const SecretProtector& protector)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath()),
      protector_(&protector)
{
    load();
}

PairingRecordStore::~PairingRecordStore()
{
    QMutexLocker locker(&mutex_);
    clearSecrets(recordsById_);
}

QString PairingRecordStore::defaultFilePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::
                GenericDataLocation);
    return QDir(root).filePath(
        QStringLiteral(
            "Codex Companion/Security/"
            "paired-devices.v1.json"));
}

QByteArray
PairingRecordStore::protectionEntropy()
{
    return QByteArray(
        "Codex Companion paired-device secret v1");
}

std::optional<CompanionError>
PairingRecordStore::loadError() const
{
    QMutexLocker locker(&mutex_);
    return loadError_;
}

std::optional<PairingRecord>
PairingRecordStore::record(
    const QString& deviceId) const
{
    QMutexLocker locker(&mutex_);
    const auto iterator =
        recordsById_.constFind(deviceId);
    if (iterator == recordsById_.cend()) {
        return std::nullopt;
    }
    return deepCopy(iterator.value());
}

QVector<PairingRecord>
PairingRecordStore::records() const
{
    QMutexLocker locker(&mutex_);
    QVector<PairingRecord> result;
    result.reserve(recordsById_.size());
    for (const PairingRecord& value :
         recordsById_) {
        result.append(deepCopy(value));
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(
        Qt::CaseInsensitive);
    std::sort(
        result.begin(),
        result.end(),
        [&collator](
            const PairingRecord& left,
            const PairingRecord& right) {
            if (left.pairedAt
                != right.pairedAt) {
                return left.pairedAt
                    < right.pairedAt;
            }
            const int displayOrder =
                collator.compare(
                    left.displayName,
                    right.displayName);
            if (displayOrder != 0) {
                return displayOrder < 0;
            }
            return left.deviceId
                < right.deviceId;
        });
    return result;
}

Result<void> PairingRecordStore::save(
    const PairingRecord& record)
{
    if (!isValidRecord(record)) {
        return Result<void>::failure(
            storeError(
                QStringLiteral(
                    "pairing.invalid_record"),
                QStringLiteral(
                    "A paired-device record is invalid."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    RecordMap candidate =
        deepCopyRecords(recordsById_);
    auto existing =
        candidate.find(record.deviceId);
    if (existing != candidate.end()) {
        WindowsCrypto::secureZero(
            existing->secret);
        *existing = deepCopy(record);
    } else {
        candidate.insert(
            record.deviceId,
            deepCopy(record));
    }

    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        clearSecrets(candidate);
        return persisted;
    }
    clearSecrets(recordsById_);
    recordsById_ = std::move(candidate);
    loadError_.reset();
    return Result<void>::success();
}

Result<void> PairingRecordStore::remove(
    const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return Result<void>::failure(
            storeError(
                QStringLiteral(
                    "pairing.invalid_device"),
                QStringLiteral(
                    "The paired-device identifier is empty."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (!recordsById_.contains(deviceId)) {
        return Result<void>::success();
    }

    RecordMap candidate =
        deepCopyRecords(recordsById_);
    auto removed =
        candidate.find(deviceId);
    WindowsCrypto::secureZero(
        removed->secret);
    candidate.erase(removed);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        clearSecrets(candidate);
        return persisted;
    }
    clearSecrets(recordsById_);
    recordsById_ = std::move(candidate);
    loadError_.reset();
    return Result<void>::success();
}

void PairingRecordStore::load()
{
    QFileInfo information(filePath_);
    if (!information.exists()) {
        return;
    }

    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)
        || file.size() < 0
        || file.size() > kMaximumStoreBytes) {
        loadError_ = storeError(
            QStringLiteral(
                "pairing.store_corrupt"),
            QStringLiteral(
                "The paired-device store could not be read."),
            filePath_);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            file.readAll(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        loadError_ = storeError(
            QStringLiteral(
                "pairing.store_corrupt"),
            QStringLiteral(
                "The paired-device store is malformed."),
            filePath_);
        return;
    }

    const QJsonObject root =
        document.object();
    const QJsonValue version =
        root.value(QStringLiteral("version"));
    const QJsonValue recordsValue =
        root.value(QStringLiteral("records"));
    if (!version.isDouble()
        || version.toInteger(-1) != 1
        || !recordsValue.isArray()) {
        loadError_ = storeError(
            QStringLiteral(
                "pairing.store_corrupt"),
            QStringLiteral(
                "The paired-device store version is invalid."),
            filePath_);
        return;
    }

    RecordMap loaded;
    const QByteArray entropy =
        protectionEntropy();
    const auto failLoad = [&](
                              QString message,
                              QString cause = {}) {
        clearSecrets(loaded);
        loadError_ = storeError(
            QStringLiteral(
                "pairing.store_corrupt"),
            std::move(message),
            filePath_,
            std::move(cause));
    };

    for (const QJsonValue value :
         recordsValue.toArray()) {
        if (!value.isObject()) {
            failLoad(
                QStringLiteral(
                    "A paired-device record is malformed."));
            return;
        }
        const QJsonObject object =
            value.toObject();
        const QJsonValue deviceValue =
            object.value(
                QStringLiteral("deviceID"));
        const QJsonValue displayValue =
            object.value(
                QStringLiteral("displayName"));
        const QJsonValue secretValue =
            object.value(
                QStringLiteral(
                    "secretProtected"));
        const QJsonValue pairedAtValue =
            object.value(
                QStringLiteral(
                    "pairedAtMilliseconds"));
        if (!deviceValue.isString()
            || deviceValue.toString().isEmpty()
            || !displayValue.isString()
            || displayValue.toString().isEmpty()
            || !secretValue.isString()
            || !pairedAtValue.isDouble()) {
            failLoad(
                QStringLiteral(
                    "A paired-device record is incomplete."));
            return;
        }

        const qint64 invalidTimestamp =
            std::numeric_limits<
                qint64>::min();
        const qint64 pairedAtMilliseconds =
            pairedAtValue.toInteger(
                invalidTimestamp);
        if (pairedAtMilliseconds
                == invalidTimestamp
            || pairedAtMilliseconds
                < -kMaximumExactJsonInteger
            || pairedAtMilliseconds
                > kMaximumExactJsonInteger
            || pairedAtValue.toDouble()
                != static_cast<double>(
                    pairedAtMilliseconds)) {
            failLoad(
                QStringLiteral(
                    "A paired-device timestamp is invalid."));
            return;
        }

        const QString deviceId =
            deviceValue.toString();
        if (loaded.contains(deviceId)) {
            failLoad(
                QStringLiteral(
                    "The paired-device store contains duplicate identifiers."));
            return;
        }

        const auto decoded =
            QByteArray::fromBase64Encoding(
                secretValue.toString()
                    .toLatin1(),
                QByteArray::
                    AbortOnBase64DecodingErrors);
        if (!decoded) {
            failLoad(
                QStringLiteral(
                    "A paired-device secret is not valid base64."));
            return;
        }
        auto secret =
            protector_->unprotect(
                decoded.decoded,
                entropy);
        if (!secret.hasValue()) {
            failLoad(
                QStringLiteral(
                    "A paired-device secret could not be unlocked."),
                secret.error().code);
            return;
        }
        QByteArray unlockedSecret =
            std::move(secret.value());
        if (unlockedSecret.size()
            != kPairingSecretBytes) {
            WindowsCrypto::secureZero(
                unlockedSecret);
            failLoad(
                QStringLiteral(
                    "A paired-device secret has an invalid size."));
            return;
        }

        std::optional<QString> relayUrl;
        const QJsonValue relayValue =
            object.value(
                QStringLiteral(
                    "relayURLString"));
        if (!relayValue.isUndefined()
            && !relayValue.isNull()) {
            if (!relayValue.isString()) {
                WindowsCrypto::secureZero(
                    unlockedSecret);
                failLoad(
                    QStringLiteral(
                        "A paired-device relay URL is malformed."));
                return;
            }
            relayUrl = relayValue.toString();
        }

        PairingRecord next{
            deviceId,
            displayValue.toString(),
            std::move(unlockedSecret),
            QDateTime::fromMSecsSinceEpoch(
                pairedAtMilliseconds,
                QTimeZone::UTC),
            std::move(relayUrl),
        };
        if (!isValidRecord(next)) {
            WindowsCrypto::secureZero(
                next.secret);
            failLoad(
                QStringLiteral(
                    "A paired-device record is invalid."));
            return;
        }
        loaded.insert(
            next.deviceId,
            std::move(next));
    }

    recordsById_ = std::move(loaded);
    loadError_.reset();
}

Result<void> PairingRecordStore::persist(
    const RecordMap& records) const
{
    QJsonArray encodedRecords;
    QStringList deviceIds =
        records.keys();
    std::sort(
        deviceIds.begin(),
        deviceIds.end());
    const QByteArray entropy =
        protectionEntropy();

    for (const QString& deviceId :
         deviceIds) {
        const auto iterator =
            records.constFind(deviceId);
        const PairingRecord& record =
            iterator.value();
        const auto protectedSecret =
            protector_->protect(
                record.secret,
                entropy);
        if (!protectedSecret.hasValue()) {
            return Result<void>::failure(
                storeError(
                    QStringLiteral(
                        "pairing.store_write_failed"),
                    QStringLiteral(
                        "A paired-device secret could not be protected."),
                    filePath_,
                    protectedSecret.error().code));
        }

        QJsonObject object{
            {QStringLiteral("deviceID"),
             record.deviceId},
            {QStringLiteral("displayName"),
             record.displayName},
            {QStringLiteral(
                 "secretProtected"),
             QString::fromLatin1(
                 protectedSecret.value()
                     .toBase64())},
            {QStringLiteral(
                 "pairedAtMilliseconds"),
             record.pairedAt
                 .toMSecsSinceEpoch()},
        };
        if (record.relayUrlString
                .has_value()) {
            object.insert(
                QStringLiteral(
                    "relayURLString"),
                *record.relayUrlString);
        }
        encodedRecords.append(object);
    }

    const QFileInfo information(filePath_);
    QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<void>::failure(
            storeError(
                QStringLiteral(
                    "pairing.store_write_failed"),
                QStringLiteral(
                    "The paired-device directory could not be created."),
                filePath_));
    }

    const QJsonDocument document(
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("records"),
             encodedRecords},
        });
    const QByteArray output =
        document.toJson(
            QJsonDocument::Indented);
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(output)
            != output.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            storeError(
                QStringLiteral(
                    "pairing.store_write_failed"),
                QStringLiteral(
                    "The paired-device store could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

} // namespace companion
