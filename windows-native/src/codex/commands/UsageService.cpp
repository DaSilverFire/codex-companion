#include "codex/commands/UsageService.h"

#include <QCborValue>
#include <QCollator>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPromise>
#include <QTextBoundaryFinder>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace companion {

namespace {

constexpr double kSwiftReferenceDateUnixSeconds =
    978307200.0;

template <typename T>
Result<T> usageFailure(
    QString message,
    QVariantMap context = {})
{
    return Result<T>::failure({
        QStringLiteral("usage.unavailable"),
        std::move(message),
        false,
        std::move(context),
    });
}

template <typename T>
Result<T> usageUnavailable(
    QVariantMap context = {})
{
    return usageFailure<T>(
        QStringLiteral("Codex usage is unavailable."),
        std::move(context));
}

template <typename T>
struct UsageCompletion final {
    explicit UsageCompletion(
        std::shared_ptr<QPromise<Result<T>>> requestedPromise)
        : promise(std::move(requestedPromise))
    {
    }

    void finish(Result<T> result)
    {
        const std::scoped_lock lock(mutex);
        if (finished) {
            return;
        }
        promise->addResult(std::move(result));
        promise->finish();
        finished = true;
    }

    void cancel()
    {
        const std::scoped_lock lock(mutex);
        if (finished) {
            return;
        }
        promise->finish();
        finished = true;
    }

    std::shared_ptr<QPromise<Result<T>>> promise;
    std::mutex mutex;
    bool finished = false;
};

void probeUsageCommit(
    const UsageCommitProbe& probe,
    const QString& phase) noexcept
{
    if (!probe) {
        return;
    }
    try {
        probe(phase);
    } catch (...) {
    }
}

BridgeDate bridgeDateFromUnix(double unixSeconds)
{
    return {
        unixSeconds - kSwiftReferenceDateUnixSeconds,
    };
}

BridgeDate currentBridgeDate()
{
    return bridgeDateFromUnix(
        static_cast<double>(
            QDateTime::currentMSecsSinceEpoch())
        / 1000.0);
}

struct ParsedUsageWindow final {
    BridgeUsageWindow window;
    double seconds = 0.0;
};

bool isMissingOrNull(const QJsonValue& value)
{
    return value.isUndefined() || value.isNull();
}

std::optional<double> doubleValue(
    const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number)) {
        return std::nullopt;
    }
    return number;
}

std::optional<qint64> integerValue(
    const QJsonValue& value)
{
    const QCborValue cbor =
        QCborValue::fromJsonValue(value);
    if (!cbor.isInteger()) {
        return std::nullopt;
    }
    return cbor.toInteger();
}

std::optional<double> optionalDoubleValue(
    const QJsonValue& value,
    bool& ok)
{
    if (isMissingOrNull(value)) {
        return std::nullopt;
    }
    const std::optional<double> parsed =
        doubleValue(value);
    if (!parsed.has_value()) {
        ok = false;
        return std::nullopt;
    }
    return parsed;
}

double requiredDoubleValue(
    const QJsonObject& object,
    const QString& key,
    bool& ok)
{
    const std::optional<double> parsed =
        optionalDoubleValue(object.value(key), ok);
    if (!parsed.has_value()) {
        ok = false;
        return 0.0;
    }
    return *parsed;
}

std::optional<qint64> optionalIntegerValue(
    const QJsonValue& value,
    bool& ok)
{
    if (isMissingOrNull(value)) {
        return std::nullopt;
    }
    const std::optional<qint64> parsed =
        integerValue(value);
    if (!parsed.has_value()) {
        ok = false;
        return std::nullopt;
    }
    return parsed;
}

qint64 requiredIntegerValue(
    const QJsonObject& object,
    const QString& key,
    bool& ok)
{
    const std::optional<qint64> parsed =
        optionalIntegerValue(object.value(key), ok);
    if (!parsed.has_value()) {
        ok = false;
        return 0;
    }
    return *parsed;
}

std::optional<QString> optionalStringValue(
    const QJsonValue& value,
    bool& ok)
{
    if (isMissingOrNull(value)) {
        return std::nullopt;
    }
    if (!value.isString()) {
        ok = false;
        return std::nullopt;
    }
    return value.toString();
}

QString requiredStringValue(
    const QJsonObject& object,
    const QString& key,
    bool& ok)
{
    const std::optional<QString> parsed =
        optionalStringValue(object.value(key), ok);
    if (!parsed.has_value()) {
        ok = false;
        return {};
    }
    return *parsed;
}

std::optional<BridgeDate> optionalUnixDate(
    const QJsonValue& value,
    bool& ok)
{
    const std::optional<double> number =
        optionalDoubleValue(value, ok);
    if (!number.has_value()) {
        return std::nullopt;
    }
    return bridgeDateFromUnix(*number);
}

QString nonempty(QString value)
{
    return value.trimmed();
}

QString uppercaseFirstGrapheme(const QString& word)
{
    if (word.isEmpty()) {
        return word;
    }
    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme,
        word);
    finder.toStart();
    const qsizetype boundary = finder.toNextBoundary();
    if (boundary <= 0) {
        return word.toUpper();
    }
    return word.left(boundary).toUpper()
        + word.mid(boundary);
}

QString displayTitleFromId(QString id)
{
    if (id == QStringLiteral("codex")) {
        return QStringLiteral("Codex");
    }
    QStringList words = id.split(
        QLatin1Char('-'),
        Qt::SkipEmptyParts);
    for (QString& word : words) {
        if (!word.isEmpty()) {
            word = uppercaseFirstGrapheme(word);
        }
    }
    return words.isEmpty() ? id : words.join(QLatin1Char(' '));
}

struct UsageDurationSeconds final {
    double raw = 0.0;
    qint64 rounded = 0;
};

std::optional<UsageDurationSeconds> durationSeconds(
    double windowDurationMins)
{
    if (!std::isfinite(windowDurationMins)) {
        return std::nullopt;
    }
    constexpr long double maxExclusive =
        9223372036854775808.0L;
    const long double raw =
        std::max<long double>(
            0.0L,
            static_cast<long double>(windowDurationMins) * 60.0L);
    if (!std::isfinite(raw) || raw >= maxExclusive) {
        return std::nullopt;
    }
    const long double rounded = std::round(raw);
    if (!std::isfinite(rounded)
        || rounded < 0.0L
        || rounded >= maxExclusive) {
        return std::nullopt;
    }
    return UsageDurationSeconds{
        static_cast<double>(raw),
        static_cast<qint64>(rounded),
    };
}

qint64 ceilDiv(qint64 value, qint64 divisor)
{
    if (value <= 0) {
        return 0;
    }
    return value / divisor
        + (value % divisor == 0 ? 0 : 1);
}

QString durationLabel(qint64 seconds)
{
    if (seconds >= 7 * 24 * 60 * 60) {
        const qint64 weeks =
            std::max<qint64>(
                1,
                ceilDiv(seconds, 7 * 24 * 60 * 60));
        return weeks == 1
            ? QStringLiteral("Weekly")
            : QStringLiteral("%1 Week").arg(weeks);
    }
    if (seconds >= 24 * 60 * 60) {
        const qint64 days =
            std::max<qint64>(
                1,
                ceilDiv(seconds, 24 * 60 * 60));
        return QStringLiteral("%1d").arg(days);
    }
    if (seconds >= 60 * 60) {
        const qint64 hours =
            std::max<qint64>(
                1,
                ceilDiv(seconds, 60 * 60));
        return QStringLiteral("%1h").arg(hours);
    }
    const qint64 minutes =
        std::max<qint64>(
            1,
            ceilDiv(seconds, 60));
    return QStringLiteral("%1m").arg(minutes);
}

Result<std::optional<ParsedUsageWindow>> parseWindow(
    const QJsonValue& value)
{
    if (isMissingOrNull(value)) {
        return Result<std::optional<ParsedUsageWindow>>::success(
            std::nullopt);
    }
    if (!value.isObject()) {
        return usageFailure<std::optional<ParsedUsageWindow>>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QJsonObject object = value.toObject();
    bool ok = true;
    const double usedPercent =
        requiredDoubleValue(
            object,
            QStringLiteral("usedPercent"),
            ok);
    const std::optional<double> durationMins =
        optionalDoubleValue(
            object.value(
                QStringLiteral("windowDurationMins")),
            ok);
    const std::optional<BridgeDate> resetsAt =
        optionalUnixDate(
            object.value(QStringLiteral("resetsAt")),
            ok);
    if (!ok) {
        return usageFailure<std::optional<ParsedUsageWindow>>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const std::optional<UsageDurationSeconds> seconds =
        durationSeconds(durationMins.value_or(0.0));
    if (!seconds.has_value()) {
        return usageFailure<std::optional<ParsedUsageWindow>>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    BridgeUsageWindow window;
    window.remainingPercent =
        std::clamp(100.0 - usedPercent, 0.0, 100.0);
    window.durationLabel =
        durationLabel(seconds->rounded);
    window.resetsAt = resetsAt;
    return Result<std::optional<ParsedUsageWindow>>::success(
        ParsedUsageWindow{
            std::move(window),
            seconds->raw,
        });
}

Result<BridgeUsageGroup> parseGroup(
    const QString& id,
    const QJsonObject& rateLimit)
{
    bool ok = true;
    const std::optional<QString> rawLimitId =
        optionalStringValue(
            rateLimit.value(QStringLiteral("limitId")),
            ok);
    const std::optional<QString> rawTitle =
        optionalStringValue(
            rateLimit.value(QStringLiteral("limitName")),
            ok);
    const std::optional<QString> rawPlanType =
        optionalStringValue(
            rateLimit.value(QStringLiteral("planType")),
            ok);
    const std::optional<QString> rawReachedType =
        optionalStringValue(
            rateLimit.value(
                QStringLiteral("rateLimitReachedType")),
            ok);
    Q_UNUSED(rawLimitId);
    Q_UNUSED(rawPlanType);
    Q_UNUSED(rawReachedType);
    if (!ok) {
        return usageFailure<BridgeUsageGroup>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QString limitId = id;
    QString title = rawTitle.has_value()
        ? nonempty(*rawTitle)
        : QString();
    if (title.isEmpty()) {
        title = displayTitleFromId(limitId);
    }

    QVector<ParsedUsageWindow> windows;
    const Result<std::optional<ParsedUsageWindow>> primary =
        parseWindow(rateLimit.value(QStringLiteral("primary")));
    if (!primary.hasValue()) {
        return Result<BridgeUsageGroup>::failure(
            primary.error());
    }
    if (primary.value().has_value()) {
        windows.append(*primary.value());
    }
    const Result<std::optional<ParsedUsageWindow>> secondary =
        parseWindow(rateLimit.value(QStringLiteral("secondary")));
    if (!secondary.hasValue()) {
        return Result<BridgeUsageGroup>::failure(
            secondary.error());
    }
    if (secondary.value().has_value()) {
        windows.append(*secondary.value());
    }

    BridgeUsageGroup group;
    group.id = limitId;
    group.title = title;
    std::optional<double> shortSeconds;
    std::optional<double> weeklySeconds;
    for (const ParsedUsageWindow& candidate : windows) {
        if (candidate.seconds < 24 * 60 * 60) {
            if (!group.shortWindow.has_value()
                || candidate.seconds
                    > shortSeconds.value_or(0.0)) {
                group.shortWindow = candidate.window;
                shortSeconds = candidate.seconds;
            }
        } else if (!group.weeklyWindow.has_value()
                   || candidate.seconds
                       > weeklySeconds.value_or(0.0)) {
            group.weeklyWindow = candidate.window;
            weeklySeconds = candidate.seconds;
        }
    }
    return Result<BridgeUsageGroup>::success(std::move(group));
}

bool isResetType(const QString& value)
{
    return value == QStringLiteral("codexRateLimits")
        || value == QStringLiteral("unknown");
}

bool isResetStatus(const QString& value)
{
    return value == QStringLiteral("available")
        || value == QStringLiteral("redeeming")
        || value == QStringLiteral("redeemed")
        || value == QStringLiteral("unknown");
}

Result<QVector<BridgeResetCredit>> parseCredits(
    const QJsonObject& summary)
{
    QVector<BridgeResetCredit> credits;
    const QJsonValue rowsValue =
        summary.value(QStringLiteral("credits"));
    if (isMissingOrNull(rowsValue)) {
        return Result<QVector<BridgeResetCredit>>::success(
            std::move(credits));
    }
    if (!rowsValue.isArray()) {
        return usageFailure<QVector<BridgeResetCredit>>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QJsonArray rows = rowsValue.toArray();
    for (const QJsonValue& rowValue : rows) {
        if (!rowValue.isObject()) {
            return usageFailure<QVector<BridgeResetCredit>>(
                QStringLiteral(
                    "Codex app-server returned unreadable usage data."));
        }
        const QJsonObject row = rowValue.toObject();
        bool ok = true;
        const QString id =
            requiredStringValue(
                row,
                QStringLiteral("id"),
                ok);
        const QString resetType =
            requiredStringValue(
                row,
                QStringLiteral("resetType"),
                ok);
        const QString status =
            requiredStringValue(
                row,
                QStringLiteral("status"),
                ok);
        const double grantedAt =
            requiredDoubleValue(
                row,
                QStringLiteral("grantedAt"),
                ok);
        const std::optional<double> expiresAt =
            optionalDoubleValue(
                row.value(QStringLiteral("expiresAt")),
                ok);
        const std::optional<QString> rawTitle =
            optionalStringValue(
                row.value(QStringLiteral("title")),
                ok);
        const std::optional<QString> rawDetail =
            optionalStringValue(
                row.value(QStringLiteral("description")),
                ok);
        Q_UNUSED(grantedAt);
        if (!ok || !isResetType(resetType)
            || !isResetStatus(status)) {
            return usageFailure<QVector<BridgeResetCredit>>(
                QStringLiteral(
                    "Codex app-server returned unreadable usage data."));
        }
        if (status != QStringLiteral("available")) {
            continue;
        }
        QString displayTitle =
            rawTitle.has_value()
            ? rawTitle->trimmed()
            : QString();
        if (displayTitle.isEmpty()) {
            displayTitle = QStringLiteral("Codex usage reset");
        }
        BridgeResetCredit credit;
        credit.id = id;
        credit.displayTitle = displayTitle;
        if (rawDetail.has_value()) {
            credit.detail = *rawDetail;
        }
        if (expiresAt.has_value()) {
            credit.expiresAt =
                bridgeDateFromUnix(*expiresAt);
        }
        credits.append(std::move(credit));
    }
    return Result<QVector<BridgeResetCredit>>::success(
        std::move(credits));
}

Result<void> parseResetCreditSummary(
    const QJsonValue& value,
    BridgeUsageSnapshot& snapshot)
{
    if (isMissingOrNull(value)) {
        snapshot.availableResetCount = 0;
        snapshot.availableResetCredits.clear();
        return Result<void>::success();
    }
    if (!value.isObject()) {
        return usageFailure<void>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QJsonObject summary = value.toObject();
    bool ok = true;
    const qint64 availableCount =
        requiredIntegerValue(
            summary,
            QStringLiteral("availableCount"),
            ok);
    if (!ok) {
        return usageFailure<void>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const Result<QVector<BridgeResetCredit>> credits =
        parseCredits(summary);
    if (!credits.hasValue()) {
        return Result<void>::failure(credits.error());
    }
    snapshot.availableResetCount =
        std::max<qint64>(0, availableCount);
    snapshot.availableResetCredits = credits.value();
    return Result<void>::success();
}

Result<BridgeUsageSnapshot> parseUsageSnapshot(
    const QJsonObject& object,
    BridgeDate updatedAt)
{
    const QJsonValue rateLimitsValue =
        object.value(QStringLiteral("rateLimits"));
    if (!rateLimitsValue.isObject()) {
        return usageFailure<BridgeUsageSnapshot>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QJsonObject rateLimits =
        rateLimitsValue.toObject();

    BridgeUsageSnapshot snapshot;
    bool ok = true;
    const std::optional<QString> plan =
        optionalStringValue(
            rateLimits.value(QStringLiteral("planType")),
            ok);
    if (!ok) {
        return usageFailure<BridgeUsageSnapshot>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    if (plan.has_value()) {
        snapshot.planType = *plan;
    }
    ok = true;
    const std::optional<QString>
        baseReachedType =
            optionalStringValue(
                rateLimits.value(
                    QStringLiteral(
                        "rateLimitReachedType")),
                ok);
    if (!ok) {
        return usageFailure<
            BridgeUsageSnapshot>(
                QStringLiteral(
                    "Codex app-server returned unreadable usage data."));
    }
    if (baseReachedType.has_value()
        && !baseReachedType
                ->trimmed()
                .isEmpty()) {
        snapshot.rateLimitReachedType =
            baseReachedType
                ->trimmed();
    }
    ok = true;
    const std::optional<QString> rawBaseId =
        optionalStringValue(
            rateLimits.value(QStringLiteral("limitId")),
            ok);
    if (!ok) {
        return usageFailure<BridgeUsageSnapshot>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    const QString baseId = rawBaseId.has_value()
        ? *rawBaseId
        : QStringLiteral("codex");
    const Result<BridgeUsageGroup> baseGroup =
        parseGroup(baseId, rateLimits);
    if (!baseGroup.hasValue()) {
        return Result<BridgeUsageSnapshot>::failure(
            baseGroup.error());
    }

    const QJsonValue byIdValue =
        object.value(QStringLiteral("rateLimitsByLimitId"));
    if (!isMissingOrNull(byIdValue)) {
        if (!byIdValue.isObject()) {
            return usageFailure<BridgeUsageSnapshot>(
                QStringLiteral(
                    "Codex app-server returned unreadable usage data."));
        }
    }
    const QJsonObject byId =
        byIdValue.isObject() ? byIdValue.toObject() : QJsonObject();
    if (!byId.isEmpty()) {
        for (auto iterator = byId.constBegin();
             iterator != byId.constEnd();
             ++iterator) {
            if (!iterator.value().isObject()) {
                return usageFailure<BridgeUsageSnapshot>(
                    QStringLiteral(
                        "Codex app-server returned unreadable usage data."));
            }
            const Result<BridgeUsageGroup> group =
                parseGroup(
                    iterator.key(),
                    iterator.value().toObject());
            if (!group.hasValue()) {
                return Result<BridgeUsageSnapshot>::failure(
                    group.error());
            }
            snapshot.groups.append(group.value());
            if (iterator.key()
                    == QStringLiteral(
                        "codex")) {
                bool reachedTypeValid =
                    true;
                const auto reachedType =
                    optionalStringValue(
                        iterator.value()
                            .toObject()
                            .value(
                                QStringLiteral(
                                    "rateLimitReachedType")),
                        reachedTypeValid);
                if (!reachedTypeValid) {
                    return usageFailure<
                        BridgeUsageSnapshot>(
                            QStringLiteral(
                                "Codex app-server returned unreadable usage data."));
                }
                if (reachedType
                        .has_value()
                    && !reachedType
                            ->trimmed()
                            .isEmpty()) {
                    snapshot
                        .rateLimitReachedType =
                        reachedType
                            ->trimmed();
                }
            }
        }
    }
    if (snapshot.groups.isEmpty()) {
        snapshot.groups.append(baseGroup.value());
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(
        snapshot.groups.begin(),
        snapshot.groups.end(),
        [&collator](
            const BridgeUsageGroup& lhs,
            const BridgeUsageGroup& rhs) {
            const bool lhsCodex =
                lhs.id == QStringLiteral("codex");
            const bool rhsCodex =
                rhs.id == QStringLiteral("codex");
            if (lhsCodex != rhsCodex) {
                return lhsCodex;
            }
            return collator.compare(lhs.title, rhs.title) < 0;
        });

    const Result<void> resetCredits =
        parseResetCreditSummary(
            object.value(QStringLiteral("rateLimitResetCredits")),
            snapshot);
    if (!resetCredits.hasValue()) {
        return Result<BridgeUsageSnapshot>::failure(
            resetCredits.error());
    }
    snapshot.updatedAt = updatedAt;
    return Result<BridgeUsageSnapshot>::success(
        std::move(snapshot));
}

Result<QJsonObject> singleResultObject(
    const QHash<int, RpcResponse>& responses)
{
    const auto iterator = responses.constFind(2);
    if (iterator == responses.constEnd()
        || iterator.value().isError
        || !iterator.value().result.isObject()) {
        return usageFailure<QJsonObject>(
            QStringLiteral(
                "Codex app-server returned unreadable usage data."));
    }
    return Result<QJsonObject>::success(
        iterator.value().result.toObject());
}

std::optional<UsageResetOutcome> resetOutcomeFrom(
    const QString& raw)
{
    if (raw == QStringLiteral("reset")) {
        return UsageResetOutcome::Reset;
    }
    if (raw == QStringLiteral("nothingToReset")) {
        return UsageResetOutcome::NothingToReset;
    }
    if (raw == QStringLiteral("noCredit")) {
        return UsageResetOutcome::NoCredit;
    }
    if (raw == QStringLiteral("alreadyRedeemed")) {
        return UsageResetOutcome::AlreadyRedeemed;
    }
    return std::nullopt;
}

} // namespace

UsageService::UsageService(
    const CodexEnvironment& environment,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds)
    : UsageService(
          [client = std::make_shared<AppServerRpcClient>(
               environment,
               std::move(processEnvironment),
               timeoutMilliseconds)](
              const QVector<RpcRequest>& requests) {
              return client->perform(requests);
          })
{
}

UsageService::UsageService(
    UsageRpcPerformer performer,
    UsageClock clock,
    UsageCommitProbe consumeCommitProbe)
    : performer_(std::move(performer)),
      clock_(clock ? std::move(clock) : currentBridgeDate),
      consumeCommitProbe_(std::move(consumeCommitProbe))
{
}

QFuture<Result<BridgeUsageSnapshot>> UsageService::read() const
{
    auto promise =
        std::make_shared<QPromise<Result<BridgeUsageSnapshot>>>();
    promise->start();
    QFuture<Result<BridgeUsageSnapshot>> future =
        promise->future();
    QThreadPool::globalInstance()->start(
        [performer = performer_, clock = clock_, promise] {
            UsageCompletion<BridgeUsageSnapshot> completion(promise);
            try {
            if (promise->isCanceled()) {
                completion.cancel();
                return;
            }
            if (!performer) {
                completion.finish(
                    usageUnavailable<BridgeUsageSnapshot>(
                        {{QStringLiteral("method"),
                          QStringLiteral("account/rateLimits/read")}}));
                return;
            }
            const Result<QHash<int, RpcResponse>> responses =
                performer({
                    {
                        2,
                        QStringLiteral("account/rateLimits/read"),
                        {},
                    },
                });
            if (promise->isCanceled()) {
                completion.cancel();
                return;
            }
            if (!responses.hasValue()) {
                completion.finish(
                    usageUnavailable<BridgeUsageSnapshot>(
                        {{QStringLiteral("method"),
                          QStringLiteral("account/rateLimits/read")}}));
                return;
            }
            const Result<QJsonObject> object =
                singleResultObject(responses.value());
            if (!object.hasValue()) {
                completion.finish(
                    Result<BridgeUsageSnapshot>::failure(
                        object.error()));
                return;
            }
            const BridgeDate updatedAt = clock();
            completion.finish(
                parseUsageSnapshot(
                    object.value(),
                    updatedAt));
            } catch (...) {
                completion.finish(
                    usageUnavailable<BridgeUsageSnapshot>(
                        {{QStringLiteral("method"),
                          QStringLiteral("account/rateLimits/read")}}));
            }
        });
    return future;
}

CommitAwareMutationHandle<UsageResetOutcome>
UsageService::consumeResetMutation(
    const QString& creditId,
    const QUuid& idempotencyKey) const
{
    auto mutation =
        CommitAwareMutation<UsageResetOutcome>::create();
    CommitAwareMutationHandle<UsageResetOutcome> handle =
        mutation->handle();
    try {
        QThreadPool::globalInstance()->start(
        [performer = performer_,
         consumeCommitProbe = consumeCommitProbe_,
         creditId = creditId.trimmed(),
         idempotencyKey,
         mutation] {
            try {
            if (creditId.isEmpty()
                || idempotencyKey.isNull()) {
                mutation->finish(
                    usageFailure<UsageResetOutcome>(
                        QStringLiteral(
                            "Choose an available Codex reset first.")));
                return;
            }
            if (!performer) {
                mutation->finish(
                    usageUnavailable<UsageResetOutcome>(
                        {{QStringLiteral("method"),
                          QStringLiteral(
                              "account/rateLimitResetCredit/consume")}}));
                return;
            }
            probeUsageCommit(
                consumeCommitProbe,
                QStringLiteral("consumeReset.commitPending"));
            probeUsageCommit(
                consumeCommitProbe,
                QStringLiteral("consumeReset.claimEstablished"));
            if (!mutation->tryCommit()) {
                return;
            }
            probeUsageCommit(
                consumeCommitProbe,
                QStringLiteral("consumeReset.committed"));
            const Result<QHash<int, RpcResponse>> responses =
                performer({
                    {
                        2,
                        QStringLiteral(
                            "account/rateLimitResetCredit/consume"),
                        {
                            {
                                QStringLiteral("creditId"),
                                creditId,
                            },
                            {
                                QStringLiteral("idempotencyKey"),
                                idempotencyKey.toString(
                                    QUuid::WithoutBraces)
                                    .toUpper(),
                            },
                        },
                    },
                });
            if (!responses.hasValue()) {
                mutation->finish(
                    usageUnavailable<UsageResetOutcome>(
                        {{QStringLiteral("method"),
                          QStringLiteral(
                              "account/rateLimitResetCredit/consume")}}));
                return;
            }
            const Result<QJsonObject> object =
                singleResultObject(responses.value());
            if (!object.hasValue()) {
                mutation->finish(
                    Result<UsageResetOutcome>::failure(
                        object.error()));
                return;
            }
            const std::optional<UsageResetOutcome> outcome =
                resetOutcomeFrom(
                    object.value()
                        .value(QStringLiteral("outcome"))
                        .toString());
            if (!outcome.has_value()) {
                mutation->finish(
                    usageFailure<UsageResetOutcome>(
                        QStringLiteral(
                            "Codex app-server returned unreadable usage data.")));
                return;
            }
            mutation->finish(
                Result<UsageResetOutcome>::success(
                    *outcome));
            } catch (...) {
                mutation->finish(
                    usageUnavailable<UsageResetOutcome>(
                        {{QStringLiteral("method"),
                          QStringLiteral(
                              "account/rateLimitResetCredit/consume")}}));
            }
        });
    } catch (...) {
        mutation->finish(
            usageUnavailable<UsageResetOutcome>(
                {{QStringLiteral("method"),
                  QStringLiteral(
                      "account/rateLimitResetCredit/consume")}}));
    }
    return handle;
}

QFuture<Result<UsageResetOutcome>> UsageService::consumeReset(
    const QString& creditId,
    const QUuid& idempotencyKey) const
{
    return cancellationDetachedMutationFuture(
        consumeResetMutation(
            creditId,
            idempotencyKey)
            .terminalFuture);
}

} // namespace companion
