#include "codex/state/HistoryCoordinator.h"

#include <QSemaphore>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

using namespace companion;
using namespace std::chrono_literals;

namespace {

class ManualClock final {
public:
    HistoryClock function() const
    {
        const auto milliseconds = milliseconds_;
        return [milliseconds] {
            return HistoryTimePoint(
                std::chrono::milliseconds(
                    milliseconds->load(
                        std::memory_order_relaxed)));
        };
    }

    void advance(std::chrono::milliseconds amount)
    {
        milliseconds_->fetch_add(
            amount.count(),
            std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<qint64>> milliseconds_ =
        std::make_shared<std::atomic<qint64>>(0);
};

HistorySnapshot snapshot(const QString& marker)
{
    return {
        {
            BridgeMessage{
                marker,
                MessageRole::Assistant,
                marker,
                std::nullopt,
                std::nullopt,
            },
        },
        QStringLiteral("message-") + marker,
        {},
        QStringLiteral("revision-") + marker,
        QStringLiteral("timeline-") + marker,
        {},
        std::nullopt,
    };
}

Result<HistorySnapshot> success(const QString& marker)
{
    return Result<HistorySnapshot>::success(snapshot(marker));
}

Result<HistorySnapshot> failure(const QString& marker)
{
    return Result<HistorySnapshot>::failure({
        QStringLiteral("codex.fixture_failure"),
        marker,
        false,
        {},
    });
}

Result<HistorySnapshot> finished(
    QFuture<Result<HistorySnapshot>>& future)
{
    future.waitForFinished();
    return future.result();
}

HistoryKey key(
    const QString& threadId,
    std::optional<QString> cursor = std::nullopt,
    int limit = 30)
{
    return {threadId, std::move(cursor), limit};
}

} // namespace

class HistoryCoordinatorTests final : public QObject {
    Q_OBJECT

private slots:
    void concurrentIdenticalKeysShareOneLoader()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(750ms, 8, clock.function());
        std::atomic<int> loaderCalls = 0;
        QSemaphore entered;
        QSemaphore release;
        const HistoryLoader loader = [&] {
            loaderCalls.fetch_add(1, std::memory_order_relaxed);
            entered.release();
            release.acquire();
            return success(QStringLiteral("shared"));
        };

        auto first = coordinator.load(
            key(QStringLiteral("thread")), loader);
        QVERIFY(entered.tryAcquire(1, 5000));
        auto second = coordinator.load(
            key(QStringLiteral("thread")), loader);
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 1);

        release.release();
        const auto firstResult = finished(first);
        const auto secondResult = finished(second);
        QVERIFY(firstResult.hasValue());
        QVERIFY(secondResult.hasValue());
        QCOMPARE(firstResult.value(), secondResult.value());
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 1);
    }

    void cursorPresenceValueAndLimitAreDistinctKeys()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(750ms, 8, clock.function());
        std::atomic<int> loaderCalls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                loaderCalls.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            return success(QString::number(call));
        };

        auto missing = coordinator.load(
            key(QStringLiteral("thread")), loader);
        auto empty = coordinator.load(
            key(QStringLiteral("thread"), QString()), loader);
        auto older = coordinator.load(
            key(
                QStringLiteral("thread"),
                QStringLiteral("120")),
            loader);
        auto differentLimit = coordinator.load(
            key(QStringLiteral("thread"), std::nullopt, 20),
            loader);

        QVERIFY(finished(missing).hasValue());
        QVERIFY(finished(empty).hasValue());
        QVERIFY(finished(older).hasValue());
        QVERIFY(finished(differentLimit).hasValue());
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 4);
    }

    void successCachesBeforeButNotAtLifetimeBoundary()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(750ms, 8, clock.function());
        std::atomic<int> loaderCalls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                loaderCalls.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            return success(QString::number(call));
        };
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));

        auto first = coordinator.load(historyKey, loader);
        const auto firstResult = finished(first);
        QVERIFY(firstResult.hasValue());
        QCOMPARE(
            firstResult.value().messages.first().text,
            QStringLiteral("1"));

        clock.advance(749ms);
        auto cached = coordinator.load(historyKey, loader);
        const auto cachedResult = finished(cached);
        QVERIFY(cachedResult.hasValue());
        QCOMPARE(
            cachedResult.value().messages.first().text,
            QStringLiteral("1"));
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 1);

        clock.advance(1ms);
        auto expired = coordinator.load(historyKey, loader);
        const auto expiredResult = finished(expired);
        QVERIFY(expiredResult.hasValue());
        QCOMPARE(
            expiredResult.value().messages.first().text,
            QStringLiteral("2"));
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 2);
    }

    void failedLoadsAreNotCached()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(750ms, 8, clock.function());
        std::atomic<int> loaderCalls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                loaderCalls.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            return failure(QString::number(call));
        };
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));

        auto first = coordinator.load(historyKey, loader);
        const auto firstResult = finished(first);
        QVERIFY(!firstResult.hasValue());
        QCOMPARE(
            firstResult.error().message,
            QStringLiteral("1"));

        auto second = coordinator.load(historyKey, loader);
        const auto secondResult = finished(second);
        QVERIFY(!secondResult.hasValue());
        QCOMPARE(
            secondResult.error().message,
            QStringLiteral("2"));
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 2);
    }

    void ninthEntryEvictsEarliestExpiry()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(750ms, 8, clock.function());
        std::atomic<int> loaderCalls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                loaderCalls.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            return success(QString::number(call));
        };

        for (int index = 0; index < 9; ++index) {
            auto loaded = coordinator.load(
                key(QStringLiteral("thread-%1").arg(index)),
                loader);
            QVERIFY(finished(loaded).hasValue());
            if (index < 8) {
                clock.advance(1ms);
            }
        }
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 9);

        auto retained = coordinator.load(
            key(QStringLiteral("thread-1")), loader);
        QVERIFY(finished(retained).hasValue());
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 9);

        auto evicted = coordinator.load(
            key(QStringLiteral("thread-0")), loader);
        QVERIFY(finished(evicted).hasValue());
        QCOMPARE(loaderCalls.load(std::memory_order_relaxed), 10);
    }

    void finalCancellableSubscriberRequestsSharedStopOnce()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(
            750ms,
            8,
            clock.function());
        std::atomic_int loaderCalls = 0;
        std::atomic_int stopCallbacks = 0;
        QSemaphore entered;
        QSemaphore stopped;
        QSemaphore release;
        const CancellableHistoryLoader loader =
            [&](std::stop_token stopToken) {
                loaderCalls.fetch_add(1);
                std::stop_callback callback(
                    stopToken,
                    [&] {
                        stopCallbacks.fetch_add(1);
                        stopped.release();
                    });
                entered.release();
                release.acquire();
                return failure(
                    QStringLiteral("stopped"));
            };
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));

        HistoryLoadHandle first =
            coordinator.loadCancellable(
                historyKey,
                loader);
        QVERIFY(entered.tryAcquire(1, 5000));
        HistoryLoadHandle second =
            coordinator.loadCancellable(
                historyKey,
                loader);
        QCOMPARE(loaderCalls.load(), 1);
        QVERIFY(first.cancellationLease);
        QVERIFY(second.cancellationLease);
        QVERIFY(
            first.cancellationLease
            != second.cancellationLease);

        first.cancellationLease->requestStop();
        QCOMPARE(stopCallbacks.load(), 0);
        second.cancellationLease->requestStop();
        QVERIFY(stopped.tryAcquire(1, 5000));
        QCOMPARE(stopCallbacks.load(), 1);
        second.cancellationLease->requestStop();
        QCOMPARE(stopCallbacks.load(), 1);

        release.release();
        QVERIFY(!finished(first.future).hasValue());
        QVERIFY(!finished(second.future).hasValue());
        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(stopCallbacks.load(), 1);
    }

    void legacySubscriberKeepsCoalescedLoadAlive()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(
            750ms,
            8,
            clock.function());
        std::atomic_int loaderCalls = 0;
        std::atomic_int stopCallbacks = 0;
        QSemaphore entered;
        QSemaphore release;
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));
        HistoryLoadHandle cancellable =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token stopToken) {
                    loaderCalls.fetch_add(1);
                    std::stop_callback callback(
                        stopToken,
                        [&] {
                            stopCallbacks.fetch_add(1);
                        });
                    entered.release();
                    release.acquire();
                    return success(
                        QStringLiteral("shared"));
                });
        QVERIFY(entered.tryAcquire(1, 5000));
        auto legacy = coordinator.load(
            historyKey,
            [] {
                return success(
                    QStringLiteral("unexpected"));
            });

        cancellable.cancellationLease->requestStop();
        QCOMPARE(stopCallbacks.load(), 0);
        release.release();

        const auto cancellableResult =
            finished(cancellable.future);
        const auto legacyResult =
            finished(legacy);
        QVERIFY(cancellableResult.hasValue());
        QVERIFY(legacyResult.hasValue());
        QCOMPARE(
            cancellableResult.value(),
            legacyResult.value());
        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(stopCallbacks.load(), 0);
    }

    void stopWinningCompletionDoesNotPopulateCache()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(
            750ms,
            8,
            clock.function());
        std::atomic_int loaderCalls = 0;
        QSemaphore entered;
        QSemaphore stopped;
        QSemaphore release;
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));
        HistoryLoadHandle first =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token stopToken) {
                    loaderCalls.fetch_add(1);
                    std::stop_callback callback(
                        stopToken,
                        [&] {
                            stopped.release();
                        });
                    entered.release();
                    release.acquire();
                    return success(
                        QStringLiteral("stop-won"));
                });
        QVERIFY(entered.tryAcquire(1, 5000));

        first.cancellationLease->requestStop();
        QVERIFY(stopped.tryAcquire(1, 5000));
        release.release();
        QVERIFY(finished(first.future).hasValue());

        auto second = coordinator.load(
            historyKey,
            [&] {
                loaderCalls.fetch_add(1);
                return success(
                    QStringLiteral("fresh"));
            });
        const auto secondResult = finished(second);
        QVERIFY(secondResult.hasValue());
        QCOMPARE(
            secondResult.value().messages.first().text,
            QStringLiteral("fresh"));
        QCOMPARE(loaderCalls.load(), 2);
    }

    void completionWinningMakesLeaseInertAndCaches()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(
            750ms,
            8,
            clock.function());
        std::atomic_int loaderCalls = 0;
        std::atomic_int stopCallbacks = 0;
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));
        HistoryLoadHandle first =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token stopToken) {
                    loaderCalls.fetch_add(1);
                    std::stop_callback callback(
                        stopToken,
                        [&] {
                            stopCallbacks.fetch_add(1);
                        });
                    return success(
                        QStringLiteral("cached"));
                });
        QVERIFY(finished(first.future).hasValue());

        first.cancellationLease->requestStop();
        QCOMPARE(stopCallbacks.load(), 0);
        HistoryLoadHandle cached =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token) {
                    loaderCalls.fetch_add(1);
                    return success(
                        QStringLiteral("unexpected"));
                });
        QVERIFY(!cached.cancellationLease);
        const auto cachedResult =
            finished(cached.future);
        QVERIFY(cachedResult.hasValue());
        QCOMPARE(
            cachedResult.value().messages.first().text,
            QStringLiteral("cached"));
        QCOMPARE(loaderCalls.load(), 1);
    }

    void newSubscriberAfterFinalStopUsesFreshEntry()
    {
        ManualClock clock;
        HistoryCoordinator coordinator(
            750ms,
            8,
            clock.function());
        std::atomic_int loaderCalls = 0;
        QSemaphore firstEntered;
        QSemaphore firstStopped;
        QSemaphore firstRelease;
        const HistoryKey historyKey =
            key(QStringLiteral("thread"));
        HistoryLoadHandle first =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token stopToken) {
                    loaderCalls.fetch_add(1);
                    std::stop_callback callback(
                        stopToken,
                        [&] {
                            firstStopped.release();
                        });
                    firstEntered.release();
                    firstRelease.acquire();
                    return success(
                        QStringLiteral("stale"));
                });
        QVERIFY(firstEntered.tryAcquire(1, 5000));

        first.cancellationLease->requestStop();
        QVERIFY(firstStopped.tryAcquire(1, 5000));
        HistoryLoadHandle second =
            coordinator.loadCancellable(
                historyKey,
                [&](std::stop_token) {
                    loaderCalls.fetch_add(1);
                    return success(
                        QStringLiteral("fresh"));
                });

        const auto secondResult =
            finished(second.future);
        QVERIFY(secondResult.hasValue());
        QCOMPARE(
            secondResult.value()
                .messages.first().text,
            QStringLiteral("fresh"));
        QCOMPARE(loaderCalls.load(), 2);

        firstRelease.release();
        QVERIFY(finished(first.future).hasValue());

        auto cached = coordinator.load(
            historyKey,
            [&] {
                loaderCalls.fetch_add(1);
                return success(
                    QStringLiteral("unexpected"));
            });
        const auto cachedResult = finished(cached);
        QVERIFY(cachedResult.hasValue());
        QCOMPARE(
            cachedResult.value()
                .messages.first().text,
            QStringLiteral("fresh"));
        QCOMPARE(loaderCalls.load(), 2);
    }
};

QTEST_GUILESS_MAIN(HistoryCoordinatorTests)
#include "HistoryCoordinatorTests.moc"
