#include "codex/runtime/CodexRuntime.h"
#include "codex/runtime/CapabilityService.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFuture>
#include <QJsonArray>
#include <QJsonObject>
#include <QPromise>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
            const std::scoped_lock lock(mutex_);
            workers_.push_back(std::move(worker));
        };
    }

    qsizetype pendingCount() const
    {
        const std::scoped_lock lock(mutex_);
        return static_cast<qsizetype>(
            workers_.size());
    }

    void runNext()
    {
        std::function<void()> worker = takeNext();
        std::thread thread(
            [worker = std::move(worker)]() mutable {
                worker();
            });
        thread.join();
    }

    std::jthread launchNext()
    {
        std::function<void()> worker = takeNext();
        return std::jthread(
            [worker = std::move(worker)]() mutable {
                worker();
            });
    }

private:
    std::function<void()> takeNext()
    {
        const std::scoped_lock lock(mutex_);
        if (workers_.empty()) {
            qFatal("no queued runtime worker");
        }
        std::function<void()> worker =
            std::move(workers_.front());
        workers_.pop_front();
        return worker;
    }

    mutable std::mutex mutex_;
    std::deque<std::function<void()>> workers_;
};

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds = 5000)
{
    const QDeadlineTimer deadline(
        timeoutMilliseconds);
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

BridgeCapabilities capabilities(
    const QString& id)
{
    BridgeCapabilities value;
    value.models.append({
        id,
        id,
        id,
        QStringLiteral("model"),
        true,
        QStringLiteral("medium"),
        {},
    });
    return value;
}

RpcResponse successResponse(
    QJsonObject result)
{
    return {
        std::move(result),
        {},
        false,
    };
}

QHash<int, RpcResponse>
emptyCapabilityResponses()
{
    return {
        {
            2,
            successResponse({
                {
                    QStringLiteral("data"),
                    QJsonArray{},
                },
            }),
        },
        {
            3,
            successResponse({
                {
                    QStringLiteral("data"),
                    QJsonArray{},
                },
            }),
        },
        {
            4,
            successResponse({
                {
                    QStringLiteral("marketplaces"),
                    QJsonArray{},
                },
            }),
        },
    };
}

BridgeUsageSnapshot usage(
    const QString& plan)
{
    BridgeUsageSnapshot value;
    value.planType = plan;
    value.updatedAt = {500.0};
    return value;
}

QFuture<Result<BridgeUsageSnapshot>>
readyUsageFuture(
    Result<BridgeUsageSnapshot> result)
{
    auto promise = std::make_shared<
        QPromise<Result<BridgeUsageSnapshot>>>();
    promise->start();
    QFuture<Result<BridgeUsageSnapshot>> future =
        promise->future();
    promise->addResult(std::move(result));
    promise->finish();
    return future;
}

class PendingUsage final {
public:
    PendingUsage()
        : promise_(std::make_shared<
              QPromise<
                  Result<BridgeUsageSnapshot>>>())
    {
        promise_->start();
        future_ = promise_->future();
    }

    QFuture<Result<BridgeUsageSnapshot>> future()
        const
    {
        return future_;
    }

    void finish(
        Result<BridgeUsageSnapshot> result)
    {
        promise_->addResult(std::move(result));
        promise_->finish();
    }

private:
    std::shared_ptr<
        QPromise<Result<BridgeUsageSnapshot>>>
        promise_;
    QFuture<Result<BridgeUsageSnapshot>> future_;
};

RuntimeGoalLoader emptyGoalLoader()
{
    return [](
               const QVector<QString>&,
               std::stop_token) {
        return Result<
            QHash<
                QString,
                std::optional<BridgeGoal>>>::success(
            {});
    };
}

CodexRuntimeDependencies dependencies(
    RuntimeExecutor executor,
    RuntimeCapabilityLoader capabilityLoader,
    RuntimeUsageReadStarter usageStarter)
{
    const auto coordinator =
        std::make_shared<HistoryCoordinator>();
    return {
        [](
            const QHash<QString, BridgeGoal>&,
            std::stop_token) {
            return Result<
                CodexProcessSnapshot>::success(
                {});
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
                return Result<
                    HistorySnapshot>::success(
                    {});
            },
            coordinator,
        },
        CodexRuntimeReadDependencies{
            std::move(capabilityLoader),
            std::move(usageStarter),
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

QVariantMap cwdArguments(const QString& cwd)
{
    return {
        {
            QStringLiteral("cwd"),
            cwd,
        },
    };
}

} // namespace

class CodexRuntimeReadTests final : public QObject {
    Q_OBJECT

private slots:
    void duplicateCapabilityLoadsCoalesceOffOwner()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const QThread* const ownerThread =
            QThread::currentThread();
        std::atomic_int calls = 0;
        std::atomic_bool ranOffOwner = false;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString& cwd,
                    std::stop_token) {
                    ++calls;
                    ranOffOwner.store(
                        QThread::currentThread()
                        != ownerThread);
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(cwd));
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success(usage(
                                QStringLiteral(
                                    "unused"))));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral(" C:\\A ")));
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("C:\\A")));

        QCOMPARE(calls.load(), 0);
        QVERIFY(runtime.capabilitiesLoading());
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QCOMPARE(calls.load(), 1);
        QVERIFY(ranOffOwner.load());
        QVERIFY(!runtime.capabilitiesLoading());
        QVERIFY(runtime.capabilities().has_value());
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("C:\\A"));
        QVERIFY(runtime.chatCapabilitiesValid());
    }

    void cachedCapabilityCompletionIsAsynchronous()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int calls = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString& cwd,
                    std::stop_token) {
                    ++calls;
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(cwd));
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);

        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral(" A ")));

        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(calls.load(), 1);
        QCOMPARE(executor.pendingCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QVERIFY(
            finishedSpy.at(1)
                .at(1)
                .toBool());
        QCOMPARE(calls.load(), 1);
    }

    void typedAvailabilityProviderRunsOnWorker()
    {
        QVERIFY(
            emptyCapabilityResponses()
                .value(2)
                .result
                .isObject());
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const QThread* const ownerThread =
            QThread::currentThread();
        std::atomic_bool providerOffOwner = false;
        std::atomic_int providerCalls = 0;
        const auto service =
            std::make_shared<CapabilityService>(
                [](
                    const QVector<RpcRequest>&,
                    std::stop_token) {
                    return Result<
                        QHash<int, RpcResponse>>::
                        success(
                            emptyCapabilityResponses());
                },
                CapabilityChatAvailabilityProvider(
                    [&] {
                        ++providerCalls;
                        providerOffOwner.store(
                            QThread::currentThread()
                            != ownerThread);
                        return ChatCatalogAvailability{
                            true,
                            true,
                            false,
                            false,
                        };
                    }));
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [service](
                    const QString& cwd,
                    std::stop_token stopToken) {
                    return service->load(
                        cwd,
                        stopToken);
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);

        QCOMPARE(providerCalls.load(), 1);
        QVERIFY(providerOffOwner.load());
        QVERIFY(
            runtime.capabilities()
                ->chatModels
                ->front()
                .isAvailable);
        QVERIFY(
            runtime.capabilities()
                ->chatModels
                ->front()
                .supportsAttachments);
    }

    void capabilityABAQueuesFreshFinalRequest()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        QSemaphore firstEntered;
        QSemaphore firstRelease;
        std::mutex callsMutex;
        QVector<QString> calls;
        std::atomic_bool firstStopRequested = false;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString& cwd,
                    std::stop_token stopToken) {
                    int callIndex = 0;
                    {
                        const std::scoped_lock lock(
                            callsMutex);
                        calls.append(cwd);
                        callIndex = calls.size();
                    }
                    if (callIndex == 1) {
                        firstEntered.release();
                        firstRelease.acquire();
                        firstStopRequested.store(
                            stopToken
                                .stop_requested());
                        return Result<
                            BridgeCapabilities>::
                            success(capabilities(
                                QStringLiteral(
                                    "old-A")));
                    }
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(
                            QStringLiteral(
                                "fresh-A")));
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        std::jthread firstWorker =
            executor.launchNext();
        QVERIFY(firstEntered.tryAcquire(1, 5000));

        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("B")));
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        for (int index = 0; index < 2; ++index) {
            QVERIFY(
                !finishedSpy.at(index)
                     .at(1)
                     .toBool());
            QCOMPARE(
                finishedSpy.at(index)
                    .at(2)
                    .toString(),
                QStringLiteral(
                    "codex.operation_superseded"));
        }

        firstRelease.release();
        firstWorker.join();
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        QVERIFY(!runtime.capabilities().has_value());
        QVERIFY(firstStopRequested.load());

        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            5000);
        QVERIFY(
            finishedSpy.at(2)
                .at(1)
                .toBool());
        {
            const std::scoped_lock lock(
                callsMutex);
            QCOMPARE(
                calls,
                QVector<QString>({
                    QStringLiteral("A"),
                    QStringLiteral("A"),
                }));
        }
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("fresh-A"));
    }

    void usageRequestsUseOneFollowUpRead()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        PendingUsage first;
        PendingUsage second;
        std::atomic_int starts = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [](const QString&,
                   std::stop_token) {
                    return Result<
                        BridgeCapabilities>::success(
                        {});
                },
                [&] {
                    const int index = ++starts;
                    return index == 1
                        ? first.future()
                        : second.future();
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (int index = 0; index < 3; ++index) {
            bus.execute(
                QStringLiteral(
                    "codex.usage.load"));
        }
        QCOMPARE(starts.load(), 1);
        QVERIFY(runtime.usageLoading());

        first.finish(
            Result<BridgeUsageSnapshot>::success(
                usage(QStringLiteral("first"))));
        QVERIFY(waitUntil([&] {
            return finishedSpy.size() == 1
                && starts.load() == 2;
        }));
        QVERIFY(runtime.usageSnapshot().has_value());
        QCOMPARE(
            *runtime.usageSnapshot()->planType,
            QStringLiteral("first"));
        QVERIFY(runtime.usageLoading());

        second.finish(
            Result<BridgeUsageSnapshot>::success(
                usage(QStringLiteral("second"))));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            5000);
        QCOMPARE(starts.load(), 2);
        QVERIFY(!runtime.usageLoading());
        QCOMPARE(
            *runtime.usageSnapshot()->planType,
            QStringLiteral("second"));
    }

    void readFailuresRetainPublicationsAndSanitize()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int capabilityCalls = 0;
        std::atomic_int usageCalls = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString&,
                    std::stop_token) {
                    if (++capabilityCalls == 1) {
                        return Result<
                            BridgeCapabilities>::
                            success(capabilities(
                                QStringLiteral(
                                    "kept")));
                    }
                    return Result<
                        BridgeCapabilities>::
                        failure({
                            QStringLiteral(
                                "SECRET_CAPABILITY"),
                            QStringLiteral(
                                "SECRET_CWD"),
                            false,
                            {
                                {
                                    QStringLiteral(
                                        "body"),
                                    QStringLiteral(
                                        "SECRET_BODY"),
                                },
                            },
                        });
                },
                [&] {
                    if (++usageCalls == 1) {
                        return readyUsageFuture(
                            Result<
                                BridgeUsageSnapshot>::
                                success(usage(
                                    QStringLiteral(
                                        "kept"))));
                    }
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            failure({
                                QStringLiteral(
                                    "SECRET_USAGE"),
                                QStringLiteral(
                                    "SECRET_CLOCK"),
                                false,
                                {},
                            }));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);

        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("B")));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QVERIFY(
            !finishedSpy.at(1)
                 .at(1)
                 .toBool());
        QCOMPARE(
            finishedSpy.at(1)
                .at(2)
                .toString(),
            QStringLiteral(
                "codex.capabilities_load_failed"));
        QCOMPARE(
            runtime.capabilitiesErrorCode(),
            QStringLiteral(
                "codex.capabilities_load_failed"));
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("kept"));

        bus.execute(
            QStringLiteral("codex.usage.load"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            5000);
        bus.execute(
            QStringLiteral("codex.usage.load"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            4,
            5000);
        QVERIFY(
            !finishedSpy.at(3)
                 .at(1)
                 .toBool());
        QCOMPARE(
            finishedSpy.at(3)
                .at(2)
                .toString(),
            QStringLiteral(
                "codex.usage_load_failed"));
        QCOMPARE(
            runtime.usageErrorCode(),
            QStringLiteral(
                "codex.usage_load_failed"));
        QCOMPARE(
            *runtime.usageSnapshot()->planType,
            QStringLiteral("kept"));
        const QString publicText =
            finishedSpy.at(1)
                .at(2)
                .toString()
            + finishedSpy.at(1)
                  .at(3)
                  .toString()
            + finishedSpy.at(3)
                  .at(2)
                  .toString()
            + finishedSpy.at(3)
                  .at(3)
                  .toString();
        QVERIFY(
            !publicText.contains(
                QStringLiteral("SECRET")));
    }

    void invalidationRetainsDisplayAndCoalesces()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        QSemaphore reloadEntered;
        QSemaphore reloadRelease;
        std::atomic_int calls = 0;
        std::atomic_bool reloadStopObserved = false;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString&,
                    std::stop_token stopToken) {
                    const int call = ++calls;
                    if (call == 2) {
                        reloadEntered.release();
                        reloadRelease.acquire();
                        reloadStopObserved.store(
                            stopToken
                                .stop_requested());
                    }
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(
                            QStringLiteral(
                                "revision-%1")
                                .arg(call)));
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        runtime.invalidateChatCapabilities();
        QCOMPARE(executor.pendingCount(), 0);
        QCOMPARE(runtime.capabilityRevision(), quint64(1));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(runtime.chatCapabilitiesValid());
        QCOMPARE(runtime.capabilityRevision(), quint64(1));

        runtime.invalidateChatCapabilities();
        QVERIFY(!runtime.chatCapabilitiesValid());
        QCOMPARE(runtime.capabilityRevision(), quint64(2));
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("revision-1"));
        std::jthread staleReload =
            executor.launchNext();
        QVERIFY(reloadEntered.tryAcquire(1, 5000));

        runtime.invalidateChatCapabilities();
        runtime.invalidateChatCapabilities();
        QCOMPARE(runtime.capabilityRevision(), quint64(4));
        reloadRelease.release();
        staleReload.join();
        QVERIFY(reloadStopObserved.load());
        QVERIFY(waitUntil([&executor] {
            return executor.pendingCount() == 1;
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.chatCapabilitiesValid()
                && !runtime.capabilitiesLoading();
        }));

        QCOMPARE(calls.load(), 3);
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("revision-3"));
        QCOMPARE(runtime.capabilityRevision(), quint64(4));
        QVERIFY(runtime.chatCapabilitiesValid());
    }

    void stoppedInvalidationRetriesFailedLatestCwd()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::mutex callsMutex;
        QVector<QString> calls;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString& cwd,
                    std::stop_token) {
                    int call = 0;
                    {
                        const std::scoped_lock lock(
                            callsMutex);
                        calls.append(cwd);
                        call = calls.size();
                    }
                    if (call == 1) {
                        return Result<
                            BridgeCapabilities>::
                            failure({
                                QStringLiteral(
                                    "SECRET_FAILURE"),
                                QStringLiteral(
                                    "SECRET_CWD"),
                                false,
                                {},
                            });
                    }
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(
                            QStringLiteral(
                                "retried")));
                },
                [] {
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral(" A ")));
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(
            !finishedSpy.at(0)
                 .at(1)
                 .toBool());

        runtime.stop();
        runtime.invalidateChatCapabilities();
        QCOMPARE(executor.pendingCount(), 0);
        QVERIFY(runtime.start().hasValue());
        QCOMPARE(executor.pendingCount(), 2);

        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return runtime.chatCapabilitiesValid()
                && !runtime.capabilitiesLoading();
        }));

        {
            const std::scoped_lock lock(
                callsMutex);
            QCOMPARE(
                calls,
                QVector<QString>({
                    QStringLiteral("A"),
                    QStringLiteral("A"),
                }));
        }
        QCOMPARE(
            runtime.capabilities()
                ->models.front()
                .id,
            QStringLiteral("retried"));
    }

    void malformedUsageStartersAreSanitized()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int starts = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [](const QString&,
                   std::stop_token) {
                    return Result<
                        BridgeCapabilities>::success(
                        {});
                },
                [&]()
                    -> QFuture<
                        Result<
                            BridgeUsageSnapshot>> {
                    const int start = ++starts;
                    if (start == 1) {
                        throw std::runtime_error(
                            "SECRET_STARTER");
                    }
                    if (start == 2) {
                        return {};
                    }
                    auto promise =
                        std::make_shared<
                            QPromise<
                                Result<
                                    BridgeUsageSnapshot>>>();
                    promise->start();
                    auto future = promise->future();
                    promise->addResult(
                        Result<
                            BridgeUsageSnapshot>::
                            success(usage(
                                QStringLiteral(
                                    "one"))));
                    promise->addResult(
                        Result<
                            BridgeUsageSnapshot>::
                            success(usage(
                                QStringLiteral(
                                    "two"))));
                    promise->finish();
                    return future;
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        for (int index = 0; index < 3; ++index) {
            bus.execute(
                QStringLiteral(
                    "codex.usage.load"));
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                index + 1,
                5000);
            QVERIFY(
                !finishedSpy.at(index)
                     .at(1)
                     .toBool());
            QCOMPARE(
                finishedSpy.at(index)
                    .at(2)
                    .toString(),
                QStringLiteral(
                    "codex.usage_load_failed"));
            QCOMPARE(
                finishedSpy.at(index)
                    .at(3)
                    .toString(),
                QStringLiteral(
                    "Could not load Codex usage."));
        }
        QCOMPARE(starts.load(), 3);
        QVERIFY(!runtime.usageSnapshot().has_value());
    }

    void invalidArgumentsFailBeforeReadStarts()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        std::atomic_int capabilityCalls = 0;
        std::atomic_int usageCalls = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString&,
                    std::stop_token) {
                    ++capabilityCalls;
                    return Result<
                        BridgeCapabilities>::success(
                        {});
                },
                [&] {
                    ++usageCalls;
                    return readyUsageFuture(
                        Result<
                            BridgeUsageSnapshot>::
                            success({}));
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            {
                {
                    QStringLiteral("cwd"),
                    42,
                },
            });
        bus.execute(
            QStringLiteral("codex.usage.load"),
            {
                {
                    QStringLiteral("cwd"),
                    QStringLiteral("A"),
                },
            });

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        for (int index = 0; index < 2; ++index) {
            QVERIFY(
                !finishedSpy.at(index)
                     .at(1)
                     .toBool());
            QCOMPARE(
                finishedSpy.at(index)
                    .at(2)
                    .toString(),
                QStringLiteral(
                    "codex.command_invalid_arguments"));
        }
        QCOMPARE(capabilityCalls.load(), 0);
        QCOMPARE(usageCalls.load(), 0);
        QCOMPARE(executor.pendingCount(), 0);
    }

    void stopCompletesWaitersAndSuppressesLateResults()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        QSemaphore capabilityEntered;
        QSemaphore capabilityRelease;
        PendingUsage pendingUsage;
        std::atomic_bool capabilityStopObserved =
            false;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            dependencies(
                executor.executor(),
                [&](const QString&,
                    std::stop_token stopToken) {
                    capabilityEntered.release();
                    capabilityRelease.acquire();
                    capabilityStopObserved.store(
                        stopToken.stop_requested());
                    return Result<
                        BridgeCapabilities>::success(
                        capabilities(
                            QStringLiteral("late")));
                },
                [&] {
                    return pendingUsage.future();
                }),
            host);
        QVERIFY(startRuntime(runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        std::jthread capabilityWorker =
            executor.launchNext();
        QVERIFY(
            capabilityEntered.tryAcquire(
                1,
                5000));
        bus.execute(
            QStringLiteral("codex.usage.load"));

        runtime.stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        for (int index = 0; index < 2; ++index) {
            QCOMPARE(
                finishedSpy.at(index)
                    .at(2)
                    .toString(),
                QStringLiteral(
                    "codex.runtime_unavailable"));
        }

        capabilityRelease.release();
        capabilityWorker.join();
        QVERIFY(capabilityStopObserved.load());
        pendingUsage.finish(
            Result<BridgeUsageSnapshot>::success(
                usage(QStringLiteral("late"))));
        QTest::qWait(50);
        QCoreApplication::processEvents();

        QVERIFY(!runtime.capabilities().has_value());
        QVERIFY(!runtime.usageSnapshot().has_value());
        QVERIFY(!runtime.capabilitiesLoading());
        QVERIFY(!runtime.usageLoading());
    }

    void destructionCompletesWaitersAndSuppressesLateResults()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        QSemaphore capabilityEntered;
        QSemaphore capabilityRelease;
        PendingUsage pendingUsage;
        std::atomic_bool stopObserved = false;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        auto runtime =
            std::make_unique<CodexRuntime>(
                state,
                bus,
                dependencies(
                    executor.executor(),
                    [&](const QString&,
                        std::stop_token stopToken) {
                        capabilityEntered.release();
                        capabilityRelease.acquire();
                        stopObserved.store(
                            stopToken
                                .stop_requested());
                        return Result<
                            BridgeCapabilities>::
                            success(capabilities(
                                QStringLiteral(
                                    "late")));
                    },
                    [&] {
                        return pendingUsage.future();
                    }),
                host);
        QVERIFY(startRuntime(*runtime, executor));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"),
            cwdArguments(QStringLiteral("A")));
        std::jthread capabilityWorker =
            executor.launchNext();
        QVERIFY(
            capabilityEntered.tryAcquire(
                1,
                5000));
        bus.execute(
            QStringLiteral("codex.usage.load"));

        runtime.reset();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        for (int index = 0; index < 2; ++index) {
            QCOMPARE(
                finishedSpy.at(index)
                    .at(2)
                    .toString(),
                QStringLiteral(
                    "codex.runtime_unavailable"));
        }

        capabilityRelease.release();
        capabilityWorker.join();
        QVERIFY(stopObserved.load());
        pendingUsage.finish(
            Result<BridgeUsageSnapshot>::success(
                usage(QStringLiteral("late"))));
        QTest::qWait(50);
        QCoreApplication::processEvents();
        QCOMPARE(finishedSpy.size(), 2);
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeReadTests)
#include "CodexRuntimeReadTests.moc"
