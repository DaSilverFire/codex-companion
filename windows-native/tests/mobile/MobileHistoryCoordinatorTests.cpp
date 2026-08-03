#include "mobile/history/MobileHistoryCoordinator.h"

#include <QFile>
#include <QFuture>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <memory>

using namespace companion;
using namespace std::chrono_literals;

namespace {

class ManualClock final {
public:
    HistoryClock function() const
    {
        const auto value = milliseconds_;
        return [value] {
            return HistoryTimePoint(
                std::chrono::milliseconds(
                    value->load(
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
        std::nullopt,
        {},
        QStringLiteral("revision-") + marker,
        std::nullopt,
        {},
        std::nullopt,
    };
}

Result<HistorySnapshot> success(const QString& marker)
{
    return Result<HistorySnapshot>::success(
        snapshot(marker));
}

Result<HistorySnapshot> failure(const QString& marker)
{
    return Result<HistorySnapshot>::failure({
        QStringLiteral("test.failure"),
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

} // namespace

class MobileHistoryCoordinatorTests final : public QObject {
    Q_OBJECT

private slots:
    void normalizesLimitAndPreservesExactCursorKey()
    {
        ManualClock clock;
        auto base =
            std::make_shared<HistoryCoordinator>(
                750ms,
                8,
                clock.function());
        MobileHistoryCoordinator coordinator(base);
        std::atomic_int calls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                calls.fetch_add(1) + 1;
            return success(QString::number(call));
        };

        auto lower = coordinator.load(
            {
                QStringLiteral("thread"),
                std::nullopt,
                0,
            },
            loader);
        QVERIFY(finished(lower).hasValue());
        auto lowerEquivalent = coordinator.load(
            {
                QStringLiteral("thread"),
                std::nullopt,
                1,
            },
            loader);
        QVERIFY(finished(lowerEquivalent).hasValue());
        QCOMPARE(calls.load(), 1);

        auto upper = coordinator.load(
            {
                QStringLiteral("thread"),
                std::nullopt,
                500,
            },
            loader);
        QVERIFY(finished(upper).hasValue());
        auto upperEquivalent = coordinator.load(
            {
                QStringLiteral("thread"),
                std::nullopt,
                50,
            },
            loader);
        QVERIFY(finished(upperEquivalent).hasValue());
        QCOMPARE(calls.load(), 2);

        auto missingCursor = coordinator.load(
            {
                QStringLiteral("thread"),
                std::nullopt,
                30,
            },
            loader);
        auto emptyCursor = coordinator.load(
            {
                QStringLiteral("thread"),
                QString(),
                30,
            },
            loader);
        QVERIFY(finished(missingCursor).hasValue());
        QVERIFY(finished(emptyCursor).hasValue());
        QCOMPARE(calls.load(), 4);
    }

    void coalescesSuccessCachesAndDoesNotCacheFailures()
    {
        ManualClock clock;
        auto base =
            std::make_shared<HistoryCoordinator>(
                750ms,
                8,
                clock.function());
        MobileHistoryCoordinator coordinator(base);
        std::atomic_int calls = 0;
        QSemaphore entered;
        QSemaphore release;
        const MobileHistoryKey key{
            QStringLiteral("shared"),
            std::nullopt,
            30,
        };
        const HistoryLoader loader = [&] {
            calls.fetch_add(1);
            entered.release();
            release.acquire();
            return success(QStringLiteral("shared"));
        };

        auto first = coordinator.load(key, loader);
        QVERIFY(entered.tryAcquire(1, 5000));
        auto second = coordinator.load(key, loader);
        QCOMPARE(calls.load(), 1);
        release.release();
        QVERIFY(finished(first).hasValue());
        QVERIFY(finished(second).hasValue());

        clock.advance(749ms);
        auto cached = coordinator.load(
            key,
            [&] {
                calls.fetch_add(1);
                return success(
                    QStringLiteral("unexpected"));
            });
        QCOMPARE(
            finished(cached)
                .value()
                .messages
                .front()
                .text,
            QStringLiteral("shared"));
        QCOMPARE(calls.load(), 1);

        clock.advance(1ms);
        auto expired = coordinator.load(
            key,
            [&] {
                calls.fetch_add(1);
                return success(
                    QStringLiteral("fresh"));
            });
        QCOMPARE(
            finished(expired)
                .value()
                .messages
                .front()
                .text,
            QStringLiteral("fresh"));
        QCOMPARE(calls.load(), 2);

        const MobileHistoryKey failingKey{
            QStringLiteral("failure"),
            std::nullopt,
            30,
        };
        auto failed = coordinator.load(
            failingKey,
            [&] {
                calls.fetch_add(1);
                return failure(QStringLiteral("one"));
            });
        QVERIFY(!finished(failed).hasValue());
        auto retried = coordinator.load(
            failingKey,
            [&] {
                calls.fetch_add(1);
                return failure(QStringLiteral("two"));
            });
        const auto retriedResult = finished(retried);
        QVERIFY(!retriedResult.hasValue());
        QCOMPARE(
            retriedResult.error().message,
            QStringLiteral("two"));
        QCOMPARE(calls.load(), 4);
    }

    void cacheRetainsAtMostEightEntries()
    {
        ManualClock clock;
        auto base =
            std::make_shared<HistoryCoordinator>(
                750ms,
                8,
                clock.function());
        MobileHistoryCoordinator coordinator(base);
        std::atomic_int calls = 0;
        const HistoryLoader loader = [&] {
            const int call =
                calls.fetch_add(1) + 1;
            return success(QString::number(call));
        };

        for (int index = 0; index < 9; ++index) {
            auto loaded = coordinator.load(
                {
                    QStringLiteral("thread-%1")
                        .arg(index),
                    std::nullopt,
                    30,
                },
                loader);
            QVERIFY(finished(loaded).hasValue());
            if (index < 8) {
                clock.advance(1ms);
            }
        }
        QCOMPARE(calls.load(), 9);

        auto retained = coordinator.load(
            {
                QStringLiteral("thread-1"),
                std::nullopt,
                30,
            },
            loader);
        QVERIFY(finished(retained).hasValue());
        QCOMPARE(calls.load(), 9);

        auto evicted = coordinator.load(
            {
                QStringLiteral("thread-0"),
                std::nullopt,
                30,
            },
            loader);
        QVERIFY(finished(evicted).hasValue());
        QCOMPARE(calls.load(), 10);
    }

    void revisionUsesFileSizeAndModifiedEpochMilliseconds()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            QDir(directory.path()).filePath(
                QStringLiteral("rollout.jsonl"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("1234567"), qint64(7));
        const QDateTime modified =
            QDateTime::fromMSecsSinceEpoch(
                1'784'765'432'109,
                QTimeZone::UTC);
        QVERIFY(
            file.setFileTime(
                modified,
                QFileDevice::FileModificationTime));
        file.close();

        const auto result =
            MobileHistoryCoordinator::revisionForFile(
                path);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value(),
            QStringLiteral("7:1784765432109"));
    }

    void selectedCompletionIsQueuedAndIgnoresStaleLoad()
    {
        auto base =
            std::make_shared<HistoryCoordinator>();
        MobileHistoryCoordinator coordinator(base);
        QObject receiver;
        QSemaphore oldEntered;
        QSemaphore oldRelease;
        int callbackCount = 0;
        QString publishedMarker;
        QThread* callbackThread = nullptr;

        auto oldFuture = coordinator.loadSelected(
            {
                QStringLiteral("old-thread"),
                std::nullopt,
                30,
            },
            [&] {
                oldEntered.release();
                oldRelease.acquire();
                return success(QStringLiteral("old"));
            },
            &receiver,
            [&](Result<HistorySnapshot> result) {
                ++callbackCount;
                callbackThread =
                    QThread::currentThread();
                if (result.hasValue()) {
                    publishedMarker =
                        result.value()
                            .messages
                            .front()
                            .text;
                }
            });
        QVERIFY(oldEntered.tryAcquire(1, 5000));

        auto newFuture = coordinator.loadSelected(
            {
                QStringLiteral("new-thread"),
                std::nullopt,
                30,
            },
            [] {
                return success(
                    QStringLiteral("new"));
            },
            &receiver,
            [&](Result<HistorySnapshot> result) {
                ++callbackCount;
                callbackThread =
                    QThread::currentThread();
                if (result.hasValue()) {
                    publishedMarker =
                        result.value()
                            .messages
                            .front()
                            .text;
                }
            });

        newFuture.waitForFinished();
        QTRY_COMPARE_WITH_TIMEOUT(
            callbackCount,
            1,
            5000);
        QCOMPARE(
            publishedMarker,
            QStringLiteral("new"));
        QCOMPARE(callbackThread, receiver.thread());

        oldRelease.release();
        oldFuture.waitForFinished();
        QTest::qWait(50);
        QCoreApplication::processEvents();
        QCOMPARE(callbackCount, 1);
        QCOMPARE(
            publishedMarker,
            QStringLiteral("new"));
    }
};

QTEST_GUILESS_MAIN(MobileHistoryCoordinatorTests)

#include "MobileHistoryCoordinatorTests.moc"
