#include "codex/chat/FoundryLocalDriverInternal.h"

#include <foundry_local.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace companion::detail {

namespace {

CompanionError sdkFailure()
{
    return {
        QStringLiteral(
            "foundry.sdk_failed"),
        QStringLiteral(
            "The Windows on-device model is unavailable."),
        false,
        {},
    };
}

FoundryModelDeviceType deviceType(
    foundry_local::DeviceType value)
{
    switch (value) {
    case foundry_local::DeviceType::CPU:
        return FoundryModelDeviceType::Cpu;
    case foundry_local::DeviceType::GPU:
        return FoundryModelDeviceType::Gpu;
    case foundry_local::DeviceType::NPU:
        return FoundryModelDeviceType::Npu;
    case foundry_local::DeviceType::Invalid:
    default:
        return FoundryModelDeviceType::Invalid;
    }
}

FoundryChatFinishReason finishReason(
    foundry_local::FinishReason value)
{
    switch (value) {
    case foundry_local::FinishReason::Stop:
        return FoundryChatFinishReason::Stop;
    case foundry_local::FinishReason::Length:
        return FoundryChatFinishReason::
            Length;
    case foundry_local::FinishReason::
        ToolCalls:
        return FoundryChatFinishReason::
            ToolCalls;
    case foundry_local::FinishReason::
        ContentFilter:
        return FoundryChatFinishReason::
            ContentFilter;
    case foundry_local::FinishReason::None:
    default:
        return FoundryChatFinishReason::None;
    }
}

std::string utf8String(
    const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {
        bytes.constData(),
        static_cast<std::size_t>(
            bytes.size()),
    };
}

std::string byteString(
    const QByteArray& value)
{
    return {
        value.constData(),
        static_cast<std::size_t>(
            value.size()),
    };
}

QString fromUtf8(
    const std::string& value)
{
    return QString::fromUtf8(
        value.data(),
        static_cast<qsizetype>(
            value.size()));
}

QByteArray fromBytes(
    const std::string& value)
{
    return {
        value.data(),
        static_cast<qsizetype>(
            value.size()),
    };
}

foundry_local::ToolCall sdkToolCall(
    const FoundryToolCall& source)
{
    foundry_local::ToolCall result;
    result.id = utf8String(source.id);
    result.type = utf8String(source.type);
    if (source.function.has_value()) {
        result.function_call =
            foundry_local::FunctionCall{
                utf8String(
                    source.function->name),
                byteString(
                    source.function
                        ->argumentsJson),
            };
    }
    return result;
}

FoundryToolCall companionToolCall(
    const foundry_local::ToolCall& source)
{
    std::optional<FoundryFunctionCall>
        function;
    if (source.function_call.has_value()) {
        function = FoundryFunctionCall{
            fromUtf8(
                source.function_call->name),
            fromBytes(
                source.function_call
                    ->arguments),
        };
    }
    return {
        fromUtf8(source.id),
        fromUtf8(source.type),
        std::move(function),
    };
}

foundry_local::ChatMessage sdkMessage(
    const FoundryChatMessage& source)
{
    std::optional<std::string>
        toolCallId;
    if (source.toolCallId.has_value()) {
        toolCallId =
            utf8String(*source.toolCallId);
    }
    std::vector<foundry_local::ToolCall>
        toolCalls;
    toolCalls.reserve(
        static_cast<std::size_t>(
            source.toolCalls.size()));
    for (const auto& call :
         source.toolCalls) {
        toolCalls.push_back(
            sdkToolCall(call));
    }
    return {
        utf8String(source.role),
        byteString(source.content),
        std::move(toolCallId),
        std::move(toolCalls),
    };
}

FoundryChatMessage companionMessage(
    const foundry_local::ChatMessage&
        source)
{
    std::optional<QString> toolCallId;
    if (source.tool_call_id.has_value()) {
        toolCallId =
            fromUtf8(*source.tool_call_id);
    }
    QVector<FoundryToolCall> toolCalls;
    toolCalls.reserve(
        static_cast<qsizetype>(
            source.tool_calls.size()));
    for (const auto& call :
         source.tool_calls) {
        toolCalls.append(
            companionToolCall(call));
    }
    return {
        fromUtf8(source.role),
        fromBytes(source.content),
        std::move(toolCallId),
        std::move(toolCalls),
    };
}

foundry_local::ToolDefinition
sdkToolDefinition(
    const FoundryToolDefinition& source)
{
    foundry_local::PropertyDefinition
        parameters;
    parameters.type = "object";

    std::unordered_map<
        std::string,
        foundry_local::PropertyDefinition>
        properties;
    properties.reserve(
        static_cast<std::size_t>(
            source.parameters.size()));
    std::vector<std::string> required;
    required.reserve(
        static_cast<std::size_t>(
            source.parameters.size()));
    for (const auto& parameter :
         source.parameters) {
        foundry_local::PropertyDefinition
            property;
        property.type =
            utf8String(parameter.type);
        if (!parameter.description
                 .trimmed()
                 .isEmpty()) {
            property.description =
                utf8String(
                    parameter.description);
        }
        const std::string name =
            utf8String(parameter.name);
        properties.emplace(
            name,
            std::move(property));
        if (parameter.required) {
            required.push_back(name);
        }
    }
    parameters.properties =
        std::move(properties);
    parameters.required =
        std::move(required);

    foundry_local::ToolDefinition result;
    result.function.name =
        utf8String(source.name);
    if (!source.description
             .trimmed()
             .isEmpty()) {
        result.function.description =
            utf8String(
                source.description);
    }
    result.function.parameters =
        std::move(parameters);
    return result;
}

class FoundryLocalApiImpl final
    : public FoundryLocalApi {
public:
    bool isInitialized()
        const noexcept override
    {
        return foundry_local::Manager::
            IsInitialized();
    }

    Result<void> create(
        const QString& applicationName,
        const QString& applicationDataDirectory,
        const QString& modelCacheDirectory) override
    {
        try {
            foundry_local::Configuration
                configuration(
                    applicationName
                        .toStdString());
            configuration.app_data_dir =
                std::filesystem::path(
                    applicationDataDirectory
                        .toStdWString());
            configuration.model_cache_dir =
                std::filesystem::path(
                    modelCacheDirectory
                        .toStdWString());
            foundry_local::Manager::Create(
                std::move(configuration));
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure(
                sdkFailure());
        }
    }

    Result<QVector<
        FoundryExecutionProviderInfo>>
    discoverExecutionProviders() override
    {
        try {
            QVector<
                FoundryExecutionProviderInfo>
                result;
            const auto providers =
                foundry_local::Manager::
                    Instance()
                        .DiscoverEps();
            result.reserve(
                static_cast<qsizetype>(
                    providers.size()));
            for (const auto& provider :
                 providers) {
                result.append({
                    QString::fromUtf8(
                        provider.name),
                    provider.is_registered,
                });
            }
            return Result<QVector<
                FoundryExecutionProviderInfo>>::
                success(std::move(result));
        } catch (...) {
            return Result<QVector<
                FoundryExecutionProviderInfo>>::
                failure(sdkFailure());
        }
    }

    Result<void>
    downloadAndRegisterExecutionProviders(
        const QStringList& names,
        FoundryProgressObserver progress,
        FoundryCancellationProbe
            cancellation) override
    {
        try {
            std::vector<std::string>
                requested;
            requested.reserve(
                static_cast<std::size_t>(
                    names.size()));
            for (const QString& name : names) {
                requested.push_back(
                    name.toStdString());
            }
            const auto outcome =
                foundry_local::Manager::
                    Instance()
                        .DownloadAndRegisterEps(
                            requested,
                            [progress](
                                const std::string&,
                                double percent) {
                                if (progress) {
                                    progress(percent);
                                }
                            },
                            [cancellation] {
                                return cancellation
                                    && cancellation();
                            });
            return outcome.success
                ? Result<void>::success()
                : Result<void>::failure(
                      sdkFailure());
        } catch (...) {
            return Result<void>::failure(
                sdkFailure());
        }
    }

    Result<void>
    invalidateCatalog() override
    {
        try {
            foundry_local::Manager::Instance()
                .GetCatalog()
                .InvalidateCache();
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure(
                sdkFailure());
        }
    }

    Result<QVector<
        FoundryModelVariantInfo>>
    resolveModelVariants(
        const QString& alias) override
    {
        try {
            foundry_local::IModel* raw =
                foundry_local::Manager::
                    Instance()
                        .GetCatalog()
                        .GetModel(
                            alias.toStdString());
            selectedModel_ =
                dynamic_cast<
                    foundry_local::Model*>(
                    raw);
            if (selectedModel_ == nullptr) {
                return Result<QVector<
                    FoundryModelVariantInfo>>::
                    failure(sdkFailure());
            }
            QVector<
                FoundryModelVariantInfo>
                variants;
            for (const auto& variant :
                 selectedModel_
                     ->GetVariants()) {
                std::optional<
                    FoundryModelRuntimeInfo>
                    runtime;
                if (variant
                        .GetInfo()
                        .runtime
                        .has_value()) {
                    const auto& source =
                        *variant
                             .GetInfo()
                             .runtime;
                    runtime =
                        FoundryModelRuntimeInfo{
                            deviceType(
                                source
                                    .device_type),
                            QString::fromUtf8(
                                source
                                    .execution_provider),
                        };
                }
                variants.append({
                    QString::fromUtf8(
                        variant.GetId()),
                    std::move(runtime),
                });
            }
            return Result<QVector<
                FoundryModelVariantInfo>>::
                success(
                    std::move(variants));
        } catch (...) {
            selectedModel_ = nullptr;
            return Result<QVector<
                FoundryModelVariantInfo>>::
                failure(sdkFailure());
        }
    }

    Result<void> selectVariant(
        const QString& variantId) override
    {
        if (selectedModel_ == nullptr) {
            return Result<void>::failure(
                sdkFailure());
        }
        try {
            for (const auto& variant :
                 selectedModel_
                     ->GetVariants()) {
                if (QString::fromUtf8(
                        variant.GetId())
                    == variantId) {
                    selectedModel_
                        ->SelectVariant(
                            variant);
                    return Result<void>::
                        success();
                }
            }
        } catch (...) {
        }
        return Result<void>::failure(
            sdkFailure());
    }

    Result<void>
    downloadSelectedModel(
        FoundryProgressObserver progress,
        FoundryCancellationProbe
            cancellation) override
    {
        if (selectedModel_ == nullptr) {
            return Result<void>::failure(
                sdkFailure());
        }
        try {
            selectedModel_->Download(
                [progress](float percent) {
                    if (progress) {
                        progress(
                            static_cast<double>(
                                percent));
                    }
                    return true;
                },
                [cancellation] {
                    return cancellation
                        && cancellation();
                });
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure(
                sdkFailure());
        }
    }

    Result<void>
    loadSelectedModel() override
    {
        if (selectedModel_ == nullptr) {
            return Result<void>::failure(
                sdkFailure());
        }
        try {
            selectedModel_->Load();
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure(
                sdkFailure());
        }
    }

    Result<FoundryChatCompletion>
    completeChat(
        const QVector<FoundryChatMessage>&
            messages,
        const QVector<FoundryToolDefinition>&
            tools,
        int maximumTokens,
        int choiceCount) override
    {
        if (selectedModel_ == nullptr) {
            return Result<
                FoundryChatCompletion>::
                failure(sdkFailure());
        }
        try {
            std::vector<
                foundry_local::ChatMessage>
                sdkMessages;
            sdkMessages.reserve(
                static_cast<std::size_t>(
                    messages.size()));
            for (const auto& message :
                 messages) {
                sdkMessages.push_back(
                    sdkMessage(message));
            }
            std::vector<
                foundry_local::ToolDefinition>
                sdkTools;
            sdkTools.reserve(
                static_cast<std::size_t>(
                    tools.size()));
            for (const auto& tool : tools) {
                sdkTools.push_back(
                    sdkToolDefinition(tool));
            }

            foundry_local::ChatSettings
                settings;
            settings.max_tokens =
                maximumTokens;
            settings.n = choiceCount;
            settings.temperature = 0.0F;
            settings.tool_choice =
                foundry_local::
                    ToolChoiceKind::Auto;
            const foundry_local::
                OpenAIChatClient client(
                    *selectedModel_);
            const auto response =
                sdkTools.empty()
                ? client.CompleteChat(
                      sdkMessages,
                      settings)
                : client.CompleteChat(
                      sdkMessages,
                      sdkTools,
                      settings);
            FoundryChatCompletion result;
            result.successful =
                response.successful;
            result.httpStatusCode =
                response.http_status_code;
            result.choices.reserve(
                static_cast<qsizetype>(
                    response
                        .choices
                        .size()));
            for (const auto& choice :
                 response.choices) {
                FoundryChatChoice mapped{
                    finishReason(
                        choice.finish_reason),
                    std::nullopt,
                };
                if (choice.message
                        .has_value()) {
                    mapped.message =
                        companionMessage(
                            *choice.message);
                }
                result.choices.append(
                    std::move(mapped));
            }
            return Result<
                FoundryChatCompletion>::
                success(std::move(result));
        } catch (...) {
            return Result<
                FoundryChatCompletion>::
                failure(sdkFailure());
        }
    }

    void unloadSelectedModel()
        noexcept override
    {
        try {
            if (selectedModel_ != nullptr
                && selectedModel_
                       ->IsLoaded()) {
                selectedModel_->Unload();
            }
        } catch (...) {
        }
        selectedModel_ = nullptr;
    }

    void destroy() noexcept override
    {
        selectedModel_ = nullptr;
        foundry_local::Manager::Destroy();
    }

private:
    foundry_local::Model* selectedModel_ =
        nullptr;
};

} // namespace

Result<std::shared_ptr<FoundryLocalApi>>
createFoundryLocalApi()
{
    try {
        return Result<std::shared_ptr<
            FoundryLocalApi>>::success(
                std::make_shared<
                    FoundryLocalApiImpl>());
    } catch (...) {
        return Result<std::shared_ptr<
            FoundryLocalApi>>::failure(
                sdkFailure());
    }
}

} // namespace companion::detail
