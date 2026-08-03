#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "core/Result.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QString>
#include <QVector>

#include <functional>
#include <stop_token>

namespace companion {

struct RpcRequest final {
    int id = 0;
    QString method;
    QJsonObject params;

    QJsonObject jsonObject() const;

    friend bool operator==(
        const RpcRequest&,
        const RpcRequest&) = default;
};

struct RpcResponse final {
    QJsonValue result;
    QString error;
    bool isError = false;

    friend bool operator==(
        const RpcResponse&,
        const RpcResponse&) = default;
};

using CodexExecutableCandidateProvider =
    std::function<QVector<QString>()>;

enum class AppServerTransportMode {
    Standalone,
    SharedDaemonProxy,
};

class AppServerRpcClient final {
public:
    static constexpr int kDefaultTimeoutMilliseconds = 20'000;

    explicit AppServerRpcClient(
        const CodexEnvironment& environment,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            kDefaultTimeoutMilliseconds,
        AppServerTransportMode transportMode =
            AppServerTransportMode::Standalone);

    explicit AppServerRpcClient(
        CodexExecutableCandidateProvider candidateProvider,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            kDefaultTimeoutMilliseconds,
        AppServerTransportMode transportMode =
            AppServerTransportMode::Standalone);

    Result<QHash<int, RpcResponse>> perform(
        const QVector<RpcRequest>& requests) const;
    Result<QHash<int, RpcResponse>> perform(
        const QVector<RpcRequest>& requests,
        std::stop_token stopToken) const;

private:
    CodexExecutableCandidateProvider candidateProvider_;
    QProcessEnvironment processEnvironment_;
    int timeoutMilliseconds_ = kDefaultTimeoutMilliseconds;
    AppServerTransportMode transportMode_ =
        AppServerTransportMode::Standalone;
};

} // namespace companion
