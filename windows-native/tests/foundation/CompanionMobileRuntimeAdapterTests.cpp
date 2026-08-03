#include "app/CompanionMobileRuntimeAdapter.h"

#include "codex/commands/CommitAwareMutation.h"

#include <QFuture>
#include <QSet>
#include <QtTest>

#include <memory>
#include <stop_token>
#include <utility>

using namespace companion;

namespace {

template <typename T>
Result<T> await(
    QFuture<Result<T>> future)
{
    future.waitForFinished();
    if (future.isCanceled()
        || future.resultCount() != 1) {
        return Result<T>::failure({
            QStringLiteral(
                "test.future_failed"),
            QStringLiteral(
                "The test future did not finish."),
            false,
            {},
        });
    }
    return future.result();
}

template <typename T>
CommitAwareMutationHandle<T>
completedMutation(T value)
{
    const auto mutation =
        CommitAwareMutation<T>::create();
    CommitAwareMutationHandle<T> handle =
        mutation->handle();
    const bool committed =
        mutation->tryCommit();
    Q_ASSERT(committed);
    mutation->finish(
        Result<T>::success(
            std::move(value)));
    return handle;
}

CommitAwareMutationHandle<void>
completedVoidMutation()
{
    const auto mutation =
        CommitAwareMutation<void>::create();
    CommitAwareMutationHandle<void> handle =
        mutation->handle();
    const bool committed =
        mutation->tryCommit();
    Q_ASSERT(committed);
    mutation->finish(
        Result<void>::success());
    return handle;
}

BridgeTask task(QString id)
{
    BridgeTask value;
    value.id = std::move(id);
    value.title = value.id;
    return value;
}

CodexRuntimeDependencies dependencies(
    int& taskLoads,
    QSet<QString>& historyApprovals,
    PendingApproval& submittedApproval)
{
    CodexProcessSnapshot snapshot;
    snapshot.tasks = {
        task(QStringLiteral("thread-0")),
        task(QStringLiteral("thread-1")),
        task(QStringLiteral("thread-2")),
        task(QStringLiteral("thread-3")),
    };
    snapshot.pendingApprovals.insert(
        QStringLiteral("thread-2"),
        PendingApproval{
            QStringLiteral("thread-2"),
            42,
            PendingApprovalMethod::
                CommandExecution,
            std::nullopt,
        });

    CodexRuntimeDependencies result;
    result.taskLoader =
        [&taskLoads,
         snapshot](
            const QHash<QString, BridgeGoal>&,
            std::stop_token) {
            ++taskLoads;
            return Result<
                CodexProcessSnapshot>::
                success(snapshot);
        };
    result.goalLoader =
        [](
            const QVector<QString>&,
            std::stop_token) {
            return Result<
                QHash<
                    QString,
                    std::optional<
                        BridgeGoal>>>::
                success({});
        };
    result.executor =
        [](std::function<void()> worker) {
            worker();
        };
    result.nowProvider = [] {
        return QDateTime::
            fromMSecsSinceEpoch(
                1'700'000'000'000,
                QTimeZone::UTC);
    };
    result.history =
        CodexRuntimeHistoryDependencies{
            [&historyApprovals](
                const HistoryKey&,
                const QSet<QString>&
                    approvals,
                const QDateTime&,
                std::stop_token) {
                historyApprovals =
                    approvals;
                return Result<
                    HistorySnapshot>::
                    success({});
            },
            std::make_shared<
                HistoryCoordinator>(),
        };
    result.reads =
        CodexRuntimeReadDependencies{
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::
                    success({});
            },
            [] {
                QPromise<
                    Result<
                        BridgeUsageSnapshot>>
                    promise;
                promise.start();
                auto future =
                    promise.future();
                promise.addResult(
                    Result<
                        BridgeUsageSnapshot>::
                        success({}));
                promise.finish();
                return future;
            },
        };
    result.mutations =
        CodexRuntimeMutationDependencies{
            [](SendRequest) {
                return completedVoidMutation();
            },
            [&submittedApproval](
                PendingApproval approval,
                ApprovalDecision) {
                submittedApproval =
                    std::move(approval);
                return completedVoidMutation();
            },
            [](
                RuntimeTaskCreateRequest) {
                return completedMutation(
                    QStringLiteral(
                        "created-thread"));
            },
            [](ChatRequest) {
                return completedMutation(
                    ChatResult{
                        QStringLiteral("answer"),
                        std::nullopt,
                        std::nullopt,
                    });
            },
            [](
                RuntimeGoalMutationRequest
                    request) {
                BridgeGoal goal;
                goal.threadId =
                    request.threadId;
                return completedMutation(
                    std::move(goal));
            },
            [](
                QString,
                QUuid) {
                return completedMutation(
                    UsageResetOutcome::Reset);
            },
        };
    return result;
}

} // namespace

class CompanionMobileRuntimeAdapterTests final
    : public QObject {
    Q_OBJECT

private slots:
    void pagesTasksAndCarriesApprovalContext()
    {
        int taskLoads = 0;
        QSet<QString> historyApprovals;
        PendingApproval submittedApproval;
        const auto adapted =
            CompanionMobileRuntimeAdapter::
                create(
                    dependencies(
                        taskLoads,
                        historyApprovals,
                        submittedApproval));
        QVERIFY(adapted.hasValue());
        const CompanionMobileRuntimeBindings
            bindings = adapted.value();

        const auto page =
            await<MobileTaskPage>(
                bindings.reads
                    .taskPageLoader(
                        QStringLiteral("1"),
                        2));
        QVERIFY(page.hasValue());
        QCOMPARE(
            page.value().tasks.size(),
            2);
        QCOMPARE(
            page.value().tasks.at(0).id,
            QStringLiteral("thread-1"));
        QCOMPARE(
            page.value().tasks.at(1).id,
            QStringLiteral("thread-2"));
        QCOMPARE(
            page.value().nextCursor,
            std::optional<QString>(
                QStringLiteral("3")));

        const auto history =
            await<HistorySnapshot>(
                bindings.reads
                    .historyLoader({
                        QStringLiteral(
                            "thread-2"),
                        std::nullopt,
                        30,
                    }));
        QVERIFY(history.hasValue());
        QVERIFY(
            historyApprovals.contains(
                QStringLiteral(
                    "thread-2")));

        const auto approval =
            await<void>(
                bindings.mutations
                    .respondToApproval(
                        QStringLiteral(
                            "thread-2"),
                        ApprovalDecision::
                            ApproveOnce));
        QVERIFY(approval.hasValue());
        QCOMPARE(
            submittedApproval.threadId,
            QStringLiteral("thread-2"));
        QCOMPARE(
            submittedApproval.requestId,
            qint64(42));
        QVERIFY(taskLoads >= 3);
    }

    void rejectsIncompleteRuntimeDependencies()
    {
        CodexRuntimeDependencies
            incomplete;
        const auto adapted =
            CompanionMobileRuntimeAdapter::
                create(
                    std::move(incomplete));
        QVERIFY(!adapted.hasValue());
        QCOMPARE(
            adapted.error().code,
            QStringLiteral(
                "mobile.runtime_dependencies_unavailable"));
    }
};

QTEST_GUILESS_MAIN(
    CompanionMobileRuntimeAdapterTests)
#include "CompanionMobileRuntimeAdapterTests.moc"
