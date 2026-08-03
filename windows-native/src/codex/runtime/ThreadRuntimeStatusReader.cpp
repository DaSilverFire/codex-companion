#include "codex/runtime/ThreadRuntimeStatusReader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSet>

#include <memory>
#include <utility>

namespace companion {

namespace {

CompanionError runtimeError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        true,
        {},
    };
}

CompanionError canceledError()
{
    return {
        QStringLiteral("codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

} // namespace

ThreadRuntimeStatusReader::
ThreadRuntimeStatusReader(
    const CodexEnvironment& environment,
    int timeoutMilliseconds)
    : ThreadRuntimeStatusReader(
          [environment] {
              return QFileInfo::exists(
                  QDir(environment.codexHome)
                      .filePath(QStringLiteral(
                          "app-server-control/"
                          "app-server-control.sock")));
          },
          [client = std::make_shared<
               AppServerRpcClient>(
               environment,
               QProcessEnvironment::
                   systemEnvironment(),
               timeoutMilliseconds,
               AppServerTransportMode::
                   SharedDaemonProxy)](
              const QVector<RpcRequest>& requests,
              std::stop_token stopToken) {
              return client->perform(
                  requests,
                  stopToken);
          })
{
}

ThreadRuntimeStatusReader::
ThreadRuntimeStatusReader(
    RuntimeDaemonAvailabilityProbe
        daemonAvailabilityProbe,
    ThreadRuntimeRpc rpc)
    : daemonAvailabilityProbe_(
          std::move(daemonAvailabilityProbe)),
      rpc_(std::move(rpc))
{
}

Result<ThreadRuntimeSnapshot>
ThreadRuntimeStatusReader::read(
    std::stop_token stopToken) const
{
    if (stopToken.stop_requested()) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            canceledError());
    }

    bool daemonAvailable = false;
    try {
        daemonAvailable =
            daemonAvailabilityProbe_
            && daemonAvailabilityProbe_();
    } catch (...) {
        daemonAvailable = false;
    }
    if (!daemonAvailable) {
        return Result<
            ThreadRuntimeSnapshot>::success(
            {{}, false});
    }
    if (!rpc_) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_unavailable"),
                QStringLiteral(
                    "Codex shared runtime status is unavailable.")));
    }

    const RpcRequest request{
        2,
        QStringLiteral("thread/list"),
        {
            {QStringLiteral("limit"), 100},
            {QStringLiteral("sortKey"),
             QStringLiteral("updated_at")},
            {QStringLiteral("sortDirection"),
             QStringLiteral("desc")},
            {QStringLiteral("archived"), false},
            {QStringLiteral("useStateDbOnly"), true},
        },
    };
    Result<QHash<int, RpcResponse>> responses =
        Result<QHash<int, RpcResponse>>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_unavailable"),
                QStringLiteral(
                    "Codex shared runtime status is unavailable.")));
    try {
        responses = rpc_({request}, stopToken);
    } catch (...) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_unavailable"),
                QStringLiteral(
                    "Codex shared runtime status is unavailable.")));
    }
    if (!responses.hasValue()) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            responses.error());
    }

    const auto responseIterator =
        responses.value().constFind(request.id);
    if (responseIterator
        == responses.value().cend()) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_invalid"),
                QStringLiteral(
                    "Codex shared runtime returned no thread list.")));
    }
    if (responseIterator->isError) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_failed"),
                responseIterator->error
                        .trimmed()
                        .isEmpty()
                    ? QStringLiteral(
                          "Codex could not read shared thread status.")
                    : responseIterator->error));
    }

    auto parsed = parse(
        responseIterator->result);
    if (!parsed.hasValue()) {
        return Result<
            ThreadRuntimeSnapshot>::failure(
            parsed.error());
    }
    return Result<
        ThreadRuntimeSnapshot>::success({
        std::move(parsed.value()),
        true,
    });
}

Result<QHash<QString, ThreadRuntimeStatus>>
ThreadRuntimeStatusReader::parse(
    const QJsonValue& result)
{
    if (!result.isObject()) {
        return Result<QHash<
            QString,
            ThreadRuntimeStatus>>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_invalid"),
                QStringLiteral(
                    "Codex shared runtime returned an invalid thread list.")));
    }
    const QJsonValue dataValue =
        result.toObject().value(
            QStringLiteral("data"));
    if (!dataValue.isArray()) {
        return Result<QHash<
            QString,
            ThreadRuntimeStatus>>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.thread_runtime_invalid"),
                QStringLiteral(
                    "Codex shared runtime returned an invalid thread list.")));
    }

    QHash<QString, ThreadRuntimeStatus>
        statuses;
    for (const QJsonValue& value :
         dataValue.toArray()) {
        if (!value.isObject()) {
            return Result<QHash<
                QString,
                ThreadRuntimeStatus>>::failure(
                runtimeError(
                    QStringLiteral(
                        "codex.thread_runtime_invalid"),
                    QStringLiteral(
                        "Codex shared runtime returned a malformed thread entry.")));
        }
        const QJsonObject thread =
            value.toObject();
        const QString id =
            thread.value(QStringLiteral("id"))
                .toString()
                .trimmed();
        const QJsonValue statusValue =
            thread.value(QStringLiteral("status"));
        if (id.isEmpty()
            || !statusValue.isObject()) {
            continue;
        }
        const QJsonObject status =
            statusValue.toObject();
        const QString type =
            status.value(QStringLiteral("type"))
                .toString();

        ThreadRuntimeStatus parsedStatus =
            ThreadRuntimeStatus::NotLoaded;
        if (type == QStringLiteral("active")) {
            QSet<QString> flags;
            const QJsonValue flagsValue =
                status.value(QStringLiteral(
                    "activeFlags"));
            if (flagsValue.isArray()) {
                for (const QJsonValue& flag :
                     flagsValue.toArray()) {
                    if (flag.isString()) {
                        flags.insert(
                            flag.toString());
                    }
                }
            }
            if (flags.contains(QStringLiteral(
                    "waitingOnApproval"))) {
                parsedStatus =
                    ThreadRuntimeStatus::
                        WaitingOnApproval;
            } else if (
                flags.contains(QStringLiteral(
                    "waitingOnUserInput"))) {
                parsedStatus =
                    ThreadRuntimeStatus::
                        WaitingOnUserInput;
            } else {
                parsedStatus =
                    ThreadRuntimeStatus::Active;
            }
        } else if (
            type == QStringLiteral("idle")) {
            parsedStatus =
                ThreadRuntimeStatus::Idle;
        } else if (
            type
            == QStringLiteral("systemError")) {
            parsedStatus =
                ThreadRuntimeStatus::SystemError;
        }
        statuses.insert(id, parsedStatus);
    }
    return Result<QHash<
        QString,
        ThreadRuntimeStatus>>::success(
        std::move(statuses));
}

} // namespace companion
