#include "codex/runtime/CodexRuntime.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QModelIndex>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

using namespace companion;

namespace {

class ManualExecutor final {
public:
    RuntimeExecutor executor()
    {
        return [this](std::function<void()> worker) {
            submit(std::move(worker));
        };
    }

    void submit(std::function<void()> worker)
    {
        const std::scoped_lock lock(mutex_);
        workers_.push_back(std::move(worker));
    }

    qsizetype pendingCount() const
    {
        const std::scoped_lock lock(mutex_);
        return static_cast<qsizetype>(workers_.size());
    }

    std::function<void()> takeAt(qsizetype index)
    {
        const std::scoped_lock lock(mutex_);
        if (index < 0
            || index
                >= static_cast<qsizetype>(
                    workers_.size())) {
            return {};
        }
        auto iterator = workers_.begin() + index;
        std::function<void()> worker =
            std::move(*iterator);
        workers_.erase(iterator);
        return worker;
    }

    void runNext()
    {
        runAt(0);
    }

    void runAt(qsizetype index)
    {
        std::function<void()> worker = takeAt(index);
        QVERIFY(worker);
        std::thread thread(std::move(worker));
        thread.join();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> workers_;
};

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

    bool waitFor(int timeoutMilliseconds)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock,
            std::chrono::milliseconds(
                timeoutMilliseconds),
            [this] {
                return released_;
            });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool released_ = false;
};

class MutableClock final {
public:
    MutableClock()
        : now_(
              QDate(2026, 7, 22),
              QTime(12, 0),
              QTimeZone::UTC)
    {
    }

    RuntimeNowProvider provider()
    {
        return [this] {
            const std::scoped_lock lock(mutex_);
            return now_;
        };
    }

    void advanceMilliseconds(qint64 milliseconds)
    {
        const std::scoped_lock lock(mutex_);
        now_ = now_.addMSecs(milliseconds);
    }

private:
    std::mutex mutex_;
    QDateTime now_;
};

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds = 1000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()
           && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(
        QEventLoop::AllEvents,
        10);
    return predicate();
}

BridgeTask task(QString id)
{
    BridgeTask result;
    result.id = std::move(id);
    result.title =
        QStringLiteral("Task ") + result.id;
    result.preview =
        QStringLiteral("Preview ") + result.id;
    result.status = TaskStatus::Running;
    return result;
}

BridgeGoal goal(
    QString threadId,
    QString objective = {})
{
    BridgeGoal result;
    result.threadId = std::move(threadId);
    result.objective = objective.isEmpty()
        ? QStringLiteral("Goal ") + result.threadId
        : std::move(objective);
    result.status = GoalStatus::Active;
    result.tokenBudget = 1000;
    result.tokensUsed = 100;
    result.elapsedSeconds = 10;
    result.createdAt = 1000;
    result.updatedAt = 2000;
    return result;
}

CodexProcessSnapshot snapshot(
    QVector<BridgeTask> tasks = {})
{
    CodexProcessSnapshot result;
    result.tasks = std::move(tasks);
    return result;
}

RuntimeGoalLoader emptyGoalLoader()
{
    return [](
               const QVector<QString>&,
               std::stop_token) {
        return Result<
            QHash<QString, std::optional<BridgeGoal>>>::
            success({});
    };
}

RuntimeNowProvider fixedNow()
{
    return [] {
        return QDateTime(
            QDate(2026, 7, 22),
            QTime(12, 0),
            QTimeZone::UTC);
    };
}

CodexRuntimeCadence shortCadence()
{
    return {
        5'000,
        5'000,
        50,
        100,
        200,
    };
}

CodexRuntimeCadence tinyCadence()
{
    return {
        15,
        10,
        5,
        8,
        20,
    };
}

QStringList modelIds(const TaskListModel& model)
{
    QStringList ids;
    ids.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row) {
        ids.append(
            model.data(
                     model.index(row, 0),
                     TaskListModel::IdRole)
                .toString());
    }
    return ids;
}

CompanionError privateFailure(QString code)
{
    return {
        std::move(code),
        QStringLiteral(
            "C:/private/path secret stderr response"),
        false,
        {
            {
                QStringLiteral("credential"),
                QStringLiteral("private-token"),
            },
        },
    };
}

} // namespace

class CodexRuntimeTests final : public QObject {
    Q_OBJECT

private slots:
    void cadenceDefaultsMatchMacOSV034()
    {
        const CodexRuntimeCadence cadence;

        QCOMPARE(
            cadence.activeRefreshMilliseconds,
            30'000);
        QCOMPARE(
            cadence.passiveRefreshMilliseconds,
            20'000);
        QCOMPARE(
            cadence.settleRefreshMilliseconds,
            140);
        QCOMPARE(
            cadence.taskStaleMilliseconds,
            8'000);
        QCOMPARE(
            cadence.goalStaleMilliseconds,
            60'000);
    }

    void startDispatchesWithoutInlineModelMutation()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        std::atomic<QThread*> loaderThread = nullptr;
        RuntimeTaskLoader taskLoader =
            [&loaderCalls, &loaderThread](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                loaderThread.store(
                    QThread::currentThread());
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("one")),
                        }));
            };
        CodexRuntime runtime(
            state,
            bus,
            std::move(taskLoader),
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QThread* const ownerThread =
            QThread::currentThread();
        std::atomic<QThread*> modelSignalThread =
            nullptr;
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::rowsInserted,
            &runtime,
            [&modelSignalThread] {
                modelSignalThread.store(
                    QThread::currentThread());
            },
            Qt::DirectConnection);

        const Result<void> started = runtime.start();

        QVERIFY(started.hasValue());
        QVERIFY(runtime.running());
        QVERIFY(runtime.loading());
        QCOMPARE(loaderCalls.load(), 0);
        QCOMPARE(executor.pendingCount(), 1);
        QCOMPARE(state.tasks()->rowCount(), 0);

        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QCOMPARE(loaderCalls.load(), 1);
        QVERIFY(loaderThread.load() != ownerThread);
        QCOMPARE(state.tasks()->rowCount(), 1);
        const QVector<BridgeTask> expectedTasks{
            task(QStringLiteral("one")),
        };
        QCOMPARE(
            state.tasks()->snapshot(),
            expectedTasks);
        QCOMPARE(
            runtime.processSnapshot().tasks,
            state.tasks()->snapshot());
        QCOMPARE(modelSignalThread.load(), ownerThread);
    }

    void emptyInjectedBoundaryFailsBeforeSideEffects()
    {
        for (int missingBoundary = 0;
             missingBoundary < 4;
             ++missingBoundary) {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            RuntimeTaskLoader taskLoader =
                [](
                    const QHash<QString, BridgeGoal>&,
                    std::stop_token) {
                    return Result<CodexProcessSnapshot>::
                        success(snapshot());
                };
            RuntimeGoalLoader goalLoader =
                emptyGoalLoader();
            RuntimeExecutor runtimeExecutor =
                executor.executor();
            RuntimeNowProvider nowProvider =
                fixedNow();

            switch (missingBoundary) {
            case 0:
                taskLoader = {};
                break;
            case 1:
                goalLoader = {};
                break;
            case 2:
                runtimeExecutor = {};
                break;
            case 3:
                nowProvider = {};
                break;
            default:
                Q_UNREACHABLE();
            }

            CodexRuntime runtime(
                state,
                bus,
                std::move(taskLoader),
                std::move(goalLoader),
                std::move(runtimeExecutor),
                std::move(nowProvider),
                shortCadence());

            const Result<void> started =
                runtime.start();

            QVERIFY(!started.hasValue());
            QCOMPARE(
                started.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QCOMPARE(
                started.error().message,
                QStringLiteral(
                    "Codex runtime is unavailable."));
            QVERIFY(!started.error().retryable);
            QVERIFY(started.error().context.isEmpty());
            QVERIFY(!runtime.running());
            QVERIFY(!runtime.loading());
            QCOMPARE(
                runtime.errorCode(),
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QCOMPARE(
                runtime.errorMessage(),
                QStringLiteral(
                    "Codex runtime is unavailable."));
            QCOMPARE(executor.pendingCount(), 0);
            QCOMPARE(state.tasks()->rowCount(), 0);

            const Result<void> probeRegistration =
                bus.registerHandler(
                    QStringLiteral("codex.refresh"),
                    [](
                        const QVariantMap&,
                        CompanionCommandBus::Completion
                            completion) {
                        completion(
                            Result<void>::success());
                    });
            QVERIFY(probeRegistration.hasValue());
        }
    }

    void inlineOwnerExecutorNeverRunsLoader()
    {
        CompanionState state;
        CompanionCommandBus bus;
        std::atomic_int loaderCalls = 0;
        RuntimeTaskLoader taskLoader =
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            };
        RuntimeExecutor inlineExecutor =
            [](std::function<void()> worker) {
                worker();
            };
        CodexRuntime runtime(
            state,
            bus,
            std::move(taskLoader),
            emptyGoalLoader(),
            std::move(inlineExecutor),
            fixedNow(),
            shortCadence());

        const Result<void> started = runtime.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(loaderCalls.load(), 0);
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        const Result<void> probeRegistration =
            bus.registerHandler(
                QStringLiteral("codex.refresh"),
                [](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                        completion(
                            Result<void>::success());
                    });
        QVERIFY(!probeRegistration.hasValue());
        QCOMPARE(
            probeRegistration.error().code,
            QStringLiteral(
                "ui.command_already_registered"));
    }

    void boundStartupCommandSurvivesExecutorFailure()
    {
        CompanionState state;
        CompanionCommandBus bus;
        std::atomic_int loaderCalls = 0;
        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            [&bus](std::function<void()>) {
                bus.execute(
                    QStringLiteral("codex.refresh"));
                throw std::runtime_error(
                    "private executor detail");
            },
            fixedNow(),
            shortCadence());

        const Result<void> started = runtime.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(loaderCalls.load(), 0);
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCOMPARE(startedSpy.size(), 1);
        QVERIFY(finishedSpy.at(0).at(1).toBool());

        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(startedSpy.size(), 2);
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(1).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));

        const Result<void> probeRegistration =
            bus.registerHandler(
                QStringLiteral("codex.refresh"),
                [](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                        completion(
                            Result<void>::success());
                    });
        QVERIFY(!probeRegistration.hasValue());
        QCOMPARE(
            probeRegistration.error().code,
            QStringLiteral(
                "ui.command_already_registered"));
    }

    void startRejectsAffinityMismatchWithoutDispatch()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        QThread busThread;
        busThread.start();
        bus->moveToThread(&busThread);
        CodexRuntime runtime(
            state,
            *bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());

        const Result<void> started = runtime.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            started.error().message,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QVERIFY(!started.error().retryable);
        QVERIFY(started.error().context.isEmpty());
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
        QCOMPARE(executor.pendingCount(), 0);
        QCOMPARE(state.tasks()->rowCount(), 0);

        QVERIFY(
            QMetaObject::invokeMethod(
                bus,
                [bus] {
                    delete bus;
                },
                Qt::BlockingQueuedConnection));
        busThread.quit();
        QVERIFY(busThread.wait(1000));
    }

    void foreignThreadStartFailsWithoutOwnerMutation()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        std::atomic_int runningSignals = 0;
        std::atomic_int statusSignals = 0;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::runningChanged,
            &receiver,
            [&runningSignals] {
                runningSignals.fetch_add(1);
            },
            Qt::DirectConnection);
        QObject::connect(
            &runtime,
            &CodexRuntime::statusChanged,
            &receiver,
            [&statusSignals] {
                statusSignals.fetch_add(1);
            },
            Qt::DirectConnection);

        std::optional<Result<void>> result;
        std::thread caller(
            [&runtime, &result] {
                result.emplace(runtime.start());
            });
        caller.join();

        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            result->error().message,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QVERIFY(!result->error().retryable);
        QVERIFY(result->error().context.isEmpty());
        QCOMPARE(runningSignals.load(), 0);
        QCOMPARE(statusSignals.load(), 0);
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
        QVERIFY(runtime.errorCode().isEmpty());
        QVERIFY(runtime.errorMessage().isEmpty());
        QCOMPARE(executor.pendingCount(), 0);
    }

    void taskCompletionRejectsPostDispatchAffinityMismatch()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();

        bool completionObserved = false;
        bool runningAfterCompletion = false;
        bool loadingAfterCompletion = true;
        QString errorCodeAfterCompletion;
        QString errorMessageAfterCompletion;
        int rowCountAfterCompletion = -1;
        qsizetype processSignalCount = 0;
        bool movedBack = false;
        bool moveBackInvoked = false;
        bool threadStopped = false;
        {
            CodexRuntime runtime(
                state,
                *bus,
                [](
                    const QHash<QString, BridgeGoal>&,
                    std::stop_token) {
                    return Result<CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(QStringLiteral("late")),
                            }));
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                shortCadence());
            QSignalSpy processSpy(
                &runtime,
                &CodexRuntime::processSnapshotChanged);

            QVERIFY(runtime.start().hasValue());
            QCOMPARE(executor.pendingCount(), 1);
            bus->moveToThread(&busThread);
            QVERIFY(bus->thread() == &busThread);

            executor.runNext();
            completionObserved = waitUntil(
                [&runtime] {
                    return !runtime.loading();
                });
            runningAfterCompletion = runtime.running();
            loadingAfterCompletion = runtime.loading();
            errorCodeAfterCompletion =
                runtime.errorCode();
            errorMessageAfterCompletion =
                runtime.errorMessage();
            rowCountAfterCompletion =
                state.tasks()->rowCount();
            processSignalCount = processSpy.size();

            moveBackInvoked =
                QMetaObject::invokeMethod(
                    bus,
                    [bus, originalThread, &movedBack] {
                        bus->moveToThread(
                            originalThread);
                        movedBack = true;
                    },
                    Qt::BlockingQueuedConnection);
            busThread.quit();
            threadStopped = busThread.wait(1000);
            runtime.stop();
        }
        delete bus;

        QVERIFY(completionObserved);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!runningAfterCompletion);
        QVERIFY(!loadingAfterCompletion);
        QCOMPARE(
            errorCodeAfterCompletion,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            errorMessageAfterCompletion,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QCOMPARE(rowCountAfterCompletion, 0);
        QCOMPARE(processSignalCount, 0);
    }

    void goalCompletionRejectsPostDispatchAffinityMismatch()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();

        bool completionObserved = false;
        bool runningAfterCompletion = false;
        QString errorCodeAfterCompletion;
        QString errorMessageAfterCompletion;
        std::optional<BridgeGoal> goalAfterCompletion;
        qsizetype processSignalCount = 0;
        bool movedBack = false;
        bool moveBackInvoked = false;
        bool threadStopped = false;
        {
            CodexRuntime runtime(
                state,
                *bus,
                [](
                    const QHash<QString, BridgeGoal>&,
                    std::stop_token) {
                    return Result<CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(QStringLiteral("goal")),
                            }));
                },
                [](
                    const QVector<QString>&,
                    std::stop_token) {
                    return Result<
                        QHash<
                            QString,
                            std::optional<BridgeGoal>>>::
                        success({
                            {
                                QStringLiteral("goal"),
                                goal(QStringLiteral("goal")),
                            },
                        });
                },
                executor.executor(),
                fixedNow(),
                shortCadence());

            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil(
                [&runtime, &executor] {
                    return !runtime.loading()
                        && executor.pendingCount()
                            == 1;
                }));
            QCOMPARE(state.tasks()->rowCount(), 1);
            QSignalSpy processSpy(
                &runtime,
                &CodexRuntime::processSnapshotChanged);

            bus->moveToThread(&busThread);
            QVERIFY(bus->thread() == &busThread);
            executor.runNext();
            completionObserved = waitUntil(
                [&runtime, &state] {
                    return !runtime.running()
                        || state.tasks()
                               ->snapshot()
                               .first()
                               .goal
                               .has_value();
                });
            runningAfterCompletion = runtime.running();
            errorCodeAfterCompletion =
                runtime.errorCode();
            errorMessageAfterCompletion =
                runtime.errorMessage();
            goalAfterCompletion =
                state.tasks()
                    ->snapshot()
                    .first()
                    .goal;
            processSignalCount = processSpy.size();

            moveBackInvoked =
                QMetaObject::invokeMethod(
                    bus,
                    [bus, originalThread, &movedBack] {
                        bus->moveToThread(
                            originalThread);
                        movedBack = true;
                    },
                    Qt::BlockingQueuedConnection);
            busThread.quit();
            threadStopped = busThread.wait(1000);
            runtime.stop();
        }
        delete bus;

        QVERIFY(completionObserved);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!runningAfterCompletion);
        QCOMPARE(
            errorCodeAfterCompletion,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            errorMessageAfterCompletion,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QVERIFY(!goalAfterCompletion.has_value());
        QCOMPARE(processSignalCount, 0);
    }

    void runningChangedReentrantStopDoesNotReportSuccessfulStart()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        bool stopRequested = false;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::runningChanged,
            &receiver,
            [&runtime, &stopRequested] {
                if (runtime.running()
                    && !stopRequested) {
                    stopRequested = true;
                    runtime.stop();
                }
            },
            Qt::DirectConnection);

        const Result<void> started = runtime.start();
        const qsizetype pendingAfterStart =
            executor.pendingCount();
        const bool runningAfterStart =
            runtime.running();
        const bool loadingAfterStart =
            runtime.loading();
        runtime.stop();

        QVERIFY(stopRequested);
        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!runningAfterStart);
        QVERIFY(!loadingAfterStart);
        QVERIFY(pendingAfterStart <= 1);
    }

    void statusChangedReentrantRefreshCoalescesOneDispatch()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                const int call =
                    loaderCalls.fetch_add(1);
                if (call == 0) {
                    return Result<CodexProcessSnapshot>::
                        failure(
                            privateFailure(
                                QStringLiteral(
                                    "private.failure")));
                }
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("fresh")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil(
            [&runtime] {
                return !runtime.loading();
            }));
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral("codex.refresh_failed"));

        bool refreshRequested = false;
        bool loadingWhenCleared = false;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::statusChanged,
            &receiver,
            [&runtime,
             &refreshRequested,
             &loadingWhenCleared] {
                if (runtime.errorCode().isEmpty()
                    && !refreshRequested) {
                    loadingWhenCleared =
                        runtime.loading();
                    refreshRequested = true;
                    runtime.refreshNow();
                }
            },
            Qt::DirectConnection);

        runtime.refreshNow();
        const qsizetype pendingAfterRefresh =
            executor.pendingCount();
        runtime.stop();

        QVERIFY(refreshRequested);
        QVERIFY(loadingWhenCleared);
        QCOMPARE(pendingAfterRefresh, 1);
    }

    void loadingChangedReentrantStopCannotLaunchUncanceledWork()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        std::atomic_bool loaderObservedStop = false;
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls, &loaderObservedStop](
                const QHash<QString, BridgeGoal>&,
                std::stop_token stopToken) {
                loaderCalls.fetch_add(1);
                loaderObservedStop.store(
                    stopToken.stop_requested());
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        bool stopRequested = false;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::loadingChanged,
            &receiver,
            [&runtime, &stopRequested] {
                if (runtime.loading()
                    && !stopRequested) {
                    stopRequested = true;
                    runtime.stop();
                }
            },
            Qt::DirectConnection);

        const Result<void> started = runtime.start();
        const qsizetype pendingAfterStart =
            executor.pendingCount();
        if (pendingAfterStart == 1) {
            executor.runNext();
            QCoreApplication::processEvents();
        }
        const int finalLoaderCalls =
            loaderCalls.load();
        const bool finalObservedStop =
            loaderObservedStop.load();
        const bool runningAfterStart =
            runtime.running();
        const bool loadingAfterStart =
            runtime.loading();
        runtime.stop();

        QVERIFY(stopRequested);
        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!runningAfterStart);
        QVERIFY(!loadingAfterStart);
        QVERIFY(pendingAfterStart <= 1);
        if (finalLoaderCalls > 0) {
            QVERIFY(finalObservedStop);
        }
    }

    void loadingChangedReentrantStopPreventsImmediateDispatch()
    {
        CompanionState state;
        CompanionCommandBus bus;
        std::atomic_int executorCalls = 0;
        std::atomic_int loaderCalls = 0;
        RuntimeExecutor immediateThreadExecutor =
            [&executorCalls](
                std::function<void()> worker) {
                executorCalls.fetch_add(1);
                std::thread thread(std::move(worker));
                thread.join();
            };
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            std::move(immediateThreadExecutor),
            fixedNow(),
            shortCadence());
        bool stopRequested = false;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::loadingChanged,
            &receiver,
            [&runtime, &stopRequested] {
                if (runtime.loading()
                    && !stopRequested) {
                    stopRequested = true;
                    runtime.stop();
                }
            },
            Qt::DirectConnection);

        const Result<void> started = runtime.start();

        QVERIFY(stopRequested);
        QVERIFY(!started.hasValue());
        QCOMPARE(executorCalls.load(), 0);
        QCOMPARE(loaderCalls.load(), 0);
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
    }

    void runningChangedReentrantAffinityMismatchStopsBeforeDispatch()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();

        bool moved = false;
        bool moveBackInvoked = false;
        bool movedBack = false;
        bool threadStopped = false;
        Result<void> started =
            Result<void>::failure(privateFailure(
                QStringLiteral("not-started")));
        bool runningAfterStart = true;
        bool loadingAfterStart = true;
        QString errorCodeAfterStart;
        qsizetype pendingAfterStart = -1;
        qsizetype stoppedCommandCount = 0;
        QString stoppedCommandError;
        Result<void> probeRegistration =
            Result<void>::success();
        {
            CodexRuntime runtime(
                state,
                *bus,
                [](
                    const QHash<QString, BridgeGoal>&,
                    std::stop_token) {
                    return Result<CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                shortCadence());
            QObject receiver;
            QObject::connect(
                &runtime,
                &CodexRuntime::runningChanged,
                &receiver,
                [&runtime,
                 bus,
                 &busThread,
                 &moved] {
                    if (runtime.running()
                        && !moved) {
                        bus->moveToThread(&busThread);
                        moved =
                            bus->thread()
                            == &busThread;
                    }
                },
                Qt::DirectConnection);

            started = runtime.start();
            runningAfterStart = runtime.running();
            loadingAfterStart = runtime.loading();
            errorCodeAfterStart =
                runtime.errorCode();
            pendingAfterStart =
                executor.pendingCount();
            runtime.stop();

            moveBackInvoked =
                QMetaObject::invokeMethod(
                    bus,
                    [bus, originalThread, &movedBack] {
                        bus->moveToThread(
                            originalThread);
                        movedBack = true;
                    },
                    Qt::BlockingQueuedConnection);
            busThread.quit();
            threadStopped = busThread.wait(1000);
        }
        QSignalSpy finishedSpy(
            bus,
            &CompanionCommandBus::commandFinished);
        bus->execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        stoppedCommandCount = finishedSpy.size();
        stoppedCommandError =
            finishedSpy.at(0).at(2).toString();
        probeRegistration =
            bus->registerHandler(
                QStringLiteral("codex.refresh"),
                [](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    completion(
                        Result<void>::success());
                });
        delete bus;

        QVERIFY(moved);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QVERIFY(!runningAfterStart);
        QVERIFY(!loadingAfterStart);
        QCOMPARE(
            errorCodeAfterStart,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(pendingAfterStart, 0);
        QCOMPARE(stoppedCommandCount, 1);
        QCOMPARE(
            stoppedCommandError,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!probeRegistration.hasValue());
        QCOMPARE(
            probeRegistration.error().code,
            QStringLiteral(
                "ui.command_already_registered"));
    }

    void failedStartupLoadingFinishObservesStoppedRuntime()
    {
        CompanionState state;
        CompanionCommandBus bus;
        RuntimeExecutor inlineExecutor =
            [](std::function<void()> worker) {
                worker();
            };
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            std::move(inlineExecutor),
            fixedNow(),
            shortCadence());
        bool finishObserved = false;
        bool runningAtFinish = true;
        QString errorCodeAtFinish;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::loadingChanged,
            &receiver,
            [&runtime,
             &finishObserved,
             &runningAtFinish,
             &errorCodeAtFinish] {
                if (!runtime.loading()) {
                    finishObserved = true;
                    runningAtFinish =
                        runtime.running();
                    errorCodeAtFinish =
                        runtime.errorCode();
                }
            },
            Qt::DirectConnection);

        const Result<void> started = runtime.start();

        QVERIFY(!started.hasValue());
        QVERIFY(finishObserved);
        QVERIFY(!runningAtFinish);
        QCOMPARE(
            errorCodeAtFinish,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
    }

    void loadingFinishedSignalObservesCommittedSnapshot()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("committed")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVector<BridgeTask> runtimeTasksAtFinish;
        QVector<BridgeTask> modelTasksAtFinish;
        QObject receiver;
        QObject::connect(
            &runtime,
            &CodexRuntime::loadingChanged,
            &receiver,
            [&runtime,
             &state,
             &runtimeTasksAtFinish,
             &modelTasksAtFinish] {
                if (!runtime.loading()) {
                    runtimeTasksAtFinish =
                        runtime.processSnapshot().tasks;
                    modelTasksAtFinish =
                        state.tasks()->snapshot();
                }
            },
            Qt::DirectConnection);

        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil(
            [&runtime] {
                return !runtime.loading();
            }));
        runtime.stop();

        const QVector<BridgeTask> expected{
            task(QStringLiteral("committed")),
        };
        QCOMPARE(runtimeTasksAtFinish, expected);
        QCOMPARE(modelTasksAtFinish, expected);
    }

    void goalDispatchObservesTaskLoadingFinished()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime* runtimePointer = nullptr;
        int submissionCount = 0;
        bool loadingAtGoalDispatch = true;
        RuntimeExecutor observingExecutor =
            [&executor,
             &runtimePointer,
             &submissionCount,
             &loadingAtGoalDispatch](
                std::function<void()> worker) {
                ++submissionCount;
                if (submissionCount == 2) {
                    QVERIFY(runtimePointer != nullptr);
                    loadingAtGoalDispatch =
                        runtimePointer->loading();
                }
                executor.submit(std::move(worker));
            };
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("goal")),
                        }));
            },
            emptyGoalLoader(),
            std::move(observingExecutor),
            fixedNow(),
            shortCadence());
        runtimePointer = &runtime;

        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil(
            [&runtime, &executor] {
                return !runtime.loading()
                    && executor.pendingCount() == 1;
            }));

        QCOMPARE(submissionCount, 2);
        QVERIFY(!loadingAtGoalDispatch);
        executor.runNext();
        QCoreApplication::processEvents();
        runtime.stop();
    }

    void activeRefreshCoalescesExactlyOneFollowUp()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 1);

        runtime.refreshNow();
        runtime.refreshNow();
        runtime.refreshNow();

        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        QCOMPARE(loaderCalls.load(), 1);

        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        QCOMPARE(loaderCalls.load(), 2);
        QCOMPARE(executor.pendingCount(), 0);
    }

    void timerRefreshRejectsAffinityMismatchBeforeCoalescing_data()
    {
        QTest::addColumn<int>("timerKind");

        QTest::newRow("passive") << 0;
        QTest::newRow("active") << 1;
        QTest::newRow("settle") << 2;
    }

    void timerRefreshRejectsAffinityMismatchBeforeCoalescing()
    {
        QFETCH(int, timerKind);

        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        MutableClock clock;
        CodexRuntimeCadence cadence{
            5'000,
            5'000,
            5'000,
            8,
            5'000,
        };
        if (timerKind == 0) {
            cadence.passiveRefreshMilliseconds = 40;
        } else if (timerKind == 1) {
            cadence.activeRefreshMilliseconds = 40;
        } else {
            cadence.settleRefreshMilliseconds = 40;
        }

        auto runtime = std::make_unique<CodexRuntime>(
            state,
            *bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("timer")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            clock.provider(),
            cadence);

        const Result<void> started = runtime->start();
        if (started.hasValue()) {
            executor.runNext();
            waitUntil([&executor] {
                return executor.pendingCount() == 1;
            });
            executor.runNext();
            waitUntil([&runtime] {
                return !runtime->loading();
            });
            clock.advanceMilliseconds(
                cadence.taskStaleMilliseconds);
            runtime->refreshNow();
            if (timerKind != 0) {
                runtime->setProcessSurfaceVisible(true);
            }
        }
        const bool loadingBeforeTimer =
            runtime->loading();
        const qsizetype pendingBeforeTimer =
            executor.pendingCount();

        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();
        if (started.hasValue()) {
            bus->moveToThread(&busThread);
        }
        const bool moved =
            bus->thread() == &busThread;
        const bool stoppedForMismatch =
            waitUntil(
                [&runtime] {
                    return !runtime->running();
                },
                1000);
        const QString errorCode =
            runtime->errorCode();
        const QString errorMessage =
            runtime->errorMessage();
        const bool loadingAfterTimer =
            runtime->loading();

        bool movedBack = false;
        const bool moveBackInvoked =
            moved
            && QMetaObject::invokeMethod(
                bus,
                [bus,
                 originalThread,
                 &movedBack] {
                    bus->moveToThread(
                        originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        busThread.quit();
        const bool threadStopped =
            busThread.wait(1000);
        if (runtime->running()) {
            runtime->stop();
        }
        runtime.reset();
        delete bus;

        QVERIFY(started.hasValue());
        QVERIFY(loadingBeforeTimer);
        QCOMPARE(pendingBeforeTimer, 1);
        QVERIFY(moved);
        QVERIFY(stoppedForMismatch);
        QVERIFY(!loadingAfterTimer);
        QCOMPARE(
            errorCode,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            errorMessage,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
    }

    void stopRequestsActiveTokenAndIgnoresLateSuccess()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_bool observedStop = false;
        CodexRuntime runtime(
            state,
            bus,
            [&observedStop](
                const QHash<QString, BridgeGoal>&,
                std::stop_token stopToken) {
                observedStop.store(
                    stopToken.stop_requested());
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("late")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 1);

        runtime.stop();

        QVERIFY(!runtime.running());
        QVERIFY(!runtime.loading());
        executor.runNext();
        QCoreApplication::processEvents();
        QTest::qWait(10);

        QVERIFY(observedStop.load());
        QCOMPARE(state.tasks()->rowCount(), 0);
        QVERIFY(runtime.processSnapshot().tasks.isEmpty());
        QVERIFY(runtime.errorCode().isEmpty());
        QVERIFY(runtime.errorMessage().isEmpty());
    }

    void duplicateExecutorInvocationRunsOneLoader()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor callbacks;
        std::atomic_int loaderCalls = 0;
        RuntimeExecutor duplicateExecutor =
            [&callbacks](std::function<void()> worker) {
                callbacks.submit(worker);
                callbacks.submit(std::move(worker));
            };
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("one")),
                        }));
            },
            emptyGoalLoader(),
            std::move(duplicateExecutor),
            fixedNow(),
            shortCadence());
        QSignalSpy snapshotSpy(
            &runtime,
            &CodexRuntime::processSnapshotChanged);

        QVERIFY(runtime.start().hasValue());
        QCOMPARE(callbacks.pendingCount(), 2);
        callbacks.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        callbacks.runNext();
        QCoreApplication::processEvents();

        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(snapshotSpy.size(), 1);
        QCOMPARE(state.tasks()->rowCount(), 1);
    }

    void dispatchClaimBeforeExecutorThrowWinsOnce()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor initialExecutor;
        ManualGate secondLoaderEntered;
        ManualGate releaseSecondLoader;
        std::optional<std::thread> acceptedThread;
        std::atomic_int executorCalls = 0;
        std::atomic_int loaderCalls = 0;
        RuntimeExecutor racingExecutor =
            [&initialExecutor,
             &secondLoaderEntered,
             &acceptedThread,
             &executorCalls](
                std::function<void()> worker) {
                const int call =
                    executorCalls.fetch_add(1);
                if (call == 0) {
                    initialExecutor.submit(
                        std::move(worker));
                    return;
                }
                acceptedThread.emplace(
                    std::move(worker));
                secondLoaderEntered.wait();
                throw std::runtime_error(
                    "executor throws after dispatch");
            };
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls,
             &secondLoaderEntered,
             &releaseSecondLoader](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                const int call =
                    loaderCalls.fetch_add(1);
                if (call == 1) {
                    secondLoaderEntered.release();
                    releaseSecondLoader.wait();
                    return Result<CodexProcessSnapshot>::
                        success(snapshot());
                }
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            std::move(racingExecutor),
            fixedNow(),
            shortCadence());
        QSignalSpy snapshotSpy(
            &runtime,
            &CodexRuntime::processSnapshotChanged);

        QVERIFY(runtime.start().hasValue());
        initialExecutor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        runtime.refreshNow();
        QVERIFY(acceptedThread.has_value());
        QVERIFY(runtime.loading());
        QVERIFY(runtime.errorCode().isEmpty());
        releaseSecondLoader.release();
        acceptedThread->join();
        acceptedThread.reset();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QCOMPARE(loaderCalls.load(), 2);
        QCOMPARE(snapshotSpy.size(), 2);
        QCOMPARE(state.tasks()->rowCount(), 0);
        QVERIFY(runtime.errorCode().isEmpty());
    }

    void executorThrowBeforeLateDispatchWinsOnce()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor initialExecutor;
        std::optional<std::function<void()>> lateWorker;
        std::atomic_int executorCalls = 0;
        std::atomic_int loaderCalls = 0;
        RuntimeExecutor throwingExecutor =
            [&initialExecutor,
             &lateWorker,
             &executorCalls](
                std::function<void()> worker) {
                const int call =
                    executorCalls.fetch_add(1);
                if (call == 0) {
                    initialExecutor.submit(
                        std::move(worker));
                    return;
                }
                lateWorker = std::move(worker);
                throw std::runtime_error(
                    "executor wins before dispatch");
            };
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            std::move(throwingExecutor),
            fixedNow(),
            shortCadence());

        QVERIFY(runtime.start().hasValue());
        initialExecutor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        runtime.refreshNow();

        QVERIFY(lateWorker.has_value());
        QVERIFY(!runtime.loading());
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral("codex.refresh_failed"));
        QCOMPARE(loaderCalls.load(), 1);

        std::thread late(
            std::move(*lateWorker));
        late.join();
        lateWorker.reset();
        QCoreApplication::processEvents();

        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral("codex.refresh_failed"));
    }

    void restartIgnoresOlderGenerationCompletion()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                const int call =
                    loaderCalls.fetch_add(1);
                const QString id = call == 0
                    ? QStringLiteral("old")
                    : QStringLiteral("new");
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(id),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());

        QVERIFY(runtime.start().hasValue());
        runtime.stop();
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 2);

        executor.runAt(0);
        QCoreApplication::processEvents();
        QCOMPARE(state.tasks()->rowCount(), 0);

        executor.runAt(0);
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QCOMPARE(loaderCalls.load(), 2);
        QCOMPARE(state.tasks()->rowCount(), 1);
        QCOMPARE(
            state.tasks()->snapshot().first().id,
            QStringLiteral("new"));
    }

    void foreignThreadPublicMethodsMarshalOnce()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                goalCalls.fetch_add(1);
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success({});
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&goalCalls] {
            return goalCalls.load() == 1;
        }));
        clock.advanceMilliseconds(500);

        std::thread visibilityCaller(
            [&runtime] {
                runtime.setProcessSurfaceVisible(
                    true);
            });
        visibilityCaller.join();
        QCOMPARE(executor.pendingCount(), 0);
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        executor.runNext();
        QVERIFY(waitUntil([&goalCalls] {
            return goalCalls.load() == 2;
        }));

        std::thread refreshCaller(
            [&runtime] {
                runtime.refreshNow();
            });
        refreshCaller.join();
        QCOMPARE(executor.pendingCount(), 0);
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        std::thread stopCaller(
            [&runtime] {
                runtime.stop();
            });
        stopCaller.join();
        QVERIFY(runtime.running());
        QVERIFY(waitUntil([&runtime] {
            return !runtime.running();
        }));
    }

    void foreignThreadRefreshCommandAndBusDestructionAreSafe()
    {
        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            CodexRuntime runtime(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                shortCadence());
            QThread* const ownerThread =
                QThread::currentThread();
            std::atomic<QThread*> startedThread =
                nullptr;
            QSignalSpy finishedSpy(
                &bus,
                &CompanionCommandBus::commandFinished);
            QObject::connect(
                &bus,
                &CompanionCommandBus::commandStarted,
                &runtime,
                [&startedThread] {
                    startedThread.store(
                        QThread::currentThread());
                },
                Qt::DirectConnection);
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return !runtime.loading();
            }));

            std::thread caller(
                [&bus] {
                    bus.execute(
                        QStringLiteral(
                            "codex.refresh"));
                });
            caller.join();
            QCOMPARE(executor.pendingCount(), 0);
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                1,
                1000);

            QCOMPARE(
                startedThread.load(),
                ownerThread);
            QVERIFY(finishedSpy.first().at(1).toBool());
            QCOMPARE(executor.pendingCount(), 1);
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return !runtime.loading();
            }));
        }

        {
            CompanionState state;
            auto bus =
                std::make_unique<CompanionCommandBus>();
            ManualExecutor executor;
            auto runtime =
                std::make_unique<CodexRuntime>(
                    state,
                    *bus,
                    [](
                        const QHash<
                            QString,
                            BridgeGoal>&,
                        std::stop_token) {
                        return Result<
                            CodexProcessSnapshot>::
                            success(snapshot());
                    },
                    emptyGoalLoader(),
                    executor.executor(),
                    fixedNow(),
                    shortCadence());
            QVERIFY(runtime->start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return !runtime->loading();
            }));

            CompanionCommandBus* const rawBus =
                bus.get();
            std::thread caller(
                [rawBus] {
                    rawBus->execute(
                        QStringLiteral(
                            "codex.refresh"));
                });
            caller.join();
            QCOMPARE(executor.pendingCount(), 0);
            bus.reset();
            QCoreApplication::processEvents();
            QCOMPARE(executor.pendingCount(), 0);
            runtime.reset();
        }
    }

    void refreshCommandValidatesAndRecoversAfterRestart()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);

        QVERIFY(runtime.start().hasValue());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        const QString secret =
            QStringLiteral("private-command-argument");
        bus.execute(
            QStringLiteral("codex.refresh"),
            {
                {
                    QStringLiteral("secret"),
                    secret,
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(1).at(2).toString(),
            QStringLiteral(
                "codex.command_invalid_arguments"));
        QVERIFY(
            !finishedSpy.at(1)
                 .at(3)
                 .toString()
                 .contains(secret));
        QCOMPARE(executor.pendingCount(), 0);

        runtime.stop();
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            1000);
        QVERIFY(!finishedSpy.at(2).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(2).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(
            finishedSpy.at(2).at(3).toString(),
            QStringLiteral(
                "Codex runtime is unavailable."));

        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 1);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            4,
            1000);
        QVERIFY(finishedSpy.at(3).at(1).toBool());
        QCOMPARE(executor.pendingCount(), 1);
    }

    void refreshCommandAffinityMoveReportsFailure()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        CodexRuntime runtime(
            state,
            *bus,
            [&loaderCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        QCOMPARE(loaderCalls.load(), 1);

        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();
        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> finishedThread = nullptr;
        std::mutex resultMutex;
        bool succeeded = true;
        QString errorCode;
        QString errorMessage;
        bool moved = false;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [bus,
             &busThread,
             &moved](
                const QString& command) {
                if (command
                    != QStringLiteral(
                        "codex.refresh")) {
                    return;
                }
                bus->moveToThread(&busThread);
                moved =
                    bus->thread() == &busThread;
            },
            Qt::DirectConnection);
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount,
             &finishedThread,
             &resultMutex,
             &succeeded,
             &errorCode,
             &errorMessage](
                const QString&,
                bool commandSucceeded,
                const QString& commandErrorCode,
                const QString& commandErrorMessage) {
                {
                    const std::scoped_lock lock(
                        resultMutex);
                    succeeded = commandSucceeded;
                    errorCode = commandErrorCode;
                    errorMessage =
                        commandErrorMessage;
                }
                finishedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);

        bus->execute(QStringLiteral("codex.refresh"));
        const bool completed =
            waitUntil(
                [&finishedCount,
                 &runtime] {
                    return finishedCount.load() == 1
                        && !runtime.running();
                },
                1000);

        bool movedBack = false;
        const bool moveBackInvoked =
            QMetaObject::invokeMethod(
                bus,
                [bus,
                 originalThread,
                 &movedBack] {
                    bus->moveToThread(
                        originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        busThread.quit();
        const bool threadStopped =
            busThread.wait(1000);
        delete bus;

        bool finalSucceeded = true;
        QString finalErrorCode;
        QString finalErrorMessage;
        {
            const std::scoped_lock lock(
                resultMutex);
            finalSucceeded = succeeded;
            finalErrorCode = errorCode;
            finalErrorMessage = errorMessage;
        }

        QVERIFY(moved);
        QVERIFY(completed);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!finalSucceeded);
        QCOMPARE(
            finalErrorCode,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            finalErrorMessage,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            finishedThread.load(),
            &busThread);
        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(executor.pendingCount(), 0);
    }

    void activeRefreshCommandAffinityMoveReportsFailure()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        std::atomic_int loaderCalls = 0;
        std::atomic_bool observedStop = false;
        CodexRuntime runtime(
            state,
            *bus,
            [&loaderCalls,
             &observedStop](
                const QHash<QString, BridgeGoal>&,
                std::stop_token stopToken) {
                loaderCalls.fetch_add(1);
                observedStop.store(
                    stopToken.stop_requested());
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        QVERIFY(runtime.loading());
        QCOMPARE(executor.pendingCount(), 1);

        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();
        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> finishedThread = nullptr;
        std::mutex resultMutex;
        bool succeeded = true;
        QString errorCode;
        QString errorMessage;
        bool moved = false;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [bus,
             &busThread,
             &moved](
                const QString& command) {
                if (command
                    != QStringLiteral(
                        "codex.refresh")) {
                    return;
                }
                bus->moveToThread(&busThread);
                moved =
                    bus->thread() == &busThread;
            },
            Qt::DirectConnection);
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount,
             &finishedThread,
             &resultMutex,
             &succeeded,
             &errorCode,
             &errorMessage](
                const QString&,
                bool commandSucceeded,
                const QString& commandErrorCode,
                const QString& commandErrorMessage) {
                {
                    const std::scoped_lock lock(
                        resultMutex);
                    succeeded = commandSucceeded;
                    errorCode = commandErrorCode;
                    errorMessage =
                        commandErrorMessage;
                }
                finishedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);

        bus->execute(QStringLiteral("codex.refresh"));
        const bool completed =
            waitUntil(
                [&finishedCount,
                 &runtime] {
                    return finishedCount.load() == 1
                        && !runtime.running();
                },
                1000);

        bool movedBack = false;
        const bool moveBackInvoked =
            QMetaObject::invokeMethod(
                bus,
                [bus,
                 originalThread,
                 &movedBack] {
                    bus->moveToThread(
                        originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        executor.runNext();
        QCoreApplication::processEvents();
        busThread.quit();
        const bool threadStopped =
            busThread.wait(1000);
        delete bus;

        bool finalSucceeded = true;
        QString finalErrorCode;
        QString finalErrorMessage;
        {
            const std::scoped_lock lock(
                resultMutex);
            finalSucceeded = succeeded;
            finalErrorCode = errorCode;
            finalErrorMessage = errorMessage;
        }

        QVERIFY(moved);
        QVERIFY(completed);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!finalSucceeded);
        QCOMPARE(
            finalErrorCode,
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            finalErrorMessage,
            QStringLiteral(
                "Codex runtime components must share one thread."));
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral(
                "codex.runtime_thread_mismatch"));
        QCOMPARE(
            finishedThread.load(),
            &busThread);
        QCOMPARE(loaderCalls.load(), 1);
        QVERIFY(observedStop.load());
        QCOMPARE(executor.pendingCount(), 0);
    }

    void retainedCommandHandlerIsUnavailableAfterDestruction()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        auto runtime = std::make_unique<CodexRuntime>(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        QVERIFY(runtime->start().hasValue());

        runtime.reset();
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);

        QVERIFY(!finishedSpy.first().at(1).toBool());
        QCOMPARE(
            finishedSpy.first().at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        executor.runNext();
        QCoreApplication::processEvents();
        QCOMPARE(state.tasks()->rowCount(), 0);
    }

    void queuedRefreshInvocationCompletesWhenRuntimeDestroyed()
    {
        CompanionState state;
        auto* bus = new CompanionCommandBus();
        ManualExecutor executor;
        auto runtime = std::make_unique<CodexRuntime>(
            state,
            *bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime->start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime->loading();
        }));

        QThread busThread;
        QThread* const originalThread =
            QThread::currentThread();
        busThread.start();
        bus->moveToThread(&busThread);

        std::atomic_int startedCount = 0;
        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> finishedThread = nullptr;
        std::mutex resultMutex;
        bool succeeded = true;
        QString errorCode;
        QString errorMessage;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [&startedCount](
                const QString& command) {
                if (command
                    == QStringLiteral(
                        "codex.refresh")) {
                    startedCount.fetch_add(1);
                }
            },
            Qt::DirectConnection);
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount,
             &finishedThread,
             &resultMutex,
             &succeeded,
             &errorCode,
             &errorMessage](
                const QString&,
                bool commandSucceeded,
                const QString& commandErrorCode,
                const QString& commandErrorMessage) {
                {
                    const std::scoped_lock lock(
                        resultMutex);
                    succeeded = commandSucceeded;
                    errorCode = commandErrorCode;
                    errorMessage =
                        commandErrorMessage;
                }
                finishedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);

        const bool invoked =
            QMetaObject::invokeMethod(
                bus,
                [bus] {
                    bus->execute(
                        QStringLiteral(
                            "codex.refresh"));
                },
                Qt::BlockingQueuedConnection);
        const int startedBeforeDestruction =
            startedCount.load();
        const int finishedBeforeDestruction =
            finishedCount.load();

        runtime.reset();
        const bool completed =
            waitUntil(
                [&finishedCount] {
                    return finishedCount.load() == 1;
                },
                1000);

        bool movedBack = false;
        const bool moveBackInvoked =
            QMetaObject::invokeMethod(
                bus,
                [bus,
                 originalThread,
                 &movedBack] {
                    bus->moveToThread(
                        originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        busThread.quit();
        const bool threadStopped =
            busThread.wait(1000);
        delete bus;

        bool finalSucceeded = true;
        QString finalErrorCode;
        QString finalErrorMessage;
        {
            const std::scoped_lock lock(
                resultMutex);
            finalSucceeded = succeeded;
            finalErrorCode = errorCode;
            finalErrorMessage = errorMessage;
        }

        QVERIFY(invoked);
        QCOMPARE(startedBeforeDestruction, 1);
        QCOMPARE(finishedBeforeDestruction, 0);
        QVERIFY(completed);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QVERIFY(!finalSucceeded);
        QCOMPARE(
            finalErrorCode,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(
            finalErrorMessage,
            QStringLiteral(
                "Codex runtime is unavailable."));
        QCOMPARE(
            finishedThread.load(),
            &busThread);
    }

    void coldStartPersistedGoalPublishesBeforeAuthoritativeRefresh()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int taskCalls = 0;
        QVector<QString> capturedIds;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                CodexProcessSnapshot result;
                result.goalCandidateThreadIds.append(
                    QStringLiteral("stale-goal"));
                BridgeTask item =
                    task(QStringLiteral("stale-goal"));
                item.goal =
                    goal(QStringLiteral("stale-goal"));
                result.tasks.append(
                    std::move(item));
                taskCalls.fetch_add(1);
                return Result<CodexProcessSnapshot>::
                    success(std::move(result));
            },
            [&capturedIds](
                const QVector<QString>& threadIds,
                std::stop_token) {
                capturedIds = threadIds;
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("stale-goal"),
                    goal(QStringLiteral("stale-goal")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>
                    ::success(std::move(goals));
            },
            executor.executor(),
            fixedNow(),
            shortCadence());

        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        QCOMPARE(state.tasks()->rowCount(), 1);
        QVERIFY(
            state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value());

        executor.runNext();
        QCoreApplication::processEvents();

        QCOMPARE(
            capturedIds,
            QVector<QString>({
                QStringLiteral("stale-goal"),
            }));
        QCOMPARE(taskCalls.load(), 1);
        QVERIFY(
            state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value());
    }

    void goalRefreshRecomputesStatusThenReappliesRuntime()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                CodexProcessSnapshot result;
                result.tasks = {
                    task(QStringLiteral("goal-paused")),
                    task(QStringLiteral("runtime-active")),
                    task(QStringLiteral("runtime-approval")),
                };
                result.tasks[1].status =
                    TaskStatus::Completed;
                result.runtimeStatuses.insert(
                    QStringLiteral("runtime-active"),
                    ThreadRuntimeStatus::Active);
                result.runtimeStatuses.insert(
                    QStringLiteral("runtime-approval"),
                    ThreadRuntimeStatus::
                        WaitingOnApproval);
                return Result<CodexProcessSnapshot>::
                    success(std::move(result));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                BridgeGoal paused =
                    goal(QStringLiteral("goal-paused"));
                paused.status = GoalStatus::Paused;
                goals.insert(paused.threadId, paused);
                BridgeGoal runtimeActive =
                    goal(QStringLiteral("runtime-active"));
                runtimeActive.status =
                    GoalStatus::Paused;
                goals.insert(
                    runtimeActive.threadId,
                    runtimeActive);
                goals.insert(
                    QStringLiteral("runtime-approval"),
                    goal(
                        QStringLiteral(
                            "runtime-approval")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>
                    ::success(std::move(goals));
            },
            executor.executor(),
            fixedNow(),
            shortCadence());

        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            const QVector<BridgeTask> tasks =
                state.tasks()->snapshot();
            return tasks.size() == 3
                && tasks.at(0).goal.has_value()
                && tasks.at(1).goal.has_value()
                && tasks.at(2).goal.has_value();
        }));

        const QVector<BridgeTask> tasks =
            state.tasks()->snapshot();
        QCOMPARE(
            tasks.at(0).status,
            TaskStatus::Waiting);
        QCOMPARE(
            tasks.at(1).status,
            TaskStatus::Running);
        QCOMPARE(
            tasks.at(2).status,
            TaskStatus::Waiting);
        QVERIFY(tasks.at(2).needsApproval);
        QCOMPARE(
            tasks.at(2).preview,
            QStringLiteral(
                "This task is waiting for your approval."));
    }

    void goalSuccessUpdatesRowsInPlaceAndUsesSortedIds()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        QVector<QString> capturedIds;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("b")),
                            task(QStringLiteral("a")),
                        }));
            },
            [&capturedIds](
                const QVector<QString>& threadIds,
                std::stop_token) {
                capturedIds = threadIds;
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                goals.insert(
                    QStringLiteral("b"),
                    goal(QStringLiteral("b")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        const QStringList beforeIds =
            modelIds(*state.tasks());
        int inserts = 0;
        int removals = 0;
        int moves = 0;
        int resets = 0;
        QVector<QList<int>> changedRoles;
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::rowsInserted,
            &runtime,
            [&inserts](
                const QModelIndex&,
                int first,
                int last) {
                inserts += last - first + 1;
            });
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::rowsRemoved,
            &runtime,
            [&removals](
                const QModelIndex&,
                int first,
                int last) {
                removals += last - first + 1;
            });
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::rowsMoved,
            &runtime,
            [&moves](
                const QModelIndex&,
                int first,
                int last,
                const QModelIndex&,
                int) {
                moves += last - first + 1;
            });
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::modelReset,
            &runtime,
            [&resets] {
                ++resets;
            });
        QObject::connect(
            state.tasks(),
            &QAbstractItemModel::dataChanged,
            &runtime,
            [&changedRoles](
                const QModelIndex&,
                const QModelIndex&,
                const QList<int>& roles) {
                changedRoles.append(roles);
            });

        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .at(0)
                .goal
                .has_value();
        }));

        const QVector<QString> expectedIds{
            QStringLiteral("a"),
            QStringLiteral("b"),
        };
        QCOMPARE(capturedIds, expectedIds);
        QCOMPARE(modelIds(*state.tasks()), beforeIds);
        QCOMPARE(inserts, 0);
        QCOMPARE(removals, 0);
        QCOMPARE(moves, 0);
        QCOMPARE(resets, 0);
        QCOMPARE(changedRoles.size(), 2);
        const QList<int> expectedGoalRoles{
            TaskListModel::GoalRole,
        };
        for (const QList<int>& roles : changedRoles) {
            QCOMPARE(roles, expectedGoalRoles);
        }
        QCOMPARE(
            state.tasks()
                ->snapshot()
                .at(0)
                .goal
                ->threadId,
            QStringLiteral("b"));
        QCOMPARE(
            state.tasks()
                ->snapshot()
                .at(1)
                .goal
                ->threadId,
            QStringLiteral("a"));
    }

    void olderGoalBatchLeavesOverlappingAddedTaskUnchanged()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int taskCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                QVector<BridgeTask> tasks{
                    task(QStringLiteral("a")),
                };
                if (taskCalls.fetch_add(1) != 0) {
                    tasks.append(
                        task(QStringLiteral("b")));
                }
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot(std::move(tasks)));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        runtime.refreshNow();
        QCOMPARE(executor.pendingCount(), 2);
        executor.runAt(1);
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 2;
        }));
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .at(1)
                 .goal
                 .has_value());

        executor.runAt(0);
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .at(0)
                .goal
                .has_value();
        }));

        const QStringList expectedIds{
            QStringLiteral("a"),
            QStringLiteral("b"),
        };
        QCOMPARE(modelIds(*state.tasks()), expectedIds);
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .at(1)
                 .goal
                 .has_value());
    }

    void nullGoalClearsRowAndCachedValue()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        QVector<bool> cachedAtTaskLoad;
        CodexRuntime runtime(
            state,
            bus,
            [&cachedAtTaskLoad](
                const QHash<QString, BridgeGoal>& cached,
                std::stop_token) {
                const QString id =
                    QStringLiteral("a");
                BridgeTask item = task(id);
                const auto iterator =
                    cached.constFind(id);
                const bool hasCached =
                    iterator != cached.constEnd();
                cachedAtTaskLoad.append(hasCached);
                if (hasCached) {
                    item.goal = iterator.value();
                }
                return Result<CodexProcessSnapshot>::
                    success(snapshot({item}));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                if (goalCalls.fetch_add(1) == 0) {
                    goals.insert(
                        QStringLiteral("a"),
                        goal(QStringLiteral("a")));
                } else {
                    goals.insert(
                        QStringLiteral("a"),
                        std::nullopt);
                }
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value();
        }));

        clock.advanceMilliseconds(
            shortCadence().goalStaleMilliseconds);
        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return !state.tasks()
                        ->snapshot()
                        .first()
                        .goal
                        .has_value();
        }));

        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QCOMPARE(goalCalls.load(), 2);
        QCOMPARE(cachedAtTaskLoad.size(), 3);
        QVERIFY(!cachedAtTaskLoad.at(0));
        QVERIFY(cachedAtTaskLoad.at(1));
        QVERIFY(!cachedAtTaskLoad.at(2));
    }

    void successfulTaskRemovalPrunesGoalCache()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        int taskCall = 0;
        QVector<bool> cachedAtTaskLoad;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCall, &cachedAtTaskLoad](
                const QHash<QString, BridgeGoal>& cached,
                std::stop_token) {
                const QString id =
                    QStringLiteral("a");
                cachedAtTaskLoad.append(
                    cached.contains(id));
                QVector<BridgeTask> tasks;
                if (taskCall != 1) {
                    BridgeTask item = task(id);
                    const auto iterator =
                        cached.constFind(id);
                    if (iterator
                        != cached.constEnd()) {
                        item.goal =
                            iterator.value();
                    }
                    tasks.append(std::move(item));
                }
                ++taskCall;
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot(std::move(tasks)));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value();
        }));

        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 0;
        }));

        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 1;
        }));

        QCOMPARE(cachedAtTaskLoad.size(), 3);
        QVERIFY(!cachedAtTaskLoad.at(0));
        QVERIFY(cachedAtTaskLoad.at(1));
        QVERIFY(!cachedAtTaskLoad.at(2));
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .first()
                 .goal
                 .has_value());
    }

    void lateGoalIsIgnoredWhileAbsentAndNotRestored()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        int taskCall = 0;
        QVector<bool> cachedAtTaskLoad;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCall, &cachedAtTaskLoad](
                const QHash<QString, BridgeGoal>& cached,
                std::stop_token) {
                const QString id =
                    QStringLiteral("a");
                cachedAtTaskLoad.append(
                    cached.contains(id));
                QVector<BridgeTask> tasks;
                if (taskCall != 1) {
                    tasks.append(task(id));
                }
                ++taskCall;
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot(std::move(tasks)));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        runtime.refreshNow();
        QCOMPARE(executor.pendingCount(), 2);
        executor.runAt(1);
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 0;
        }));

        executor.runAt(0);
        QCoreApplication::processEvents();
        QCOMPARE(state.tasks()->rowCount(), 0);

        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 1;
        }));

        QCOMPARE(cachedAtTaskLoad.size(), 3);
        QVERIFY(!cachedAtTaskLoad.at(2));
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .first()
                 .goal
                 .has_value());
    }

    void inFlightGoalAppliesWhenIdReappearsBeforeCompletion()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        int taskCall = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCall](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                QVector<BridgeTask> tasks;
                if (taskCall != 1) {
                    tasks.append(
                        task(QStringLiteral("a")));
                }
                ++taskCall;
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot(std::move(tasks)));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        runtime.refreshNow();
        executor.runAt(1);
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 0;
        }));

        runtime.refreshNow();
        executor.runAt(1);
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 1;
        }));
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .first()
                 .goal
                 .has_value());

        executor.runAt(0);
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value();
        }));
        QCOMPARE(
            state.tasks()
                ->snapshot()
                .first()
                .goal
                ->threadId,
            QStringLiteral("a"));
    }

    void refreshErrorsRetainRowsAndSanitizeBoundaries()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int taskCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                if (taskCalls.fetch_add(1) == 0) {
                    return Result<CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(
                                    QStringLiteral(
                                        "retained")),
                            }));
                }
                throw std::runtime_error(
                    "C:/private/task stderr");
            },
            emptyGoalLoader(),
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()->rowCount() == 1;
        }));

        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QCOMPARE(state.tasks()->rowCount(), 1);
        QCOMPARE(
            state.tasks()->snapshot().first().id,
            QStringLiteral("retained"));
        QCOMPARE(
            runtime.errorCode(),
            QStringLiteral("codex.refresh_failed"));
        QCOMPARE(
            runtime.errorMessage(),
            QStringLiteral(
                "Could not refresh Codex tasks."));
        const QString publicError =
            runtime.errorCode()
            + QStringLiteral("\n")
            + runtime.errorMessage();
        QVERIFY(
            !publicError.contains(
                QStringLiteral("private")));
        QVERIFY(
            !publicError.contains(
                QStringLiteral("C:/")));
    }

    void goalFailurePreservesCacheAndDelaysRetry()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>& cached,
                std::stop_token) {
                BridgeTask item =
                    task(QStringLiteral("a"));
                const auto iterator =
                    cached.constFind(
                        QStringLiteral("a"));
                if (iterator != cached.constEnd()) {
                    item.goal = iterator.value();
                }
                return Result<CodexProcessSnapshot>::
                    success(snapshot({item}));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                if (goalCalls.fetch_add(1) == 0) {
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>
                        goals;
                    goals.insert(
                        QStringLiteral("a"),
                        goal(QStringLiteral("a")));
                    return Result<
                        QHash<
                            QString,
                            std::optional<BridgeGoal>>>::
                        success(std::move(goals));
                }
                throw std::runtime_error(
                    "C:/private/goal stderr");
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&state] {
            return state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value();
        }));

        clock.advanceMilliseconds(
            shortCadence().goalStaleMilliseconds);
        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode()
                == QStringLiteral(
                    "codex.goal_refresh_failed");
        }));

        QVERIFY(
            state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value());
        QCOMPARE(
            runtime.errorMessage(),
            QStringLiteral(
                "Could not refresh Codex goals."));

        clock.advanceMilliseconds(
            shortCadence().goalStaleMilliseconds
            - 1);
        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        QCOMPARE(executor.pendingCount(), 0);
        QCOMPARE(goalCalls.load(), 2);
        QVERIFY(
            state.tasks()
                ->snapshot()
                .first()
                .goal
                .has_value());

        clock.advanceMilliseconds(1);
        runtime.refreshNow();
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        QCOMPARE(goalCalls.load(), 2);
    }

    void laterTaskAndGoalFailuresOwnSharedError()
    {
        const auto runOrder =
            [](
                bool goalFinishesLast,
                QString& finalError) {
                CompanionState state;
                CompanionCommandBus bus;
                ManualExecutor executor;
                MutableClock clock;
                std::atomic_int taskCalls = 0;
                CodexRuntime runtime(
                    state,
                    bus,
                    [&taskCalls](
                        const QHash<
                            QString,
                            BridgeGoal>&,
                        std::stop_token) {
                        if (taskCalls
                                .fetch_add(1)
                            == 0) {
                            return Result<
                                CodexProcessSnapshot>::
                                success(
                                    snapshot({
                                        task(
                                            QStringLiteral(
                                                "a")),
                                    }));
                        }
                        return Result<
                            CodexProcessSnapshot>::
                            failure(
                                privateFailure(
                                    QStringLiteral(
                                        "private.task")));
                    },
                    [](
                        const QVector<QString>&,
                        std::stop_token) {
                        return Result<
                            QHash<
                                QString,
                                std::optional<
                                    BridgeGoal>>>::
                            failure(
                                privateFailure(
                                    QStringLiteral(
                                        "private.goal")));
                    },
                    executor.executor(),
                    clock.provider(),
                    shortCadence());
                QVERIFY(runtime.start().hasValue());
                executor.runNext();
                QVERIFY(waitUntil([&executor] {
                    return executor.pendingCount()
                        == 1;
                }));
                runtime.refreshNow();
                QCOMPARE(
                    executor.pendingCount(),
                    2);

                if (goalFinishesLast) {
                    executor.runAt(1);
                    QVERIFY(waitUntil([&runtime] {
                        return runtime.errorCode()
                            == QStringLiteral(
                                "codex.refresh_failed");
                    }));
                    executor.runAt(0);
                    QVERIFY(waitUntil([&runtime] {
                        return runtime.errorCode()
                            == QStringLiteral(
                                "codex.goal_refresh_failed");
                    }));
                } else {
                    executor.runAt(0);
                    QVERIFY(waitUntil([&runtime] {
                        return runtime.errorCode()
                            == QStringLiteral(
                                "codex.goal_refresh_failed");
                    }));
                    executor.runAt(0);
                    QVERIFY(waitUntil([&runtime] {
                        return runtime.errorCode()
                            == QStringLiteral(
                                "codex.refresh_failed");
                    }));
                }
                finalError = runtime.errorCode();
            };

        QString goalLastError;
        QString taskLastError;
        runOrder(true, goalLastError);
        runOrder(false, taskLastError);
        QCOMPARE(
            goalLastError,
            QStringLiteral(
                "codex.goal_refresh_failed"));
        QCOMPARE(
            taskLastError,
            QStringLiteral("codex.refresh_failed"));
    }

    void taskStartAndSuccessRecoverGoalFailure()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                goalCalls.fetch_add(1);
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    failure(
                        privateFailure(
                            QStringLiteral(
                                "private.goal")));
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode()
                == QStringLiteral(
                    "codex.goal_refresh_failed");
        }));

        clock.advanceMilliseconds(
            shortCadence().goalStaleMilliseconds);
        runtime.setProcessSurfaceVisible(true);
        QCOMPARE(executor.pendingCount(), 1);
        runtime.refreshNow();
        QCOMPARE(executor.pendingCount(), 2);
        QVERIFY(runtime.errorCode().isEmpty());

        executor.runAt(0);
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode()
                == QStringLiteral(
                    "codex.goal_refresh_failed");
        }));
        executor.runAt(0);
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QVERIFY(runtime.errorCode().isEmpty());
        QVERIFY(runtime.errorMessage().isEmpty());
        QCOMPARE(goalCalls.load(), 2);
    }

    void taskFailureThenGoalSuccessClearsSharedError()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int taskCalls = 0;
        CodexRuntime runtime(
            state,
            bus,
            [&taskCalls](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                if (taskCalls.fetch_add(1) == 0) {
                    return Result<CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(QStringLiteral("a")),
                            }));
                }
                return Result<CodexProcessSnapshot>::
                    failure(
                        privateFailure(
                            QStringLiteral(
                                "private.task")));
            },
            [](
                const QVector<QString>&,
                std::stop_token) {
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success({});
            },
            executor.executor(),
            clock.provider(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        runtime.refreshNow();
        executor.runAt(1);
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode()
                == QStringLiteral(
                    "codex.refresh_failed");
        }));

        executor.runAt(0);
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode().isEmpty();
        }));
        QVERIFY(runtime.errorMessage().isEmpty());
    }

    void goalSuccessRecoversPriorGoalFailure()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        CodexRuntimeCadence cadence =
            shortCadence();
        cadence.settleRefreshMilliseconds = 5'000;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                if (goalCalls.fetch_add(1) == 0) {
                    return Result<
                        QHash<
                            QString,
                            std::optional<BridgeGoal>>>::
                        failure(
                            privateFailure(
                                QStringLiteral(
                                    "private.goal")));
                }
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success({});
            },
            executor.executor(),
            clock.provider(),
            cadence);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode()
                == QStringLiteral(
                    "codex.goal_refresh_failed");
        }));

        clock.advanceMilliseconds(
            cadence.goalStaleMilliseconds);
        runtime.setProcessSurfaceVisible(true);
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.errorCode().isEmpty();
        }));

        QVERIFY(runtime.errorMessage().isEmpty());
        QCOMPARE(goalCalls.load(), 2);
        runtime.stop();
    }

    void hiddenPassiveAndVisibleActiveCadence()
    {
        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            CodexRuntime runtime(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                tinyCadence());
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return !runtime.loading();
            }));
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            runtime.stop();
        }

        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            MutableClock clock;
            CodexRuntime runtime(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(
                                    QStringLiteral(
                                        "a")),
                            }));
                },
                emptyGoalLoader(),
                executor.executor(),
                clock.provider(),
                tinyCadence());
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            executor.runNext();
            runtime.setProcessSurfaceVisible(true);
            QTest::qWait(
                tinyCadence()
                    .settleRefreshMilliseconds
                + 2);
            QCOMPARE(executor.pendingCount(), 0);
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            runtime.stop();
        }
    }

    void settleChecksTaskAndGoalStaleness()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            clock.provider(),
            tinyCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QCoreApplication::processEvents();
        clock.advanceMilliseconds(
            tinyCadence().goalStaleMilliseconds);

        runtime.setProcessSurfaceVisible(true);

        QCOMPARE(executor.pendingCount(), 1);
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 2;
        }));
        runtime.stop();
    }

    void freshSettleSkipsAndVisibilityDoesNotDuplicate()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        CodexRuntimeCadence cadence =
            tinyCadence();
        cadence.activeRefreshMilliseconds = 20;
        cadence.passiveRefreshMilliseconds = 20;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            clock.provider(),
            cadence);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();

        runtime.setProcessSurfaceVisible(true);
        runtime.setProcessSurfaceVisible(true);
        runtime.setProcessSurfaceVisible(true);
        QTest::qWait(
            cadence.settleRefreshMilliseconds + 3);
        QCOMPARE(executor.pendingCount(), 0);
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        QCOMPARE(executor.pendingCount(), 0);
        runtime.setProcessSurfaceVisible(false);
    }

    void hidingPreservesPassiveTimerPhase()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        CodexRuntimeCadence cadence =
            tinyCadence();
        cadence.activeRefreshMilliseconds = 20;
        cadence.passiveRefreshMilliseconds = 20;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            clock.provider(),
            cadence);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();

        QTest::qWait(5);
        runtime.setProcessSurfaceVisible(true);
        QTest::qWait(5);
        runtime.setProcessSurfaceVisible(false);

        QVERIFY(waitUntil(
            [&executor] {
                return executor.pendingCount()
                    == 1;
            },
            15));
        runtime.stop();
    }

    void visibleRestartRestoresImmediateAndActiveCadence()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            tinyCadence());
        runtime.setProcessSurfaceVisible(true);
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 1);
        runtime.stop();
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 2);

        executor.runAt(0);
        executor.runAt(0);
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() >= 1;
        }));
        runtime.stop();
    }

    void workerPostAndDestructionRaceIsHarmless()
    {
        for (int iteration = 0;
             iteration < 32;
             ++iteration) {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            auto runtime =
                std::make_unique<CodexRuntime>(
                    state,
                    bus,
                    [](
                        const QHash<
                            QString,
                            BridgeGoal>&,
                        std::stop_token) {
                        return Result<
                            CodexProcessSnapshot>::
                            success(
                                snapshot({
                                    task(
                                        QStringLiteral(
                                            "late")),
                                }));
                    },
                    emptyGoalLoader(),
                    executor.executor(),
                    fixedNow(),
                    shortCadence());
            QVERIFY(runtime->start().hasValue());
            std::function<void()> worker =
                executor.takeAt(0);
            QVERIFY(worker);
            std::barrier start(2);
            std::thread thread(
                [worker = std::move(worker),
                 &start]() mutable {
                    start.arrive_and_wait();
                    worker();
                });
            start.arrive_and_wait();
            runtime.reset();
            thread.join();
            QCoreApplication::processEvents();
            QCOMPARE(state.tasks()->rowCount(), 0);
        }
    }

    void collaboratorDestructionBeforeQueuedDeliveryIsHarmless()
    {
        auto state =
            std::make_unique<CompanionState>();
        auto bus =
            std::make_unique<CompanionCommandBus>();
        ManualExecutor executor;
        auto runtime =
            std::make_unique<CodexRuntime>(
                *state,
                *bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(
                                    QStringLiteral(
                                        "late")),
                            }));
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                shortCadence());
        QVERIFY(runtime->start().hasValue());

        executor.runNext();
        state.reset();
        bus.reset();
        QCoreApplication::processEvents();
        QVERIFY(!runtime->running());
        QVERIFY(!runtime->loading());
        QCOMPARE(
            runtime->errorCode(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(
            runtime->errorMessage(),
            QStringLiteral(
                "Codex runtime is unavailable."));
        runtime.reset();
    }

    void startupBoundaryExceptionsAreSanitized()
    {
        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            CodexRuntime runtime(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                executor.executor(),
                []() -> QDateTime {
                    throw std::runtime_error(
                        "private now detail");
                },
                shortCadence());

            const Result<void> started =
                runtime.start();

            QVERIFY(!started.hasValue());
            QCOMPARE(
                started.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QCOMPARE(
                runtime.errorMessage(),
                QStringLiteral(
                    "Codex runtime is unavailable."));
            QCOMPARE(executor.pendingCount(), 0);
        }

        {
            CompanionState state;
            CompanionCommandBus bus;
            std::atomic_int loaderCalls = 0;
            CodexRuntime runtime(
                state,
                bus,
                [&loaderCalls](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    loaderCalls.fetch_add(1);
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                [](
                    std::function<void()>) {
                    throw std::runtime_error(
                        "private executor detail");
                },
                fixedNow(),
                shortCadence());

            const Result<void> started =
                runtime.start();

            QVERIFY(!started.hasValue());
            QCOMPARE(
                started.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QCOMPARE(loaderCalls.load(), 0);
            QVERIFY(!runtime.running());
            QVERIFY(!runtime.loading());
            QSignalSpy finishedSpy(
                &bus,
                &CompanionCommandBus::commandFinished);
            bus.execute(
                QStringLiteral("codex.refresh"));
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                1,
                1000);
            QVERIFY(
                !finishedSpy.at(0).at(1).toBool());
            QCOMPARE(
                finishedSpy.at(0).at(2).toString(),
                QStringLiteral(
                    "codex.runtime_unavailable"));
            const Result<void> probeRegistration =
                bus.registerHandler(
                    QStringLiteral("codex.refresh"),
                    [](
                        const QVariantMap&,
                        CompanionCommandBus::Completion
                            completion) {
                        completion(
                            Result<void>::success());
                    });
            QVERIFY(!probeRegistration.hasValue());
            QCOMPARE(
                probeRegistration.error().code,
                QStringLiteral(
                    "ui.command_already_registered"));
        }
    }

    void stopRequestsGoalTokenAndIgnoresLateGoal()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_bool observedStop = false;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            [&observedStop](
                const QVector<QString>&,
                std::stop_token stopToken) {
                observedStop.store(
                    stopToken.stop_requested());
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    goals;
                goals.insert(
                    QStringLiteral("a"),
                    goal(QStringLiteral("a")));
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success(std::move(goals));
            },
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));

        runtime.stop();
        executor.runNext();
        QCoreApplication::processEvents();

        QVERIFY(observedStop.load());
        QCOMPARE(state.tasks()->rowCount(), 1);
        QVERIFY(
            !state.tasks()
                 ->snapshot()
                 .first()
                 .goal
                 .has_value());
        QVERIFY(runtime.errorCode().isEmpty());
    }

    void goalBatchIdsAreUniqueNonblankAndSorted()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        QVector<QString> capturedIds;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(
                                QStringLiteral(" b ")),
                            task(QString()),
                            task(QStringLiteral("a")),
                            task(QStringLiteral("a")),
                        }));
            },
            [&capturedIds](
                const QVector<QString>& ids,
                std::stop_token) {
                capturedIds = ids;
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success({});
            },
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();

        const QVector<QString> expected{
            QStringLiteral("a"),
            QStringLiteral("b"),
        };
        QCOMPARE(capturedIds, expected);
    }

    void taskCompletionQueuesStaleGoalBeforeFollowUp()
    {
        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            std::atomic_int taskCalls = 0;
            std::atomic_int goalCalls = 0;
            CodexRuntime runtime(
                state,
                bus,
                [&taskCalls](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    taskCalls.fetch_add(1);
                    return Result<
                        CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(
                                    QStringLiteral(
                                        "a")),
                            }));
                },
                [&goalCalls](
                    const QVector<QString>&,
                    std::stop_token) {
                    goalCalls.fetch_add(1);
                    return Result<
                        QHash<
                            QString,
                            std::optional<
                                BridgeGoal>>>::
                        success({});
                },
                executor.executor(),
                fixedNow(),
                shortCadence());
            QVERIFY(runtime.start().hasValue());
            runtime.refreshNow();
            executor.runNext();
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 2;
            }));

            executor.runAt(0);
            QCOMPARE(goalCalls.load(), 1);
            QCOMPARE(taskCalls.load(), 1);
            runtime.stop();
        }

        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            MutableClock clock;
            std::atomic_int taskCalls = 0;
            std::atomic_int goalCalls = 0;
            CodexRuntime runtime(
                state,
                bus,
                [&taskCalls](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    if (taskCalls
                            .fetch_add(1)
                        == 0) {
                        return Result<
                            CodexProcessSnapshot>::
                            success(
                                snapshot({
                                    task(
                                        QStringLiteral(
                                            "a")),
                                }));
                    }
                    return Result<
                        CodexProcessSnapshot>::
                        failure(
                            privateFailure(
                                QStringLiteral(
                                    "private.task")));
                },
                [&goalCalls](
                    const QVector<QString>&,
                    std::stop_token) {
                    goalCalls.fetch_add(1);
                    return Result<
                        QHash<
                            QString,
                            std::optional<
                                BridgeGoal>>>::
                        success({});
                },
                executor.executor(),
                clock.provider(),
                shortCadence());
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            executor.runNext();
            QCoreApplication::processEvents();
            clock.advanceMilliseconds(
                shortCadence()
                    .goalStaleMilliseconds);
            runtime.refreshNow();
            runtime.refreshNow();
            executor.runNext();
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 2;
            }));

            executor.runAt(0);
            QCOMPARE(goalCalls.load(), 2);
            QCOMPARE(taskCalls.load(), 2);
            runtime.stop();
        }
    }

    void openingChecksGoalsAgainAfterSettle()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        MutableClock clock;
        std::atomic_int goalCalls = 0;
        CodexRuntimeCadence cadence{
            5'000,
            5'000,
            15,
            20,
            5,
        };
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(
                        snapshot({
                            task(QStringLiteral("a")),
                        }));
            },
            [&goalCalls](
                const QVector<QString>&,
                std::stop_token) {
                goalCalls.fetch_add(1);
                return Result<
                    QHash<
                        QString,
                        std::optional<BridgeGoal>>>::
                    success({});
            },
            executor.executor(),
            clock.provider(),
            cadence);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QCoreApplication::processEvents();
        QCOMPARE(goalCalls.load(), 1);

        clock.advanceMilliseconds(
            cadence.goalStaleMilliseconds);
        runtime.setProcessSurfaceVisible(true);
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QCoreApplication::processEvents();
        QCOMPARE(goalCalls.load(), 2);

        clock.advanceMilliseconds(
            cadence.goalStaleMilliseconds);
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QCOMPARE(goalCalls.load(), 3);
        runtime.stop();
    }

    void settleRefreshesEmptyAndRespectsFailedTaskAge()
    {
        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            CodexRuntimeCadence cadence{
                20,
                20,
                5,
                8,
                20,
            };
            CodexRuntime runtime(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                executor.executor(),
                fixedNow(),
                cadence);
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return !runtime.loading();
            }));
            runtime.setProcessSurfaceVisible(true);
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            runtime.stop();
        }

        {
            CompanionState state;
            CompanionCommandBus bus;
            ManualExecutor executor;
            MutableClock clock;
            std::atomic_int taskCalls = 0;
            CodexRuntimeCadence cadence{
                20,
                20,
                5,
                8,
                1000,
            };
            CodexRuntime runtime(
                state,
                bus,
                [&taskCalls](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    if (taskCalls
                            .fetch_add(1)
                        == 1) {
                        return Result<
                            CodexProcessSnapshot>::
                            failure(
                                privateFailure(
                                    QStringLiteral(
                                        "private.task")));
                    }
                    return Result<
                        CodexProcessSnapshot>::
                        success(
                            snapshot({
                                task(
                                    QStringLiteral(
                                        "a")),
                            }));
                },
                emptyGoalLoader(),
                executor.executor(),
                clock.provider(),
                cadence);
            QVERIFY(runtime.start().hasValue());
            executor.runNext();
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            executor.runNext();
            runtime.refreshNow();
            executor.runNext();
            QVERIFY(waitUntil([&runtime] {
                return runtime.errorCode()
                    == QStringLiteral(
                        "codex.refresh_failed");
            }));

            runtime.setProcessSurfaceVisible(true);
            QTest::qWait(
                cadence.settleRefreshMilliseconds
                + 3);
            QCOMPARE(executor.pendingCount(), 0);
            runtime.setProcessSurfaceVisible(false);
            clock.advanceMilliseconds(
                cadence.taskStaleMilliseconds);
            runtime.setProcessSurfaceVisible(true);
            QVERIFY(waitUntil([&executor] {
                return executor.pendingCount()
                    == 1;
            }));
            runtime.stop();
        }
    }

    void replacementRuntimeReusesSameBus()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor firstExecutor;
        auto first =
            std::make_unique<CodexRuntime>(
                state,
                bus,
                [](
                    const QHash<
                        QString,
                        BridgeGoal>&,
                    std::stop_token) {
                    return Result<
                        CodexProcessSnapshot>::
                        success(snapshot());
                },
                emptyGoalLoader(),
                firstExecutor.executor(),
                fixedNow(),
                shortCadence());
        QVERIFY(first->start().hasValue());
        first.reset();

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));

        ManualExecutor replacementExecutor;
        CodexRuntime replacement(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            replacementExecutor.executor(),
            fixedNow(),
            shortCadence());

        const Result<void> started =
            replacement.start();

        QVERIFY(started.hasValue());
        QCOMPARE(
            replacementExecutor.pendingCount(),
            1);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QVERIFY(finishedSpy.at(1).at(1).toBool());
        QCOMPARE(firstExecutor.pendingCount(), 1);
    }

    void runningStartRebindsRuntimeCommandGroup()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        CodexRuntime runtime(
            state,
            bus,
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                return Result<CodexProcessSnapshot>::
                    success(snapshot());
            },
            emptyGoalLoader(),
            executor.executor(),
            fixedNow(),
            shortCadence());
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 1);

        int displacedHandlerCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("codex.refresh"),
                           [&displacedHandlerCalls](
                               const QVariantMap&,
                               CompanionCommandBus::Completion
                                   completion) {
                               ++displacedHandlerCalls;
                               completion(
                                   Result<void>::success());
                           },
                       },
                   })
                .hasValue());

        QVERIFY(runtime.start().hasValue());
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(displacedHandlerCalls, 0);
        QCOMPARE(executor.pendingCount(), 1);
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeTests)

#include "CodexRuntimeTests.moc"
