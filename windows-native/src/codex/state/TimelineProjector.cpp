#include "codex/state/TimelineProjector.h"

#include "codex/models/CodexModels.h"
#include "codex/state/ToolProjection.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace companion {

namespace {

inline constexpr qint64 kReferenceDateUnixMilliseconds =
    978307200000LL;
inline constexpr auto kSuppressedToolWrapperTitle =
    "__companion_suppressed_tool_wrapper__";

struct RawLine final {
    qint64 offset = 0;
    QByteArray data;
};

struct ParsedVisibleMessage final {
    BridgeMessage message;
    std::optional<QString> turnId;
};

struct RawTimelineRecord final {
    qint64 offset = 0;
    BridgeTimelineItem item;
};

struct SemanticTimelineRecord final {
    qint64 offset = 0;
    BridgeTimelineItem item;
};

struct DelegationSummary final {
    std::optional<QString> targetId;
    QString targetLabel;
    std::optional<QString> message;
};

CompanionError historyError(
    QString code,
    QString message,
    const QString& path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {{QStringLiteral("path"), path}},
    };
}

std::optional<QString> nonempty(QString value)
{
    value = value.trimmed();
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(std::move(value));
}

BridgeDate bridgeDate(const QDateTime& date)
{
    if (!date.isValid()) {
        return {};
    }
    return {
        static_cast<double>(
            date.toMSecsSinceEpoch() -
            kReferenceDateUnixMilliseconds) /
        1000.0,
    };
}

std::optional<QDateTime> createdAt(
    const QJsonObject& root)
{
    const auto timestamp = nonempty(
        root.value(QStringLiteral("timestamp")).toString());
    if (!timestamp.has_value()) {
        return std::nullopt;
    }
    QDateTime parsed =
        QDateTime::fromString(*timestamp, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(*timestamp, Qt::ISODate);
    }
    return parsed.isValid()
        ? std::optional<QDateTime>(parsed.toUTC())
        : std::nullopt;
}

std::optional<BridgeDate> createdBridgeDate(
    const QJsonObject& root)
{
    const auto date = createdAt(root);
    return date.has_value()
        ? std::optional<BridgeDate>(bridgeDate(*date))
        : std::nullopt;
}

std::optional<QString> turnId(
    const QJsonObject& payload)
{
    if (const auto direct = nonempty(
            payload.value(QStringLiteral("turn_id")).toString());
        direct.has_value()) {
        return direct;
    }
    const QJsonValue metadata = payload.value(QStringLiteral(
        "internal_chat_message_metadata_passthrough"));
    if (!metadata.isObject()) {
        return std::nullopt;
    }
    return nonempty(metadata.toObject()
        .value(QStringLiteral("turn_id"))
        .toString());
}

QString strippingBoundaryEnvironmentContext(QString text)
{
    const QString opening =
        QStringLiteral("<environment_context>");
    const QString closing =
        QStringLiteral("</environment_context>");
    text = text.trimmed();

    while (text.startsWith(opening)) {
        const qsizetype closingIndex =
            text.indexOf(closing, opening.size());
        if (closingIndex < 0) {
            break;
        }
        text = text.mid(
            closingIndex + closing.size()).trimmed();
    }

    while (text.endsWith(closing)) {
        const qsizetype openingIndex =
            text.lastIndexOf(opening);
        if (openingIndex < 0) {
            break;
        }
        text = text.left(openingIndex).trimmed();
    }
    return text;
}

QString sanitizedVisibleText(QString text)
{
    text = strippingBoundaryEnvironmentContext(
        std::move(text));
    if (text.startsWith(
            QStringLiteral("<subagent_notification")) &&
        text.endsWith(
            QStringLiteral("</subagent_notification>"))) {
        return {};
    }

    const QString openingPrefix =
        QStringLiteral("<in-app-browser-context");
    const QString trustedSource =
        QStringLiteral("source=\"ambient-ui-state\"");
    const QString closingTag =
        QStringLiteral("</in-app-browser-context>");
    bool removedAmbientContext = false;
    while (text.startsWith(openingPrefix)) {
        const qsizetype openingEnd =
            text.indexOf(QLatin1Char('>'));
        if (openingEnd < 0 ||
            !text.left(openingEnd + 1)
                 .contains(trustedSource)) {
            break;
        }
        const qsizetype closingIndex =
            text.indexOf(closingTag, openingEnd + 1);
        if (closingIndex < 0) {
            break;
        }
        text = text.mid(
            closingIndex + closingTag.size()).trimmed();
        removedAmbientContext = true;
    }

    const QString requestHeading =
        QStringLiteral("## My request for Codex:");
    const qsizetype headingIndex =
        text.lastIndexOf(requestHeading);
    if (headingIndex >= 0) {
        const QString prefix = text.left(headingIndex);
        const bool generatedMetadata =
            removedAmbientContext ||
            prefix.contains(QStringLiteral(
                "# Files mentioned by the user:")) ||
            prefix.contains(QStringLiteral(
                "# Applications mentioned by the user:")) ||
            prefix.contains(QStringLiteral(
                "<in-app-browser-context "
                "source=\"ambient-ui-state\">"));
        if (generatedMetadata) {
            text = text.mid(
                headingIndex + requestHeading.size()).trimmed();
        }
    }

    static const QRegularExpression imagePattern(QStringLiteral(
        "(?is)[ \\t]*<image\\s+name=\\[[^\\]]+\\]\\s+"
        "path=\"[^\"]*\">[ \\t\\r\\n]*</image>[ \\t]*"));
    text.remove(imagePattern);
    return text.trimmed();
}

std::optional<QString> visibleText(
    const QJsonValue& rawContent)
{
    QString text;
    if (rawContent.isString()) {
        text = rawContent.toString();
    } else if (rawContent.isArray()) {
        QStringList fragments;
        for (const QJsonValue& value : rawContent.toArray()) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject fragment = value.toObject();
            const QString type =
                fragment.value(QStringLiteral("type")).toString();
            if (type != QStringLiteral("input_text") &&
                type != QStringLiteral("output_text")) {
                continue;
            }
            if (const auto fragmentText = nonempty(
                    fragment.value(QStringLiteral("text"))
                        .toString());
                fragmentText.has_value()) {
                fragments.append(*fragmentText);
            }
        }
        text = fragments.join(QLatin1Char('\n'));
    }

    text = sanitizedVisibleText(std::move(text));
    if (text.isEmpty() ||
        text.startsWith(
            QStringLiteral("<codex_internal_context")) ||
        (text.startsWith(
             QStringLiteral("<subagent_notification")) &&
         text.endsWith(
             QStringLiteral("</subagent_notification>")))) {
        return std::nullopt;
    }
    return text;
}

std::optional<ParsedVisibleMessage> visibleMessage(
    const QByteArray& data,
    const QString& fallbackId)
{
    if (data.size() >
        TimelineProjector::kMaximumHistoryLineBytes) {
        return std::nullopt;
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("type")).toString() !=
        QStringLiteral("response_item")) {
        return std::nullopt;
    }
    const QJsonValue payloadValue =
        root.value(QStringLiteral("payload"));
    if (!payloadValue.isObject()) {
        return std::nullopt;
    }
    const QJsonObject payload = payloadValue.toObject();
    if (payload.value(QStringLiteral("type")).toString() !=
        QStringLiteral("message")) {
        return std::nullopt;
    }

    const QString rawRole =
        payload.value(QStringLiteral("role")).toString();
    MessageRole role;
    if (rawRole == QStringLiteral("user")) {
        role = MessageRole::User;
    } else if (rawRole == QStringLiteral("assistant")) {
        role = MessageRole::Assistant;
    } else {
        return std::nullopt;
    }

    const auto text = visibleText(
        payload.value(QStringLiteral("content")));
    if (!text.has_value()) {
        return std::nullopt;
    }

    QString id =
        payload.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        id = payload.value(
            QStringLiteral("message_id")).toString();
    }
    if (id.isEmpty()) {
        id = fallbackId;
    }
    return ParsedVisibleMessage{
        {
            id,
            role,
            *text,
            createdBridgeDate(root),
            std::nullopt,
        },
        turnId(payload),
    };
}

QVector<BridgeMedia> inlineMedia(
    const QJsonValue& rawContent,
    const QString& messageId)
{
    QVector<BridgeMedia> media;
    if (!rawContent.isArray()) {
        return media;
    }

    const QJsonArray fragments = rawContent.toArray();
    for (qsizetype index = 0; index < fragments.size(); ++index) {
        if (!fragments.at(index).isObject()) {
            continue;
        }
        const QJsonObject fragment =
            fragments.at(index).toObject();
        if (fragment.value(QStringLiteral("type")).toString() !=
            QStringLiteral("input_image")) {
            continue;
        }
        const QString url =
            fragment.value(QStringLiteral("image_url")).toString();
        if (!url.startsWith(QStringLiteral("data:"))) {
            continue;
        }
        const qsizetype separator = url.indexOf(QLatin1Char(','));
        if (separator < 0) {
            continue;
        }
        const QString metadata = url.left(separator);
        if (!metadata.endsWith(QStringLiteral(";base64"))) {
            continue;
        }
        const QString mimeType = metadata.mid(
            5, metadata.size() - 5 - 7);
        if (mimeType.isEmpty()) {
            continue;
        }
        const QByteArray bytes = QByteArray::fromBase64(
            url.mid(separator + 1).toUtf8());
        if (bytes.size() >
            TimelineProjector::kMaximumInlineMediaBytes) {
            continue;
        }
        media.append({
            messageId + QStringLiteral("-media-") +
                QString::number(index),
            MediaKind::Image,
            mimeType,
            bytes,
        });
    }
    return media;
}

QString boundedDetail(QString detail)
{
    detail = detail.trimmed();
    if (detail.isEmpty()) {
        return {};
    }

    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme, detail);
    QVector<qsizetype> boundaries{0};
    finder.toStart();
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            break;
        }
        boundaries.append(boundary);
    }
    if (boundaries.size() - 1 <=
        TimelineProjector::kMaximumToolDetailCharacters) {
        return detail;
    }
    return detail.left(boundaries.at(
        TimelineProjector::kMaximumToolDetailCharacters - 3)) +
        QStringLiteral("...");
}

std::optional<QString> boundedOptional(
    const std::optional<QString>& detail)
{
    if (!detail.has_value()) {
        return std::nullopt;
    }
    const QString value = boundedDetail(*detail);
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(value);
}

std::optional<QString> textualToolOutput(
    const QJsonValue& rawOutput)
{
    if (rawOutput.isString()) {
        const QString text = rawOutput.toString();
        if (text.startsWith(QStringLiteral("data:"))) {
            return std::nullopt;
        }
        return boundedOptional(nonempty(text));
    }

    const QStringList keys{
        QStringLiteral("text"),
        QStringLiteral("output_text"),
        QStringLiteral("message"),
        QStringLiteral("result"),
    };
    if (rawOutput.isArray()) {
        QStringList fragments;
        for (const QJsonValue& value : rawOutput.toArray()) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject object = value.toObject();
            for (const QString& key : keys) {
                if (!object.value(key).isString()) {
                    continue;
                }
                const QString text = object.value(key).toString();
                if (!text.startsWith(QStringLiteral("data:"))) {
                    fragments.append(text);
                    break;
                }
            }
        }
        return boundedOptional(nonempty(
            fragments.join(QLatin1Char('\n'))));
    }

    if (rawOutput.isObject()) {
        const QJsonObject object = rawOutput.toObject();
        for (const QString& key : keys) {
            if (!object.value(key).isString()) {
                continue;
            }
            const QString text = object.value(key).toString();
            if (!text.startsWith(QStringLiteral("data:"))) {
                return boundedOptional(nonempty(text));
            }
        }
    }
    return std::nullopt;
}

TimelineStatus toolOutputStatus(const QJsonValue& rawOutput)
{
    if (rawOutput.isObject()) {
        const QJsonObject object = rawOutput.toObject();
        if (!object.value(QStringLiteral("error")).isUndefined() ||
            !object.value(QStringLiteral("errored")).isUndefined()) {
            return TimelineStatus::Failed;
        }
        const QString status =
            object.value(QStringLiteral("status"))
                .toString()
                .toLower();
        if (status == QStringLiteral("failed") ||
            status == QStringLiteral("error") ||
            status == QStringLiteral("errored")) {
            return TimelineStatus::Failed;
        }
    }

    const QString text =
        textualToolOutput(rawOutput)
            .value_or(QString())
            .toLower();
    if (text.startsWith(QStringLiteral("error:")) ||
        text.startsWith(QStringLiteral("script failed")) ||
        text.contains(QStringLiteral(
            "process exited with code 1"))) {
        return TimelineStatus::Failed;
    }
    return TimelineStatus::Completed;
}

QString reasoningTitle(QString raw)
{
    raw = raw.trimmed();
    if (raw.startsWith(QStringLiteral("**")) &&
        raw.endsWith(QStringLiteral("**")) &&
        raw.size() >= 4) {
        return raw.mid(2, raw.size() - 4);
    }
    return raw;
}

std::optional<BridgeTimelineItem> timelineItem(
    const RawLine& line)
{
    if (line.data.size() >
        TimelineProjector::kMaximumHistoryLineBytes) {
        return std::nullopt;
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(line.data, &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    const QJsonValue payloadValue =
        root.value(QStringLiteral("payload"));
    if (!payloadValue.isObject()) {
        return std::nullopt;
    }
    const QJsonObject payload = payloadValue.toObject();
    const QString recordType =
        root.value(QStringLiteral("type")).toString();
    const QString payloadType =
        payload.value(QStringLiteral("type")).toString();
    QString id =
        payload.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        id = QString::number(line.offset);
    }
    const auto itemCreatedAt = createdBridgeDate(root);
    const auto itemTurnId = turnId(payload);

    if (recordType == QStringLiteral("response_item") &&
        payloadType == QStringLiteral("message")) {
        const auto parsed = visibleMessage(
            line.data, QString::number(line.offset));
        if (!parsed.has_value()) {
            return std::nullopt;
        }

        std::optional<TimelinePhase> phase;
        const QString rawPhase =
            payload.value(QStringLiteral("phase")).toString();
        if (rawPhase == QStringLiteral("commentary")) {
            phase = TimelinePhase::Commentary;
        } else if (rawPhase == QStringLiteral("final")) {
            phase = TimelinePhase::Final;
        }
        return BridgeTimelineItem{
            parsed->message.id,
            TimelineKind::Message,
            TimelineStatus::Completed,
            parsed->message.role,
            std::nullopt,
            parsed->message.text,
            std::nullopt,
            phase,
            parsed->message.createdAt,
            parsed->turnId,
            std::nullopt,
            inlineMedia(
                payload.value(QStringLiteral("content")),
                parsed->message.id),
        };
    }

    if (recordType == QStringLiteral("response_item") &&
        (payloadType == QStringLiteral("custom_tool_call") ||
         payloadType == QStringLiteral("function_call"))) {
        const QString name = payload
            .value(QStringLiteral("name"))
            .toString(QStringLiteral("tool"));
        std::optional<QString> input;
        if (payload.value(QStringLiteral("input")).isString()) {
            input = payload.value(
                QStringLiteral("input")).toString();
        } else if (
            payload.value(QStringLiteral("arguments")).isString()) {
            input = payload.value(
                QStringLiteral("arguments")).toString();
        }
        const ToolProjection projection =
            ToolProjector::project(name, input);
        return BridgeTimelineItem{
            id,
            TimelineKind::Tool,
            ToolProjector::callStatus(nonempty(
                payload.value(QStringLiteral("status"))
                    .toString())),
            std::nullopt,
            projection.omitsWrapper
                ? std::optional<QString>(
                      QString::fromLatin1(
                          kSuppressedToolWrapperTitle))
                : std::optional<QString>(projection.title),
            std::nullopt,
            projection.omitsWrapper
                ? std::nullopt
                : projection.detail,
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            nonempty(payload
                .value(QStringLiteral("call_id"))
                .toString()),
            {},
        };
    }

    if (recordType == QStringLiteral("response_item") &&
        (payloadType ==
             QStringLiteral("function_call_output") ||
         payloadType ==
             QStringLiteral("custom_tool_call_output"))) {
        const QJsonValue output =
            payload.value(QStringLiteral("output"));
        return BridgeTimelineItem{
            id,
            TimelineKind::Tool,
            toolOutputStatus(output),
            std::nullopt,
            QStringLiteral("Tool result"),
            std::nullopt,
            textualToolOutput(output),
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            nonempty(payload
                .value(QStringLiteral("call_id"))
                .toString()),
            inlineMedia(output, id),
        };
    }

    if (recordType != QStringLiteral("event_msg")) {
        return std::nullopt;
    }
    if (payloadType == QStringLiteral("agent_reasoning")) {
        const auto title = nonempty(reasoningTitle(
            payload.value(QStringLiteral("text")).toString()));
        if (!title.has_value()) {
            return std::nullopt;
        }
        return BridgeTimelineItem{
            id,
            TimelineKind::Reasoning,
            TimelineStatus::Completed,
            std::nullopt,
            *title,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            std::nullopt,
            {},
        };
    }
    if (payloadType == QStringLiteral("context_compacted")) {
        return BridgeTimelineItem{
            id,
            TimelineKind::Compaction,
            TimelineStatus::Completed,
            std::nullopt,
            QStringLiteral("Context compacted"),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            std::nullopt,
            {},
        };
    }
    if (payloadType == QStringLiteral("patch_apply_end")) {
        const bool succeeded =
            !payload.value(QStringLiteral("success")).isBool() ||
            payload.value(QStringLiteral("success")).toBool();
        return BridgeTimelineItem{
            id,
            TimelineKind::Tool,
            succeeded
                ? TimelineStatus::Completed
                : TimelineStatus::Failed,
            std::nullopt,
            succeeded
                ? std::optional<QString>(
                      QStringLiteral("Edited files"))
                : std::optional<QString>(
                      QStringLiteral("File edit failed")),
            std::nullopt,
            ToolProjector::editedFilePathsFromChanges(
                payload.value(QStringLiteral("changes"))),
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            nonempty(payload
                .value(QStringLiteral("call_id"))
                .toString()),
            {},
        };
    }
    if (payloadType == QStringLiteral("mcp_tool_call_end")) {
        const QJsonValue invocationValue =
            payload.value(QStringLiteral("invocation"));
        if (!invocationValue.isObject()) {
            return std::nullopt;
        }
        const QJsonObject invocation =
            invocationValue.toObject();
        const QString tool = invocation
            .value(QStringLiteral("tool"))
            .toString(QStringLiteral("tool"));
        const auto server = nonempty(invocation
            .value(QStringLiteral("server"))
            .toString());
        std::optional<QString> arguments;
        const QJsonValue rawArguments =
            invocation.value(QStringLiteral("arguments"));
        if (rawArguments.isObject()) {
            arguments = QString::fromUtf8(
                QJsonDocument(rawArguments.toObject())
                    .toJson(QJsonDocument::Compact));
        } else if (rawArguments.isArray()) {
            arguments = QString::fromUtf8(
                QJsonDocument(rawArguments.toArray())
                    .toJson(QJsonDocument::Compact));
        }
        const ToolProjection projection =
            ToolProjector::project(tool, arguments, server);
        return BridgeTimelineItem{
            id,
            TimelineKind::Tool,
            TimelineStatus::Completed,
            std::nullopt,
            projection.title,
            std::nullopt,
            projection.detail,
            std::nullopt,
            itemCreatedAt,
            itemTurnId,
            nonempty(payload
                .value(QStringLiteral("call_id"))
                .toString()),
            {},
        };
    }
    return std::nullopt;
}

std::optional<TaskLifecycle> taskLifecycleState(
    const QByteArray& data)
{
    if (data.size() >
        TimelineProjector::kMaximumHistoryLineBytes) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("type")).toString() !=
        QStringLiteral("event_msg") ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return std::nullopt;
    }
    const QJsonObject payload =
        root.value(QStringLiteral("payload")).toObject();
    const QString type =
        payload.value(QStringLiteral("type")).toString();
    LifecycleState state;
    if (type == QStringLiteral("task_started") ||
        type == QStringLiteral("turn_started")) {
        state = LifecycleState::Active;
    } else if (
        type == QStringLiteral("task_complete") ||
        type == QStringLiteral("task_completed") ||
        type == QStringLiteral("turn_complete") ||
        type == QStringLiteral("turn_completed")) {
        state = LifecycleState::Completed;
    } else if (
        type == QStringLiteral("task_aborted") ||
        type == QStringLiteral("task_failed") ||
        type == QStringLiteral("turn_aborted") ||
        type == QStringLiteral("turn_failed")) {
        state = LifecycleState::Failed;
    } else {
        return std::nullopt;
    }
    return TaskLifecycle{state, nonempty(
        payload.value(QStringLiteral("turn_id")).toString())};
}

std::optional<qint64> integer(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) ||
        std::trunc(number) != number ||
        number <
            static_cast<double>(
                std::numeric_limits<qint64>::min()) ||
        number >
            static_cast<double>(
                std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }
    return static_cast<qint64>(number);
}

std::optional<BridgeContextUsage> contextUsage(
    const QByteArray& data)
{
    if (data.size() >
        TimelineProjector::kMaximumHistoryLineBytes) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("type")).toString() !=
        QStringLiteral("event_msg") ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return std::nullopt;
    }
    const QJsonObject payload =
        root.value(QStringLiteral("payload")).toObject();
    if (payload.value(QStringLiteral("type")).toString() !=
        QStringLiteral("token_count") ||
        !payload.value(QStringLiteral("info")).isObject()) {
        return std::nullopt;
    }
    const QJsonObject info =
        payload.value(QStringLiteral("info")).toObject();
    if (!info.value(
            QStringLiteral("last_token_usage")).isObject()) {
        return std::nullopt;
    }
    const QJsonObject lastUsage = info.value(
        QStringLiteral("last_token_usage")).toObject();
    const auto used = integer(
        lastUsage.value(QStringLiteral("total_tokens")));
    const auto window = integer(
        info.value(QStringLiteral("model_context_window")));
    if (!used.has_value() || !window.has_value() ||
        *used < 0 || *window <= 0) {
        return std::nullopt;
    }
    return BridgeContextUsage{*used, *window};
}

class ReverseLineReader final {
public:
    ReverseLineReader(QFile& file, qint64 endOffset)
        : file_(file), endOffset_(endOffset)
    {
    }

    bool hasMore() const noexcept
    {
        return endOffset_ > 0;
    }

    qint64 endOffset() const noexcept
    {
        return endOffset_;
    }

    Result<QVector<RawLine>> next()
    {
        const qint64 startOffset = std::max<qint64>(
            0,
            endOffset_ -
                TimelineProjector::kHistoryChunkBytes);
        if (!file_.seek(startOffset)) {
            return Result<QVector<RawLine>>::failure(
                historyError(
                    QStringLiteral("codex.history_read_failed"),
                    QStringLiteral(
                        "Could not seek in Codex history."),
                    file_.fileName()));
        }
        QByteArray block =
            file_.read(endOffset_ - startOffset);
        if (block.size() != endOffset_ - startOffset) {
            return Result<QVector<RawLine>>::failure(
                historyError(
                    QStringLiteral("codex.history_read_failed"),
                    QStringLiteral(
                        "Could not read Codex history."),
                    file_.fileName()));
        }

        if (skippingOversizedLine_) {
            const qsizetype newline =
                block.lastIndexOf('\n');
            if (newline >= 0) {
                endOffset_ =
                    startOffset + newline;
                skippingOversizedLine_ = false;
            } else {
                endOffset_ = startOffset;
            }
            return Result<QVector<RawLine>>::success({});
        }

        block.append(carry_);
        qsizetype firstCompleteIndex = 0;
        if (startOffset == 0) {
            carry_.clear();
        } else {
            const qsizetype newline =
                block.indexOf('\n');
            if (newline >= 0) {
                carry_ = block.left(newline);
                firstCompleteIndex = newline + 1;
            } else {
                if (block.size() >
                    TimelineProjector::
                        kMaximumHistoryLineBytes) {
                    carry_.clear();
                    skippingOversizedLine_ = true;
                } else {
                    carry_ = block;
                }
                endOffset_ = startOffset;
                return Result<QVector<RawLine>>::success({});
            }
        }

        const QByteArray complete =
            block.mid(firstCompleteIndex);
        QVector<RawLine> lines;
        qsizetype lineStart = 0;
        while (lineStart < complete.size()) {
            qsizetype lineEnd =
                complete.indexOf('\n', lineStart);
            if (lineEnd < 0) {
                lineEnd = complete.size();
            }
            if (lineEnd > lineStart) {
                qsizetype contentEnd = lineEnd;
                if (contentEnd > lineStart &&
                    complete.at(contentEnd - 1) == '\r') {
                    --contentEnd;
                }
                if (contentEnd > lineStart) {
                    lines.append({
                        startOffset +
                            firstCompleteIndex +
                            lineStart,
                        complete.mid(
                            lineStart,
                            contentEnd - lineStart),
                    });
                }
            }
            if (lineEnd >= complete.size()) {
                break;
            }
            lineStart = lineEnd + 1;
        }
        endOffset_ = startOffset;
        return Result<QVector<RawLine>>::success(
            std::move(lines));
    }

private:
    QFile& file_;
    qint64 endOffset_ = 0;
    QByteArray carry_;
    bool skippingOversizedLine_ = false;
};

template <typename Page>
Result<Page> openHistory(
    const QString& rolloutPath,
    const std::optional<QString>& cursor,
    QFile& file,
    qint64& endOffset)
{
    const QFileInfo information(rolloutPath);
    if (!information.isFile()) {
        return Result<Page>::failure(historyError(
            QStringLiteral("codex.history_missing"),
            QStringLiteral(
                "This task's local history is unavailable."),
            rolloutPath));
    }
    file.setFileName(rolloutPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<Page>::failure(historyError(
            QStringLiteral("codex.history_read_failed"),
            QStringLiteral("Codex history could not be read."),
            rolloutPath));
    }

    const qint64 fileSize = file.size();
    endOffset = fileSize;
    if (!cursor.has_value()) {
        return Result<Page>::success({});
    }

    const QString raw = cursor->trimmed();
    bool valid = false;
    const qint64 decoded = raw.toLongLong(&valid, 10);
    if (!valid || raw.isEmpty() ||
        decoded < 0 || decoded > fileSize) {
        return Result<Page>::failure(historyError(
            QStringLiteral("codex.invalid_cursor"),
            QStringLiteral("The Codex history cursor is invalid."),
            rolloutPath));
    }
    endOffset = decoded;
    return Result<Page>::success({});
}

void appendDetail(
    const QString& detail,
    std::optional<QString>& existing)
{
    const auto value = nonempty(detail);
    if (!value.has_value()) {
        return;
    }
    const auto current = existing.has_value()
        ? nonempty(*existing)
        : std::nullopt;
    if (!current.has_value()) {
        existing = *value;
        return;
    }
    if (*current == *value ||
        current->endsWith(
            QStringLiteral("\n\n") + *value)) {
        return;
    }
    existing = *current +
        QStringLiteral("\n\n") + *value;
}

void appendMedia(
    const QVector<BridgeMedia>& media,
    QVector<BridgeMedia>& existing)
{
    QSet<QString> ids;
    for (const BridgeMedia& item : existing) {
        ids.insert(item.id);
    }
    for (const BridgeMedia& item : media) {
        if (!ids.contains(item.id)) {
            existing.append(item);
            ids.insert(item.id);
        }
    }
}

void mergeUniqueLines(
    const QStringList& values,
    std::optional<QString>& existing)
{
    QStringList lines;
    if (existing.has_value()) {
        for (QString line :
             existing->split(
                 QLatin1Char('\n'),
                 Qt::SkipEmptyParts)) {
            line = line.trimmed();
            if (!line.isEmpty()) {
                lines.append(line);
            }
        }
    }
    for (const QString& value : values) {
        if (const auto item = nonempty(value);
            item.has_value() &&
            !lines.contains(*item)) {
            lines.append(*item);
        }
    }
    existing = lines.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              lines.join(QLatin1Char('\n')));
}

void mergeOutputMediaAndFailure(
    const QVector<RawTimelineRecord>& outputs,
    BridgeTimelineItem& item,
    bool includesText)
{
    QSet<QString> semanticEditOutputs;
    if (item.title ==
        std::optional<QString>(
            QStringLiteral("Edited files"))) {
        QStringList recoveredPaths;
        for (const RawTimelineRecord& output : outputs) {
            if (output.item.status == TimelineStatus::Failed ||
                !output.item.detail.has_value()) {
                continue;
            }
            const auto paths =
                ToolProjector::editedFilePathsFromToolOutput(
                    *output.item.detail);
            if (!paths.has_value()) {
                continue;
            }
            semanticEditOutputs.insert(
                *output.item.detail);
            for (const QString& path :
                 paths->split(QLatin1Char('\n'))) {
                if (!recoveredPaths.contains(path)) {
                    recoveredPaths.append(path);
                }
            }
        }
        if (!recoveredPaths.isEmpty()) {
            mergeUniqueLines(recoveredPaths, item.detail);
        }
    }

    QVector<TimelineStatus> statuses;
    statuses.reserve(outputs.size());
    for (const RawTimelineRecord& output : outputs) {
        statuses.append(output.item.status);
        if (includesText &&
            output.item.detail.has_value() &&
            output.item.detail != item.detail &&
            !semanticEditOutputs.contains(
                *output.item.detail)) {
            appendDetail(
                QStringLiteral("Result\n") +
                    *output.item.detail,
                item.detail);
        }
        appendMedia(output.item.media, item.media);
        if (output.item.status ==
            TimelineStatus::Failed) {
            item.status = TimelineStatus::Failed;
        }
    }
    item.status = ToolProjector::resolvedStatus(
        item.status, statuses);
}

std::optional<QJsonObject> parsedObject(
    const std::optional<QString>& text)
{
    if (!text.has_value()) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(text->toUtf8(), &error);
    return error.error == QJsonParseError::NoError &&
        document.isObject()
        ? std::optional<QJsonObject>(document.object())
        : std::nullopt;
}

std::optional<QString> textFromInputItems(
    const QJsonValue& rawItems)
{
    if (!rawItems.isArray()) {
        return std::nullopt;
    }
    QStringList values;
    for (const QJsonValue& value : rawItems.toArray()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() ==
                QStringLiteral("text") &&
            item.value(QStringLiteral("text")).isString()) {
            values.append(
                item.value(QStringLiteral("text")).toString());
        }
    }
    return nonempty(values.join(QLatin1Char('\n')));
}

QString capitalized(QString value)
{
    value = value.toLower();
    bool startsWord = true;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character.isLetterOrNumber()) {
            if (startsWord) {
                value[index] = character.toUpper();
            }
            startsWord = false;
        } else {
            startsWord = true;
        }
    }
    return value;
}

DelegationSummary delegationSummary(
    const BridgeTimelineItem& item,
    const QVector<RawTimelineRecord>& outputs)
{
    const auto arguments = parsedObject(item.detail);
    std::optional<QJsonObject> outputObject;
    for (const RawTimelineRecord& output : outputs) {
        outputObject = parsedObject(output.item.detail);
        if (outputObject.has_value()) {
            break;
        }
    }

    std::optional<QString> targetId;
    if (arguments.has_value()) {
        targetId = nonempty(arguments->value(
            QStringLiteral("target")).toString());
    }
    if (!targetId.has_value() &&
        outputObject.has_value()) {
        targetId = nonempty(outputObject->value(
            QStringLiteral("agent_id")).toString());
    }

    std::optional<QString> targetLabel;
    if (outputObject.has_value()) {
        targetLabel = nonempty(outputObject->value(
            QStringLiteral("nickname")).toString());
    }
    if (!targetLabel.has_value()) {
        targetLabel = targetId;
    }
    if (!targetLabel.has_value() &&
        arguments.has_value()) {
        const auto agentType = nonempty(arguments->value(
            QStringLiteral("agent_type")).toString());
        if (agentType.has_value()) {
            targetLabel = capitalized(*agentType);
        }
    }

    std::optional<QString> message;
    if (arguments.has_value()) {
        message = nonempty(arguments->value(
            QStringLiteral("message")).toString());
        if (!message.has_value()) {
            message = textFromInputItems(arguments->value(
                QStringLiteral("items")));
        }
    }
    return {
        targetId,
        targetLabel.value_or(QStringLiteral("Agent")),
        message,
    };
}

QString delegationDetail(
    const DelegationSummary& summary)
{
    if (summary.message.has_value()) {
        return QStringLiteral("Target: ") +
            summary.targetLabel +
            QStringLiteral("\n\n") +
            *summary.message;
    }
    return QStringLiteral("Target: ") +
        summary.targetLabel;
}

void mergeDelegation(
    const DelegationSummary& summary,
    const QVector<RawTimelineRecord>& outputs,
    BridgeTimelineItem& item)
{
    if (summary.message.has_value()) {
        appendDetail(*summary.message, item.detail);
    }
    mergeOutputMediaAndFailure(outputs, item, false);
}

void mergeTool(
    const BridgeTimelineItem& tool,
    BridgeTimelineItem& activity)
{
    const QString title =
        tool.title.value_or(QStringLiteral("Used a tool"));
    appendDetail(
        tool.detail.has_value()
            ? title + QLatin1Char('\n') + *tool.detail
            : title,
        activity.detail);
    appendMedia(tool.media, activity.media);
    if (tool.status == TimelineStatus::Failed) {
        activity.status = TimelineStatus::Failed;
    }
}

QVector<SemanticTimelineRecord> semanticTimelineRecords(
    const QVector<RawTimelineRecord>& records)
{
    QSet<QString> representedCallIds;
    QHash<QString, QVector<RawTimelineRecord>> outputsByCallId;
    for (const RawTimelineRecord& record : records) {
        if (record.item.kind != TimelineKind::Tool ||
            !record.item.callId.has_value()) {
            continue;
        }
        if (record.item.title ==
            std::optional<QString>(
                QStringLiteral("Tool result"))) {
            outputsByCallId[*record.item.callId].append(record);
        } else {
            representedCallIds.insert(*record.item.callId);
        }
    }

    QVector<SemanticTimelineRecord> projected;
    struct DelegationContext final {
        qsizetype index = 0;
        std::optional<QString> targetId;
    };
    std::optional<DelegationContext> delegationContext;

    for (const RawTimelineRecord& record : records) {
        BridgeTimelineItem item = record.item;
        if (item.kind == TimelineKind::Tool &&
            item.title == std::optional<QString>(
                QString::fromLatin1(
                    kSuppressedToolWrapperTitle))) {
            continue;
        }

        if (item.kind == TimelineKind::Tool &&
            item.title == std::optional<QString>(
                QStringLiteral("Tool result"))) {
            if (item.callId.has_value() &&
                representedCallIds.contains(*item.callId)) {
                continue;
            }
            if (item.status != TimelineStatus::Failed &&
                !item.detail.has_value() &&
                item.media.isEmpty()) {
                continue;
            }
            projected.append({record.offset, std::move(item)});
            delegationContext = std::nullopt;
            continue;
        }

        if (item.kind != TimelineKind::Tool) {
            projected.append({record.offset, std::move(item)});
            delegationContext = std::nullopt;
            continue;
        }

        const QVector<RawTimelineRecord> outputs =
            item.callId.has_value()
                ? outputsByCallId.value(*item.callId)
                : QVector<RawTimelineRecord>{};
        if (item.title == std::optional<QString>(
                QStringLiteral("Messaged an agent"))) {
            const DelegationSummary summary =
                delegationSummary(item, outputs);
            const bool canMerge =
                delegationContext.has_value() &&
                (!delegationContext->targetId.has_value() ||
                 !summary.targetId.has_value() ||
                 delegationContext->targetId ==
                     summary.targetId);
            if (canMerge) {
                mergeDelegation(
                    summary,
                    outputs,
                    projected[
                        delegationContext->index].item);
                if (!delegationContext->targetId.has_value() &&
                    summary.targetId.has_value()) {
                    delegationContext->targetId =
                        summary.targetId;
                }
            } else {
                item.detail = delegationDetail(summary);
                mergeOutputMediaAndFailure(
                    outputs, item, false);
                projected.append({
                    record.offset,
                    std::move(item),
                });
                delegationContext = DelegationContext{
                    projected.size() - 1,
                    summary.targetId,
                };
            }
            continue;
        }

        if (item.title == std::optional<QString>(
                QStringLiteral("Wait"))) {
            mergeOutputMediaAndFailure(
                outputs, item, true);
            if (item.status != TimelineStatus::Failed &&
                item.media.isEmpty()) {
                continue;
            }
            if (delegationContext.has_value()) {
                mergeTool(
                    item,
                    projected[
                        delegationContext->index].item);
            } else {
                projected.append({
                    record.offset,
                    std::move(item),
                });
            }
            continue;
        }

        mergeOutputMediaAndFailure(outputs, item, true);
        delegationContext = std::nullopt;
        projected.append({record.offset, std::move(item)});
    }
    return projected;
}

bool hasCompleteLeadingSemanticGroup(
    const QVector<RawTimelineRecord>& reverseChronological)
{
    if (reverseChronological.isEmpty()) {
        return true;
    }
    const BridgeTimelineItem& earliest =
        reverseChronological.last().item;
    if (earliest.kind == TimelineKind::Message ||
        earliest.kind == TimelineKind::Reasoning ||
        earliest.kind == TimelineKind::Status ||
        earliest.kind == TimelineKind::Compaction) {
        return true;
    }
    return earliest.kind == TimelineKind::Tool &&
        earliest.title == std::optional<QString>(
            QStringLiteral("Messaged an agent"));
}

QVector<RawTimelineRecord> chronologicalRecords(
    const QVector<RawTimelineRecord>& reverseChronological)
{
    QVector<RawTimelineRecord> records;
    records.reserve(reverseChronological.size());
    for (auto iterator = reverseChronological.crbegin();
         iterator != reverseChronological.crend();
         ++iterator) {
        records.append(*iterator);
    }
    return records;
}

bool shouldMarkLatestReasoningActive(
    const QVector<SemanticTimelineRecord>& records,
    const std::optional<TaskLifecycle>& lifecycle)
{
    if (lifecycle.has_value()) {
        if (!lifecycle->isActive()) {
            return false;
        }
        for (const SemanticTimelineRecord& record : records) {
            if (record.item.kind != TimelineKind::Reasoning) {
                continue;
            }
            if (!lifecycle->turnId.has_value() ||
                record.item.turnId == lifecycle->turnId) {
                return true;
            }
        }
        return false;
    }
    return !records.isEmpty() &&
        records.last().item.kind == TimelineKind::Reasoning;
}

int boundedLimit(int limit)
{
    return std::clamp(limit, 1, 50);
}

} // namespace

Result<MessagePage> TimelineProjector::loadMessages(
    const QString& rolloutPath,
    const std::optional<QString>& cursor,
    int requestedLimit)
{
    QFile file;
    qint64 endOffset = 0;
    const Result<MessagePage> opened = openHistory<MessagePage>(
        rolloutPath, cursor, file, endOffset);
    if (!opened.hasValue()) {
        return opened;
    }

    const int limit = boundedLimit(requestedLimit);
    ReverseLineReader reader(file, endOffset);
    struct CollectedMessage final {
        qint64 offset = 0;
        BridgeMessage message;
    };
    QVector<CollectedMessage> collected;
    collected.reserve(limit);

    while (reader.hasMore() &&
           collected.size() < limit) {
        const Result<QVector<RawLine>> block =
            reader.next();
        if (!block.hasValue()) {
            return Result<MessagePage>::failure(block.error());
        }
        const QVector<RawLine>& lines = block.value();
        for (auto iterator = lines.crbegin();
             iterator != lines.crend();
             ++iterator) {
            const auto parsed = visibleMessage(
                iterator->data,
                QString::number(iterator->offset));
            if (!parsed.has_value()) {
                continue;
            }
            collected.append({
                iterator->offset,
                parsed->message,
            });
            if (collected.size() == limit) {
                break;
            }
        }
    }

    QVector<BridgeMessage> chronological;
    chronological.reserve(collected.size());
    for (auto iterator = collected.crbegin();
         iterator != collected.crend();
         ++iterator) {
        chronological.append(iterator->message);
    }
    std::optional<QString> nextCursor;
    if (collected.size() == limit &&
        !collected.isEmpty() &&
        collected.last().offset > 0) {
        nextCursor =
            QString::number(collected.last().offset);
    }
    return Result<MessagePage>::success({
        std::move(chronological),
        nextCursor,
    });
}

Result<TimelinePage> TimelineProjector::loadTimeline(
    const QString& rolloutPath,
    const std::optional<QString>& cursor,
    int requestedLimit)
{
    QFile file;
    qint64 endOffset = 0;
    const Result<TimelinePage> opened = openHistory<TimelinePage>(
        rolloutPath, cursor, file, endOffset);
    if (!opened.hasValue()) {
        return opened;
    }

    const int limit = boundedLimit(requestedLimit);
    const qint64 fileSize = file.size();
    ReverseLineReader reader(file, endOffset);
    QVector<RawTimelineRecord> reverseChronological;
    QVector<SemanticTimelineRecord> semantic;
    std::optional<BridgeContextUsage> latestContextUsage;
    std::optional<TaskLifecycle> latestLifecycle;

    while (reader.hasMore() &&
           (semantic.size() < limit ||
            !hasCompleteLeadingSemanticGroup(
                reverseChronological))) {
        const Result<QVector<RawLine>> block =
            reader.next();
        if (!block.hasValue()) {
            return Result<TimelinePage>::failure(block.error());
        }
        const QVector<RawLine>& lines = block.value();
        for (auto iterator = lines.crbegin();
             iterator != lines.crend();
             ++iterator) {
            if (!cursor.has_value() &&
                !latestContextUsage.has_value()) {
                latestContextUsage =
                    contextUsage(iterator->data);
            }
            if (!cursor.has_value() &&
                !latestLifecycle.has_value()) {
                latestLifecycle =
                    taskLifecycleState(iterator->data);
            }
            const auto item = timelineItem(*iterator);
            if (item.has_value()) {
                reverseChronological.append({
                    iterator->offset,
                    *item,
                });
            }
        }
        semantic = semanticTimelineRecords(
            chronologicalRecords(reverseChronological));
    }

    const qsizetype pageStart =
        std::max<qsizetype>(0, semantic.size() - limit);
    QVector<SemanticTimelineRecord> pageRecords =
        semantic.mid(pageStart);

    if (!cursor.has_value() &&
        shouldMarkLatestReasoningActive(
            pageRecords, latestLifecycle)) {
        for (qsizetype index = pageRecords.size();
             index > 0;
             --index) {
            BridgeTimelineItem& item =
                pageRecords[index - 1].item;
            if (item.kind != TimelineKind::Reasoning) {
                continue;
            }
            if (!latestLifecycle.has_value() ||
                !latestLifecycle->turnId.has_value() ||
                item.turnId == latestLifecycle->turnId) {
                item.status = TimelineStatus::InProgress;
                break;
            }
        }
    }
    if (!cursor.has_value() &&
        latestLifecycle.has_value() &&
        !latestLifecycle->isActive()) {
        for (SemanticTimelineRecord& record : pageRecords) {
            if (record.item.kind == TimelineKind::Tool &&
                record.item.status ==
                    TimelineStatus::InProgress) {
                record.item.status =
                    TimelineStatus::Completed;
            }
        }
    }

    const bool hasOlderItems =
        semantic.size() > pageRecords.size() ||
        reader.endOffset() > 0;
    std::optional<QString> nextCursor;
    if (hasOlderItems && !pageRecords.isEmpty()) {
        nextCursor =
            QString::number(pageRecords.first().offset);
    }

    QVector<BridgeTimelineItem> items;
    items.reserve(pageRecords.size());
    for (SemanticTimelineRecord& record : pageRecords) {
        items.append(std::move(record.item));
    }
    const QFileInfo information(rolloutPath);
    return Result<TimelinePage>::success({
        std::move(items),
        nextCursor,
        QString::number(fileSize) +
            QLatin1Char(':') +
            QString::number(
                information.lastModified()
                    .toMSecsSinceEpoch()),
        latestContextUsage,
    });
}

} // namespace companion
