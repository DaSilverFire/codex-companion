#include "codex/runtime/CodexRuntime.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "codex/chat/ChatCatalog.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QPromise>
#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace companion;

namespace companion::detail {

struct CodexRuntimeTestAccess final {
    static Result<void> startGoalRefresh(
        CodexRuntime& runtime)
    {
        return runtime.startGoalRefresh();
    }
};

} // namespace companion::detail

namespace {

class ManualExecutor final {
public:
    RuntimeExecutor executor()
    {
        return [this](std::function<void()> worker) {
            const std::scoped_lock lock(mutex_);
            workers_.push_back(std::move(worker));
        };
    }

    qsizetype pendingCount() const
    {
        const std::scoped_lock lock(mutex_);
        return static_cast<qsizetype>(workers_.size());
    }

    void runNext()
    {
        std::function<void()> worker;
        {
            const std::scoped_lock lock(mutex_);
            QVERIFY(!workers_.empty());
            worker = std::move(workers_.front());
            workers_.pop_front();
        }
        std::thread thread(
            [worker = std::move(worker)]() mutable {
                worker();
            });
        thread.join();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> workers_;
};

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds = 5000)
{
    const QDeadlineTimer deadline(timeoutMilliseconds);
    while (!predicate()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QTest::qWait(1);
    }
    return true;
}

QFuture<Result<BridgeUsageSnapshot>>
readyUsageFuture()
{
    auto promise = std::make_shared<
        QPromise<Result<BridgeUsageSnapshot>>>();
    promise->start();
    QFuture<Result<BridgeUsageSnapshot>> future =
        promise->future();
    promise->addResult(
        Result<BridgeUsageSnapshot>::success({}));
    promise->finish();
    return future;
}

template <typename T>
CommitAwareMutationHandle<T> readyMutation(
    Result<T> result)
{
    const auto mutation =
        CommitAwareMutation<T>::create();
    CommitAwareMutationHandle<T> handle =
        mutation->handle();
    if (mutation->tryCommit()) {
        mutation->finish(std::move(result));
    }
    return handle;
}

BridgeCapabilities availableChatCapabilities()
{
    BridgeCapabilities result;
    result.chatAgents = ChatCatalog::agents();
    result.chatModels =
        ChatCatalog::capabilities({
            true,
            true,
            true,
            true,
        });
    return result;
}

bool publishChatCapabilities(
    CodexRuntime& runtime,
    CompanionCommandBus& bus,
    ManualExecutor& executor)
{
    QSignalSpy finishedSpy(
        &bus,
        &CompanionCommandBus::commandFinished);
    bus.execute(
        QStringLiteral(
            "codex.capabilities.load"),
        {
            {
                QStringLiteral("cwd"),
                QStringLiteral("C:\\repo"),
            },
        });
    if (!waitUntil([&executor] {
            return executor.pendingCount() == 1;
        })) {
        return false;
    }
    executor.runNext();
    return waitUntil([&runtime, &finishedSpy] {
        return finishedSpy.size() == 1
            && finishedSpy.at(0).at(1).toBool()
            && runtime.chatCapabilitiesValid();
    });
}

struct MutationCounters final {
    std::atomic_int send = 0;
    std::atomic_int approval = 0;
    std::atomic_int taskCreate = 0;
    std::atomic_int chat = 0;
    std::atomic_int goal = 0;
    std::atomic_int usageReset = 0;

    int total() const
    {
        return send.load()
            + approval.load()
            + taskCreate.load()
            + chat.load()
            + goal.load()
            + usageReset.load();
    }
};

CodexRuntimeDependencies dependencies(
    ManualExecutor& executor,
    MutationCounters& counters)
{
    const auto coordinator =
        std::make_shared<HistoryCoordinator>();
    return {
        [](
            const QHash<QString, BridgeGoal>&,
            std::stop_token) {
            return Result<CodexProcessSnapshot>::success(
                {});
        },
        [](
            const QVector<QString>&,
            std::stop_token) {
            return Result<
                QHash<
                    QString,
                    std::optional<BridgeGoal>>>::success(
                {});
        },
        executor.executor(),
        [] {
            return QDateTime(
                QDate(2026, 7, 22),
                QTime(12, 0),
                QTimeZone::UTC);
        },
        CodexRuntimeHistoryDependencies{
            [](
                const HistoryKey&,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token) {
                return Result<HistorySnapshot>::success(
                    {});
            },
            coordinator,
        },
        CodexRuntimeReadDependencies{
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    {});
            },
            [] {
                return readyUsageFuture();
            },
        },
        CodexRuntimeMutationDependencies{
            [&counters](SendRequest) {
                ++counters.send;
                return CommitAwareMutationHandle<void>{};
            },
            [&counters](
                PendingApproval,
                ApprovalDecision) {
                ++counters.approval;
                return CommitAwareMutationHandle<void>{};
            },
            [&counters](RuntimeTaskCreateRequest) {
                ++counters.taskCreate;
                return CommitAwareMutationHandle<QString>{};
            },
            [&counters](ChatRequest) {
                ++counters.chat;
                return CommitAwareMutationHandle<ChatResult>{};
            },
            [&counters](RuntimeGoalMutationRequest) {
                ++counters.goal;
                return CommitAwareMutationHandle<BridgeGoal>{};
            },
            [&counters](QString, QUuid) {
                ++counters.usageReset;
                return CommitAwareMutationHandle<
                    UsageResetOutcome>{};
            },
        },
    };
}

bool startRuntime(
    CodexRuntime& runtime,
    ManualExecutor& executor)
{
    if (!runtime.start().hasValue()
        || executor.pendingCount() != 1) {
        return false;
    }
    executor.runNext();
    return waitUntil([&runtime] {
        return !runtime.loading();
    });
}

QVariantMap validArguments(const QString& command)
{
    if (command == QStringLiteral("codex.reply")) {
        return {
            {QStringLiteral("threadId"), QStringLiteral("thread-a")},
            {QStringLiteral("text"), QStringLiteral("hello")},
        };
    }
    if (command == QStringLiteral("codex.steer")) {
        return {
            {QStringLiteral("threadId"), QStringLiteral("thread-a")},
            {QStringLiteral("text"), QStringLiteral("redirect")},
        };
    }
    if (command
        == QStringLiteral("codex.approval.respond")) {
        return {
            {QStringLiteral("threadId"), QStringLiteral("thread-a")},
            {
                QStringLiteral("approvalDecision"),
                QStringLiteral("approveOnce"),
            },
        };
    }
    if (command == QStringLiteral("codex.task.create")) {
        return {
            {QStringLiteral("text"), QStringLiteral("create")},
        };
    }
    if (command == QStringLiteral("codex.chat.send")) {
        return {
            {QStringLiteral("text"), QStringLiteral("chat")},
        };
    }
    if (command == QStringLiteral("codex.goal.create")
        || command == QStringLiteral("codex.goal.update")) {
        return {
            {QStringLiteral("threadId"), QStringLiteral("thread-a")},
            {
                QStringLiteral("goalObjective"),
                QStringLiteral("ship parity"),
            },
        };
    }
    if (command == QStringLiteral("codex.goal.pause")
        || command == QStringLiteral("codex.goal.resume")) {
        return {
            {QStringLiteral("threadId"), QStringLiteral("thread-a")},
        };
    }
    return {
        {
            QStringLiteral("resetCreditId"),
            QStringLiteral("credit-a"),
        },
        {
            QStringLiteral("idempotencyKey"),
            QStringLiteral(
                "7f46c6d8-074e-4ec2-b927-a858c198a8c1"),
        },
    };
}

} // namespace

class CodexRuntimeCommandTests final : public QObject {
    Q_OBJECT

private slots:
    void readOnlyBindsCompleteSurfaceAndRejectsMutations()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(executor, counters),
            host,
            CodexRuntimeMode::ReadOnlyProbe);
        QVERIFY(startRuntime(runtime, executor));

        const QVector<QString> mutations{
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
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);

        for (const QString& command : mutations) {
            bus.execute(
                command,
                validArguments(command));
        }

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            mutations.size(),
            5000);
        QCOMPARE(counters.total(), 0);
        for (qsizetype index = 0;
             index < finishedSpy.size();
             ++index) {
            const QList<QVariant> result =
                finishedSpy.at(index);
            QCOMPARE(result.at(0).toString(), mutations.at(index));
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                QStringLiteral("codex.probe_read_only"));
            QCOMPARE(
                result.at(3).toString(),
                QStringLiteral(
                    "This Codex probe is read-only."));
        }

        const QVector<QPair<QString, QVariantMap>>
            invalidReads{
                {
                    QStringLiteral("codex.refresh"),
                    {
                        {
                            QStringLiteral("unexpected"),
                            1,
                        },
                    },
                },
                {
                    QStringLiteral("codex.history.load"),
                    {},
                },
                {
                    QStringLiteral(
                        "codex.capabilities.load"),
                    {
                        {
                            QStringLiteral("unexpected"),
                            1,
                        },
                    },
                },
                {
                    QStringLiteral("codex.usage.load"),
                    {
                        {
                            QStringLiteral("unexpected"),
                            1,
                        },
                    },
                },
            };
        finishedSpy.clear();
        for (const auto& entry : invalidReads) {
            bus.execute(entry.first, entry.second);
        }

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            invalidReads.size(),
            5000);
        for (qsizetype index = 0;
             index < finishedSpy.size();
             ++index) {
            const QList<QVariant> result =
                finishedSpy.at(index);
            QCOMPARE(
                result.at(0).toString(),
                invalidReads.at(index).first);
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                QStringLiteral(
                    "codex.command_invalid_arguments"));
        }
    }

    void invalidMutationArgumentsDoNotReachStarters()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(executor, counters),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));

        QVariantMap malformedAttachment{
            {
                QStringLiteral("id"),
                QStringLiteral(
                    "7f46c6d8-074e-4ec2-b927-a858c198a8c1"),
            },
            {
                QStringLiteral("kind"),
                QStringLiteral("file"),
            },
            {
                QStringLiteral("filename"),
                QStringLiteral("notes.txt"),
            },
            {
                QStringLiteral("data"),
                QStringLiteral("not-bytes"),
            },
        };
        const QVector<QPair<QString, QVariantMap>>
            cases{
                {
                    QStringLiteral("codex.reply"),
                    {},
                },
                {
                    QStringLiteral("codex.reply"),
                    {
                        {
                            QStringLiteral("threadId"),
                            7,
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.reply"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral(" "),
                        },
                    },
                },
                {
                    QStringLiteral("codex.reply"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                        {
                            QStringLiteral("unexpected"),
                            true,
                        },
                    },
                },
                {
                    QStringLiteral("codex.reply"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                        {
                            QStringLiteral("clientMessageId"),
                            QStringLiteral("not-a-uuid"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.reply"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                        {
                            QStringLiteral("attachments"),
                            QVariantList{
                                malformedAttachment,
                            },
                        },
                    },
                },
                {
                    QStringLiteral("codex.steer"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                        {
                            QStringLiteral("expectedTurnId"),
                            false,
                        },
                    },
                },
                {
                    QStringLiteral(
                        "codex.approval.respond"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral(
                                "approvalDecision"),
                            QStringLiteral("always"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.task.create"),
                    {
                        {
                            QStringLiteral("text"),
                            QStringLiteral("create"),
                        },
                        {
                            QStringLiteral("skillName"),
                            QStringLiteral("skill"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.task.create"),
                    {
                        {
                            QStringLiteral("text"),
                            QStringLiteral("create"),
                        },
                        {
                            QStringLiteral("attachments"),
                            QVariantMap{},
                        },
                    },
                },
                {
                    QStringLiteral("codex.chat.send"),
                    {},
                },
                {
                    QStringLiteral("codex.chat.send"),
                    {
                        {
                            QStringLiteral("text"),
                            QStringLiteral(" "),
                        },
                    },
                },
                {
                    QStringLiteral("codex.chat.send"),
                    {
                        {
                            QStringLiteral("text"),
                            QStringLiteral("hello"),
                        },
                        {
                            QStringLiteral("chatProvider"),
                            QStringLiteral("other"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.goal.create"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("goalObjective"),
                            QStringLiteral("ship"),
                        },
                        {
                            QStringLiteral("goalTokenBudget"),
                            1.5,
                        },
                    },
                },
                {
                    QStringLiteral("codex.goal.update"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("goalObjective"),
                            QStringLiteral("ship"),
                        },
                        {
                            QStringLiteral("goalTokenBudget"),
                            0,
                        },
                    },
                },
                {
                    QStringLiteral("codex.goal.pause"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QStringLiteral("thread-a"),
                        },
                        {
                            QStringLiteral("goalObjective"),
                            QStringLiteral("unexpected"),
                        },
                    },
                },
                {
                    QStringLiteral("codex.goal.resume"),
                    {
                        {
                            QStringLiteral("threadId"),
                            QByteArray("thread-a"),
                        },
                    },
                },
                {
                    QStringLiteral(
                        "codex.usage.consume-reset"),
                    {
                        {
                            QStringLiteral("resetCreditId"),
                            QStringLiteral("credit-a"),
                        },
                        {
                            QStringLiteral("idempotencyKey"),
                            QStringLiteral("not-a-uuid"),
                        },
                    },
                },
            };
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);

        for (const auto& entry : cases) {
            const qsizetype expectedCount =
                finishedSpy.size() + 1;
            bus.execute(entry.first, entry.second);
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                expectedCount,
                5000);
            const QList<QVariant> result =
                finishedSpy.constLast();
            QCOMPARE(result.at(0).toString(), entry.first);
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                QStringLiteral(
                    "codex.command_invalid_arguments"));
        }
        QCOMPARE(counters.total(), 0);
    }

    void replyAndSteerRouteTypedRequests()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        QVector<SendRequest> requests;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        BridgeTask task;
        task.id = QStringLiteral("thread-a");
        task.cwd = QStringLiteral("C:\\task-cwd");
        task.activeTurnId =
            QStringLiteral("turn-current");
        requested.taskLoader =
            [task](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                CodexProcessSnapshot snapshot;
                snapshot.tasks.append(task);
                return Result<
                    CodexProcessSnapshot>::success(
                    std::move(snapshot));
            };
        requested.mutations
            ->sendMutationStarter =
            [&requests](SendRequest request) {
                requests.append(std::move(request));
                return readyMutation<void>(
                    Result<void>::success());
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));

        const QString preservedUuid =
            QStringLiteral(
                "{7F46C6D8-074E-4EC2-B927-A858C198A8C1}");
        const QVariantMap attachment{
            {
                QStringLiteral("id"),
                QStringLiteral(
                    "0955fb1d-9828-456f-954f-f53df900b8de"),
            },
            {
                QStringLiteral("kind"),
                QStringLiteral("image"),
            },
            {
                QStringLiteral("filename"),
                QStringLiteral("frame.png"),
            },
            {
                QStringLiteral("mimeType"),
                QStringLiteral("image/png"),
            },
            {
                QStringLiteral("data"),
                QByteArray("pixels"),
            },
        };
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.reply"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral(" thread-a "),
                },
                {
                    QStringLiteral("text"),
                    QStringLiteral(" hello "),
                },
                {
                    QStringLiteral("cwd"),
                    QStringLiteral(" "),
                },
                {
                    QStringLiteral("clientMessageId"),
                    QStringLiteral(" ")
                        + preservedUuid
                        + QStringLiteral(" "),
                },
                {
                    QStringLiteral("model"),
                    QStringLiteral(" gpt-test "),
                },
                {
                    QStringLiteral("reasoningEffort"),
                    QStringLiteral(" high "),
                },
                {
                    QStringLiteral("attachments"),
                    QVariantList{attachment},
                },
            });

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(requests.size(), 1);
        const SendRequest reply = requests.at(0);
        QCOMPARE(reply.action, SendAction::Reply);
        QCOMPARE(reply.threadId, QStringLiteral("thread-a"));
        QCOMPARE(reply.prompt, QStringLiteral("hello"));
        QCOMPARE(reply.cwd, QStringLiteral("C:\\task-cwd"));
        QCOMPARE(reply.expectedTurnId, QString());
        QCOMPARE(reply.clientMessageId, preservedUuid);
        QCOMPARE(reply.model, QStringLiteral("gpt-test"));
        QCOMPARE(
            reply.reasoningEffort,
            QStringLiteral("high"));
        QCOMPARE(reply.attachments.size(), 1);
        QCOMPARE(
            reply.attachments.front().kind,
            AttachmentKind::Image);
        QCOMPARE(
            reply.attachments.front().data,
            QByteArray("pixels"));

        bus.execute(
            QStringLiteral("codex.steer"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-a"),
                },
                {
                    QStringLiteral("text"),
                    QStringLiteral("redirect"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QVERIFY(finishedSpy.at(1).at(1).toBool());
        QCOMPARE(requests.size(), 2);
        const SendRequest steer = requests.at(1);
        QCOMPARE(steer.action, SendAction::Steer);
        QCOMPARE(
            steer.expectedTurnId,
            QStringLiteral("turn-current"));
        QCOMPARE(steer.cwd, QStringLiteral("C:\\task-cwd"));
        QVERIFY(!QUuid(steer.clientMessageId).isNull());
    }

    void remainingTypedMutationsRouteExactValues()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        QVector<QPair<
            PendingApproval,
            ApprovalDecision>>
            approvals;
        QVector<RuntimeTaskCreateRequest>
            taskCreates;
        QVector<RuntimeGoalMutationRequest> goals;
        QVector<QPair<QString, QUuid>> usageResets;
        std::atomic_int taskLoads = 0;
        std::atomic_int usageReads = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        const PendingApproval pending{
            QStringLiteral("thread-a"),
            91,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };
        requested.taskLoader =
            [pending, &taskLoads](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                ++taskLoads;
                CodexProcessSnapshot snapshot;
                snapshot.pendingApprovals.insert(
                    pending.threadId,
                    pending);
                return Result<
                    CodexProcessSnapshot>::success(
                    std::move(snapshot));
            };
        requested.reads->usageReadStarter =
            [&usageReads] {
                ++usageReads;
                return readyUsageFuture();
            };
        requested.mutations
            ->approvalMutationStarter =
            [&approvals](
                PendingApproval approval,
                ApprovalDecision decision) {
                approvals.append({
                    std::move(approval),
                    decision,
                });
                return readyMutation<void>(
                    Result<void>::success());
            };
        requested.mutations
            ->taskCreateMutationStarter =
            [&taskCreates](
                RuntimeTaskCreateRequest request) {
                taskCreates.append(
                    std::move(request));
                return readyMutation<QString>(
                    Result<QString>::success(
                        QStringLiteral(
                            "thread-created")));
            };
        requested.mutations
            ->goalMutationStarter =
            [&goals](
                RuntimeGoalMutationRequest request) {
                goals.append(request);
                BridgeGoal goal;
                goal.threadId = request.threadId;
                goal.objective =
                    request.objective.value_or(
                        QStringLiteral("existing"));
                goal.tokenBudget =
                    request.tokenBudget;
                return readyMutation<BridgeGoal>(
                    Result<BridgeGoal>::success(
                        std::move(goal)));
            };
        requested.mutations
            ->usageResetMutationStarter =
            [&usageResets](
                QString creditId,
                QUuid idempotencyKey) {
                usageResets.append({
                    std::move(creditId),
                    idempotencyKey,
                });
                return readyMutation<
                    UsageResetOutcome>(
                    Result<
                        UsageResetOutcome>::success(
                        UsageResetOutcome::Reset));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QString createdThreadId;
        QVector<BridgeGoal> changedGoals;
        std::optional<UsageResetOutcome>
            resetOutcome;
        QObject::connect(
            &runtime,
            &CodexRuntime::taskCreated,
            &runtime,
            [&createdThreadId](
                const QString& threadId) {
                createdThreadId = threadId;
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::goalChanged,
            &runtime,
            [&changedGoals](
                const BridgeGoal& goal) {
                changedGoals.append(goal);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::usageResetFinished,
            &runtime,
            [&resetOutcome](
                UsageResetOutcome outcome) {
                resetOutcome = outcome;
            });
        QVERIFY(startRuntime(runtime, executor));
        QCOMPARE(taskLoads.load(), 1);

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        const auto execute =
            [&bus, &finishedSpy](
                const QString& command,
                const QVariantMap& arguments) {
                const qsizetype expected =
                    finishedSpy.size() + 1;
                bus.execute(command, arguments);
                QTRY_COMPARE_WITH_TIMEOUT(
                    finishedSpy.size(),
                    expected,
                    5000);
                QVERIFY(
                    finishedSpy.constLast()
                        .at(1)
                        .toBool());
            };

        execute(
            QStringLiteral(
                "codex.approval.respond"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral(" thread-a "),
                },
                {
                    QStringLiteral("approvalDecision"),
                    QStringLiteral("approveSimilar"),
                },
            });
        QCOMPARE(approvals.size(), 1);
        QCOMPARE(approvals.front().first, pending);
        QCOMPARE(
            approvals.front().second,
            ApprovalDecision::ApproveSimilar);

        const QString clientMessageId =
            QStringLiteral(
                "7f46c6d8-074e-4ec2-b927-a858c198a8c1");
        execute(
            QStringLiteral("codex.task.create"),
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral(" build it "),
                },
                {
                    QStringLiteral("cwd"),
                    QStringLiteral(" C:\\repo "),
                },
                {
                    QStringLiteral("clientMessageId"),
                    clientMessageId,
                },
                {
                    QStringLiteral("skillName"),
                    QStringLiteral(" parity "),
                },
                {
                    QStringLiteral("skillPath"),
                    QStringLiteral(" C:\\skills\\parity "),
                },
            });
        QCOMPARE(taskCreates.size(), 1);
        QCOMPARE(
            taskCreates.front().text,
            QStringLiteral("build it"));
        QCOMPARE(
            taskCreates.front().cwd,
            QStringLiteral("C:\\repo"));
        QCOMPARE(
            taskCreates.front().clientMessageId,
            clientMessageId);
        QCOMPARE(
            taskCreates.front().skillName,
            QStringLiteral("parity"));
        QCOMPARE(
            taskCreates.front().skillPath,
            QStringLiteral("C:\\skills\\parity"));
        QTRY_COMPARE_WITH_TIMEOUT(
            createdThreadId,
            QStringLiteral("thread-created"),
            5000);

        execute(
            QStringLiteral("codex.goal.create"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral(" thread-a "),
                },
                {
                    QStringLiteral("goalObjective"),
                    QStringLiteral(" ship parity "),
                },
                {
                    QStringLiteral("goalTokenBudget"),
                    42.0,
                },
            });
        execute(
            QStringLiteral("codex.goal.update"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-a"),
                },
                {
                    QStringLiteral("goalObjective"),
                    QStringLiteral("finish parity"),
                },
            });
        execute(
            QStringLiteral("codex.goal.pause"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-a"),
                },
            });
        execute(
            QStringLiteral("codex.goal.resume"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-a"),
                },
            });
        QCOMPARE(goals.size(), 4);
        QCOMPARE(
            goals.at(0).kind,
            RuntimeGoalMutationKind::Create);
        QCOMPARE(
            goals.at(0).objective,
            std::optional<QString>(
                QStringLiteral("ship parity")));
        QCOMPARE(
            goals.at(0).tokenBudget,
            std::optional<qint64>(42));
        QCOMPARE(
            goals.at(1).kind,
            RuntimeGoalMutationKind::Update);
        QCOMPARE(
            goals.at(2).kind,
            RuntimeGoalMutationKind::Pause);
        QCOMPARE(
            goals.at(3).kind,
            RuntimeGoalMutationKind::Resume);
        QTRY_COMPARE_WITH_TIMEOUT(
            changedGoals.size(),
            4,
            5000);

        const QUuid resetId(
            QStringLiteral(
                "{607F02C6-7312-4EAA-A09A-2A092027C189}"));
        execute(
            QStringLiteral(
                "codex.usage.consume-reset"),
            {
                {
                    QStringLiteral("resetCreditId"),
                    QStringLiteral(" credit-a "),
                },
                {
                    QStringLiteral("idempotencyKey"),
                    QStringLiteral(" ")
                        + resetId.toString()
                        + QStringLiteral(" "),
                },
            });
        QCOMPARE(usageResets.size(), 1);
        QCOMPARE(
            usageResets.front().first,
            QStringLiteral("credit-a"));
        QCOMPARE(
            usageResets.front().second,
            resetId);
        QTRY_VERIFY_WITH_TIMEOUT(
            resetOutcome.has_value(),
            5000);
        QCOMPARE(
            *resetOutcome,
            UsageResetOutcome::Reset);
        QTRY_COMPARE_WITH_TIMEOUT(
            usageReads.load(),
            1,
            5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !runtime.usageLoading(),
            5000);

        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            1,
            5000);
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            1,
            5000);
        executor.runNext();
        QTRY_VERIFY_WITH_TIMEOUT(
            !runtime.loading(),
            5000);
        QCOMPARE(taskLoads.load(), 3);
    }

    void chatRoutesProviderDefaultsAndPublishesMessages()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        QVector<ChatRequest> requests;
        std::atomic_int taskLoads = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.taskLoader =
            [&taskLoads](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                ++taskLoads;
                return Result<
                    CodexProcessSnapshot>::success(
                    {});
            };
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->chatMutationStarter =
            [&requests](ChatRequest request) {
                requests.append(request);
                return readyMutation<ChatResult>(
                    Result<ChatResult>::success({
                        QStringLiteral("answer-%1")
                            .arg(requests.size()),
                        10,
                        20,
                    }));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<BridgeMessage> messages;
        QObject::connect(
            &runtime,
            &CodexRuntime::chatMessageReceived,
            &runtime,
            [&messages](
                const BridgeMessage& message) {
                messages.append(message);
            });
        QVERIFY(startRuntime(runtime, executor));
        QCOMPARE(taskLoads.load(), 1);
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));
        QCOMPARE(executor.pendingCount(), 0);

        const QVariantMap attachment{
            {
                QStringLiteral("id"),
                QStringLiteral(
                    "0955fb1d-9828-456f-954f-f53df900b8de"),
            },
            {
                QStringLiteral("kind"),
                QStringLiteral("file"),
            },
            {
                QStringLiteral("filename"),
                QStringLiteral("notes.txt"),
            },
            {
                QStringLiteral("mimeType"),
                QStringLiteral("text/plain"),
            },
            {
                QStringLiteral("data"),
                QByteArray("notes"),
            },
        };
        const QVector<QVariantMap> commands{
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral(" explain this "),
                },
                {
                    QStringLiteral("chatAgentId"),
                    QStringLiteral("explain"),
                },
                {
                    QStringLiteral("chatProvider"),
                    QStringLiteral("openAIAPI"),
                },
                {
                    QStringLiteral("chatModelId"),
                    QStringLiteral("gpt56Terra"),
                },
            },
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral(" "),
                },
                {
                    QStringLiteral("chatAgentId"),
                    QStringLiteral("unknown"),
                },
                {
                    QStringLiteral("chatProvider"),
                    QStringLiteral("onDevice"),
                },
                {
                    QStringLiteral("chatModelId"),
                    QStringLiteral("lumo:thinking"),
                },
                {
                    QStringLiteral("attachments"),
                    QVariantList{attachment},
                },
            },
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral("openai default"),
                },
                {
                    QStringLiteral("chatProvider"),
                    QStringLiteral("openAIAPI"),
                },
                {
                    QStringLiteral("chatModelId"),
                    QStringLiteral("lumo:thinking"),
                },
            },
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral("lumo default"),
                },
                {
                    QStringLiteral("chatAgentId"),
                    QStringLiteral("plan"),
                },
                {
                    QStringLiteral("chatProvider"),
                    QStringLiteral("lumoAPI"),
                },
                {
                    QStringLiteral("chatModelId"),
                    QStringLiteral("unknown"),
                },
            },
        };

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (const QVariantMap& arguments :
             commands) {
            const qsizetype expected =
                finishedSpy.size() + 1;
            bus.execute(
                QStringLiteral("codex.chat.send"),
                arguments);
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                expected,
                5000);
            QVERIFY(
                finishedSpy.constLast()
                    .at(1)
                    .toBool());
        }

        QCOMPARE(requests.size(), 4);
        QCOMPARE(
            requests.at(0).provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            requests.at(0).modelId,
            QStringLiteral("gpt56Terra"));
        QCOMPARE(
            requests.at(0).prompt,
            QStringLiteral(
                "Mode: Explain\n"
                "Explain the answer clearly, define unfamiliar terms, and use a short example when useful.\n\n"
                "User request:\n"
                "explain this"));

        QCOMPARE(
            requests.at(1).provider,
            ChatProvider::OnDevice);
        QCOMPARE(
            requests.at(1).modelId,
            QStringLiteral("on-device"));
        QCOMPARE(
            requests.at(1).prompt,
            QStringLiteral(
                "Mode: General\n"
                "Answer directly and concisely.\n\n"
                "User request:\n"));
        QCOMPARE(
            requests.at(1).attachments.size(),
            1);
        QCOMPARE(
            requests.at(1).attachments.front()
                .data,
            QByteArray("notes"));

        QCOMPARE(
            requests.at(2).provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            requests.at(2).modelId,
            QStringLiteral("gpt56Luna"));
        QCOMPARE(
            requests.at(3).provider,
            ChatProvider::LumoAPI);
        QCOMPARE(
            requests.at(3).modelId,
            QStringLiteral("automatic"));
        QCOMPARE(
            requests.at(3).prompt,
            QStringLiteral(
                "Mode: Plan\n"
                "Turn the request into a practical ordered plan. State important constraints and tradeoffs.\n\n"
                "User request:\n"
                "lumo default"));

        QTRY_COMPARE_WITH_TIMEOUT(
            messages.size(),
            4,
            5000);
        QSet<QString> messageIds;
        for (qsizetype index = 0;
             index < messages.size();
             ++index) {
            const BridgeMessage& message =
                messages.at(index);
            QVERIFY(!QUuid(message.id).isNull());
            messageIds.insert(message.id);
            QCOMPARE(
                message.role,
                MessageRole::Assistant);
            QCOMPARE(
                message.text,
                QStringLiteral("answer-%1")
                    .arg(index + 1));
            QVERIFY(message.createdAt.has_value());
            QVERIFY(!message.attachments.has_value());
        }
        QCOMPARE(messageIds.size(), 4);
        QCOMPARE(taskLoads.load(), 1);
        QCOMPARE(executor.pendingCount(), 0);
    }

    void chatPreflightUsesOnlyPublishedCapabilities()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::atomic_int chatStarts = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                BridgeCapabilities capabilities;
                capabilities.chatAgents =
                    ChatCatalog::agents();
                capabilities.chatModels =
                    QVector<BridgeChatModel>{
                        {
                            QStringLiteral(
                                "on-device"),
                            ChatProvider::OnDevice,
                            QStringLiteral(
                                "on-device"),
                            QStringLiteral(
                                "On-device"),
                            QString(),
                            true,
                            false,
                            false,
                        },
                        {
                            QStringLiteral(
                                "openai:gpt56Terra"),
                            ChatProvider::OpenAIAPI,
                            QStringLiteral(
                                "gpt56Terra"),
                            QStringLiteral(
                                "5.6 Terra"),
                            QString(),
                            false,
                            false,
                            false,
                        },
                        {
                            QStringLiteral(
                                "lumo:automatic"),
                            ChatProvider::LumoAPI,
                            QStringLiteral(
                                "automatic"),
                            QStringLiteral(
                                "Lumo Auto"),
                            QString(),
                            false,
                            true,
                            false,
                        },
                    };
                return Result<
                    BridgeCapabilities>::success(
                    std::move(capabilities));
            };
        requested.mutations->chatMutationStarter =
            [&chatStarts](ChatRequest) {
                ++chatStarts;
                return readyMutation<ChatResult>(
                    Result<ChatResult>::success({
                        QStringLiteral("unexpected"),
                        std::nullopt,
                        std::nullopt,
                    }));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));

        const auto expectFailure =
            [&bus](
                const QVariantMap& arguments,
                const QString& code,
                const QString& message) {
                QSignalSpy finishedSpy(
                    &bus,
                    &CompanionCommandBus::
                        commandFinished);
                bus.execute(
                    QStringLiteral(
                        "codex.chat.send"),
                    arguments);
                if (!waitUntil([&finishedSpy] {
                        return finishedSpy.size()
                            == 1;
                    })) {
                    return false;
                }
                const QList<QVariant> result =
                    finishedSpy.front();
                return !result.at(1).toBool()
                    && result.at(2).toString()
                        == code
                    && result.at(3).toString()
                        == message;
            };

        const QVariantMap base{
            {
                QStringLiteral("text"),
                QStringLiteral("hello"),
            },
        };
        QVERIFY(expectFailure(
            base,
            QStringLiteral(
                "codex.chat_capabilities_unavailable"),
            QStringLiteral(
                "Companion chat capabilities are not ready.")));
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));

        QVariantMap missingModel = base;
        missingModel.insert(
            QStringLiteral("chatProvider"),
            QStringLiteral("openAIAPI"));
        missingModel.insert(
            QStringLiteral("chatModelId"),
            QStringLiteral("unknown"));
        QVERIFY(expectFailure(
            missingModel,
            QStringLiteral(
                "codex.chat_model_unavailable"),
            QStringLiteral(
                "The selected chat model is unavailable.")));

        QVariantMap unavailableCloud = base;
        unavailableCloud.insert(
            QStringLiteral("chatProvider"),
            QStringLiteral("openAIAPI"));
        unavailableCloud.insert(
            QStringLiteral("chatModelId"),
            QStringLiteral("gpt56Terra"));
        QVERIFY(expectFailure(
            unavailableCloud,
            QStringLiteral(
                "codex.chat_credentials_missing"),
            QStringLiteral(
                "The selected chat provider is not configured.")));

        QVariantMap unavailableOnDevice = base;
        unavailableOnDevice.insert(
            QStringLiteral("chatProvider"),
            QStringLiteral("onDevice"));
        QVERIFY(expectFailure(
            unavailableOnDevice,
            QStringLiteral(
                "codex.chat_on_device_unavailable"),
            QStringLiteral(
                "The Windows on-device chat model is unavailable.")));

        QVariantMap unsupportedAttachments =
            base;
        unsupportedAttachments.insert(
            QStringLiteral("chatProvider"),
            QStringLiteral("lumoAPI"));
        unsupportedAttachments.insert(
            QStringLiteral("attachments"),
            QVariantList{
                QVariantMap{
                    {
                        QStringLiteral("id"),
                        QStringLiteral(
                            "0955fb1d-9828-456f-954f-f53df900b8de"),
                    },
                    {
                        QStringLiteral("kind"),
                        QStringLiteral("file"),
                    },
                    {
                        QStringLiteral("filename"),
                        QStringLiteral("notes.txt"),
                    },
                    {
                        QStringLiteral("data"),
                        QByteArray("notes"),
                    },
                },
            });
        QVERIFY(expectFailure(
            unsupportedAttachments,
            QStringLiteral(
                "codex.chat_attachments_unsupported"),
            QStringLiteral(
                "The selected chat model does not support attachments.")));

        runtime.invalidateChatCapabilities();
        QVERIFY(expectFailure(
            base,
            QStringLiteral(
                "codex.chat_capabilities_unavailable"),
            QStringLiteral(
                "Companion chat capabilities are not ready.")));
        QCOMPARE(chatStarts.load(), 0);
    }

    void chatFailuresAndMalformedResultsAreSanitized()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::atomic_int starts = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->chatMutationStarter =
            [&starts](ChatRequest) {
                const int call = ++starts;
                if (call == 1) {
                    return readyMutation<ChatResult>(
                        Result<ChatResult>::failure({
                            QStringLiteral(
                                "SECRET_CHAT_FAILURE"),
                            QStringLiteral(
                                "SECRET_CHAT_BODY"),
                            false,
                            {
                                {
                                    QStringLiteral(
                                        "secret"),
                                    QStringLiteral(
                                        "hidden"),
                                },
                            },
                        }));
                }
                if (call == 2) {
                    return CommitAwareMutationHandle<
                        ChatResult>{
                        {},
                        [] {},
                    };
                }
                if (call == 3) {
                    auto handle =
                        readyMutation<ChatResult>(
                            Result<ChatResult>::success({
                                QStringLiteral("answer"),
                                std::nullopt,
                                std::nullopt,
                            }));
                    handle.requestStopBeforeCommit =
                        {};
                    return handle;
                }
                if (call == 4) {
                    auto promise =
                        std::make_shared<
                            QPromise<
                                Result<ChatResult>>>();
                    promise->start();
                    QFuture<Result<ChatResult>>
                        future = promise->future();
                    future.cancel();
                    promise->finish();
                    return CommitAwareMutationHandle<
                        ChatResult>{
                        std::move(future),
                        [] {},
                    };
                }
                if (call == 5) {
                    auto promise =
                        std::make_shared<
                            QPromise<
                                Result<ChatResult>>>();
                    promise->start();
                    QFuture<Result<ChatResult>>
                        future = promise->future();
                    promise->finish();
                    return CommitAwareMutationHandle<
                        ChatResult>{
                        std::move(future),
                        [] {},
                    };
                }
                if (call == 6) {
                    auto promise =
                        std::make_shared<
                            QPromise<
                                Result<ChatResult>>>();
                    promise->start();
                    QFuture<Result<ChatResult>>
                        future = promise->future();
                    promise->addResult(
                        Result<ChatResult>::success({
                            QStringLiteral("one"),
                            std::nullopt,
                            std::nullopt,
                        }));
                    promise->addResult(
                        Result<ChatResult>::success({
                            QStringLiteral("two"),
                            std::nullopt,
                            std::nullopt,
                        }));
                    promise->finish();
                    return CommitAwareMutationHandle<
                        ChatResult>{
                        std::move(future),
                        [] {},
                    };
                }
                if (call == 7) {
                    throw std::runtime_error(
                        "SECRET_THROWN_CHAT");
                }
                return readyMutation<ChatResult>(
                    Result<ChatResult>::success({
                        QStringLiteral(" \t\r\n "),
                        std::nullopt,
                        std::nullopt,
                    }));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<BridgeMessage> messages;
        QObject::connect(
            &runtime,
            &CodexRuntime::chatMessageReceived,
            &runtime,
            [&messages](
                const BridgeMessage& message) {
                messages.append(message);
        });
        QVERIFY(startRuntime(runtime, executor));
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (int call = 1; call <= 8; ++call) {
            bus.execute(
                QStringLiteral("codex.chat.send"),
                {
                    {
                        QStringLiteral("text"),
                        QStringLiteral("hello"),
                    },
                });
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                call,
                5000);
            const QList<QVariant> result =
                finishedSpy.at(call - 1);
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                QStringLiteral(
                    "codex.chat_failed"));
            QCOMPARE(
                result.at(3).toString(),
                QStringLiteral(
                    "Could not complete the Companion chat request."));
        }
        QCOMPARE(starts.load(), 8);
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            20);
        QCOMPARE(messages.size(), 0);
    }

    void chatPreCommitStopCompletesAsCancellation()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::shared_ptr<
            CommitAwareMutation<ChatResult>>
            pendingMutation;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->chatMutationStarter =
            [&pendingMutation](ChatRequest) {
                pendingMutation =
                    CommitAwareMutation<
                        ChatResult>::create();
                return pendingMutation->handle();
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<BridgeMessage> messages;
        QObject::connect(
            &runtime,
            &CodexRuntime::chatMessageReceived,
            &runtime,
            [&messages](
                const BridgeMessage& message) {
                messages.append(message);
            });
        QVERIFY(startRuntime(runtime, executor));
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.chat.send"),
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral("hello"),
                },
            });
        QTRY_VERIFY_WITH_TIMEOUT(
            pendingMutation != nullptr,
            5000);
        QCOMPARE(finishedSpy.size(), 0);

        runtime.stop();
        QVERIFY(!pendingMutation->tryCommit());
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        const QList<QVariant> result =
            finishedSpy.front();
        QVERIFY(!result.at(1).toBool());
        QCOMPARE(
            result.at(2).toString(),
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(
            result.at(3).toString(),
            QStringLiteral(
                "The Codex operation was canceled."));
        QCOMPARE(messages.size(), 0);
    }

    void chatPostCommitStopSuppressesPublication()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::shared_ptr<
            CommitAwareMutation<ChatResult>>
            pendingMutation;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->chatMutationStarter =
            [&pendingMutation](ChatRequest) {
                pendingMutation =
                    CommitAwareMutation<
                        ChatResult>::create();
                CommitAwareMutationHandle<
                    ChatResult> handle =
                        pendingMutation->handle();
                if (!pendingMutation->tryCommit()) {
                    pendingMutation->finish(
                        Result<ChatResult>::failure({
                            QStringLiteral(
                                "unexpected_cancel"),
                            QStringLiteral(
                                "Unexpected cancel."),
                            false,
                            {},
                        }));
                }
                handle.requestStopBeforeCommit =
                    [] {
                        throw std::runtime_error(
                            "SECRET_STOP_CALLBACK");
                    };
                return handle;
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<BridgeMessage> messages;
        QObject::connect(
            &runtime,
            &CodexRuntime::chatMessageReceived,
            &runtime,
            [&messages](
                const BridgeMessage& message) {
                messages.append(message);
            });
        QVERIFY(startRuntime(runtime, executor));
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.chat.send"),
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral("hello"),
                },
            });
        QTRY_VERIFY_WITH_TIMEOUT(
            pendingMutation != nullptr,
            5000);
        QCOMPARE(finishedSpy.size(), 0);

        runtime.stop();
        QVERIFY(
            pendingMutation->finish(
                Result<ChatResult>::success({
                    QStringLiteral("committed answer"),
                    3,
                    4,
                })));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(
            finishedSpy.front()
                .at(1)
                .toBool());
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            20);
        QCOMPARE(messages.size(), 0);
    }

    void closedContinuationHostRejectsChatBeforeStarter()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::atomic_int chatStarts = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->chatMutationStarter =
            [&chatStarts](ChatRequest) {
                ++chatStarts;
                return readyMutation<ChatResult>(
                    Result<ChatResult>::success({
                        QStringLiteral("unexpected"),
                        std::nullopt,
                        std::nullopt,
                    }));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));
        host->stopAcceptingAndDrain();

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.chat.send"),
            {
                {
                    QStringLiteral("text"),
                    QStringLiteral("hello"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        const QList<QVariant> result =
            finishedSpy.front();
        QVERIFY(!result.at(1).toBool());
        QCOMPARE(
            result.at(2).toString(),
            QStringLiteral("codex.chat_failed"));
        QCOMPARE(
            result.at(3).toString(),
            QStringLiteral(
                "Could not complete the Companion chat request."));
        QCOMPARE(chatStarts.load(), 0);
    }

    void serviceFailuresUseCommandSpecificSanitizers()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        std::atomic_int taskLoads = 0;
        std::atomic_int approvalStarts = 0;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        const PendingApproval pending{
            QStringLiteral("thread-a"),
            77,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };
        requested.taskLoader =
            [pending, &taskLoads](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                ++taskLoads;
                CodexProcessSnapshot snapshot;
                BridgeTask task;
                task.id =
                    QStringLiteral("thread-a");
                task.title =
                    QStringLiteral("Task A");
                task.activeTurnId =
                    QStringLiteral("turn-a");
                snapshot.tasks.append(task);
                snapshot.pendingApprovals.insert(
                    pending.threadId,
                    pending);
                return Result<
                    CodexProcessSnapshot>::success(
                    std::move(snapshot));
            };
        requested.reads->capabilityLoader =
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    availableChatCapabilities());
            };
        requested.mutations->sendMutationStarter =
            [&counters](SendRequest) {
                ++counters.send;
                return readyMutation<void>(
                    Result<void>::failure({
                        QStringLiteral(
                            "SECRET_SEND"),
                        QStringLiteral(
                            "SECRET_SEND_BODY"),
                        false,
                        {},
                    }));
            };
        requested.mutations
            ->approvalMutationStarter =
            [&counters,
             &approvalStarts](
                PendingApproval,
                ApprovalDecision) {
                ++counters.approval;
                const int call =
                    ++approvalStarts;
                return readyMutation<void>(
                    Result<void>::failure({
                        call == 1
                            ? QStringLiteral(
                                  "SECRET_APPROVAL")
                            : QStringLiteral(
                                  "approval.request_not_found"),
                        QStringLiteral(
                            "SECRET_APPROVAL_BODY"),
                        false,
                        {},
                    }));
            };
        requested.mutations
            ->taskCreateMutationStarter =
            [&counters](
                RuntimeTaskCreateRequest) {
                ++counters.taskCreate;
                return readyMutation<QString>(
                    Result<QString>::failure({
                        QStringLiteral(
                            "SECRET_CREATE"),
                        QStringLiteral(
                            "SECRET_CREATE_BODY"),
                        false,
                        {},
                    }));
            };
        requested.mutations->chatMutationStarter =
            [&counters](ChatRequest) {
                ++counters.chat;
                return readyMutation<ChatResult>(
                    Result<ChatResult>::failure({
                        QStringLiteral(
                            "SECRET_CHAT"),
                        QStringLiteral(
                            "SECRET_CHAT_BODY"),
                        false,
                        {},
                    }));
            };
        requested.mutations->goalMutationStarter =
            [&counters](
                RuntimeGoalMutationRequest) {
                ++counters.goal;
                return readyMutation<BridgeGoal>(
                    Result<BridgeGoal>::failure({
                        QStringLiteral(
                            "SECRET_GOAL"),
                        QStringLiteral(
                            "SECRET_GOAL_BODY"),
                        false,
                        {},
                    }));
            };
        requested.mutations
            ->usageResetMutationStarter =
            [&counters](QString, QUuid) {
                ++counters.usageReset;
                return readyMutation<
                    UsageResetOutcome>(
                    Result<
                        UsageResetOutcome>::failure({
                        QStringLiteral(
                            "SECRET_USAGE"),
                        QStringLiteral(
                            "SECRET_USAGE_BODY"),
                        false,
                        {},
                    }));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<QString> taskCreated;
        QVector<BridgeMessage> chatMessages;
        QVector<BridgeGoal> changedGoals;
        QVector<UsageResetOutcome>
            resetOutcomes;
        QObject::connect(
            &runtime,
            &CodexRuntime::taskCreated,
            &runtime,
            [&taskCreated](
                const QString& threadId) {
                taskCreated.append(threadId);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::chatMessageReceived,
            &runtime,
            [&chatMessages](
                const BridgeMessage& message) {
                chatMessages.append(message);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::goalChanged,
            &runtime,
            [&changedGoals](
                const BridgeGoal& goal) {
                changedGoals.append(goal);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::usageResetFinished,
            &runtime,
            [&resetOutcomes](
                UsageResetOutcome outcome) {
                resetOutcomes.append(outcome);
            });
        QVERIFY(startRuntime(runtime, executor));
        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            1,
            5000);
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            0,
            5000);
        QVERIFY(
            publishChatCapabilities(
                runtime,
                bus,
                executor));

        struct Case final {
            QString command;
            QVariantMap arguments;
            QString code;
            QString message;
        };
        const QVector<Case> cases{
            {
                QStringLiteral("codex.reply"),
                validArguments(
                    QStringLiteral("codex.reply")),
                QStringLiteral("codex.send_failed"),
                QStringLiteral(
                    "Codex could not accept the message."),
            },
            {
                QStringLiteral("codex.steer"),
                validArguments(
                    QStringLiteral("codex.steer")),
                QStringLiteral("codex.send_failed"),
                QStringLiteral(
                    "Codex could not accept the message."),
            },
            {
                QStringLiteral(
                    "codex.approval.respond"),
                validArguments(
                    QStringLiteral(
                        "codex.approval.respond")),
                QStringLiteral(
                    "codex.approval_failed"),
                QStringLiteral(
                    "Codex could not apply the approval decision."),
            },
            {
                QStringLiteral(
                    "codex.approval.respond"),
                validArguments(
                    QStringLiteral(
                        "codex.approval.respond")),
                QStringLiteral(
                    "codex.approval_not_pending"),
                QStringLiteral(
                    "The Codex approval request is no longer pending."),
            },
            {
                QStringLiteral(
                    "codex.task.create"),
                validArguments(
                    QStringLiteral(
                        "codex.task.create")),
                QStringLiteral(
                    "codex.task_create_failed"),
                QStringLiteral(
                    "Could not create the Codex task."),
            },
            {
                QStringLiteral("codex.chat.send"),
                validArguments(
                    QStringLiteral(
                        "codex.chat.send")),
                QStringLiteral(
                    "codex.chat_failed"),
                QStringLiteral(
                    "Could not complete the Companion chat request."),
            },
            {
                QStringLiteral(
                    "codex.goal.update"),
                validArguments(
                    QStringLiteral(
                        "codex.goal.update")),
                QStringLiteral(
                    "codex.goal_mutation_failed"),
                QStringLiteral(
                    "Could not update the Codex goal."),
            },
            {
                QStringLiteral(
                    "codex.usage.consume-reset"),
                validArguments(
                    QStringLiteral(
                        "codex.usage.consume-reset")),
                QStringLiteral(
                    "codex.usage_reset_failed"),
                QStringLiteral(
                    "Could not apply the Codex usage reset."),
            },
        };

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (const Case& testCase : cases) {
            const qsizetype expected =
                finishedSpy.size() + 1;
            bus.execute(
                testCase.command,
                testCase.arguments);
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                expected,
                5000);
            const QList<QVariant> result =
                finishedSpy.constLast();
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                testCase.code);
            QCOMPARE(
                result.at(3).toString(),
                testCase.message);
            QVERIFY(
                !result.at(2)
                     .toString()
                     .contains(
                         QStringLiteral("SECRET")));
            QVERIFY(
                !result.at(3)
                     .toString()
                     .contains(
                         QStringLiteral("SECRET")));
        }

        QCOMPARE(counters.send.load(), 2);
        QCOMPARE(counters.approval.load(), 2);
        QCOMPARE(counters.taskCreate.load(), 1);
        QCOMPARE(counters.chat.load(), 1);
        QCOMPARE(counters.goal.load(), 1);
        QCOMPARE(counters.usageReset.load(), 1);
        QCOMPARE(taskLoads.load(), 1);
        QCOMPARE(taskCreated.size(), 0);
        QCOMPARE(chatMessages.size(), 0);
        QCOMPARE(changedGoals.size(), 0);
        QCOMPARE(resetOutcomes.size(), 0);
        QCOMPARE(executor.pendingCount(), 0);
    }

    void malformedTypedSuccessesDoNotPublish()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.mutations
            ->taskCreateMutationStarter =
            [](RuntimeTaskCreateRequest) {
                return readyMutation<QString>(
                    Result<QString>::success(
                        QStringLiteral(" ")));
            };
        requested.mutations->goalMutationStarter =
            [](
                RuntimeGoalMutationRequest) {
                BridgeGoal goal;
                goal.threadId =
                    QStringLiteral(
                        "different-thread");
                goal.objective =
                    QStringLiteral("wrong");
                return readyMutation<BridgeGoal>(
                    Result<BridgeGoal>::success(
                        std::move(goal)));
            };
        requested.mutations
            ->usageResetMutationStarter =
            [](QString, QUuid) {
                return readyMutation<
                    UsageResetOutcome>(
                    Result<
                        UsageResetOutcome>::success(
                        static_cast<
                            UsageResetOutcome>(99)));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVector<QString> taskCreated;
        QVector<BridgeGoal> changedGoals;
        QVector<UsageResetOutcome>
            resetOutcomes;
        QObject::connect(
            &runtime,
            &CodexRuntime::taskCreated,
            &runtime,
            [&taskCreated](
                const QString& threadId) {
                taskCreated.append(threadId);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::goalChanged,
            &runtime,
            [&changedGoals](
                const BridgeGoal& goal) {
                changedGoals.append(goal);
            });
        QObject::connect(
            &runtime,
            &CodexRuntime::usageResetFinished,
            &runtime,
            [&resetOutcomes](
                UsageResetOutcome outcome) {
                resetOutcomes.append(outcome);
            });
        QVERIFY(startRuntime(runtime, executor));

        const QVector<QPair<QString, QString>>
            cases{
                {
                    QStringLiteral(
                        "codex.task.create"),
                    QStringLiteral(
                        "codex.task_create_failed"),
                },
                {
                    QStringLiteral(
                        "codex.goal.update"),
                    QStringLiteral(
                        "codex.goal_mutation_failed"),
                },
                {
                    QStringLiteral(
                        "codex.usage.consume-reset"),
                    QStringLiteral(
                        "codex.usage_reset_failed"),
                },
            };
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (const auto& testCase : cases) {
            const qsizetype expected =
                finishedSpy.size() + 1;
            bus.execute(
                testCase.first,
                validArguments(testCase.first));
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                expected,
                5000);
            QVERIFY(
                !finishedSpy.constLast()
                     .at(1)
                     .toBool());
            QCOMPARE(
                finishedSpy.constLast()
                    .at(2)
                    .toString(),
                testCase.second);
        }
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            20);
        QCOMPARE(taskCreated.size(), 0);
        QCOMPARE(changedGoals.size(), 0);
        QCOMPARE(resetOutcomes.size(), 0);
    }

    void goalMutationRecomputesStatusThenReappliesRuntimeBeforePublishing()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        requested.taskLoader =
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                CodexProcessSnapshot snapshot;
                BridgeTask goalTask;
                goalTask.id =
                    QStringLiteral("goal-paused");
                goalTask.status =
                    TaskStatus::Running;
                snapshot.tasks.append(goalTask);

                BridgeTask runtimeTask;
                runtimeTask.id =
                    QStringLiteral("runtime-active");
                runtimeTask.status =
                    TaskStatus::Completed;
                snapshot.tasks.append(runtimeTask);
                snapshot.runtimeStatuses.insert(
                    runtimeTask.id,
                    ThreadRuntimeStatus::Active);
                return Result<
                    CodexProcessSnapshot>::success(
                    std::move(snapshot));
            };
        requested.mutations
            ->goalMutationStarter =
            [](
                RuntimeGoalMutationRequest request) {
                BridgeGoal goal;
                goal.threadId = request.threadId;
                goal.objective =
                    QStringLiteral("paused objective");
                goal.status = GoalStatus::Paused;
                return readyMutation<BridgeGoal>(
                    Result<BridgeGoal>::success(
                        std::move(goal)));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));
        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            1,
            5000);
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            0,
            5000);

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        const auto pause =
            [&bus, &finishedSpy, &runtime](
                const QString& threadId,
                qsizetype taskIndex) {
                const qsizetype expected =
                    finishedSpy.size() + 1;
                bus.execute(
                    QStringLiteral("codex.goal.pause"),
                    {
                        {
                            QStringLiteral("threadId"),
                            threadId,
                        },
                    });
                QTRY_COMPARE_WITH_TIMEOUT(
                    finishedSpy.size(),
                    expected,
                    5000);
                QVERIFY(
                    finishedSpy.constLast()
                        .at(1)
                        .toBool());
                QTRY_VERIFY_WITH_TIMEOUT(
                    runtime.processSnapshot()
                        .tasks.at(taskIndex)
                        .goal.has_value(),
                    5000);
            };

        pause(
            QStringLiteral("goal-paused"),
            0);
        QCOMPARE(
            runtime.processSnapshot()
                .tasks.at(0)
                .status,
            TaskStatus::Waiting);

        pause(
            QStringLiteral("runtime-active"),
            1);
        QCOMPARE(
            runtime.processSnapshot()
                .tasks.at(1)
                .status,
            TaskStatus::Running);
    }

    void olderGoalReadCannotOverwriteMutation()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutationCounters counters;
        CodexRuntimeDependencies requested =
            dependencies(executor, counters);
        BridgeTask task;
        task.id = QStringLiteral("thread-a");
        task.title = QStringLiteral("Task A");
        requested.taskLoader =
            [task](
                const QHash<QString, BridgeGoal>&
                    cachedGoals,
                std::stop_token) mutable {
                const auto cached =
                    cachedGoals.constFind(task.id);
                task.goal =
                    cached != cachedGoals.constEnd()
                    ? std::optional<BridgeGoal>(
                          cached.value())
                    : std::nullopt;
                CodexProcessSnapshot snapshot;
                snapshot.tasks.append(task);
                return Result<
                    CodexProcessSnapshot>::success(
                    std::move(snapshot));
            };
        requested.goalLoader =
            [](
                const QVector<QString>&,
                std::stop_token) {
                BridgeGoal oldGoal;
                oldGoal.threadId =
                    QStringLiteral("thread-a");
                oldGoal.objective =
                    QStringLiteral("old objective");
                return Result<
                    QHash<
                        QString,
                        std::optional<
                            BridgeGoal>>>::success(
                    {
                        {
                            oldGoal.threadId,
                            oldGoal,
                        },
                    });
            };
        requested.mutations
            ->goalMutationStarter =
            [](
                RuntimeGoalMutationRequest request) {
                BridgeGoal newGoal;
                newGoal.threadId =
                    request.threadId;
                newGoal.objective =
                    request.objective.value_or(
                        QString());
                return readyMutation<BridgeGoal>(
                    Result<BridgeGoal>::success(
                        std::move(newGoal)));
            };

        const auto host =
            std::make_shared<RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            CodexRuntimeMode::Interactive);
        QVERIFY(startRuntime(runtime, executor));
        QCOMPARE(
            runtime.processSnapshot()
                .tasks.size(),
            1);
        QVERIFY(
            detail::CodexRuntimeTestAccess::
                startGoalRefresh(runtime)
                .hasValue());
        QCOMPARE(executor.pendingCount(), 1);

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.goal.update"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-a"),
                },
                {
                    QStringLiteral("goalObjective"),
                    QStringLiteral("new objective"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime.processSnapshot()
                .tasks.front()
                .goal.has_value(),
            5000);
        QCOMPARE(
            runtime.processSnapshot()
                .tasks.front()
                .goal->objective,
            QStringLiteral("new objective"));

        QTRY_COMPARE_WITH_TIMEOUT(
            executor.pendingCount(),
            2,
            5000);
        executor.runNext();
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            20);
        QCOMPARE(
            runtime.processSnapshot()
                .tasks.front()
                .goal->objective,
            QStringLiteral("new objective"));
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeCommandTests)

#include "CodexRuntimeCommandTests.moc"
