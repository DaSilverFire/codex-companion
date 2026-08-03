#include "mobile/security/PairingCoordinator.h"

#include "platform/windows/security/WindowsCrypto.h"

#include <QMutexLocker>

#include <limits>
#include <utility>

namespace companion {
namespace {

CompanionError coordinatorError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

bool isAsciiPairingCode(
    const QString& code)
{
    if (code.size() != 6) {
        return false;
    }
    for (const QChar character : code) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

} // namespace

PairingCoordinator::PairingCoordinator(
    PairingRecordStore& store,
    PairingClock clock,
    PairingCodeGenerator codeGenerator,
    PairingSecretGenerator secretGenerator,
    QObject* parent)
    : QObject(parent),
      store_(&store),
      clock_(std::move(clock)),
      codeGenerator_(
          std::move(codeGenerator)),
      secretGenerator_(
          std::move(secretGenerator))
{
    if (!clock_) {
        clock_ = [] {
            return QDateTime::
                currentDateTimeUtc();
        };
    }
    if (!codeGenerator_) {
        codeGenerator_ = [] {
            return BridgeSecurity::
                randomPairingCode();
        };
    }
    if (!secretGenerator_) {
        secretGenerator_ = [] {
            return BridgeSecurity::
                randomSecret();
        };
    }
}

Result<ActivePairing>
PairingCoordinator::beginPairing(
    std::chrono::seconds validFor)
{
    if (validFor <=
        std::chrono::seconds::zero()) {
        return Result<ActivePairing>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_duration"),
                    QStringLiteral(
                        "The pairing duration must be positive.")));
    }
    if (validFor.count()
        > std::numeric_limits<qint64>::max()) {
        return Result<ActivePairing>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_duration"),
                    QStringLiteral(
                        "The pairing duration is too large.")));
    }

    const auto code = codeGenerator_();
    if (!code.hasValue()) {
        return Result<ActivePairing>::
            failure(code.error());
    }
    if (!isAsciiPairingCode(
            code.value())) {
        return Result<ActivePairing>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_code"),
                    QStringLiteral(
                        "The pairing code generator did not return six ASCII digits.")));
    }

    const QDateTime now = clock_();
    if (!now.isValid()) {
        return Result<ActivePairing>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_time"),
                    QStringLiteral(
                        "The pairing clock returned an invalid time.")));
    }
    const QDateTime expiresAt =
        now.addSecs(
            static_cast<qint64>(
                validFor.count()));
    if (!expiresAt.isValid()) {
        return Result<ActivePairing>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_duration"),
                    QStringLiteral(
                        "The pairing expiration time is invalid.")));
    }

    ActivePairing next{
        code.value(),
        expiresAt,
    };
    {
        QMutexLocker locker(&mutex_);
        pairing_ = next;
        ++pairingGeneration_;
        completingGeneration_.reset();
    }
    emit pairingStateChanged();
    return Result<ActivePairing>::success(
        std::move(next));
}

void PairingCoordinator::cancelPairing()
{
    bool changed = false;
    {
        QMutexLocker locker(&mutex_);
        changed = pairing_.has_value();
        pairing_.reset();
        if (changed) {
            ++pairingGeneration_;
            completingGeneration_.reset();
        }
    }
    if (changed) {
        emit pairingStateChanged();
    }
}

std::optional<ActivePairing>
PairingCoordinator::activePairing()
{
    const QDateTime now = clock_();
    bool expired = false;
    std::optional<ActivePairing> result;
    {
        QMutexLocker locker(&mutex_);
        if (pairing_.has_value()
            && (!now.isValid()
                || pairing_->expiresAt
                    < now)) {
            pairing_.reset();
            ++pairingGeneration_;
            completingGeneration_.reset();
            expired = true;
        }
        result = pairing_;
    }
    if (expired) {
        emit pairingStateChanged();
    }
    return result;
}

QVector<PairingRecord>
PairingCoordinator::trustedRecords() const
{
    return store_->records();
}

std::optional<PairingRecord>
PairingCoordinator::trustedRecord(
    const QString& deviceId) const
{
    return store_->record(deviceId);
}

Result<void> PairingCoordinator::remember(
    const PairingRecord& record)
{
    const auto saved = store_->save(record);
    if (!saved.hasValue()) {
        return saved;
    }
    emit pairingStateChanged();
    return Result<void>::success();
}

InvitationDecision
PairingCoordinator::invitationDecision(
    const BridgeInvitation& invitation)
{
    const QDateTime now = clock_();
    bool expired = false;
    std::optional<ActivePairing> active;
    {
        QMutexLocker locker(&mutex_);
        if (pairing_.has_value()
            && (!now.isValid()
                || pairing_->expiresAt
                    < now)) {
            pairing_.reset();
            ++pairingGeneration_;
            completingGeneration_.reset();
            expired = true;
        }
        active = pairing_;
    }
    if (expired) {
        emit pairingStateChanged();
    }

    std::optional<QByteArray> trustedSecret;
    auto trusted =
        store_->record(invitation.deviceId);
    if (trusted.has_value()) {
        trustedSecret =
            std::move(trusted->secret);
    }
    return BridgeSecurity::decideInvitation(
        invitation,
        std::move(trustedSecret),
        std::move(active),
        now);
}

Result<PairingRecord>
PairingCoordinator::completePairing(
    const BridgeInvitation& invitation)
{
    const QDateTime decisionTime = clock_();
    bool expired = false;
    std::optional<ActivePairing> active;
    quint64 generation = 0;
    {
        QMutexLocker locker(&mutex_);
        if (pairing_.has_value()
            && (!decisionTime.isValid()
                || pairing_->expiresAt
                    < decisionTime)) {
            pairing_.reset();
            ++pairingGeneration_;
            completingGeneration_.reset();
            expired = true;
        }
        if (pairing_.has_value()) {
            active = pairing_;
            generation = pairingGeneration_;
        }
    }
    if (expired) {
        emit pairingStateChanged();
    }
    if (!active.has_value()) {
        return Result<PairingRecord>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.not_active"),
                    QStringLiteral(
                        "A pairing session is not active.")));
    }

    std::optional<QByteArray> trustedSecret;
    auto trusted =
        store_->record(invitation.deviceId);
    if (trusted.has_value()) {
        trustedSecret =
            std::move(trusted->secret);
    }
    if (BridgeSecurity::decideInvitation(
            invitation,
            std::move(trustedSecret),
            active,
            decisionTime)
        != InvitationDecision::
            AcceptPairing) {
        return Result<PairingRecord>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invitation_rejected"),
                    QStringLiteral(
                        "The Companion pairing invitation was rejected.")));
    }

    {
        QMutexLocker locker(&mutex_);
        if (!pairing_.has_value()
            || pairingGeneration_
                != generation) {
            return Result<PairingRecord>::
                failure(
                    coordinatorError(
                        QStringLiteral(
                            "pairing.session_changed"),
                        QStringLiteral(
                            "The pairing session changed before completion.")));
        }
        if (completingGeneration_
                .has_value()) {
            return Result<PairingRecord>::
                failure(
                    coordinatorError(
                        QStringLiteral(
                            "pairing.completion_in_progress"),
                        QStringLiteral(
                            "The pairing session is already being completed.")));
        }
        completingGeneration_ =
            generation;
    }

    auto secret = secretGenerator_();
    if (!secret.hasValue()) {
        releaseCompletionClaim(
            generation);
        return Result<PairingRecord>::
            failure(secret.error());
    }
    if (secret.value().size() != 32) {
        WindowsCrypto::secureZero(
            secret.value());
        releaseCompletionClaim(
            generation);
        return Result<PairingRecord>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_secret"),
                    QStringLiteral(
                        "The pairing secret generator did not return exactly 32 bytes.")));
    }

    const QDateTime now = clock_();
    if (!now.isValid()) {
        WindowsCrypto::secureZero(
            secret.value());
        releaseCompletionClaim(
            generation);
        return Result<PairingRecord>::
            failure(
                coordinatorError(
                    QStringLiteral(
                        "pairing.invalid_time"),
                    QStringLiteral(
                        "The pairing clock returned an invalid time.")));
    }

    PairingRecord record{
        invitation.deviceId,
        invitation.displayName,
        std::move(secret.value()),
        now,
        std::nullopt,
    };
    Result<void> saved =
        Result<void>::failure(
            coordinatorError(
                QStringLiteral(
                    "pairing.session_changed"),
                QStringLiteral(
                    "The pairing session changed before completion.")));
    bool completed = false;
    {
        QMutexLocker locker(&mutex_);
        if (pairing_.has_value()
            && pairingGeneration_
                == generation
            && completingGeneration_
                == generation) {
            saved = store_->save(record);
            if (saved.hasValue()) {
                pairing_.reset();
                ++pairingGeneration_;
                completingGeneration_.reset();
                completed = true;
            } else {
                completingGeneration_.reset();
            }
        }
    }
    if (!completed) {
        WindowsCrypto::secureZero(
            record.secret);
        releaseCompletionClaim(
            generation);
        return Result<PairingRecord>::
            failure(saved.error());
    }

    emit pairingStateChanged();
    return Result<PairingRecord>::success(
        std::move(record));
}

void PairingCoordinator::releaseCompletionClaim(
    quint64 generation)
{
    QMutexLocker locker(&mutex_);
    if (completingGeneration_
        == generation) {
        completingGeneration_.reset();
    }
}

Result<void> PairingCoordinator::forget(
    const QString& deviceId)
{
    const auto removed =
        store_->remove(deviceId);
    if (!removed.hasValue()) {
        return removed;
    }
    emit pairingStateChanged();
    return Result<void>::success();
}

} // namespace companion
