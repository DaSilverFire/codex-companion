#include "codex/runtime/RuntimeContinuationHost.h"

#include <QtTest>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace companion;

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

} // namespace

class RuntimeContinuationHostTests final
    : public QObject {
    Q_OBJECT

private slots:
    void acceptedTaskRunsOffCallerExactlyOnce()
    {
        RuntimeContinuationHost host;
        const std::thread::id caller =
            std::this_thread::get_id();
        std::thread::id observed;
        std::atomic_int calls = 0;
        ManualGate finished;

        const Result<void> submitted =
            host.submit(
                [&] {
                    observed =
                        std::this_thread::get_id();
                    calls.fetch_add(1);
                    finished.release();
                });

        QVERIFY(submitted.hasValue());
        QVERIFY(finished.waitFor(1000));
        host.stopAcceptingAndDrain();
        QCOMPARE(calls.load(), 1);
        QVERIFY(observed != caller);
        QVERIFY(!host.accepting());
    }

    void drainWaitsForAcceptedTaskAndRejectsLaterSubmission()
    {
        RuntimeContinuationHost host;
        ManualGate entered;
        ManualGate release;
        std::atomic_int acceptedCalls = 0;
        std::atomic_int rejectedCalls = 0;
        QVERIFY(
            host.submit(
                    [&] {
                        acceptedCalls.fetch_add(1);
                        entered.release();
                        release.wait();
                    })
                .hasValue());
        QVERIFY(entered.waitFor(1000));

        std::atomic_bool drainReturned = false;
        std::thread drain(
            [&] {
                host.stopAcceptingAndDrain();
                drainReturned.store(true);
            });
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.accepting(),
            1000);
        const Result<void> rejected =
            host.submit(
                [&rejectedCalls] {
                    rejectedCalls.fetch_add(1);
                });
        QVERIFY(!rejected.hasValue());
        QCOMPARE(
            rejected.error().code,
            QStringLiteral(
                "codex.continuation_host_closed"));
        QTest::qWait(20);
        QVERIFY(!drainReturned.load());

        release.release();
        drain.join();
        QVERIFY(drainReturned.load());
        QCOMPARE(acceptedCalls.load(), 1);
        QCOMPARE(rejectedCalls.load(), 0);
    }

    void submitAndStopRaceHasOneLinearizedOutcome()
    {
        for (int iteration = 0;
             iteration < 64;
             ++iteration) {
            RuntimeContinuationHost host;
            std::barrier start(3);
            std::atomic_int calls = 0;
            std::optional<Result<void>> submitted;

            std::thread submitter(
                [&] {
                    start.arrive_and_wait();
                    submitted.emplace(
                        host.submit(
                            [&calls] {
                                calls.fetch_add(1);
                            }));
                });
            std::thread stopper(
                [&] {
                    start.arrive_and_wait();
                    host.stopAcceptingAndDrain();
                });
            start.arrive_and_wait();
            submitter.join();
            stopper.join();
            host.stopAcceptingAndDrain();

            QVERIFY(submitted.has_value());
            QCOMPARE(
                calls.load(),
                submitted->hasValue() ? 1 : 0);
            QVERIFY(!host.accepting());
        }
    }

    void throwingTaskDoesNotEscapeOrDropLaterTask()
    {
        RuntimeContinuationHost host;
        std::atomic_int calls = 0;
        QVERIFY(
            host.submit(
                    [] {
                        throw std::runtime_error(
                            "private task detail");
                    })
                .hasValue());
        QVERIFY(
            host.submit(
                    [&calls] {
                        calls.fetch_add(1);
                    })
                .hasValue());

        host.stopAcceptingAndDrain();

        QCOMPARE(calls.load(), 1);
    }
};

QTEST_GUILESS_MAIN(RuntimeContinuationHostTests)

#include "RuntimeContinuationHostTests.moc"
