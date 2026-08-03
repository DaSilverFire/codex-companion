#pragma once

#include "codex/chat/WindowsOnDeviceChatBackendInternal.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace companion::detail {

struct FoundryExecutionProviderInfo final {
    QString name;
    bool registered = false;
};

enum class FoundryModelDeviceType {
    Invalid,
    Cpu,
    Gpu,
    Npu,
};

struct FoundryModelRuntimeInfo final {
    FoundryModelDeviceType deviceType =
        FoundryModelDeviceType::Invalid;
    QString executionProvider;
};

struct FoundryModelVariantInfo final {
    QString id;
    std::optional<
        FoundryModelRuntimeInfo> runtime;
};

enum class FoundryChatFinishReason {
    None,
    Stop,
    Length,
    ToolCalls,
    ContentFilter,
};

struct FoundryFunctionCall final {
    QString name;
    QByteArray argumentsJson;
};

struct FoundryToolCall final {
    QString id;
    QString type;
    std::optional<FoundryFunctionCall>
        function;
};

struct FoundryChatMessage final {
    QString role;
    QByteArray content;
    std::optional<QString> toolCallId;
    QVector<FoundryToolCall> toolCalls;
};

struct FoundryChatChoice final {
    FoundryChatFinishReason finishReason =
        FoundryChatFinishReason::None;
    std::optional<FoundryChatMessage>
        message;
};

struct FoundryChatCompletion final {
    bool successful = false;
    int httpStatusCode = 0;
    QVector<FoundryChatChoice> choices;
};

struct FoundryToolParameter final {
    QString name;
    QString type;
    QString description;
    bool required = true;
};

struct FoundryToolDefinition final {
    QString name;
    QString description;
    QVector<FoundryToolParameter>
        parameters;
};

using FoundryProgressObserver =
    std::function<void(double)>;
using FoundryCancellationProbe =
    std::function<bool()>;

class FoundryLocalApi {
public:
    virtual ~FoundryLocalApi() = default;

    virtual bool isInitialized()
        const noexcept = 0;
    virtual Result<void> create(
        const QString& applicationName,
        const QString& applicationDataDirectory,
        const QString& modelCacheDirectory) = 0;
    virtual Result<QVector<
        FoundryExecutionProviderInfo>>
    discoverExecutionProviders() = 0;
    virtual Result<void>
    downloadAndRegisterExecutionProviders(
        const QStringList& names,
        FoundryProgressObserver progress,
        FoundryCancellationProbe
            cancellation) = 0;
    virtual Result<void>
    invalidateCatalog() = 0;
    virtual Result<QVector<
        FoundryModelVariantInfo>>
    resolveModelVariants(
        const QString& alias) = 0;
    virtual Result<void> selectVariant(
        const QString& variantId) = 0;
    virtual Result<void>
    downloadSelectedModel(
        FoundryProgressObserver progress,
        FoundryCancellationProbe
            cancellation) = 0;
    virtual Result<void>
    loadSelectedModel() = 0;
    virtual Result<FoundryChatCompletion>
    completeChat(
        const QVector<FoundryChatMessage>&
            messages,
        const QVector<FoundryToolDefinition>&
            tools,
        int maximumTokens,
        int choiceCount) = 0;
    virtual void unloadSelectedModel()
        noexcept = 0;
    virtual void destroy() noexcept = 0;
};

Result<std::shared_ptr<FoundryLocalApi>>
createFoundryLocalApi();

Result<std::shared_ptr<
    WindowsOnDeviceChatDriver>>
createFoundryLocalChatDriver(
    std::shared_ptr<FoundryLocalApi> api,
    QString localAppDataRoot = {});

} // namespace companion::detail
