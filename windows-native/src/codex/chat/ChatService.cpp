#include "codex/chat/ChatService.h"
#include "core/ChatCredentialService.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QtConcurrentRun>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace companion {

namespace {

constexpr int kMaximumOutputTokens = 700;
constexpr int kDiagnosticBodyCharacters = 360;
constexpr int kTransferTimeoutMilliseconds = 30000;

struct OpenAIModel final {
    QString apiModel;
    QString reasoningEffort;
    QString verbosity;
};

struct LumoModel final {
    QString apiModel;
};

class SensitiveBytes final {
public:
    SensitiveBytes() = default;

    explicit SensitiveBytes(
        QByteArray bytes)
        : bytes_(std::move(bytes))
    {
    }

    ~SensitiveBytes()
    {
        clear();
    }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(
        const SensitiveBytes&) = delete;

    SensitiveBytes(SensitiveBytes&& other) noexcept
        : bytes_(std::move(other.bytes_))
    {
    }

    SensitiveBytes& operator=(
        SensitiveBytes&& other) noexcept
    {
        if (this != &other) {
            clear();
            bytes_ = std::move(other.bytes_);
        }
        return *this;
    }

    QByteArray& bytes() noexcept
    {
        return bytes_;
    }

    const QByteArray& bytes() const noexcept
    {
        return bytes_;
    }

private:
    void clear() noexcept
    {
        if (!bytes_.isEmpty()) {
            SecureZeroMemory(
                const_cast<char*>(
                    bytes_.constData()),
                static_cast<SIZE_T>(
                    bytes_.size()));
            bytes_.clear();
        }
    }

    QByteArray bytes_;
};

struct SensitiveHttpRequest final {
    ChatHttpRequest request;

    ~SensitiveHttpRequest()
    {
        if (!request.authorization.isEmpty()) {
            SecureZeroMemory(
                request.authorization.data(),
                static_cast<SIZE_T>(
                    request.authorization.size()));
        }
    }
};

std::optional<ChatCredentialKind> credentialKind(
    ChatProvider provider) noexcept
{
    switch (provider) {
    case ChatProvider::OpenAIAPI:
        return ChatCredentialKind::OpenAI;
    case ChatProvider::LumoAPI:
        return ChatCredentialKind::Lumo;
    case ChatProvider::OnDevice:
    default:
        return std::nullopt;
    }
}

} // namespace

namespace chat_detail {

Result<QNetworkRequest> makeNetworkRequest(
    const ChatHttpRequest& request)
{
    if (!request.endpoint.isValid()
        || request.endpoint.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive)
            != 0) {
        return Result<QNetworkRequest>::failure({
            QStringLiteral("chat.https_required"),
            QStringLiteral(
                "Chat providers require an HTTPS endpoint."),
            false,
            {
                {
                    QStringLiteral("endpoint"),
                    request.endpoint.toString(),
                },
            },
        });
    }

    QNetworkRequest networkRequest(
        request.endpoint);
    networkRequest.setRawHeader(
        QByteArray("Authorization"),
        request.authorization);
    networkRequest.setRawHeader(
        QByteArray("Content-Type"),
        request.contentType);
    networkRequest.setTransferTimeout(
        kTransferTimeoutMilliseconds);
    networkRequest.setAttribute(
        QNetworkRequest::
            RedirectPolicyAttribute,
        QNetworkRequest::
            ManualRedirectPolicy);
    return Result<QNetworkRequest>::success(
        std::move(networkRequest));
}

Result<ChatHttpResponse>
classifyNetworkResponse(
    const QUrl& endpoint,
    std::optional<int> statusCode,
    QNetworkReply::NetworkError networkError,
    const QString& networkErrorText,
    QByteArray body)
{
    const bool successfulStatus =
        statusCode.has_value()
        && *statusCode >= 200
        && *statusCode < 300;
    if (!statusCode.has_value()
        || (successfulStatus
            && networkError
                != QNetworkReply::NoError)) {
        return Result<ChatHttpResponse>::failure({
            QStringLiteral(
                "chat.transport_failed"),
            networkErrorText.trimmed().isEmpty()
                ? QStringLiteral(
                      "The chat provider did not return a complete HTTP response.")
                : networkErrorText,
            networkError
                != QNetworkReply::
                    OperationCanceledError,
            {
                {
                    QStringLiteral("endpoint"),
                    endpoint.toString(),
                },
                {
                    QStringLiteral("networkError"),
                    static_cast<int>(
                        networkError),
                },
            },
        });
    }
    return Result<ChatHttpResponse>::success({
        *statusCode,
        std::move(body),
    });
}

} // namespace chat_detail

namespace {

class QtChatHttpTransport final
    : public ChatHttpTransport {
public:
    Result<ChatHttpResponse> post(
        const ChatHttpRequest& request) override
    {
        const Result<QNetworkRequest> prepared =
            chat_detail::makeNetworkRequest(
                request);
        if (!prepared.hasValue()) {
            return Result<ChatHttpResponse>::failure(
                prepared.error());
        }

        QNetworkAccessManager manager;
        QNetworkRequest networkRequest =
            prepared.value();

        QEventLoop eventLoop;
        QNetworkReply* reply =
            manager.post(
                networkRequest,
                request.body);
        QObject::connect(
            reply,
            &QNetworkReply::finished,
            &eventLoop,
            &QEventLoop::quit);
        eventLoop.exec();

        const QVariant statusValue =
            reply->attribute(
                QNetworkRequest::
                    HttpStatusCodeAttribute);
        const QByteArray body =
            reply->readAll();
        const QNetworkReply::NetworkError
            networkError = reply->error();
        const QString networkErrorText =
            reply->errorString();
        reply->deleteLater();

        return chat_detail::
            classifyNetworkResponse(
                request.endpoint,
                statusValue.isValid()
                    ? std::optional<int>(
                          statusValue.toInt())
                    : std::nullopt,
                networkError,
                networkErrorText,
                body);
    }
};

QString clippedBody(
    const QByteArray& body)
{
    return QString::fromUtf8(body)
        .left(kDiagnosticBodyCharacters);
}

CompanionError chatError(
    QString code,
    QString message,
    bool retryable = false,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        std::move(context),
    };
}

Result<ChatResult> chatFailure(
    QString code,
    QString message,
    bool retryable = false,
    QVariantMap context = {})
{
    return Result<ChatResult>::failure(
        chatError(
            std::move(code),
            std::move(message),
            retryable,
            std::move(context)));
}

SensitiveBytes trimAsciiWhitespace(
    SensitiveBytes bytes)
{
    const QByteArray& source =
        bytes.bytes();
    qsizetype first = 0;
    while (first < source.size()
           && std::isspace(
               static_cast<unsigned char>(
                   source.at(first)))) {
        ++first;
    }
    qsizetype last = source.size();
    while (last > first
           && std::isspace(
               static_cast<unsigned char>(
                   source.at(last - 1)))) {
        --last;
    }
    return SensitiveBytes(QByteArray(
        source.constData() + first,
        last - first));
}

OpenAIModel openAIModel(
    const QString& selection)
{
    if (selection
            == QStringLiteral("gpt56Terra")
        || selection
            == QStringLiteral(
                "gpt55Thinking")) {
        return {
            QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("high"),
            QStringLiteral("medium"),
        };
    }
    if (selection
            == QStringLiteral("gpt56Sol")
        || selection
            == QStringLiteral("gpt55Pro")) {
        return {
            QStringLiteral("gpt-5.6-sol"),
            QStringLiteral("xhigh"),
            QStringLiteral("medium"),
        };
    }
    return {
        QStringLiteral("gpt-5.6-luna"),
        QStringLiteral("low"),
        QStringLiteral("low"),
    };
}

LumoModel lumoModel(
    const QString& selection)
{
    if (selection
        == QStringLiteral("fast")) {
        return {
            QStringLiteral(
                "lumo-basic-v1"),
        };
    }
    if (selection
        == QStringLiteral("thinking")) {
        return {
            QStringLiteral(
                "lumo-plus-v1"),
        };
    }
    return {
        QStringLiteral("auto"),
    };
}

Result<std::optional<qint64>>
optionalInteger(
    const QJsonObject& object,
    const QString& field,
    const QString& provider)
{
    const QJsonValue value =
        object.value(field);
    if (value.isUndefined()
        || value.isNull()) {
        return Result<std::optional<qint64>>::
            success(std::nullopt);
    }
    if (!value.isDouble()) {
        return Result<std::optional<qint64>>::
            failure(chatError(
                QStringLiteral(
                    "chat.response_invalid"),
                provider
                    + QStringLiteral(
                        " returned invalid token usage.")));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number)
        || std::floor(number) != number
        || number
            < static_cast<double>(
                std::numeric_limits<qint64>::
                    min())
        || number
            > static_cast<double>(
                std::numeric_limits<qint64>::
                    max())) {
        return Result<std::optional<qint64>>::
            failure(chatError(
                QStringLiteral(
                    "chat.response_invalid"),
                provider
                    + QStringLiteral(
                        " returned invalid token usage.")));
    }
    return Result<std::optional<qint64>>::
        success(static_cast<qint64>(number));
}

Result<QJsonObject> parsedObject(
    const QByteArray& body,
    const QString& provider,
    const QString& errorCode)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            body, &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<QJsonObject>::failure(
            chatError(
                errorCode,
                QStringLiteral(
                    "Could not read %1 response: %2.")
                    .arg(
                        provider,
                        parseError.errorString()),
                false,
                {
                    {
                        QStringLiteral("body"),
                        clippedBody(body),
                    },
                }));
    }
    return Result<QJsonObject>::success(
        document.object());
}

std::optional<QString> errorEnvelopeMessage(
    const QByteArray& body)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            body, &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject error =
        document.object()
            .value(QStringLiteral("error"))
            .toObject();
    const QString message =
        error.value(QStringLiteral("message"))
            .toString()
            .trimmed();
    return message.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              message.left(
                  kDiagnosticBodyCharacters));
}

Result<ChatResult> httpStatusFailure(
    const ChatHttpResponse& response,
    const QString& provider,
    const QString& errorCode)
{
    const QString body =
        clippedBody(response.body);
    const std::optional<QString>
        envelope = errorEnvelopeMessage(
            response.body);
    return chatFailure(
        errorCode,
        envelope.value_or(
            QStringLiteral("%1 returned %2: %3")
                .arg(
                    provider,
                    QString::number(
                        response.statusCode),
                    body)),
        false,
        {
            {
                QStringLiteral("statusCode"),
                response.statusCode,
            },
            {
                QStringLiteral("body"), body,
            },
        });
}

Result<ChatResult> parseOpenAI(
    const ChatHttpResponse& response)
{
    constexpr auto errorCode =
        "chat.openai_request_failed";
    if (response.statusCode < 200
        || response.statusCode >= 300) {
        return httpStatusFailure(
            response,
            QStringLiteral("OpenAI"),
            QString::fromLatin1(errorCode));
    }
    const Result<QJsonObject> parsed =
        parsedObject(
            response.body,
            QStringLiteral("OpenAI"),
            QString::fromLatin1(errorCode));
    if (!parsed.hasValue()) {
        return Result<ChatResult>::failure(
            parsed.error());
    }
    const QJsonObject object =
        parsed.value();

    std::optional<QString> text;
    const QJsonValue outputText =
        object.value(
            QStringLiteral("output_text"));
    if (outputText.isString()) {
        text = outputText.toString();
    } else if (!outputText.isUndefined()
               && !outputText.isNull()) {
        return chatFailure(
            QString::fromLatin1(errorCode),
            QStringLiteral(
                "Could not read OpenAI response."),
            false,
            {
                {
                    QStringLiteral("body"),
                    clippedBody(response.body),
                },
            });
    } else {
        const QJsonValue output =
            object.value(
                QStringLiteral("output"));
        if (!output.isUndefined()
            && !output.isNull()
            && !output.isArray()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read OpenAI response."),
                false,
                {
                    {
                        QStringLiteral("body"),
                        clippedBody(
                            response.body),
                    },
                });
        }
        QStringList chunks;
        for (const QJsonValue& outputItem :
             output.toArray()) {
            if (!outputItem.isObject()) {
                return chatFailure(
                    QString::fromLatin1(
                        errorCode),
                    QStringLiteral(
                        "Could not read OpenAI response."));
            }
            const QJsonValue content =
                outputItem.toObject().value(
                    QStringLiteral("content"));
            if (content.isUndefined()
                || content.isNull()) {
                continue;
            }
            if (!content.isArray()) {
                return chatFailure(
                    QString::fromLatin1(
                        errorCode),
                    QStringLiteral(
                        "Could not read OpenAI response."));
            }
            for (const QJsonValue& contentItem :
                 content.toArray()) {
                if (!contentItem.isObject()) {
                    return chatFailure(
                        QString::fromLatin1(
                            errorCode),
                        QStringLiteral(
                            "Could not read OpenAI response."));
                }
                const QJsonValue chunk =
                    contentItem.toObject().value(
                        QStringLiteral("text"));
                if (chunk.isString()) {
                    chunks.append(
                        chunk.toString());
                } else if (!chunk.isUndefined()
                           && !chunk.isNull()) {
                    return chatFailure(
                        QString::fromLatin1(
                            errorCode),
                        QStringLiteral(
                            "Could not read OpenAI response."));
                }
            }
        }
        if (!chunks.isEmpty()) {
            text = chunks.join('\n');
        }
    }

    const QString normalized =
        text.value_or(QString()).trimmed();
    if (normalized.isEmpty()) {
        return chatFailure(
            QString::fromLatin1(errorCode),
            QStringLiteral(
                "OpenAI returned no response text."));
    }

    std::optional<qint64> inputTokens;
    std::optional<qint64> outputTokens;
    const QJsonValue usage =
        object.value(QStringLiteral("usage"));
    if (!usage.isUndefined()
        && !usage.isNull()) {
        if (!usage.isObject()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read OpenAI response."));
        }
        const Result<std::optional<qint64>>
            input = optionalInteger(
                usage.toObject(),
                QStringLiteral(
                    "input_tokens"),
                QStringLiteral("OpenAI"));
        const Result<std::optional<qint64>>
            output = optionalInteger(
                usage.toObject(),
                QStringLiteral(
                    "output_tokens"),
                QStringLiteral("OpenAI"));
        if (!input.hasValue()) {
            return Result<ChatResult>::failure(
                input.error());
        }
        if (!output.hasValue()) {
            return Result<ChatResult>::failure(
                output.error());
        }
        inputTokens = input.value();
        outputTokens = output.value();
    }
    return Result<ChatResult>::success({
        normalized,
        inputTokens,
        outputTokens,
    });
}

Result<ChatResult> parseLumo(
    const ChatHttpResponse& response)
{
    constexpr auto errorCode =
        "chat.lumo_request_failed";
    if (response.statusCode < 200
        || response.statusCode >= 300) {
        return httpStatusFailure(
            response,
            QStringLiteral("Lumo"),
            QString::fromLatin1(errorCode));
    }
    const Result<QJsonObject> parsed =
        parsedObject(
            response.body,
            QStringLiteral("Lumo"),
            QString::fromLatin1(errorCode));
    if (!parsed.hasValue()) {
        return Result<ChatResult>::failure(
            parsed.error());
    }
    const QJsonObject object =
        parsed.value();
    const QJsonValue choicesValue =
        object.value(QStringLiteral("choices"));
    if (!choicesValue.isArray()) {
        return chatFailure(
            QString::fromLatin1(errorCode),
            QStringLiteral(
                "Could not read Lumo response."),
            false,
            {
                {
                    QStringLiteral("body"),
                    clippedBody(response.body),
                },
            });
    }
    QStringList chunks;
    for (const QJsonValue& choiceValue :
         choicesValue.toArray()) {
        if (!choiceValue.isObject()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read Lumo response."));
        }
        const QJsonValue messageValue =
            choiceValue.toObject().value(
                QStringLiteral("message"));
        if (!messageValue.isObject()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read Lumo response."));
        }
        const QJsonObject message =
            messageValue.toObject();
        if (!message
                 .value(QStringLiteral("role"))
                 .isString()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read Lumo response."));
        }
        const QJsonValue content =
            message.value(
                QStringLiteral("content"));
        if (content.isString()) {
            chunks.append(
                content.toString());
        } else if (!content.isUndefined()
                   && !content.isNull()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read Lumo response."));
        }
    }
    const QString text =
        chunks.join('\n').trimmed();
    if (text.isEmpty()) {
        return chatFailure(
            QString::fromLatin1(errorCode),
            QStringLiteral(
                "Lumo returned no response text."));
    }

    std::optional<qint64> inputTokens;
    std::optional<qint64> outputTokens;
    const QJsonValue usage =
        object.value(QStringLiteral("usage"));
    if (!usage.isUndefined()
        && !usage.isNull()) {
        if (!usage.isObject()) {
            return chatFailure(
                QString::fromLatin1(errorCode),
                QStringLiteral(
                    "Could not read Lumo response."));
        }
        const Result<std::optional<qint64>>
            input = optionalInteger(
                usage.toObject(),
                QStringLiteral(
                    "prompt_tokens"),
                QStringLiteral("Lumo"));
        const Result<std::optional<qint64>>
            output = optionalInteger(
                usage.toObject(),
                QStringLiteral(
                    "completion_tokens"),
                QStringLiteral("Lumo"));
        if (!input.hasValue()) {
            return Result<ChatResult>::failure(
                input.error());
        }
        if (!output.hasValue()) {
            return Result<ChatResult>::failure(
                output.error());
        }
        inputTokens = input.value();
        outputTokens = output.value();
    }
    return Result<ChatResult>::success({
        text,
        inputTokens,
        outputTokens,
    });
}

Result<SensitiveBytes> providerKey(
    const std::shared_ptr<CredentialStore>& store,
    const QString& service,
    const QString& missingCode,
    const QString& missingMessage)
{
    if (!store) {
        return Result<SensitiveBytes>::failure(
            chatError(
                missingCode,
                missingMessage));
    }
    Result<QByteArray> loaded =
        store->read(service);
    if (!loaded.hasValue()) {
        if (loaded.error().code
            == QStringLiteral(
                "credential.not_found")) {
            return Result<SensitiveBytes>::
                failure(chatError(
                    missingCode,
                    missingMessage));
        }
        return Result<SensitiveBytes>::failure(
            loaded.error());
    }
    SensitiveBytes key =
        trimAsciiWhitespace(
            SensitiveBytes(
                std::move(loaded.value())));
    if (key.bytes().isEmpty()) {
        return Result<SensitiveBytes>::failure(
            chatError(
                missingCode,
                missingMessage));
    }
    return Result<SensitiveBytes>::success(
        std::move(key));
}

Result<ChatResult> sendOpenAI(
    const ChatRequest& request,
    const std::shared_ptr<CredentialStore>&
        credentials,
    const std::shared_ptr<ChatHttpTransport>&
        transport)
{
    if (!request.attachments.isEmpty()) {
        return chatFailure(
            QStringLiteral(
                "chat.openai_attachments_unsupported"),
            QStringLiteral(
                "OpenAI API chat does not support Companion attachments yet."));
    }
    Result<SensitiveBytes> loadedKey =
        providerKey(
            credentials,
            ChatCredentialService::serviceName(
                ChatCredentialKind::OpenAI),
            QStringLiteral(
                "chat.openai_key_missing"),
            QStringLiteral(
                "Add an OpenAI API key in Codex Companion Settings."));
    if (!loadedKey.hasValue()) {
        return Result<ChatResult>::failure(
            loadedKey.error());
    }
    if (!transport) {
        return chatFailure(
            QStringLiteral(
                "chat.openai_unavailable"),
            QStringLiteral(
                "OpenAI API chat is unavailable."));
    }

    const OpenAIModel model =
        openAIModel(request.modelId);
    const QJsonObject body{
        {
            QStringLiteral("model"),
            model.apiModel,
        },
        {
            QStringLiteral("input"),
            request.prompt,
        },
        {
            QStringLiteral("reasoning"),
            QJsonObject{
                {
                    QStringLiteral("effort"),
                    model.reasoningEffort,
                },
            },
        },
        {
            QStringLiteral("text"),
            QJsonObject{
                {
                    QStringLiteral("verbosity"),
                    model.verbosity,
                },
            },
        },
        {
            QStringLiteral(
                "max_output_tokens"),
            kMaximumOutputTokens,
        },
    };

    SensitiveHttpRequest http{
        {
            QUrl(QStringLiteral(
                "https://api.openai.com/v1/responses")),
            QByteArray("Bearer ")
                + loadedKey.value().bytes(),
            QByteArray("application/json"),
            QJsonDocument(body).toJson(
                QJsonDocument::Compact),
        },
    };
    const Result<ChatHttpResponse> response =
        transport->post(http.request);
    if (!response.hasValue()) {
        return Result<ChatResult>::failure(
            response.error());
    }
    return parseOpenAI(response.value());
}

Result<ChatResult> sendLumo(
    const ChatRequest& request,
    const std::shared_ptr<CredentialStore>&
        credentials,
    const std::shared_ptr<ChatHttpTransport>&
        transport)
{
    if (!request.attachments.isEmpty()) {
        return chatFailure(
            QStringLiteral(
                "chat.lumo_attachments_unsupported"),
            QStringLiteral(
                "Lumo API chat does not support Companion attachments yet."));
    }
    Result<SensitiveBytes> loadedKey =
        providerKey(
            credentials,
            ChatCredentialService::serviceName(
                ChatCredentialKind::Lumo),
            QStringLiteral(
                "chat.lumo_key_missing"),
            QStringLiteral(
                "Add a Lumo API key in Codex Companion Settings."));
    if (!loadedKey.hasValue()) {
        return Result<ChatResult>::failure(
            loadedKey.error());
    }
    if (!transport) {
        return chatFailure(
            QStringLiteral(
                "chat.lumo_unavailable"),
            QStringLiteral(
                "Lumo API chat is unavailable."));
    }

    const LumoModel model =
        lumoModel(request.modelId);
    const QJsonObject body{
        {
            QStringLiteral("model"),
            model.apiModel,
        },
        {
            QStringLiteral("messages"),
            QJsonArray{
                QJsonObject{
                    {
                        QStringLiteral("role"),
                        QStringLiteral("user"),
                    },
                    {
                        QStringLiteral("content"),
                        request.prompt,
                    },
                },
            },
        },
        {
            QStringLiteral("stream"), false,
        },
        {
            QStringLiteral("max_tokens"),
            kMaximumOutputTokens,
        },
    };

    SensitiveHttpRequest http{
        {
            QUrl(QStringLiteral(
                "https://lumo.proton.me/api/ai/v1/chat/completions")),
            QByteArray("Bearer ")
                + loadedKey.value().bytes(),
            QByteArray("application/json"),
            QJsonDocument(body).toJson(
                QJsonDocument::Compact),
        },
    };
    const Result<ChatHttpResponse> response =
        transport->post(http.request);
    if (!response.hasValue()) {
        return Result<ChatResult>::failure(
            response.error());
    }
    return parseLumo(response.value());
}

} // namespace

struct ChatService::State final {
    std::shared_ptr<CredentialStore>
        credentialStore;
    std::shared_ptr<ChatHttpTransport>
        openAITransport;
    std::shared_ptr<ChatHttpTransport>
        lumoTransport;
    OnDeviceChatSender onDeviceSender;
};

ChatService::ChatService(
    std::shared_ptr<CredentialStore>
        credentialStore,
    OnDeviceChatSender onDeviceSender)
    : ChatService(
          std::move(credentialStore),
          std::make_shared<
              QtChatHttpTransport>(),
          std::make_shared<
              QtChatHttpTransport>(),
          std::move(onDeviceSender))
{
}

ChatService::ChatService(
    std::shared_ptr<CredentialStore>
        credentialStore,
    std::shared_ptr<ChatHttpTransport>
        openAITransport,
    std::shared_ptr<ChatHttpTransport>
        lumoTransport,
    OnDeviceChatSender onDeviceSender)
    : state_(std::make_shared<State>(
          State{
              std::move(credentialStore),
              std::move(openAITransport),
              std::move(lumoTransport),
              std::move(onDeviceSender),
          }))
{
}

ChatService::~ChatService() = default;

bool ChatService::hasUsableCredential(
    const CredentialStore& store,
    ChatProvider provider) noexcept
{
    const auto kind = credentialKind(provider);
    if (!kind.has_value()) {
        return false;
    }
    return ChatCredentialService::
        hasUsableCredential(
            store,
            *kind);
}

std::shared_ptr<ChatHttpTransport>
ChatService::createDefaultHttpTransport()
{
    return std::make_shared<
        QtChatHttpTransport>();
}

QFuture<Result<ChatResult>> ChatService::send(
    const ChatRequest& request) const
{
    const std::shared_ptr<State> state =
        state_;
    return QtConcurrent::run(
        QThreadPool::globalInstance(),
        [state, request] {
            try {
                switch (request.provider) {
                case ChatProvider::OnDevice:
                    if (!state->onDeviceSender) {
                        return chatFailure(
                            QStringLiteral(
                                "chat.on_device_unavailable"),
                            QStringLiteral(
                                "The Windows on-device chat provider is unavailable."));
                    }
                    return state->onDeviceSender(
                        request);
                case ChatProvider::OpenAIAPI:
                    return sendOpenAI(
                        request,
                        state->credentialStore,
                        state->openAITransport);
                case ChatProvider::LumoAPI:
                    return sendLumo(
                        request,
                        state->credentialStore,
                        state->lumoTransport);
                default:
                    return chatFailure(
                        QStringLiteral(
                            "chat.provider_unsupported"),
                        QStringLiteral(
                            "The requested chat provider is unsupported."));
                }
            } catch (const std::exception& error) {
                return chatFailure(
                    QStringLiteral(
                        "chat.provider_failed"),
                    QStringLiteral(
                        "The chat provider failed."),
                    false,
                    {
                        {
                            QStringLiteral("detail"),
                            QString::fromUtf8(
                                error.what()),
                        },
                    });
            } catch (...) {
                return chatFailure(
                    QStringLiteral(
                        "chat.provider_failed"),
                    QStringLiteral(
                        "The chat provider failed."));
            }
        });
}

} // namespace companion
