#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/ThreadRuntimeStatus.h"
#include "core/Result.h"

#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <functional>
#include <stop_token>

namespace companion {

using RuntimeDaemonAvailabilityProbe =
    std::function<bool()>;
using ThreadRuntimeRpc =
    std::function<Result<QHash<int, RpcResponse>>(
        const QVector<RpcRequest>&,
        std::stop_token)>;

class ThreadRuntimeStatusReader final {
public:
    static constexpr int kDefaultTimeoutMilliseconds =
        4'000;

    explicit ThreadRuntimeStatusReader(
        const CodexEnvironment& environment,
        int timeoutMilliseconds =
            kDefaultTimeoutMilliseconds);

    ThreadRuntimeStatusReader(
        RuntimeDaemonAvailabilityProbe
            daemonAvailabilityProbe,
        ThreadRuntimeRpc rpc);

    Result<ThreadRuntimeSnapshot> read(
        std::stop_token stopToken = {}) const;

    static Result<QHash<
        QString,
        ThreadRuntimeStatus>>
    parse(const QJsonValue& result);

private:
    RuntimeDaemonAvailabilityProbe
        daemonAvailabilityProbe_;
    ThreadRuntimeRpc rpc_;
};

} // namespace companion
