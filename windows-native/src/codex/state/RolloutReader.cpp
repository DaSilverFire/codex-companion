#include "codex/state/RolloutReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

namespace companion {

namespace {

QString resolvedPath(const QString& rawPath, const QString& codexHome)
{
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const QFileInfo candidate(trimmed);
    return QDir::cleanPath(
        candidate.isAbsolute()
            ? candidate.absoluteFilePath()
            : QDir(codexHome).absoluteFilePath(trimmed));
}

std::optional<QString> nonemptyString(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString trimmed = value.toString().trimmed();
    return trimmed.isEmpty()
        ? std::nullopt
        : std::optional<QString>(trimmed);
}

std::optional<QString> turnId(const QJsonObject& payload)
{
    if (const auto direct =
            nonemptyString(payload.value(QStringLiteral("turn_id")));
        direct.has_value()) {
        return direct;
    }

    const QJsonValue metadataValue = payload.value(
        QStringLiteral("internal_chat_message_metadata_passthrough"));
    if (!metadataValue.isObject()) {
        return std::nullopt;
    }
    return nonemptyString(
        metadataValue.toObject().value(QStringLiteral("turn_id")));
}

std::optional<QDateTime> createdAt(const QJsonObject& root)
{
    const auto timestamp =
        nonemptyString(root.value(QStringLiteral("timestamp")));
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

QString strippingBoundaryEnvironmentContext(QString text)
{
    const QString openingTag =
        QStringLiteral("<environment_context>");
    const QString closingTag =
        QStringLiteral("</environment_context>");
    text = text.trimmed();

    while (text.startsWith(openingTag)) {
        const qsizetype closingIndex =
            text.indexOf(closingTag, openingTag.size());
        if (closingIndex < 0) {
            break;
        }
        text = text.mid(closingIndex + closingTag.size()).trimmed();
    }

    while (text.endsWith(closingTag)) {
        const qsizetype openingIndex = text.lastIndexOf(openingTag);
        if (openingIndex < 0) {
            break;
        }
        text = text.left(openingIndex).trimmed();
    }

    return text;
}

QString sanitizedVisibleText(QString text)
{
    text = strippingBoundaryEnvironmentContext(std::move(text));
    if (text.startsWith(QStringLiteral("<subagent_notification")) &&
        text.endsWith(QStringLiteral("</subagent_notification>"))) {
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
        const qsizetype openingEnd = text.indexOf(QLatin1Char('>'));
        if (openingEnd < 0 ||
            !text.left(openingEnd + 1).contains(trustedSource)) {
            break;
        }
        const qsizetype closingIndex =
            text.indexOf(closingTag, openingEnd + 1);
        if (closingIndex < 0) {
            break;
        }
        text = text.mid(closingIndex + closingTag.size()).trimmed();
        removedAmbientContext = true;
    }

    const QString requestHeading =
        QStringLiteral("## My request for Codex:");
    const qsizetype headingIndex = text.lastIndexOf(requestHeading);
    if (headingIndex >= 0) {
        const QString prefix = text.left(headingIndex);
        const bool hasGeneratedMetadata =
            removedAmbientContext ||
            prefix.contains(
                QStringLiteral("# Files mentioned by the user:")) ||
            prefix.contains(
                QStringLiteral("# Applications mentioned by the user:")) ||
            prefix.contains(QStringLiteral(
                "<in-app-browser-context source=\"ambient-ui-state\">"));
        if (hasGeneratedMetadata) {
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
    const QJsonValue& content,
    CodexMessageRole role)
{
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else if (content.isArray()) {
        QStringList fragments;
        for (const QJsonValue& value : content.toArray()) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject fragment = value.toObject();
            const QString type =
                fragment.value(QStringLiteral("type")).toString();
            const bool visible =
                role == CodexMessageRole::Assistant
                    ? type == QStringLiteral("output_text")
                    : type == QStringLiteral("input_text");
            if (!visible) {
                continue;
            }
            const auto fragmentText =
                nonemptyString(fragment.value(QStringLiteral("text")));
            if (fragmentText.has_value()) {
                fragments.append(*fragmentText);
            }
        }
        text = fragments.join(QLatin1Char('\n'));
    }

    text = sanitizedVisibleText(std::move(text));
    if (text.isEmpty() ||
        text.startsWith(QStringLiteral("<codex_internal_context")) ||
        (text.startsWith(QStringLiteral("<subagent_notification")) &&
         text.endsWith(QStringLiteral("</subagent_notification>")))) {
        return std::nullopt;
    }
    return text;
}

std::optional<QString> mobileVisibleText(
    const QJsonValue& content)
{
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else if (content.isArray()) {
        QStringList fragments;
        for (const QJsonValue& value : content.toArray()) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject fragment = value.toObject();
            const QString type =
                fragment.value(QStringLiteral("type")).toString();
            if (type != QStringLiteral("input_text")
                && type != QStringLiteral("output_text")) {
                continue;
            }
            const QJsonValue textValue =
                fragment.value(QStringLiteral("text"));
            if (textValue.isString()) {
                fragments.append(textValue.toString());
            }
        }
        text = fragments.join(QLatin1Char('\n'));
    }

    text = sanitizedVisibleText(std::move(text));
    if (text.isEmpty()
        || text.startsWith(
            QStringLiteral("<codex_internal_context"))
        || (text.startsWith(
                QStringLiteral("<subagent_notification"))
            && text.endsWith(
                QStringLiteral("</subagent_notification>")))) {
        return std::nullopt;
    }
    return text;
}

std::optional<QString> rawString(const QJsonValue& value)
{
    return value.isString()
        ? std::optional<QString>(value.toString())
        : std::nullopt;
}

std::optional<QString> mobileMessageTurnId(
    const QJsonObject& payload)
{
    if (const auto direct =
            rawString(payload.value(QStringLiteral("turn_id")));
        direct.has_value()) {
        return direct;
    }

    const QJsonValue metadataValue = payload.value(
        QStringLiteral("internal_chat_message_metadata_passthrough"));
    if (!metadataValue.isObject()) {
        return std::nullopt;
    }
    return rawString(
        metadataValue.toObject().value(
            QStringLiteral("turn_id")));
}

std::optional<RolloutMessage> message(
    const QJsonObject& root,
    const QJsonObject& payload)
{
    const QString rootType =
        root.value(QStringLiteral("type")).toString();
    const QString payloadType =
        payload.value(QStringLiteral("type")).toString();

    if (rootType == QStringLiteral("event_msg") &&
        payloadType == QStringLiteral("agent_message")) {
        const auto text =
            nonemptyString(payload.value(QStringLiteral("message")));
        if (!text.has_value()) {
            return std::nullopt;
        }
        return RolloutMessage{
            CodexMessageRole::Assistant,
            *text,
            turnId(payload),
            createdAt(root),
        };
    }

    if (rootType != QStringLiteral("response_item") ||
        payloadType != QStringLiteral("message")) {
        return std::nullopt;
    }

    const QString roleName =
        payload.value(QStringLiteral("role")).toString();
    CodexMessageRole role;
    if (roleName == QStringLiteral("user")) {
        role = CodexMessageRole::User;
    } else if (roleName == QStringLiteral("assistant")) {
        role = CodexMessageRole::Assistant;
    } else {
        return std::nullopt;
    }

    const auto text = visibleText(
        payload.value(QStringLiteral("content")), role);
    if (!text.has_value()) {
        return std::nullopt;
    }

    return RolloutMessage{
        role,
        *text,
        turnId(payload),
        createdAt(root),
    };
}

std::optional<RolloutMessage> mobileMessage(
    const QJsonObject& root,
    const QJsonObject& payload)
{
    if (root.value(QStringLiteral("type")).toString()
            != QStringLiteral("response_item")
        || payload.value(QStringLiteral("type")).toString()
            != QStringLiteral("message")) {
        return std::nullopt;
    }

    const QString roleName =
        payload.value(QStringLiteral("role")).toString();
    CodexMessageRole role;
    if (roleName == QStringLiteral("user")) {
        role = CodexMessageRole::User;
    } else if (roleName == QStringLiteral("assistant")) {
        role = CodexMessageRole::Assistant;
    } else {
        return std::nullopt;
    }

    const auto text =
        mobileVisibleText(
            payload.value(QStringLiteral("content")));
    if (!text.has_value()) {
        return std::nullopt;
    }

    return RolloutMessage{
        role,
        *text,
        mobileMessageTurnId(payload),
        createdAt(root),
    };
}

std::optional<TaskLifecycle> lifecycle(
    const QJsonObject& root,
    const QJsonObject& payload)
{
    if (root.value(QStringLiteral("type")).toString() !=
        QStringLiteral("event_msg")) {
        return std::nullopt;
    }

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

    return TaskLifecycle{state, turnId(payload)};
}

std::optional<TaskLifecycle> mobileLifecycle(
    const QJsonObject& root,
    const QJsonObject& payload)
{
    if (root.value(QStringLiteral("type")).toString()
        != QStringLiteral("event_msg")) {
        return std::nullopt;
    }

    const QString type =
        payload.value(QStringLiteral("type")).toString();
    LifecycleState state;
    if (type == QStringLiteral("task_started")
        || type == QStringLiteral("turn_started")) {
        state = LifecycleState::Active;
    } else if (
        type == QStringLiteral("task_complete")
        || type == QStringLiteral("task_completed")
        || type == QStringLiteral("turn_complete")
        || type == QStringLiteral("turn_completed")) {
        state = LifecycleState::Completed;
    } else if (
        type == QStringLiteral("task_aborted")
        || type == QStringLiteral("task_failed")
        || type == QStringLiteral("turn_aborted")
        || type == QStringLiteral("turn_failed")) {
        state = LifecycleState::Failed;
    } else {
        return std::nullopt;
    }

    return TaskLifecycle{
        state,
        rawString(
            payload.value(QStringLiteral("turn_id"))),
    };
}

} // namespace

Result<RolloutSnapshot> RolloutReader::readTail(
    const QString& rawPath,
    const QString& codexHome,
    qint64 maximumBytes)
{
    const QString path = resolvedPath(rawPath, codexHome);
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        return Result<RolloutSnapshot>::success({});
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<RolloutSnapshot>::success({});
    }

    const qint64 fileSize = file.size();
    const qint64 readLength = std::min(
        std::max<qint64>(maximumBytes, 0),
        std::min(fileSize, kMaximumTailBytes));
    if (readLength <= 0) {
        return Result<RolloutSnapshot>::success({});
    }

    const qint64 startOffset = fileSize - readLength;
    if (!file.seek(startOffset)) {
        return Result<RolloutSnapshot>::failure({
            QStringLiteral("codex.rollout_read_failed"),
            QStringLiteral("Could not seek in the Codex rollout."),
            false,
            {{QStringLiteral("path"), path}},
        });
    }

    QByteArray tail = file.read(readLength);
    if (tail.isEmpty()) {
        return Result<RolloutSnapshot>::success({});
    }
    if (startOffset > 0) {
        const qsizetype newline = tail.indexOf('\n');
        if (newline < 0) {
            return Result<RolloutSnapshot>::success({});
        }
        tail.remove(0, newline + 1);
    }

    RolloutSnapshot snapshot;
    qsizetype cursor = tail.size();
    while (cursor > 0 &&
           (!snapshot.latestUserMessage.has_value() ||
            !snapshot.latestAssistantMessage.has_value() ||
            !snapshot.lifecycle.has_value())) {
        while (cursor > 0 &&
               (tail.at(cursor - 1) == '\n' ||
                tail.at(cursor - 1) == '\r')) {
            --cursor;
        }
        if (cursor <= 0) {
            break;
        }

        const qsizetype lineEnd = cursor;
        const qsizetype previousNewline =
            tail.lastIndexOf('\n', lineEnd - 1);
        const qsizetype lineStart =
            previousNewline < 0 ? 0 : previousNewline + 1;
        const qsizetype lineLength = lineEnd - lineStart;
        cursor = previousNewline < 0 ? 0 : previousNewline;

        if (lineLength <= 0 ||
            lineLength > kMaximumLifecycleLineBytes) {
            continue;
        }

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray(tail.constData() + lineStart, lineLength),
            &error);
        if (error.error != QJsonParseError::NoError ||
            !document.isObject()) {
            continue;
        }

        const QJsonObject root = document.object();
        const QJsonValue payloadValue =
            root.value(QStringLiteral("payload"));
        if (!payloadValue.isObject()) {
            continue;
        }
        const QJsonObject payload = payloadValue.toObject();

        if (!snapshot.lifecycle.has_value()) {
            snapshot.lifecycle = lifecycle(root, payload);
        }
        if (lineLength > kMaximumPreviewLineBytes) {
            continue;
        }

        const auto parsedMessage = message(root, payload);
        if (!parsedMessage.has_value()) {
            continue;
        }
        if (parsedMessage->role == CodexMessageRole::User &&
            !snapshot.latestUserMessage.has_value()) {
            snapshot.latestUserMessage = parsedMessage;
        } else if (
            parsedMessage->role == CodexMessageRole::Assistant &&
            !snapshot.latestAssistantMessage.has_value()) {
            snapshot.latestAssistantMessage = parsedMessage;
        }
    }

    return Result<RolloutSnapshot>::success(std::move(snapshot));
}

Result<RolloutSnapshot> RolloutReader::readMobileTaskTail(
    const QString& rawPath,
    const QString& codexHome)
{
    const QString path = resolvedPath(rawPath, codexHome);
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        return Result<RolloutSnapshot>::success({});
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<RolloutSnapshot>::success({});
    }

    const qint64 fileSize = file.size();
    const qint64 readLength = std::min(
        fileSize,
        kMobileTaskTailBytes);
    if (readLength <= 0) {
        return Result<RolloutSnapshot>::success({});
    }

    const qint64 startOffset = fileSize - readLength;
    if (!file.seek(startOffset)) {
        return Result<RolloutSnapshot>::success({});
    }

    const QByteArray tail = file.read(readLength);
    if (tail.isEmpty()) {
        return Result<RolloutSnapshot>::success({});
    }

    RolloutSnapshot snapshot;
    qsizetype cursor = tail.size();
    while (cursor > 0
           && (!snapshot.latestAssistantMessage.has_value()
               || !snapshot.lifecycle.has_value())) {
        while (cursor > 0
               && (tail.at(cursor - 1) == '\n'
                   || tail.at(cursor - 1) == '\r')) {
            --cursor;
        }
        if (cursor <= 0) {
            break;
        }

        const qsizetype lineEnd = cursor;
        const qsizetype previousNewline =
            tail.lastIndexOf('\n', lineEnd - 1);
        const qsizetype lineStart =
            previousNewline < 0 ? 0 : previousNewline + 1;
        const qsizetype lineLength = lineEnd - lineStart;
        cursor = previousNewline < 0 ? 0 : previousNewline;

        if (lineLength <= 0
            || lineLength > kMobileTaskMaximumLineBytes) {
            continue;
        }

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray(
                tail.constData() + lineStart,
                lineLength),
            &error);
        if (error.error != QJsonParseError::NoError
            || !document.isObject()) {
            continue;
        }

        const QJsonObject root = document.object();
        const QJsonValue payloadValue =
            root.value(QStringLiteral("payload"));
        if (!payloadValue.isObject()) {
            continue;
        }
        const QJsonObject payload = payloadValue.toObject();

        if (!snapshot.lifecycle.has_value()) {
            snapshot.lifecycle =
                mobileLifecycle(root, payload);
        }
        if (!snapshot.latestAssistantMessage.has_value()) {
            const auto parsed =
                mobileMessage(root, payload);
            if (parsed.has_value()
                && parsed->role
                    == CodexMessageRole::Assistant) {
                snapshot.latestAssistantMessage =
                    parsed;
            }
        }
    }

    return Result<RolloutSnapshot>::success(
        std::move(snapshot));
}

} // namespace companion
