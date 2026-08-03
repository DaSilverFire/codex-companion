#include "codex/runtime/ThreadRuntimeStatusReader.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <stop_token>

using namespace companion;

namespace {

QJsonObject threadStatus(
    const QString& id,
    const QString& type,
    QJsonArray flags = {})
{
    QJsonObject status{
        {QStringLiteral("type"), type},
    };
    if (!flags.isEmpty()) {
        status.insert(
            QStringLiteral("activeFlags"),
            std::move(flags));
    }
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("status"),
         std::move(status)},
    };
}

} // namespace

class ThreadRuntimeStatusReaderTests final
    : public QObject {
    Q_OBJECT

private slots:
    void unavailableDaemonReturnsNonAuthoritativeEmptySnapshot()
    {
        int rpcCalls = 0;
        ThreadRuntimeStatusReader reader(
            [] { return false; },
            [&rpcCalls](
                const QVector<RpcRequest>&,
                std::stop_token) {
                ++rpcCalls;
                return Result<
                    QHash<int, RpcResponse>>::
                    success({});
            });

        const auto result = reader.read();

        QVERIFY(result.hasValue());
        QVERIFY(!result.value().authoritative);
        QVERIFY(result.value().statuses.isEmpty());
        QCOMPARE(rpcCalls, 0);
    }

    void requestsAndParsesTheMacRuntimeStatusContract()
    {
        QVector<RpcRequest> observedRequests;
        ThreadRuntimeStatusReader reader(
            [] { return true; },
            [&observedRequests](
                const QVector<RpcRequest>& requests,
                std::stop_token)
                -> Result<
                    QHash<int, RpcResponse>> {
                observedRequests = requests;
                const QJsonObject result{
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            threadStatus(
                                QStringLiteral("active"),
                                QStringLiteral("active")),
                            threadStatus(
                                QStringLiteral("approval"),
                                QStringLiteral("active"),
                                {
                                    QStringLiteral(
                                        "waitingOnUserInput"),
                                    QStringLiteral(
                                        "waitingOnApproval"),
                                }),
                            threadStatus(
                                QStringLiteral("input"),
                                QStringLiteral("active"),
                                {
                                    QStringLiteral(
                                        "waitingOnUserInput"),
                                }),
                            threadStatus(
                                QStringLiteral("idle"),
                                QStringLiteral("idle")),
                            threadStatus(
                                QStringLiteral("error"),
                                QStringLiteral(
                                    "systemError")),
                            threadStatus(
                                QStringLiteral("unknown"),
                                QStringLiteral("future")),
                            QJsonObject{
                                {QStringLiteral("id"),
                                 QStringLiteral(
                                     "missing-status")},
                            },
                        },
                    },
                };
                return Result<
                    QHash<int, RpcResponse>>::
                    success({
                        {
                            2,
                            RpcResponse{
                                result,
                                {},
                                false,
                            },
                        },
                    });
            });

        const auto result = reader.read();

        QVERIFY(result.hasValue());
        QCOMPARE(observedRequests.size(), 1);
        const RpcRequest& request =
            observedRequests.first();
        QCOMPARE(request.id, 2);
        QCOMPARE(
            request.method,
            QStringLiteral("thread/list"));
        QCOMPARE(
            request.params.value(
                QStringLiteral("limit"))
                .toInt(),
            100);
        QCOMPARE(
            request.params.value(
                QStringLiteral("sortKey"))
                .toString(),
            QStringLiteral("updated_at"));
        QCOMPARE(
            request.params.value(
                QStringLiteral("sortDirection"))
                .toString(),
            QStringLiteral("desc"));
        QCOMPARE(
            request.params.value(
                QStringLiteral("archived"))
                .toBool(),
            false);
        QCOMPARE(
            request.params.value(
                QStringLiteral("useStateDbOnly"))
                .toBool(),
            true);
        QVERIFY(result.value().authoritative);
        const auto& statuses =
            result.value().statuses;
        QCOMPARE(
            statuses.value(QStringLiteral("active")),
            ThreadRuntimeStatus::Active);
        QCOMPARE(
            statuses.value(QStringLiteral("approval")),
            ThreadRuntimeStatus::
                WaitingOnApproval);
        QCOMPARE(
            statuses.value(QStringLiteral("input")),
            ThreadRuntimeStatus::
                WaitingOnUserInput);
        QCOMPARE(
            statuses.value(QStringLiteral("idle")),
            ThreadRuntimeStatus::Idle);
        QCOMPARE(
            statuses.value(QStringLiteral("error")),
            ThreadRuntimeStatus::SystemError);
        QCOMPARE(
            statuses.value(QStringLiteral("unknown")),
            ThreadRuntimeStatus::NotLoaded);
        QVERIFY(!statuses.contains(
            QStringLiteral("missing-status")));
    }

    void malformedThreadListFailsClosed()
    {
        const auto result =
            ThreadRuntimeStatusReader::parse(
                QJsonObject{
                    {QStringLiteral("data"),
                     QStringLiteral("not-an-array")},
                });

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.thread_runtime_invalid"));
    }

    void nonObjectThreadEntryFailsClosed()
    {
        const auto result =
            ThreadRuntimeStatusReader::parse(
                QJsonObject{
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            threadStatus(
                                QStringLiteral(
                                    "valid"),
                                QStringLiteral(
                                    "active")),
                            QStringLiteral(
                                "not-an-object"),
                        },
                    },
                });

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.thread_runtime_invalid"));
    }
};

QTEST_GUILESS_MAIN(ThreadRuntimeStatusReaderTests)
#include "ThreadRuntimeStatusReaderTests.moc"
