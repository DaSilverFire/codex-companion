#include "codex/runtime/CodexRuntimeLifetime.h"
#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/CodexRuntimeOperationState.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QPromise>
#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

using namespace companion;

namespace companion::detail {

struct CodexRuntimeTestAccess final {
    static std::shared_ptr<
        CodexRuntimeOperationRegistry>
    operationRegistry(CodexRuntime& runtime)
    {
        return runtime.operationRegistry_;
    }

    static void fail(
        CodexRuntime& runtime,
        CompanionError error)
    {
        runtime.stopForRuntimeFailure(error);
    }
};

} // namespace companion::detail

namespace {

class ManualGate final {
public:
    void release()
    {
        {
            const std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    void wait()
    {
        std::unique_lock lock(mutex_);
        condition_.wait(
            lock,
            [this] {
                return released_;
            });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool released_ = false;
};

class ManualExecutor final {
public:
    RuntimeExecutor executor()
    {
        return [this](std::function<void()> worker) {
            const std::scoped_lock lock(mutex_);
            workers_.push_back(
                std::move(worker));
        };
    }

    qsizetype pendingCount() const
    {
        const std::scoped_lock lock(mutex_);
        return static_cast<qsizetype>(
            workers_.size());
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> workers_;
};

CodexProcessSnapshot emptySnapshot()
{
    return {};
}

RuntimeGoalLoader emptyGoalLoader()
{
    return [](
               const QVector<QString>&,
               std::stop_token) {
        return Result<
            QHash<
                QString,
                std::optional<BridgeGoal>>>::
            success({});
    };
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
        Result<BridgeUsageSnapshot>::success(
            {}));
    promise->finish();
    return future;
}

CodexRuntimeDependencies dependencies(
    RuntimeExecutor executor)
{
    const auto historyCoordinator =
        std::make_shared<HistoryCoordinator>();
    return {
        [](
            const QHash<QString, BridgeGoal>&,
            std::stop_token) {
            return Result<CodexProcessSnapshot>::
                success(emptySnapshot());
        },
        emptyGoalLoader(),
        std::move(executor),
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
                return Result<HistorySnapshot>::
                    success({});
            },
            historyCoordinator,
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
    };
}

} // namespace

class CodexRuntimeLifetimeTests final
    : public QObject {
    Q_OBJECT

private slots:
    void lifetimeStartsWithD2Dependencies()
    {
        ManualExecutor executor;
        auto created =
            CodexRuntimeLifetime::create(
                dependencies(
                    executor.executor()));
        QVERIFY(created.hasValue());
        std::unique_ptr<CodexRuntimeLifetime>
            lifetime =
                std::move(created.value());
        QVERIFY(lifetime);
        QVERIFY(
            lifetime->continuationHost()
                .accepting());
        QVERIFY(
            lifetime->runtime().start()
                .hasValue());
        QCOMPARE(executor.pendingCount(), 1);
    }

    void missingD2DependencyIsRejected()
    {
        for (int missing = 0;
             missing < 10;
             ++missing) {
            ManualExecutor executor;
            CodexRuntimeDependencies requested =
                dependencies(
                    executor.executor());
            switch (missing) {
            case 0:
                requested.taskLoader = {};
                break;
            case 1:
                requested.goalLoader = {};
                break;
            case 2:
                requested.executor = {};
                break;
            case 3:
                requested.nowProvider = {};
                break;
            case 4:
                requested.history.reset();
                break;
            case 5:
                requested.history
                    ->historyLoader = {};
                break;
            case 6:
                requested.history
                    ->historyCoordinator.reset();
                break;
            case 7:
                requested.reads.reset();
                break;
            case 8:
                requested.reads
                    ->capabilityLoader = {};
                break;
            case 9:
                requested.reads
                    ->usageReadStarter = {};
                break;
            default:
                Q_UNREACHABLE();
            }

            auto created =
                CodexRuntimeLifetime::create(
                    std::move(requested));

            QVERIFY(!created.hasValue());
            QCOMPARE(
                created.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QCOMPARE(executor.pendingCount(), 0);
        }
    }

    void invalidContinuationHostFailsBeforeCommandBinding()
    {
        ManualExecutor executor;
        {
            CompanionState state;
            CompanionCommandBus bus;
            CodexRuntime runtime(
                state,
                bus,
                dependencies(
                    executor.executor()),
                {},
                {});

            const Result<void> started =
                runtime.start();

            QVERIFY(!started.hasValue());
            QCOMPARE(
                started.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QVERIFY(
                bus.registerHandler(
                       QStringLiteral(
                           "codex.refresh"),
                       [](
                           const QVariantMap&,
                           CompanionCommandBus::
                               Completion completion) {
                           completion(
                               Result<void>::success());
                       })
                    .hasValue());
        }

        {
            CompanionState state;
            CompanionCommandBus bus;
            const auto host =
                std::make_shared<
                    RuntimeContinuationHost>();
            host->stopAcceptingAndDrain();
            CodexRuntime runtime(
                state,
                bus,
                dependencies(
                    executor.executor()),
                host,
                {});

            const Result<void> started =
                runtime.start();

            QVERIFY(!started.hasValue());
            QCOMPARE(
                started.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QVERIFY(
                bus.registerHandler(
                       QStringLiteral(
                           "codex.refresh"),
                       [](
                           const QVariantMap&,
                           CompanionCommandBus::
                               Completion completion) {
                           completion(
                               Result<void>::success());
                       })
                    .hasValue());
        }
        QCOMPARE(executor.pendingCount(), 0);
    }

    void missingHistoryLeavesPreviousCommandGroupUntouched()
    {
        ManualExecutor executor;
        CompanionState state;
        CompanionCommandBus bus;
        int previousRefreshCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral(
                               "codex.refresh"),
                           [&previousRefreshCalls](
                               const QVariantMap&,
                               CompanionCommandBus::
                                   Completion completion) {
                               ++previousRefreshCalls;
                               completion(
                                   Result<void>::
                                       success());
                           },
                       },
                   })
                .hasValue());
        CodexRuntimeDependencies requested =
            dependencies(executor.executor());
        requested.history.reset();
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            {});

        const Result<void> started =
            runtime.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(executor.pendingCount(), 0);

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(previousRefreshCalls, 1);

        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(1).at(2).toString(),
            QStringLiteral("ui.unknown_command"));
    }

    void missingReadsLeavesPreviousCommandGroupUntouched()
    {
        ManualExecutor executor;
        CompanionState state;
        CompanionCommandBus bus;
        int previousRefreshCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral(
                               "codex.refresh"),
                           [&previousRefreshCalls](
                               const QVariantMap&,
                               CompanionCommandBus::
                                   Completion completion) {
                               ++previousRefreshCalls;
                               completion(
                                   Result<void>::
                                       success());
                           },
                       },
                   })
                .hasValue());
        CodexRuntimeDependencies requested =
            dependencies(executor.executor());
        requested.reads.reset();
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            std::move(requested),
            host,
            {});

        const Result<void> started =
            runtime.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(executor.pendingCount(), 0);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(previousRefreshCalls, 1);

        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(1).at(2).toString(),
            QStringLiteral("ui.unknown_command"));
    }

    void destructionDrainsBeforeOwnersAndServices()
    {
        struct ServiceOwner final {
        };

        ManualExecutor executor;
        auto service =
            std::make_shared<ServiceOwner>();
        std::weak_ptr<ServiceOwner> weakService =
            service;
        CodexRuntimeDependencies requested =
            dependencies(executor.executor());
        requested.taskLoader =
            [service](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(emptySnapshot());
            };
        service.reset();

        auto created =
            CodexRuntimeLifetime::create(
                std::move(requested));
        QVERIFY(created.hasValue());
        std::unique_ptr<CodexRuntimeLifetime>
            lifetime =
                std::move(created.value());

        std::mutex mutex;
        std::condition_variable condition;
        std::atomic_bool runtimeDestroyed = false;
        std::atomic_bool stateDestroyed = false;
        std::atomic_bool busDestroyed = false;
        std::atomic_bool continuationRan = false;
        std::atomic_bool stateAliveDuringDrain = false;
        std::atomic_bool busAliveDuringDrain = false;
        std::atomic_bool serviceAliveDuringDrain = false;

        QObject::connect(
            &lifetime->runtime(),
            &QObject::destroyed,
            this,
            [&] {
                runtimeDestroyed.store(true);
                condition.notify_all();
            },
            Qt::DirectConnection);
        QObject::connect(
            &lifetime->state(),
            &QObject::destroyed,
            this,
            [&] {
                stateDestroyed.store(true);
            },
            Qt::DirectConnection);
        QObject::connect(
            &lifetime->commandBus(),
            &QObject::destroyed,
            this,
            [&] {
                busDestroyed.store(true);
            },
            Qt::DirectConnection);

        QVERIFY(
            lifetime->continuationHost()
                .submit(
                    [&] {
                        std::unique_lock lock(mutex);
                        condition.wait(
                            lock,
                            [&] {
                                return runtimeDestroyed
                                    .load();
                            });
                        stateAliveDuringDrain.store(
                            !stateDestroyed.load());
                        busAliveDuringDrain.store(
                            !busDestroyed.load());
                        serviceAliveDuringDrain.store(
                            !weakService.expired());
                        continuationRan.store(true);
                    })
                .hasValue());

        lifetime.reset();

        QVERIFY(runtimeDestroyed.load());
        QVERIFY(continuationRan.load());
        QVERIFY(stateAliveDuringDrain.load());
        QVERIFY(busAliveDuringDrain.load());
        QVERIFY(serviceAliveDuringDrain.load());
        QVERIFY(stateDestroyed.load());
        QVERIFY(busDestroyed.load());
        QVERIFY(weakService.expired());
    }

    void acceptedMutationContinuationOutlivesRuntime()
    {
        struct ServiceOwner final {
        };

        ManualExecutor executor;
        auto service =
            std::make_shared<ServiceOwner>();
        std::weak_ptr<ServiceOwner> weakService =
            service;
        CodexRuntimeDependencies requested =
            dependencies(executor.executor());
        requested.taskLoader =
            [service](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(emptySnapshot());
            };
        service.reset();

        auto created =
            CodexRuntimeLifetime::create(
                std::move(requested));
        QVERIFY(created.hasValue());
        std::unique_ptr<CodexRuntimeLifetime>
            lifetime =
                std::move(created.value());
        const auto registry =
            detail::CodexRuntimeTestAccess::
                operationRegistry(
                    lifetime->runtime());
        QVERIFY(registry);

        std::atomic_bool runtimeDestroyed = false;
        std::atomic_bool stateDestroyed = false;
        std::atomic_bool busDestroyed = false;
        std::atomic_int stopCalls = 0;
        std::atomic_int completionCalls = 0;
        std::atomic_bool completionSucceeded = false;
        std::atomic_bool ownersAliveAtCompletion =
            false;
        ManualGate runtimeDestroyedGate;

        QObject::connect(
            &lifetime->runtime(),
            &QObject::destroyed,
            this,
            [&] {
                runtimeDestroyed.store(true);
                runtimeDestroyedGate.release();
            },
            Qt::DirectConnection);
        QObject::connect(
            &lifetime->state(),
            &QObject::destroyed,
            this,
            [&] {
                stateDestroyed.store(true);
            },
            Qt::DirectConnection);
        QObject::connect(
            &lifetime->commandBus(),
            &QObject::destroyed,
            this,
            [&] {
                busDestroyed.store(true);
            },
            Qt::DirectConnection);

        const auto operation =
            CodexRuntimeOperationState::createMutation(
                [&](Result<void> result) {
                    completionCalls.fetch_add(1);
                    completionSucceeded.store(
                        result.hasValue());
                    ownersAliveAtCompletion.store(
                        runtimeDestroyed.load()
                        && !stateDestroyed.load()
                        && !busDestroyed.load()
                        && !weakService.expired());
                },
                14,
                15);
        QVERIFY(
            registry->registerOperation(operation)
            != 0);
        QVERIFY(
            lifetime->continuationHost()
                .submit(
                    [operation] {
                        operation
                            ->observeMutationTerminal();
                    })
                .hasValue());
        QVERIFY(
            operation->installMutationObservation({
                [&stopCalls] {
                    stopCalls.fetch_add(1);
                },
                [&runtimeDestroyedGate](
                    CodexRuntimeOperationState& state) {
                    runtimeDestroyedGate.wait();
                    state.finish(
                        Result<void>::success());
                },
                {
                    QStringLiteral(
                        "codex.mutation_failed"),
                    QStringLiteral(
                        "Codex mutation failed."),
                    true,
                    {},
                },
            }));

        lifetime.reset();

        QVERIFY(runtimeDestroyed.load());
        QCOMPARE(stopCalls.load(), 1);
        QCOMPARE(completionCalls.load(), 1);
        QVERIFY(completionSucceeded.load());
        QVERIFY(ownersAliveAtCompletion.load());
        QCOMPARE(registry->activeOperationCount(), 0);
        QVERIFY(stateDestroyed.load());
        QVERIFY(busDestroyed.load());
        QVERIFY(weakService.expired());
    }

    void fatalRuntimeTransitionStopsRegisteredOperations()
    {
        ManualExecutor executor;
        CompanionState state;
        CompanionCommandBus bus;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(executor.executor()),
            host,
            {});
        QVERIFY(runtime.start().hasValue());

        std::atomic_int stopCalls = 0;
        int completionCalls = 0;
        QString completionError;
        const auto operation =
            CodexRuntimeOperationState::createRead(
                [&](Result<void> result) {
                    ++completionCalls;
                    completionError =
                        result.hasValue()
                        ? QString()
                        : result.error().code;
                },
                1,
                1,
                [&stopCalls] {
                    stopCalls.fetch_add(1);
                });
        const auto registry =
            detail::CodexRuntimeTestAccess::
                operationRegistry(runtime);
        QVERIFY(registry);
        QVERIFY(
            registry->registerOperation(
                operation)
            != 0);

        detail::CodexRuntimeTestAccess::fail(
            runtime,
            {
                QStringLiteral(
                    "codex.runtime_thread_mismatch"),
                QStringLiteral(
                    "Codex runtime components must share one thread."),
                false,
                {},
            });

        QCOMPARE(stopCalls.load(), 1);
        QCOMPARE(completionCalls, 1);
        QCOMPARE(
            completionError,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(registry->activeOperationCount(), 0);
        QVERIFY(!runtime.running());
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeLifetimeTests)

#include "CodexRuntimeLifetimeTests.moc"
