#pragma once

#include "codex/models/BridgeModels.h"
#include "core/CredentialStore.h"
#include "core/Result.h"

#include <QByteArray>
#include <QFuture>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace companion {

struct ChatRequest final {
    ChatProvider provider = ChatProvider::OnDevice;
    QString modelId;
    QString prompt;
    QVector<BridgeAttachment> attachments;
};

struct ChatResult final {
    QString text;
    std::optional<qint64> inputTokens;
    std::optional<qint64> outputTokens;
};

struct ChatHttpRequest final {
    QUrl endpoint;
    QByteArray authorization;
    QByteArray contentType;
    QByteArray body;
};

struct ChatHttpResponse final {
    int statusCode = 0;
    QByteArray body;
};

class ChatHttpTransport {
public:
    virtual ~ChatHttpTransport() = default;

    virtual Result<ChatHttpResponse> post(
        const ChatHttpRequest& request) = 0;
};

namespace chat_detail {

Result<QNetworkRequest> makeNetworkRequest(
    const ChatHttpRequest& request);
Result<ChatHttpResponse> classifyNetworkResponse(
    const QUrl& endpoint,
    std::optional<int> statusCode,
    QNetworkReply::NetworkError networkError,
    const QString& networkErrorText,
    QByteArray body);

} // namespace chat_detail

using OnDeviceChatSender =
    std::function<Result<ChatResult>(
        const ChatRequest&)>;

class ChatService final {
public:
    explicit ChatService(
        std::shared_ptr<CredentialStore>
            credentialStore,
        OnDeviceChatSender onDeviceSender = {});
    ChatService(
        std::shared_ptr<CredentialStore>
            credentialStore,
        std::shared_ptr<ChatHttpTransport>
            openAITransport,
        std::shared_ptr<ChatHttpTransport>
            lumoTransport,
        OnDeviceChatSender onDeviceSender = {});
    ~ChatService();

    ChatService(const ChatService&) = delete;
    ChatService& operator=(
        const ChatService&) = delete;
    ChatService(ChatService&&) noexcept = default;
    ChatService& operator=(
        ChatService&&) noexcept = default;

    static bool hasUsableCredential(
        const CredentialStore& store,
        ChatProvider provider) noexcept;
    static std::shared_ptr<
        ChatHttpTransport>
    createDefaultHttpTransport();

    QFuture<Result<ChatResult>> send(
        const ChatRequest& request) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace companion
