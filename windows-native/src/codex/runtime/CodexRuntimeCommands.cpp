#include "codex/runtime/CodexRuntime.h"

#include "codex/chat/ChatCatalog.h"
#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/CodexRuntimeOperationState.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "codex/state/TaskProjector.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QMetaObject>
#include <QMetaType>
#include <QSet>
#include <QThread>
#include <QVariantList>

#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace companion {

namespace {

CompanionError runtimeUnavailableError()
{
    return {
        QStringLiteral("codex.runtime_unavailable"),
        QStringLiteral(
            "Codex runtime is unavailable."),
        false,
        {},
    };
}

CompanionError invalidCommandArgumentsError()
{
    return {
        QStringLiteral(
            "codex.command_invalid_arguments"),
        QStringLiteral(
            "Invalid Codex command arguments."),
        false,
        {},
    };
}

CompanionError readOnlyProbeError()
{
    return {
        QStringLiteral("codex.probe_read_only"),
        QStringLiteral(
            "This Codex probe is read-only."),
        false,
        {},
    };
}

CompanionError operationCanceledError()
{
    return {
        QStringLiteral(
            "codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

CompanionError taskNotFoundError()
{
    return {
        QStringLiteral("codex.task_not_found"),
        QStringLiteral(
            "The selected Codex task is no longer available."),
        false,
        {},
    };
}

CompanionError noActiveTurnError()
{
    return {
        QStringLiteral("codex.no_active_turn"),
        QStringLiteral(
            "This Codex task has no active turn to steer."),
        false,
        {},
    };
}

CompanionError activeTurnChangedError()
{
    return {
        QStringLiteral(
            "codex.active_turn_changed"),
        QStringLiteral(
            "The Codex task changed before it could be steered."),
        false,
        {},
    };
}

CompanionError sendFailedError()
{
    return {
        QStringLiteral("codex.send_failed"),
        QStringLiteral(
            "Codex could not accept the message."),
        true,
        {},
    };
}

CompanionError approvalNotPendingError()
{
    return {
        QStringLiteral(
            "codex.approval_not_pending"),
        QStringLiteral(
            "The Codex approval request is no longer pending."),
        false,
        {},
    };
}

CompanionError approvalFailedError()
{
    return {
        QStringLiteral("codex.approval_failed"),
        QStringLiteral(
            "Codex could not apply the approval decision."),
        true,
        {},
    };
}

CompanionError taskCreateFailedError()
{
    return {
        QStringLiteral("codex.task_create_failed"),
        QStringLiteral(
            "Could not create the Codex task."),
        true,
        {},
    };
}

CompanionError chatCapabilitiesUnavailableError()
{
    return {
        QStringLiteral(
            "codex.chat_capabilities_unavailable"),
        QStringLiteral(
            "Companion chat capabilities are not ready."),
        true,
        {},
    };
}

CompanionError chatModelUnavailableError()
{
    return {
        QStringLiteral(
            "codex.chat_model_unavailable"),
        QStringLiteral(
            "The selected chat model is unavailable."),
        false,
        {},
    };
}

CompanionError chatOnDeviceUnavailableError()
{
    return {
        QStringLiteral(
            "codex.chat_on_device_unavailable"),
        QStringLiteral(
            "The Windows on-device chat model is unavailable."),
        false,
        {},
    };
}

CompanionError chatCredentialsMissingError()
{
    return {
        QStringLiteral(
            "codex.chat_credentials_missing"),
        QStringLiteral(
            "The selected chat provider is not configured."),
        false,
        {},
    };
}

CompanionError chatAttachmentsUnsupportedError()
{
    return {
        QStringLiteral(
            "codex.chat_attachments_unsupported"),
        QStringLiteral(
            "The selected chat model does not support attachments."),
        false,
        {},
    };
}

CompanionError chatFailedError()
{
    return {
        QStringLiteral("codex.chat_failed"),
        QStringLiteral(
            "Could not complete the Companion chat request."),
        true,
        {},
    };
}

CompanionError goalMutationFailedError()
{
    return {
        QStringLiteral(
            "codex.goal_mutation_failed"),
        QStringLiteral(
            "Could not update the Codex goal."),
        true,
        {},
    };
}

CompanionError usageResetFailedError()
{
    return {
        QStringLiteral(
            "codex.usage_reset_failed"),
        QStringLiteral(
            "Could not apply the Codex usage reset."),
        true,
        {},
    };
}

const QVector<QString>& mutationCommands()
{
    static const QVector<QString> commands{
        QStringLiteral("codex.reply"),
        QStringLiteral("codex.steer"),
        QStringLiteral("codex.approval.respond"),
        QStringLiteral("codex.task.create"),
        QStringLiteral("codex.chat.send"),
        QStringLiteral("codex.goal.create"),
        QStringLiteral("codex.goal.update"),
        QStringLiteral("codex.goal.pause"),
        QStringLiteral("codex.goal.resume"),
        QStringLiteral(
            "codex.usage.consume-reset"),
    };
    return commands;
}

struct ParsedApprovalCommand final {
    QString threadId;
    ApprovalDecision decision =
        ApprovalDecision::Decline;
};

struct ParsedChatCommand final {
    QString text;
    QString agentId;
    ChatProvider provider =
        ChatProvider::OnDevice;
    QString modelId;
    QVector<BridgeAttachment> attachments;
};

struct ParsedUsageResetCommand final {
    QString creditId;
    QUuid idempotencyKey;
};

using ParsedMutationCommand = std::variant<
    SendRequest,
    ParsedApprovalCommand,
    RuntimeTaskCreateRequest,
    ParsedChatCommand,
    RuntimeGoalMutationRequest,
    ParsedUsageResetCommand>;

QString defaultChatModelId(ChatProvider provider)
{
    switch (provider) {
    case ChatProvider::OnDevice:
        return QStringLiteral("on-device");
    case ChatProvider::OpenAIAPI:
        return QStringLiteral(
            "openai:gpt56Luna");
    case ChatProvider::LumoAPI:
        return QStringLiteral(
            "lumo:automatic");
    }
    return {};
}

std::optional<BridgeChatModel> resolveChatModel(
    const BridgeCapabilities& capabilities,
    ChatProvider provider,
    const QString& requestedModelId)
{
    if (!capabilities.chatModels.has_value()) {
        return std::nullopt;
    }
    const QVector<BridgeChatModel>& models =
        *capabilities.chatModels;
    if (provider != ChatProvider::OnDevice
        && !requestedModelId.isEmpty()) {
        const auto requested = std::find_if(
            models.cbegin(),
            models.cend(),
            [provider,
             &requestedModelId](
                const BridgeChatModel& model) {
                return model.provider == provider
                    && (model.id
                            == requestedModelId
                        || model.model
                            == requestedModelId);
            });
        if (requested != models.cend()) {
            return *requested;
        }
    }

    const QString defaultId =
        defaultChatModelId(provider);
    const auto fallback = std::find_if(
        models.cbegin(),
        models.cend(),
        [provider,
         &defaultId](
            const BridgeChatModel& model) {
            return model.provider == provider
                && model.id == defaultId;
        });
    return fallback != models.cend()
        ? std::optional<BridgeChatModel>(
              *fallback)
        : std::nullopt;
}

QString buildChatPrompt(
    const ResolvedChatAgent& agent,
    const QString& text)
{
    return QStringLiteral(
        "Mode: %1\n%2\n\nUser request:\n%3")
        .arg(
            agent.agent.name,
            agent.promptInstruction,
            text);
}

BridgeDate bridgeDate(const QDateTime& date)
{
    constexpr qint64
        kReferenceDateUnixMilliseconds =
            978307200000LL;
    if (!date.isValid()) {
        return {};
    }
    return {
        static_cast<double>(
            date.toMSecsSinceEpoch()
            - kReferenceDateUnixMilliseconds)
        / 1000.0,
    };
}

bool hasOnlyKeys(
    const QVariantMap& arguments,
    std::initializer_list<QString> allowedKeys)
{
    const QSet<QString> allowed(
        allowedKeys.begin(),
        allowedKeys.end());
    for (auto argument = arguments.cbegin();
         argument != arguments.cend();
         ++argument) {
        if (!allowed.contains(argument.key())) {
            return false;
        }
    }
    return true;
}

bool requiredTrimmedString(
    const QVariantMap& arguments,
    const QString& key,
    QString& result)
{
    const auto argument = arguments.constFind(key);
    if (argument == arguments.constEnd()
        || argument->metaType().id()
            != QMetaType::QString) {
        return false;
    }
    result = argument->toString().trimmed();
    return !result.isEmpty();
}

bool optionalTrimmedString(
    const QVariantMap& arguments,
    const QString& key,
    QString& result)
{
    result.clear();
    const auto argument = arguments.constFind(key);
    if (argument == arguments.constEnd()) {
        return true;
    }
    if (argument->metaType().id()
        != QMetaType::QString) {
        return false;
    }
    result = argument->toString().trimmed();
    return true;
}

bool parseClientMessageId(
    const QVariantMap& arguments,
    QString& result)
{
    const auto argument =
        arguments.constFind(
            QStringLiteral("clientMessageId"));
    if (argument == arguments.constEnd()) {
        result = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        return true;
    }
    if (argument->metaType().id()
        != QMetaType::QString) {
        return false;
    }
    result = argument->toString().trimmed();
    return !result.isEmpty()
        && !QUuid(result).isNull();
}

std::optional<qint64> parsePositiveInteger(
    const QVariant& value)
{
    qint64 signedValue = 0;
    quint64 unsignedValue = 0;
    bool isUnsigned = false;

    switch (value.metaType().id()) {
    case QMetaType::Int:
        signedValue = value.toInt();
        break;
    case QMetaType::UInt:
        unsignedValue = value.toUInt();
        isUnsigned = true;
        break;
    case QMetaType::LongLong:
        signedValue = value.toLongLong();
        break;
    case QMetaType::ULongLong:
        unsignedValue = value.toULongLong();
        isUnsigned = true;
        break;
    case QMetaType::Float:
    case QMetaType::Double: {
        const double number = value.toDouble();
        if (!std::isfinite(number)
            || std::trunc(number) != number
            || number <= 0.0
            || static_cast<long double>(number)
                > static_cast<long double>(
                    std::numeric_limits<
                        qint64>::max())) {
            return std::nullopt;
        }
        signedValue = static_cast<qint64>(number);
        break;
    }
    default:
        return std::nullopt;
    }

    if (isUnsigned) {
        if (unsignedValue == 0
            || unsignedValue
                > static_cast<quint64>(
                    std::numeric_limits<
                        qint64>::max())) {
            return std::nullopt;
        }
        return static_cast<qint64>(
            unsignedValue);
    }
    if (signedValue <= 0) {
        return std::nullopt;
    }
    return signedValue;
}

bool parseAttachments(
    const QVariantMap& arguments,
    QVector<BridgeAttachment>& result)
{
    result.clear();
    const auto argument =
        arguments.constFind(
            QStringLiteral("attachments"));
    if (argument == arguments.constEnd()) {
        return true;
    }
    if (argument->metaType().id()
        != QMetaType::QVariantList) {
        return false;
    }

    const QVariantList values = argument->toList();
    result.reserve(values.size());
    for (const QVariant& value : values) {
        if (value.metaType().id()
            != QMetaType::QVariantMap) {
            return false;
        }
        const QVariantMap attachment =
            value.toMap();
        if (!hasOnlyKeys(
                attachment,
                {
                    QStringLiteral("id"),
                    QStringLiteral("kind"),
                    QStringLiteral("filename"),
                    QStringLiteral("mimeType"),
                    QStringLiteral("data"),
                })) {
            return false;
        }

        const auto id = attachment.constFind(
            QStringLiteral("id"));
        const auto kind = attachment.constFind(
            QStringLiteral("kind"));
        const auto filename = attachment.constFind(
            QStringLiteral("filename"));
        const auto data = attachment.constFind(
            QStringLiteral("data"));
        if (id == attachment.constEnd()
            || id->metaType().id()
                != QMetaType::QString
            || kind == attachment.constEnd()
            || kind->metaType().id()
                != QMetaType::QString
            || filename == attachment.constEnd()
            || filename->metaType().id()
                != QMetaType::QString
            || data == attachment.constEnd()
            || data->metaType().id()
                != QMetaType::QByteArray) {
            return false;
        }

        BridgeAttachment parsed;
        parsed.id = QUuid(id->toString());
        if (parsed.id.isNull()) {
            return false;
        }
        const QString kindValue = kind->toString();
        if (kindValue == QStringLiteral("file")) {
            parsed.kind = AttachmentKind::File;
        } else if (
            kindValue == QStringLiteral("image")) {
            parsed.kind = AttachmentKind::Image;
        } else {
            return false;
        }
        parsed.filename = filename->toString();
        if (parsed.filename.trimmed().isEmpty()) {
            return false;
        }
        const auto mimeType =
            attachment.constFind(
                QStringLiteral("mimeType"));
        if (mimeType != attachment.constEnd()) {
            if (mimeType->metaType().id()
                != QMetaType::QString) {
                return false;
            }
            parsed.mimeType =
                mimeType->toString();
        }
        parsed.data = data->toByteArray();
        result.append(std::move(parsed));
    }
    return true;
}

Result<ParsedMutationCommand> parseSendCommand(
    const QVariantMap& arguments,
    SendAction action)
{
    const bool steer = action == SendAction::Steer;
    if (!hasOnlyKeys(
            arguments,
            steer
                ? std::initializer_list<QString>{
                      QStringLiteral("threadId"),
                      QStringLiteral("text"),
                      QStringLiteral("cwd"),
                      QStringLiteral("expectedTurnId"),
                      QStringLiteral("clientMessageId"),
                      QStringLiteral("model"),
                      QStringLiteral("reasoningEffort"),
                      QStringLiteral("attachments"),
                  }
                : std::initializer_list<QString>{
                      QStringLiteral("threadId"),
                      QStringLiteral("text"),
                      QStringLiteral("cwd"),
                      QStringLiteral("clientMessageId"),
                      QStringLiteral("model"),
                      QStringLiteral("reasoningEffort"),
                      QStringLiteral("attachments"),
                  })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    SendRequest request;
    request.action = action;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("threadId"),
            request.threadId)
        || !requiredTrimmedString(
            arguments,
            QStringLiteral("text"),
            request.prompt)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("cwd"),
            request.cwd)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("expectedTurnId"),
            request.expectedTurnId)
        || !parseClientMessageId(
            arguments,
            request.clientMessageId)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("model"),
            request.model)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("reasoningEffort"),
            request.reasoningEffort)
        || !parseAttachments(
            arguments,
            request.attachments)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand>
parseApprovalCommand(
    const QVariantMap& arguments)
{
    if (!hasOnlyKeys(
            arguments,
            {
                QStringLiteral("threadId"),
                QStringLiteral("approvalDecision"),
            })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    ParsedApprovalCommand request;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("threadId"),
            request.threadId)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    const auto decision = arguments.constFind(
        QStringLiteral("approvalDecision"));
    if (decision == arguments.constEnd()
        || decision->metaType().id()
            != QMetaType::QString) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    const QString value = decision->toString();
    if (value == QStringLiteral("approveOnce")) {
        request.decision =
            ApprovalDecision::ApproveOnce;
    } else if (
        value == QStringLiteral("approveSimilar")) {
        request.decision =
            ApprovalDecision::ApproveSimilar;
    } else if (value == QStringLiteral("decline")) {
        request.decision =
            ApprovalDecision::Decline;
    } else {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand>
parseTaskCreateCommand(
    const QVariantMap& arguments)
{
    if (!hasOnlyKeys(
            arguments,
            {
                QStringLiteral("text"),
                QStringLiteral("cwd"),
                QStringLiteral("clientMessageId"),
                QStringLiteral("model"),
                QStringLiteral("reasoningEffort"),
                QStringLiteral("skillName"),
                QStringLiteral("skillPath"),
                QStringLiteral("attachments"),
            })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    RuntimeTaskCreateRequest request;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("text"),
            request.text)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("cwd"),
            request.cwd)
        || !parseClientMessageId(
            arguments,
            request.clientMessageId)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("model"),
            request.model)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("reasoningEffort"),
            request.reasoningEffort)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("skillName"),
            request.skillName)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("skillPath"),
            request.skillPath)
        || !parseAttachments(
            arguments,
            request.attachments)
        || request.skillName.isEmpty()
            != request.skillPath.isEmpty()) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand> parseChatCommand(
    const QVariantMap& arguments)
{
    if (!hasOnlyKeys(
            arguments,
            {
                QStringLiteral("text"),
                QStringLiteral("chatAgentId"),
                QStringLiteral("chatProvider"),
                QStringLiteral("chatModelId"),
                QStringLiteral("attachments"),
            })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    ParsedChatCommand request;
    if (!optionalTrimmedString(
            arguments,
            QStringLiteral("text"),
            request.text)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("chatAgentId"),
            request.agentId)
        || !optionalTrimmedString(
            arguments,
            QStringLiteral("chatModelId"),
            request.modelId)
        || !parseAttachments(
            arguments,
            request.attachments)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    QString provider;
    if (!optionalTrimmedString(
            arguments,
            QStringLiteral("chatProvider"),
            provider)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    if (provider.isEmpty()
        || provider == QStringLiteral("onDevice")) {
        request.provider = ChatProvider::OnDevice;
    } else if (
        provider == QStringLiteral("openAIAPI")) {
        request.provider = ChatProvider::OpenAIAPI;
    } else if (
        provider == QStringLiteral("lumoAPI")) {
        request.provider = ChatProvider::LumoAPI;
    } else {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    if (request.text.isEmpty()
        && request.attachments.isEmpty()) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand> parseGoalCommand(
    const QString& command,
    const QVariantMap& arguments)
{
    const bool hasObjective =
        command == QStringLiteral("codex.goal.create")
        || command
            == QStringLiteral("codex.goal.update");
    if (!hasOnlyKeys(
            arguments,
            hasObjective
                ? std::initializer_list<QString>{
                      QStringLiteral("threadId"),
                      QStringLiteral("goalObjective"),
                      QStringLiteral("goalTokenBudget"),
                  }
                : std::initializer_list<QString>{
                      QStringLiteral("threadId"),
                  })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    RuntimeGoalMutationRequest request;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("threadId"),
            request.threadId)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    if (command == QStringLiteral("codex.goal.create")) {
        request.kind =
            RuntimeGoalMutationKind::Create;
    } else if (
        command == QStringLiteral("codex.goal.update")) {
        request.kind =
            RuntimeGoalMutationKind::Update;
    } else if (
        command == QStringLiteral("codex.goal.pause")) {
        request.kind =
            RuntimeGoalMutationKind::Pause;
    } else {
        request.kind =
            RuntimeGoalMutationKind::Resume;
    }

    if (hasObjective) {
        QString objective;
        if (!requiredTrimmedString(
                arguments,
                QStringLiteral("goalObjective"),
                objective)) {
            return Result<
                ParsedMutationCommand>::failure(
                invalidCommandArgumentsError());
        }
        request.objective = std::move(objective);
        const auto budget = arguments.constFind(
            QStringLiteral("goalTokenBudget"));
        if (budget != arguments.constEnd()) {
            const std::optional<qint64> parsed =
                parsePositiveInteger(*budget);
            if (!parsed.has_value()) {
                return Result<
                    ParsedMutationCommand>::failure(
                    invalidCommandArgumentsError());
            }
            request.tokenBudget = *parsed;
        }
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand>
parseUsageResetCommand(
    const QVariantMap& arguments)
{
    if (!hasOnlyKeys(
            arguments,
            {
                QStringLiteral("resetCreditId"),
                QStringLiteral("idempotencyKey"),
            })) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }

    ParsedUsageResetCommand request;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("resetCreditId"),
            request.creditId)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    QString idempotencyKey;
    if (!requiredTrimmedString(
            arguments,
            QStringLiteral("idempotencyKey"),
            idempotencyKey)) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    request.idempotencyKey = QUuid(idempotencyKey);
    if (request.idempotencyKey.isNull()) {
        return Result<ParsedMutationCommand>::failure(
            invalidCommandArgumentsError());
    }
    return Result<ParsedMutationCommand>::success(
        std::move(request));
}

Result<ParsedMutationCommand> parseMutationCommand(
    const QString& command,
    const QVariantMap& arguments)
{
    if (command == QStringLiteral("codex.reply")) {
        return parseSendCommand(
            arguments,
            SendAction::Reply);
    }
    if (command == QStringLiteral("codex.steer")) {
        return parseSendCommand(
            arguments,
            SendAction::Steer);
    }
    if (command
        == QStringLiteral("codex.approval.respond")) {
        return parseApprovalCommand(arguments);
    }
    if (command == QStringLiteral("codex.task.create")) {
        return parseTaskCreateCommand(arguments);
    }
    if (command == QStringLiteral("codex.chat.send")) {
        return parseChatCommand(arguments);
    }
    if (command.startsWith(
            QStringLiteral("codex.goal."))) {
        return parseGoalCommand(
            command,
            arguments);
    }
    if (command
        == QStringLiteral(
            "codex.usage.consume-reset")) {
        return parseUsageResetCommand(arguments);
    }
    return Result<ParsedMutationCommand>::failure(
        invalidCommandArgumentsError());
}

template <typename Callback>
void postToRuntimeOwner(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    Callback callback)
{
    const auto delivery = weakState.lock();
    if (delivery == nullptr) {
        return;
    }
    QPointer<CodexRuntime> runtime;
    {
        const std::scoped_lock lock(
            delivery->mutex);
        if (delivery->destroying
            || delivery->runtime.isNull()) {
            return;
        }
        runtime = delivery->runtime;
    }
    if (runtime.isNull()) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime.data(),
        [weakState,
         callback =
             std::move(callback)]() mutable {
            const auto current =
                weakState.lock();
            if (current == nullptr) {
                return;
            }
            QPointer<CodexRuntime> owner;
            {
                const std::scoped_lock lock(
                    current->mutex);
                if (current->destroying
                    || current->runtime.isNull()) {
                    return;
                }
                owner = current->runtime;
            }
            if (!owner.isNull()) {
                callback(*owner);
            }
        },
        Qt::QueuedConnection);
}

std::uint64_t nextOperationGeneration(
    std::uint64_t& generation)
{
    ++generation;
    if (generation == 0) {
        ++generation;
    }
    return generation;
}

template <typename T>
Result<void> sanitizeMutationResult(
    const Result<T>& result,
    const CompanionError& genericFailure,
    bool recognizeApprovalDisappearance = false)
{
    if (result.hasValue()) {
        return Result<void>::success();
    }
    if (result.error().code
        == QStringLiteral(
            "codex.operation_canceled")) {
        return Result<void>::failure(
            operationCanceledError());
    }
    if (recognizeApprovalDisappearance
        && result.error().code
            == QStringLiteral(
                "approval.request_not_found")) {
        return Result<void>::failure(
            approvalNotPendingError());
    }
    return Result<void>::failure(
        genericFailure);
}

template <
    typename T,
    typename Starter,
    typename Request,
    typename SuccessObserver>
void launchMutation(
    const std::shared_ptr<
        CodexRuntimeOperationRegistry>& registry,
    const std::shared_ptr<
        RuntimeContinuationHost>& continuationHost,
    std::uint64_t runtimeGeneration,
    std::uint64_t operationGeneration,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation,
    Starter starter,
    Request request,
    CompanionError genericFailure,
    bool recognizeApprovalDisappearance,
    SuccessObserver successObserver,
    QString cancellationKey = {})
{
    const auto operation =
        CodexRuntimeOperationState::createMutation(
            [invocation](Result<void> result) {
                invocation->finish(
                    std::move(result));
            },
            runtimeGeneration,
            operationGeneration);
    if (operation == nullptr
        || registry == nullptr
        || registry->registerOperation(
               operation,
               std::move(cancellationKey))
            == 0
        || continuationHost == nullptr) {
        invocation->finish(
            Result<void>::failure(
                genericFailure));
        return;
    }

    const Result<void> submitted =
        continuationHost->submit(
            [operation] {
                operation->observeMutationTerminal();
            });
    if (!submitted.hasValue()) {
        operation->finishBeforeMutationHandle(
            Result<void>::failure(
                genericFailure));
        return;
    }

    try {
        CommitAwareMutationHandle<T> handle =
            starter(std::move(request));
        if (!handle.terminalFuture.isValid()
            || !handle.requestStopBeforeCommit) {
            operation->finishBeforeMutationHandle(
                Result<void>::failure(
                    genericFailure));
            return;
        }

        CodexRuntimeMutationObservation observation{
            std::move(
                handle.requestStopBeforeCommit),
            [future =
                 std::move(handle.terminalFuture),
             genericFailure,
             recognizeApprovalDisappearance,
             successObserver =
                 std::move(successObserver)](
                CodexRuntimeOperationState&
                    operationState) mutable {
                Result<void> publicResult =
                    Result<void>::failure(
                        genericFailure);
                try {
                    if (future.isValid()) {
                        future.waitForFinished();
                        if (!future.isCanceled()
                            && future.resultCount()
                                == 1) {
                            const Result<T> terminal =
                                future.result();
                            publicResult =
                                sanitizeMutationResult(
                                    terminal,
                                    genericFailure,
                                    recognizeApprovalDisappearance);
                            if (terminal.hasValue()
                                && publicResult
                                       .hasValue()) {
                                try {
                                    if (!successObserver(
                                            terminal)) {
                                        publicResult =
                                            Result<void>::
                                                failure(
                                                    genericFailure);
                                    }
                                } catch (...) {
                                    publicResult =
                                        Result<void>::failure(
                                            genericFailure);
                                }
                            }
                        }
                    }
                } catch (...) {
                    publicResult =
                        Result<void>::failure(
                            genericFailure);
                }
                operationState.finish(
                    std::move(publicResult));
            },
            genericFailure,
        };
        if (!operation->installMutationObservation(
                std::move(observation))
            && !operation->terminal()) {
            operation->finishBeforeMutationHandle(
                Result<void>::failure(
                    genericFailure));
        }
    } catch (...) {
        operation->finishBeforeMutationHandle(
            Result<void>::failure(
                genericFailure));
    }
}

template <
    typename T,
    typename Starter,
    typename Request>
void launchMutation(
    const std::shared_ptr<
        CodexRuntimeOperationRegistry>& registry,
    const std::shared_ptr<
        RuntimeContinuationHost>& continuationHost,
    std::uint64_t runtimeGeneration,
    std::uint64_t operationGeneration,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation,
    Starter starter,
    Request request,
    CompanionError genericFailure,
    bool recognizeApprovalDisappearance = false,
    QString cancellationKey = {})
{
    launchMutation<T>(
        registry,
        continuationHost,
        runtimeGeneration,
        operationGeneration,
        invocation,
        std::move(starter),
        std::move(request),
        std::move(genericFailure),
        recognizeApprovalDisappearance,
        [](const Result<T>&) {
            return true;
        },
        std::move(cancellationKey));
}

} // namespace

void CodexRuntime::postTaskMutationSuccess(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& deliveryState,
    std::uint64_t runtimeGeneration)
{
    postToRuntimeOwner(
        deliveryState,
        [runtimeGeneration](CodexRuntime& runtime) {
            if (!runtime.running_
                || runtime.deferredStop_
                || runtime.generation_
                    != runtimeGeneration) {
                return;
            }
            Q_UNUSED(
                runtime
                    .requestRefreshOnOwnerThread());
        });
}

void CodexRuntime::postTaskCreated(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& deliveryState,
    std::uint64_t runtimeGeneration,
    QString threadId)
{
    postToRuntimeOwner(
        deliveryState,
        [runtimeGeneration,
         threadId =
             std::move(threadId)](
            CodexRuntime& runtime) {
            if (!runtime.running_
                || runtime.deferredStop_
                || runtime.generation_
                    != runtimeGeneration) {
                return;
            }
            emit runtime.taskCreated(threadId);
            Q_UNUSED(
                runtime
                    .requestRefreshOnOwnerThread());
        });
}

void CodexRuntime::postChatMessageSuccess(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& deliveryState,
    std::uint64_t runtimeGeneration,
    ChatResult result)
{
    postToRuntimeOwner(
        deliveryState,
        [runtimeGeneration,
         result = std::move(result)](
            CodexRuntime& runtime) mutable {
            if (!runtime.running_
                || runtime.deferredStop_
                || runtime.generation_
                    != runtimeGeneration) {
                return;
            }

            QDateTime createdAt;
            try {
                createdAt =
                    runtime.nowProvider_();
            } catch (...) {
            }
            if (!createdAt.isValid()) {
                createdAt =
                    QDateTime::currentDateTimeUtc();
            } else {
                createdAt = createdAt.toUTC();
            }

            BridgeMessage message;
            message.id =
                QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
            message.role =
                MessageRole::Assistant;
            message.text =
                result.text.trimmed();
            message.createdAt =
                bridgeDate(createdAt);
            emit runtime.chatMessageReceived(
                message);
        });
}

void CodexRuntime::postGoalMutationSuccess(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& deliveryState,
    std::uint64_t runtimeGeneration,
    BridgeGoal goal)
{
    postToRuntimeOwner(
        deliveryState,
        [runtimeGeneration,
         goal = std::move(goal)](
            CodexRuntime& runtime) mutable {
            if (!runtime.running_
                || runtime.deferredStop_
                || runtime.generation_
                    != runtimeGeneration) {
                return;
            }

            ++runtime.goalRevision_;
            if (runtime.goalRevision_ == 0) {
                ++runtime.goalRevision_;
            }
            QDateTime completionTime;
            try {
                const QDateTime now =
                    runtime.nowProvider_();
                completionTime =
                    now.isValid()
                    ? now.toUTC()
                    : QDateTime();
            } catch (...) {
            }
            const QString threadId =
                goal.threadId.trimmed();
            if (!threadId.isEmpty()) {
                runtime.cachedGoals_.insert(
                    threadId,
                    goal);
                const auto runtimeStatus =
                    runtime.processSnapshot_
                        .runtimeStatuses
                        .constFind(threadId);
                const std::optional<
                    ThreadRuntimeStatus>
                    currentRuntimeStatus =
                        runtimeStatus
                            != runtime
                                .processSnapshot_
                                .runtimeStatuses
                                .constEnd()
                        ? std::optional<
                              ThreadRuntimeStatus>(
                              runtimeStatus.value())
                        : std::nullopt;
                bool changed = false;
                for (BridgeTask& task :
                     runtime.processSnapshot_.tasks) {
                    if (task.id.trimmed()
                        != threadId) {
                        continue;
                    }
                    BridgeTask updated =
                        TaskProjector::applyingGoal(
                            task,
                            goal,
                            currentRuntimeStatus,
                            completionTime);
                    if (task != updated) {
                        task = std::move(updated);
                        changed = true;
                    }
                }
                if (changed
                    && !runtime.state_.isNull()) {
                    runtime.state_->tasks()
                        ->setSnapshot(
                            runtime
                                .processSnapshot_
                                .tasks);
                    emit runtime
                        .processSnapshotChanged();
                }
            }

            if (completionTime.isValid()) {
                runtime.lastGoalRefreshFinishedAt_ =
                    completionTime;
            }
            emit runtime.goalChanged(goal);
            Q_UNUSED(
                runtime
                    .requestRefreshOnOwnerThread());
        });
}

void CodexRuntime::postUsageResetSuccess(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& deliveryState,
    std::uint64_t runtimeGeneration,
    UsageResetOutcome outcome)
{
    postToRuntimeOwner(
        deliveryState,
        [runtimeGeneration,
         outcome](CodexRuntime& runtime) {
            if (!runtime.running_
                || runtime.deferredStop_
                || runtime.generation_
                    != runtimeGeneration) {
                return;
            }
            emit runtime.usageResetFinished(
                outcome);
            if (outcome
                != UsageResetOutcome::
                    NothingToReset) {
                runtime
                    .scheduleUsageRefreshAfterMutation();
            }
        });
}

Result<void> CodexRuntime::bindRuntimeCommandGroup()
{
    const auto weakState =
        std::weak_ptr<CodexRuntimeDeliveryState>(
            deliveryState_);
    QVector<CompanionCommandBus::HandlerEntry>
        handlers{
            {
                QStringLiteral(
                    "codex.refresh"),
                [weakState](
                    const QVariantMap&
                        arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    if (!arguments.isEmpty()) {
                        completion(
                            Result<void>::failure(
                                invalidCommandArgumentsError()));
                        return;
                    }
                    dispatchRefreshCommand(
                        std::make_shared<
                            CodexRuntimeCommandInvocationState>(
                            weakState,
                            std::move(completion)));
                },
            },
        };
    if (historyCommandsEnabled_) {
        handlers.append({
            QStringLiteral(
                "codex.history.load"),
            [weakState](
                const QVariantMap& arguments,
                CompanionCommandBus::Completion
                    completion) {
                dispatchHistoryCommand(
                    arguments,
                    std::make_shared<
                        CodexRuntimeCommandInvocationState>(
                        weakState,
                        std::move(completion)));
            },
        });
    }
    if (readCommandsEnabled_) {
        handlers.append({
            QStringLiteral(
                "codex.capabilities.load"),
            [weakState](
                const QVariantMap& arguments,
                CompanionCommandBus::Completion
                    completion) {
                dispatchCapabilitiesCommand(
                    arguments,
                    std::make_shared<
                        CodexRuntimeCommandInvocationState>(
                        weakState,
                        std::move(completion)));
            },
        });
        handlers.append({
            QStringLiteral(
                "codex.usage.load"),
            [weakState](
                const QVariantMap& arguments,
                CompanionCommandBus::Completion
                    completion) {
                dispatchUsageCommand(
                    arguments,
                    std::make_shared<
                        CodexRuntimeCommandInvocationState>(
                        weakState,
                        std::move(completion)));
            },
        });
    }
    if (mutationCommandsEnabled_) {
        for (const QString& command :
             mutationCommands()) {
            handlers.append({
                command,
                [weakState, command](
                    const QVariantMap& arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    dispatchMutationCommand(
                        command,
                        arguments,
                        std::make_shared<
                            CodexRuntimeCommandInvocationState>(
                            weakState,
                            std::move(completion)));
                },
            });
        }
    }
    return CompanionCommandBus::
        replaceHandlerGroupGuarded(
            commandBusDeliveryState_,
            QStringLiteral("codex.runtime"),
            std::move(handlers));
}

void CodexRuntime::dispatchMutationCommand(
    QString command,
    QVariantMap arguments,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation)
{
    if (invocation == nullptr) {
        return;
    }

    const auto delivery =
        invocation->deliveryState.lock();
    if (delivery == nullptr) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }

    QPointer<CodexRuntime> owner;
    QThread* ownerThread = nullptr;
    bool unavailable = false;
    bool queued = false;
    {
        const std::scoped_lock lock(
            delivery->mutex);
        if (delivery->destroying
            || delivery->runtime.isNull()) {
            unavailable = true;
        } else {
            owner = delivery->runtime;
            ownerThread = owner->thread();
            if (QThread::currentThread()
                != ownerThread) {
                queued =
                    QMetaObject::invokeMethod(
                        owner.data(),
                        [command =
                             std::move(command),
                         arguments =
                             std::move(arguments),
                         invocation]() mutable {
                            dispatchMutationCommand(
                                std::move(command),
                                std::move(arguments),
                                invocation);
                        },
                        Qt::QueuedConnection);
            }
        }
    }

    if (unavailable) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }
    if (QThread::currentThread() != ownerThread) {
        if (!queued) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
        }
        return;
    }
    if (owner.isNull()
        || !invocation->claimInvocation()) {
        if (owner.isNull()) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
        }
        return;
    }

    try {
        if (!owner->running_
            || owner->deferredStop_
            || !owner->mutationCommandsEnabled_) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
            return;
        }
        if (owner->mode_
            == CodexRuntimeMode::ReadOnlyProbe) {
            invocation->finish(
                Result<void>::failure(
                    readOnlyProbeError()));
            return;
        }

        QString operationKey;
        const QString operationKeyArgument =
            codexRuntimeOperationKeyArgument();
        const auto operationKeyValue =
            arguments.find(
                operationKeyArgument);
        if (operationKeyValue
            != arguments.end()) {
            if (operationKeyValue->metaType().id()
                    != QMetaType::QString
                || operationKeyValue
                       ->toString()
                       .trimmed()
                       .isEmpty()) {
                invocation->finish(
                    Result<void>::failure(
                        invalidCommandArgumentsError()));
                return;
            }
            operationKey =
                operationKeyValue
                    ->toString()
                    .trimmed();
            arguments.erase(
                operationKeyValue);
        }

        const Result<ParsedMutationCommand> parsed =
            parseMutationCommand(
                command,
                arguments);
        if (!parsed.hasValue()) {
            invocation->finish(
                Result<void>::failure(
                    invalidCommandArgumentsError()));
            return;
        }
        if (!operationKey.isEmpty()
            && !std::holds_alternative<
                SendRequest>(parsed.value())
            && !std::holds_alternative<
                ParsedApprovalCommand>(
                    parsed.value())) {
            invocation->finish(
                Result<void>::failure(
                    invalidCommandArgumentsError()));
            return;
        }

        const std::uint64_t runtimeGeneration =
            owner->generation_;
        const std::uint64_t operationGeneration =
            nextOperationGeneration(
                owner->nextOperationGeneration_);
        if (std::holds_alternative<SendRequest>(
                parsed.value())) {
            SendRequest request =
                std::get<SendRequest>(
                    parsed.value());
            const auto task =
                std::find_if(
                    owner->processSnapshot_
                        .tasks.cbegin(),
                    owner->processSnapshot_
                        .tasks.cend(),
                    [&request](
                        const BridgeTask& candidate) {
                        return candidate.id
                            == request.threadId;
                    });
            if (task
                == owner->processSnapshot_
                       .tasks.cend()) {
                invocation->finish(
                    Result<void>::failure(
                        taskNotFoundError()));
                return;
            }
            if (request.cwd.isEmpty()
                && task->cwd.has_value()) {
                request.cwd = *task->cwd;
            }
            if (request.action
                == SendAction::Steer) {
                if (!task->activeTurnId
                         .has_value()
                    || task->activeTurnId
                           ->trimmed()
                           .isEmpty()) {
                    invocation->finish(
                        Result<void>::failure(
                            noActiveTurnError()));
                    return;
                }
                const QString activeTurnId =
                    *task->activeTurnId;
                if (!request.expectedTurnId
                         .isEmpty()
                    && request.expectedTurnId
                        != activeTurnId) {
                    invocation->finish(
                        Result<void>::failure(
                            activeTurnChangedError()));
                    return;
                }
                request.expectedTurnId =
                    activeTurnId;
            }
            launchMutation<void>(
                owner->operationRegistry_,
                owner->continuationHost_,
                runtimeGeneration,
                operationGeneration,
                invocation,
                owner->sendMutationStarter_,
                std::move(request),
                sendFailedError(),
                false,
                [deliveryState =
                     std::weak_ptr<
                         CodexRuntimeDeliveryState>(
                         owner->deliveryState_),
                 runtimeGeneration](
                    const Result<void>&) {
                    postTaskMutationSuccess(
                        deliveryState,
                        runtimeGeneration);
                    return true;
                },
                operationKey);
            return;
        }
        if (std::holds_alternative<
                ParsedApprovalCommand>(
                parsed.value())) {
            const ParsedApprovalCommand request =
                std::get<ParsedApprovalCommand>(
                    parsed.value());
            const auto approval =
                owner->processSnapshot_
                    .pendingApprovals.constFind(
                        request.threadId);
            if (approval
                == owner->processSnapshot_
                       .pendingApprovals.cend()) {
                invocation->finish(
                    Result<void>::failure(
                        approvalNotPendingError()));
                return;
            }
            launchMutation<void>(
                owner->operationRegistry_,
                owner->continuationHost_,
                runtimeGeneration,
                operationGeneration,
                invocation,
                [starter =
                     owner->approvalMutationStarter_](
                    std::pair<
                        PendingApproval,
                        ApprovalDecision> values) {
                    return starter(
                        std::move(values.first),
                        values.second);
                },
                std::pair{
                    *approval,
                    request.decision,
                },
                approvalFailedError(),
                true,
                [deliveryState =
                     std::weak_ptr<
                         CodexRuntimeDeliveryState>(
                         owner->deliveryState_),
                 runtimeGeneration](
                    const Result<void>&) {
                    postTaskMutationSuccess(
                        deliveryState,
                        runtimeGeneration);
                    return true;
                },
                operationKey);
            return;
        }
        if (std::holds_alternative<
                RuntimeTaskCreateRequest>(
                parsed.value())) {
            launchMutation<QString>(
                owner->operationRegistry_,
                owner->continuationHost_,
                runtimeGeneration,
                operationGeneration,
                invocation,
                owner->taskCreateMutationStarter_,
                std::get<
                    RuntimeTaskCreateRequest>(
                    parsed.value()),
                taskCreateFailedError(),
                false,
                [deliveryState =
                     std::weak_ptr<
                         CodexRuntimeDeliveryState>(
                         owner->deliveryState_),
                 runtimeGeneration](
                    const Result<QString>& result) {
                    const QString threadId =
                        result.value().trimmed();
                    if (threadId.isEmpty()) {
                        return false;
                    }
                    postTaskCreated(
                        deliveryState,
                        runtimeGeneration,
                        threadId);
                    return true;
                });
            return;
        }
        if (std::holds_alternative<
                ParsedChatCommand>(
                parsed.value())) {
            if (!owner->capabilities_.has_value()
                || !owner->chatCapabilityValid_) {
                invocation->finish(
                    Result<void>::failure(
                        chatCapabilitiesUnavailableError()));
                return;
            }

            const ParsedChatCommand parsedChat =
                std::get<ParsedChatCommand>(
                    parsed.value());
            const std::optional<BridgeChatModel>
                selectedModel =
                    resolveChatModel(
                        *owner->capabilities_,
                        parsedChat.provider,
                        parsedChat.modelId);
            if (!selectedModel.has_value()) {
                invocation->finish(
                    Result<void>::failure(
                        chatModelUnavailableError()));
                return;
            }
            if (!selectedModel->isAvailable) {
                invocation->finish(
                    Result<void>::failure(
                        parsedChat.provider
                                == ChatProvider::
                                    OnDevice
                            ? chatOnDeviceUnavailableError()
                            : chatCredentialsMissingError()));
                return;
            }
            if (!parsedChat.attachments.isEmpty()
                && !selectedModel
                        ->supportsAttachments) {
                invocation->finish(
                    Result<void>::failure(
                        chatAttachmentsUnsupportedError()));
                return;
            }

            const ResolvedChatAgent agent =
                ChatCatalog::resolveAgent(
                    parsedChat.agentId);
            ChatRequest request;
            request.provider =
                parsedChat.provider;
            request.modelId =
                selectedModel->model;
            request.prompt =
                buildChatPrompt(
                    agent,
                    parsedChat.text);
            request.attachments =
                parsedChat.attachments;
            launchMutation<ChatResult>(
                owner->operationRegistry_,
                owner->continuationHost_,
                runtimeGeneration,
                operationGeneration,
                invocation,
                owner->chatMutationStarter_,
                std::move(request),
                chatFailedError(),
                false,
                [deliveryState =
                     std::weak_ptr<
                         CodexRuntimeDeliveryState>(
                         owner->deliveryState_),
                 runtimeGeneration](
                    const Result<
                        ChatResult>& result) {
                    ChatResult chatResult =
                        result.value();
                    chatResult.text =
                        chatResult.text.trimmed();
                    if (chatResult.text.isEmpty()) {
                        return false;
                    }
                    postChatMessageSuccess(
                        deliveryState,
                        runtimeGeneration,
                        std::move(chatResult));
                    return true;
                });
            return;
        }
        if (std::holds_alternative<
                RuntimeGoalMutationRequest>(
                parsed.value())) {
            RuntimeGoalMutationRequest request =
                std::get<
                    RuntimeGoalMutationRequest>(
                    parsed.value());
            const QString expectedThreadId =
                request.threadId;
            launchMutation<BridgeGoal>(
                owner->operationRegistry_,
                owner->continuationHost_,
                runtimeGeneration,
                operationGeneration,
                invocation,
                owner->goalMutationStarter_,
                std::move(request),
                goalMutationFailedError(),
                false,
                [deliveryState =
                     std::weak_ptr<
                         CodexRuntimeDeliveryState>(
                         owner->deliveryState_),
                 runtimeGeneration,
                 expectedThreadId](
                    const Result<BridgeGoal>& result) {
                    BridgeGoal goal =
                        result.value();
                    goal.threadId =
                        goal.threadId.trimmed();
                    if (goal.threadId.isEmpty()
                        || goal.threadId
                            != expectedThreadId) {
                        return false;
                    }
                    postGoalMutationSuccess(
                        deliveryState,
                        runtimeGeneration,
                        std::move(goal));
                    return true;
                });
            return;
        }
        const ParsedUsageResetCommand request =
            std::get<ParsedUsageResetCommand>(
                parsed.value());
        launchMutation<UsageResetOutcome>(
            owner->operationRegistry_,
            owner->continuationHost_,
            runtimeGeneration,
            operationGeneration,
            invocation,
            [starter =
                 owner->usageResetMutationStarter_](
                std::pair<QString, QUuid> values) {
                return starter(
                    std::move(values.first),
                    values.second);
            },
            std::pair{
                request.creditId,
                request.idempotencyKey,
            },
            usageResetFailedError(),
            false,
            [deliveryState =
                 std::weak_ptr<
                     CodexRuntimeDeliveryState>(
                     owner->deliveryState_),
             runtimeGeneration](
                const Result<
                    UsageResetOutcome>& result) {
                switch (result.value()) {
                case UsageResetOutcome::Reset:
                case UsageResetOutcome::
                    NothingToReset:
                case UsageResetOutcome::NoCredit:
                case UsageResetOutcome::
                    AlreadyRedeemed:
                    break;
                default:
                    return false;
                }
                postUsageResetSuccess(
                    deliveryState,
                    runtimeGeneration,
                    result.value());
                return true;
            });
    } catch (...) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    }
}

} // namespace companion
