#include "mobile/security/RelayStateStore.h"

#include "platform/windows/security/WindowsCrypto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <limits>
#include <utility>

namespace companion {
namespace {

constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;

CompanionError relayStateError(
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

CompanionError invalidKeyError(
    const QString& path)
{
    return relayStateError(
        QStringLiteral(
            "relay.invalid_state_key"),
        QStringLiteral(
            "Relay state channel and sender identifiers must not be empty."),
        path);
}

std::optional<quint64>
parseCanonicalUnsigned(
    const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text =
        value.toString();
    if (text.isEmpty()
        || (text.size() > 1
            && text.front()
                == QLatin1Char('0'))) {
        return std::nullopt;
    }
    for (const QChar character :
         text) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return std::nullopt;
        }
    }
    bool converted = false;
    const quint64 result =
        text.toULongLong(
            &converted,
            10);
    if (!converted
        || QString::number(result)
            != text) {
        return std::nullopt;
    }
    return result;
}

class RelayStateJsonShapeValidator final {
public:
    explicit RelayStateJsonShapeValidator(
        QByteArrayView bytes)
        : bytes_(bytes.data(), bytes.size())
    {
    }

    bool validate()
    {
        skipWhitespace();
        if (!parseRoot()) {
            return false;
        }
        skipWhitespace();
        return position_ == bytes_.size();
    }

private:
    bool parseRoot()
    {
        if (!consume('{')) {
            return false;
        }
        skipWhitespace();

        QSet<QString> seen;
        bool hasEntries = false;
        bool hasVersion = false;
        if (consume('}')) {
            return false;
        }
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)
                || seen.contains(key)) {
                return false;
            }
            seen.insert(key);
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            skipWhitespace();

            if (key
                == QStringLiteral("entries")) {
                if (!parseEntries()) {
                    return false;
                }
                hasEntries = true;
            } else if (
                key
                == QStringLiteral("version")) {
                if (!consume('1')) {
                    return false;
                }
                hasVersion = true;
            } else {
                return false;
            }

            skipWhitespace();
            if (consume('}')) {
                return hasEntries
                    && hasVersion
                    && seen.size() == 2;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseEntries()
    {
        if (!consume('[')) {
            return false;
        }
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (position_ < bytes_.size()) {
            if (!parseEntry()) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseEntry()
    {
        if (!consume('{')) {
            return false;
        }
        skipWhitespace();

        static const QSet<QString> allowed{
            QStringLiteral("channelID"),
            QStringLiteral("highestInbound"),
            QStringLiteral("nextOutbound"),
            QStringLiteral("senderID"),
        };
        QSet<QString> seen;
        if (consume('}')) {
            return false;
        }
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)
                || !allowed.contains(key)
                || seen.contains(key)) {
                return false;
            }
            seen.insert(key);
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            skipWhitespace();
            if (!readString(nullptr)) {
                return false;
            }

            skipWhitespace();
            if (consume('}')) {
                return seen.size()
                    == allowed.size();
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool readString(QString* value)
    {
        if (!consume('"')) {
            return false;
        }
        const qsizetype start =
            position_ - 1;
        while (position_ < bytes_.size()) {
            const char current =
                bytes_.at(position_++);
            if (current == '\\') {
                if (position_
                    >= bytes_.size()) {
                    return false;
                }
                const char escape =
                    bytes_.at(position_++);
                if (escape == 'u') {
                    if (position_ + 4
                        > bytes_.size()) {
                        return false;
                    }
                    for (int index = 0;
                         index < 4;
                         ++index) {
                        const char digit =
                            bytes_.at(
                                position_ + index);
                        const bool hexadecimal =
                            (digit >= '0'
                             && digit <= '9')
                            || (digit >= 'a'
                                && digit <= 'f')
                            || (digit >= 'A'
                                && digit <= 'F');
                        if (!hexadecimal) {
                            return false;
                        }
                    }
                    position_ += 4;
                } else if (
                    escape != '"'
                    && escape != '\\'
                    && escape != '/'
                    && escape != 'b'
                    && escape != 'f'
                    && escape != 'n'
                    && escape != 'r'
                    && escape != 't') {
                    return false;
                }
                continue;
            }
            if (current == '"') {
                if (value != nullptr) {
                    QByteArray wrapped("[");
                    wrapped.append(
                        bytes_.mid(
                            start,
                            position_ - start));
                    wrapped.append(']');
                    const QJsonDocument document =
                        QJsonDocument::fromJson(
                            wrapped);
                    if (!document.isArray()
                        || document.array()
                               .size()
                            != 1
                        || !document.array()
                                .at(0)
                                .isString()) {
                        return false;
                    }
                    *value =
                        document.array()
                            .at(0)
                            .toString();
                }
                return true;
            }
            if (static_cast<unsigned char>(
                    current)
                < 0x20U) {
                return false;
            }
        }
        return false;
    }

    bool consume(char expected)
    {
        if (position_ < bytes_.size()
            && bytes_.at(position_)
                == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (
            position_ < bytes_.size()
            && (bytes_.at(position_) == ' '
                || bytes_.at(position_) == '\n'
                || bytes_.at(position_) == '\r'
                || bytes_.at(position_) == '\t')) {
            ++position_;
        }
    }

    QByteArray bytes_;
    qsizetype position_ = 0;
};

} // namespace

RelayStateStore::RelayStateStore(
    QString filePath,
    const SecretProtector& protector)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath()),
      protector_(&protector)
{
    load();
}

QString RelayStateStore::defaultFilePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::
                GenericDataLocation);
    return QDir(root).filePath(
        QStringLiteral(
            "Codex Companion/Security/"
            "relay-state.v1.dpapi"));
}

QByteArray
RelayStateStore::protectionEntropy()
{
    return QByteArray(
        "Codex Companion relay state v1");
}

Result<quint64>
RelayStateStore::nextOutbound(
    const QString& channelId,
    const QString& senderId)
{
    if (channelId.isEmpty()
        || senderId.isEmpty()) {
        return Result<quint64>::failure(
            invalidKeyError(filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<quint64>::failure(
            *loadError_);
    }

    Entry current;
    const auto channel =
        state_.constFind(channelId);
    if (channel != state_.cend()) {
        const auto sender =
            channel->constFind(senderId);
        if (sender
            != channel->cend()) {
            current = sender.value();
        }
    }
    if (current.nextOutbound
        == std::numeric_limits<
            quint64>::max()) {
        return Result<quint64>::failure(
            relayStateError(
                QStringLiteral(
                    "relay.sequence_exhausted"),
                QStringLiteral(
                    "The relay sequence counter is exhausted."),
                filePath_));
    }

    const quint64 issued =
        current.nextOutbound;
    Entry next = current;
    ++next.nextOutbound;
    State candidate = state_;
    candidate[channelId].insert(
        senderId,
        next);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return Result<quint64>::failure(
            persisted.error());
    }
    state_ = std::move(candidate);
    return Result<quint64>::success(
        issued);
}

Result<bool>
RelayStateStore::acceptInbound(
    const QString& channelId,
    const QString& senderId,
    quint64 sequence)
{
    if (channelId.isEmpty()
        || senderId.isEmpty()) {
        return Result<bool>::failure(
            invalidKeyError(filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<bool>::failure(
            *loadError_);
    }

    Entry current;
    const auto channel =
        state_.constFind(channelId);
    if (channel != state_.cend()) {
        const auto sender =
            channel->constFind(senderId);
        if (sender
            != channel->cend()) {
            current = sender.value();
        }
    }
    if (sequence == 0
        || sequence
            <= current.highestInbound) {
        return Result<bool>::success(
            false);
    }

    Entry next = current;
    next.highestInbound = sequence;
    State candidate = state_;
    candidate[channelId].insert(
        senderId,
        next);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return Result<bool>::failure(
            persisted.error());
    }
    state_ = std::move(candidate);
    return Result<bool>::success(true);
}

Result<void> RelayStateStore::eraseChannel(
    const QString& channelId)
{
    if (channelId.isEmpty()) {
        return Result<void>::failure(
            invalidKeyError(filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<void>::failure(
            *loadError_);
    }
    if (!state_.contains(channelId)) {
        return Result<void>::success();
    }

    State candidate = state_;
    candidate.remove(channelId);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return persisted;
    }
    state_ = std::move(candidate);
    return Result<void>::success();
}

void RelayStateStore::load()
{
    const QFileInfo information(
        filePath_);
    if (!information.exists()) {
        return;
    }

    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)
        || file.size() < 0
        || file.size()
            > kMaximumStoreBytes) {
        loadError_ = relayStateError(
            QStringLiteral(
                "relay.state_corrupt"),
            QStringLiteral(
                "The relay state file could not be read."),
            filePath_);
        return;
    }
    const QByteArray protectedState =
        file.readAll();
    auto unprotected =
        protector_->unprotect(
            protectedState,
            protectionEntropy());
    if (!unprotected.hasValue()) {
        loadError_ = relayStateError(
            QStringLiteral(
                "relay.state_corrupt"),
            QStringLiteral(
                "The relay state file could not be unlocked."),
            filePath_,
            unprotected.error().code);
        return;
    }

    QByteArray plaintext =
        std::move(
            unprotected.value());
    const bool validShape =
        RelayStateJsonShapeValidator(
            plaintext)
            .validate();
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            plaintext,
            &parseError);
    WindowsCrypto::secureZero(
        plaintext);
    if (!validShape
        || parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        loadError_ = relayStateError(
            QStringLiteral(
                "relay.state_corrupt"),
            QStringLiteral(
                "The relay state plaintext is malformed."),
            filePath_);
        return;
    }

    const QJsonObject root =
        document.object();
    const QJsonValue version =
        root.value(
            QStringLiteral("version"));
    const QJsonValue entriesValue =
        root.value(
            QStringLiteral("entries"));
    if (!version.isDouble()
        || version.toInteger(-1) != 1
        || !entriesValue.isArray()) {
        loadError_ = relayStateError(
            QStringLiteral(
                "relay.state_corrupt"),
            QStringLiteral(
                "The relay state version is invalid."),
            filePath_);
        return;
    }

    State loaded;
    for (const QJsonValue value :
         entriesValue.toArray()) {
        if (!value.isObject()) {
            loadError_ = relayStateError(
                QStringLiteral(
                    "relay.state_corrupt"),
                QStringLiteral(
                    "A relay state entry is malformed."),
                filePath_);
            return;
        }
        const QJsonObject object =
            value.toObject();
        const QJsonValue channelValue =
            object.value(
                QStringLiteral(
                    "channelID"));
        const QJsonValue senderValue =
            object.value(
                QStringLiteral(
                    "senderID"));
        const auto nextOutbound =
            parseCanonicalUnsigned(
                object.value(
                    QStringLiteral(
                        "nextOutbound")));
        const auto highestInbound =
            parseCanonicalUnsigned(
                object.value(
                    QStringLiteral(
                        "highestInbound")));
        if (!channelValue.isString()
            || channelValue.toString()
                .isEmpty()
            || !senderValue.isString()
            || senderValue.toString()
                .isEmpty()
            || !nextOutbound.has_value()
            || *nextOutbound == 0
            || !highestInbound
                    .has_value()) {
            loadError_ = relayStateError(
                QStringLiteral(
                    "relay.state_corrupt"),
                QStringLiteral(
                    "A relay state entry is invalid."),
                filePath_);
            return;
        }

        const QString channelId =
            channelValue.toString();
        const QString senderId =
            senderValue.toString();
        SenderState& senders =
            loaded[channelId];
        if (senders.contains(senderId)) {
            loadError_ = relayStateError(
                QStringLiteral(
                    "relay.state_corrupt"),
                QStringLiteral(
                    "The relay state contains duplicate entries."),
                filePath_);
            return;
        }
        senders.insert(
            senderId,
            {
                *nextOutbound,
                *highestInbound,
            });
    }

    state_ = std::move(loaded);
    loadError_.reset();
}

Result<void> RelayStateStore::persist(
    const State& state) const
{
    QStringList channelIds =
        state.keys();
    std::sort(
        channelIds.begin(),
        channelIds.end());

    QJsonArray entries;
    for (const QString& channelId :
         channelIds) {
        const SenderState& senders =
            state.value(channelId);
        QStringList senderIds =
            senders.keys();
        std::sort(
            senderIds.begin(),
            senderIds.end());
        for (const QString& senderId :
             senderIds) {
            const Entry entry =
                senders.value(senderId);
            entries.append(
                QJsonObject{
                    {
                        QStringLiteral(
                            "channelID"),
                        channelId,
                    },
                    {
                        QStringLiteral(
                            "highestInbound"),
                        QString::number(
                            entry.highestInbound),
                    },
                    {
                        QStringLiteral(
                            "nextOutbound"),
                        QString::number(
                            entry.nextOutbound),
                    },
                    {
                        QStringLiteral(
                            "senderID"),
                        senderId,
                    },
                });
        }
    }

    QByteArray plaintext =
        QJsonDocument(
            QJsonObject{
                {
                    QStringLiteral(
                        "entries"),
                    entries,
                },
                {
                    QStringLiteral(
                        "version"),
                    1,
                },
            })
            .toJson(
                QJsonDocument::Compact);
    auto protectedState =
        protector_->protect(
            plaintext,
            protectionEntropy());
    WindowsCrypto::secureZero(
        plaintext);
    if (!protectedState.hasValue()) {
        return Result<void>::failure(
            relayStateError(
                QStringLiteral(
                    "relay.state_write_failed"),
                QStringLiteral(
                    "The relay state could not be protected."),
                filePath_,
                protectedState.error().code));
    }

    const QFileInfo information(
        filePath_);
    QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<void>::failure(
            relayStateError(
                QStringLiteral(
                    "relay.state_write_failed"),
                QStringLiteral(
                    "The relay state directory could not be created."),
                filePath_));
    }

    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(
               protectedState.value())
            != protectedState.value()
                   .size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            relayStateError(
                QStringLiteral(
                    "relay.state_write_failed"),
                QStringLiteral(
                    "The relay state file could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

} // namespace companion
