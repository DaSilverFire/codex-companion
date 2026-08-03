#include "codex/commands/CommitAwareMutation.h"
#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/CodexRuntimeOperationState.h"

#include <QtTest>

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace companion;

namespace companion::detail {

struct CodexRuntimeOperationTestAccess final {
    static void remove(
        CodexRuntimeOperationRegistry& registry,
        quint64 operationId,
        const CodexRuntimeOperationState* operation)
    {
        registry.removeOperation(
            operationId,
            operation);
    }
};

} // namespace companion::detail

namespace {

CompanionError failure(
    QString code,
    QString message = QStringLiteral("Failure."))
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

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

struct CompletionCapture final {
    CodexRuntimeOperationState::Completion completion()
    {
        return [this](Result<void> result) {
            const std::scoped_lock lock(mutex);
            ++calls;
            succeeded = result.hasValue();
            errorCode = result.hasValue()
                ? QString()
                : result.error().code;
            errorMessage = result.hasValue()
                ? QString()
                : result.error().message;
            errorContext = result.hasValue()
                ? QVariantMap()
                : result.error().context;
        };
    }

    std::mutex mutex;
    int calls = 0;
    bool succeeded = false;
    QString errorCode;
    QString errorMessage;
    QVariantMap errorContext;
};

} // namespace

class CodexRuntimeOperationTests final : public QObject {
    Q_OBJECT

private slots:
    void targetedStopOnlyVisitsMatchingOperation()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture firstCapture;
        CompletionCapture secondCapture;
        std::atomic_int firstStops = 0;
        std::atomic_int secondStops = 0;
        const auto first =
            CodexRuntimeOperationState::createMutation(
                firstCapture.completion(),
                1,
                1);
        const auto second =
            CodexRuntimeOperationState::createMutation(
                secondCapture.completion(),
                1,
                2);
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->installMutationObservation({
            [&firstStops] {
                firstStops.fetch_add(1);
            },
            [](CodexRuntimeOperationState&) {},
            failure(QStringLiteral("codex.synthetic")),
        }));
        QVERIFY(second->installMutationObservation({
            [&secondStops] {
                secondStops.fetch_add(1);
            },
            [](CodexRuntimeOperationState&) {},
            failure(QStringLiteral("codex.synthetic")),
        }));
        QVERIFY(
            registry->registerOperation(
                first,
                QStringLiteral("process-first"))
            != 0);
        QVERIFY(
            registry->registerOperation(
                second,
                QStringLiteral("process-second"))
            != 0);

        QVERIFY(
            registry->requestOperationStop(
                QStringLiteral("process-first")));

        QCOMPARE(firstStops.load(), 1);
        QCOMPARE(secondStops.load(), 0);
        QVERIFY(
            !registry->requestOperationStop(
                QStringLiteral("missing")));

        QVERIFY(first->finish(Result<void>::success()));
        QVERIFY(second->finish(Result<void>::success()));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void completionAndRuntimeStopRaceFinishesOnce()
    {
        for (int iteration = 0;
             iteration < 64;
             ++iteration) {
            const auto registry =
                CodexRuntimeOperationRegistry::create();
            CompletionCapture capture;
            std::atomic_int stopCalls = 0;
            const auto operation =
                CodexRuntimeOperationState::createRead(
                    capture.completion(),
                    4,
                    static_cast<std::uint64_t>(
                        iteration + 1),
                    [&stopCalls] {
                        stopCalls.fetch_add(1);
                    });
            QVERIFY(operation);
            QVERIFY(
                registry->registerOperation(
                    operation)
                != 0);

            std::barrier start(3);
            std::thread completionThread(
                [&operation, &start] {
                    start.arrive_and_wait();
                    operation->finish(
                        Result<void>::success());
                });
            std::thread stopThread(
                [&registry, &start] {
                    start.arrive_and_wait();
                    registry->requestRuntimeStop();
                });
            start.arrive_and_wait();
            completionThread.join();
            stopThread.join();

            QCOMPARE(capture.calls, 1);
            QVERIFY(
                capture.succeeded
                || capture.errorCode
                    == QStringLiteral(
                        "codex.runtime_unavailable"));
            QVERIFY(stopCalls.load() >= 0);
            QVERIFY(stopCalls.load() <= 1);
            QCOMPARE(
                registry->activeOperationCount(),
                0);
        }
    }

    void registryPublishesBeforeLaunchAndRemovesExactIdentity()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture firstCapture;
        const auto first =
            CodexRuntimeOperationState::createRead(
                firstCapture.completion(),
                9,
                12,
                [] {});
        const quint64 firstId =
            registry->registerOperation(first);
        QVERIFY(firstId != 0);

        qsizetype visibleAtLaunch = -1;
        const auto launch = [&] {
            visibleAtLaunch =
                registry->activeOperationCount();
        };
        launch();
        QCOMPARE(visibleAtLaunch, 1);
        QCOMPARE(first->operationId(), firstId);
        QCOMPARE(first->runtimeGeneration(), 9);
        QCOMPARE(first->operationGeneration(), 12);

        QVERIFY(first->finish(Result<void>::success()));
        QCOMPARE(firstCapture.calls, 1);
        QCOMPARE(registry->activeOperationCount(), 0);

        CompletionCapture secondCapture;
        const auto second =
            CodexRuntimeOperationState::createRead(
                secondCapture.completion(),
                9,
                13,
                [] {});
        const quint64 secondId =
            registry->registerOperation(second);
        QVERIFY(secondId != 0);
        QVERIFY(secondId != firstId);

        detail::CodexRuntimeOperationTestAccess::
            remove(*registry, secondId, first.get());
        QCOMPARE(registry->activeOperationCount(), 1);
        detail::CodexRuntimeOperationTestAccess::
            remove(*registry, firstId, first.get());
        QCOMPARE(registry->activeOperationCount(), 1);

        QVERIFY(second->finish(Result<void>::success()));
        QCOMPARE(secondCapture.calls, 1);
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void runtimeStopVisitsEachOperationOnce()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture firstCapture;
        CompletionCapture secondCapture;
        std::atomic_int firstStops = 0;
        std::atomic_int secondStops = 0;
        const auto first =
            CodexRuntimeOperationState::createRead(
                firstCapture.completion(),
                1,
                1,
                [&firstStops] {
                    firstStops.fetch_add(1);
                });
        const auto second =
            CodexRuntimeOperationState::createRead(
                secondCapture.completion(),
                1,
                2,
                [&secondStops] {
                    secondStops.fetch_add(1);
                });
        QVERIFY(registry->registerOperation(first) != 0);
        QVERIFY(registry->registerOperation(second) != 0);

        registry->requestRuntimeStop();
        registry->requestRuntimeStop();

        QCOMPARE(firstStops.load(), 1);
        QCOMPARE(secondStops.load(), 1);
        QCOMPARE(firstCapture.calls, 1);
        QCOMPARE(secondCapture.calls, 1);
        QCOMPARE(
            firstCapture.errorCode,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(
            secondCapture.errorCode,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void readStopClaimsTerminalBeforeStopCallback()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        std::weak_ptr<
            CodexRuntimeOperationState>
            weakOperation;
        bool completionCalled = false;
        bool completionWasPublishedBeforeStop = true;
        bool serviceCompletionWon = true;
        const auto operation =
            CodexRuntimeOperationState::createRead(
                [&](Result<void> result) {
                    completionCalled = true;
                    capture.completion()(
                        std::move(result));
                },
                2,
                3,
                [&] {
                    completionWasPublishedBeforeStop =
                        completionCalled;
                    if (const auto active =
                            weakOperation.lock()) {
                        serviceCompletionWon =
                            active->finish(
                                Result<void>::success());
                    }
                });
        weakOperation = operation;
        QVERIFY(registry->registerOperation(operation) != 0);

        registry->requestRuntimeStop();

        QVERIFY(!serviceCompletionWon);
        QVERIFY(!completionWasPublishedBeforeStop);
        QVERIFY(completionCalled);
        QCOMPARE(capture.calls, 1);
        QVERIFY(!capture.succeeded);
        QCOMPARE(
            capture.errorCode,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void pendingMutationStopWaitsForInstalledHandle()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        std::atomic_int stopCalls = 0;
        ManualGate terminalGate;
        const auto operation =
            CodexRuntimeOperationState::createMutation(
                capture.completion(),
                3,
                5);
        QVERIFY(registry->registerOperation(operation) != 0);

        std::thread observer(
            [&operation] {
                operation->observeMutationTerminal();
            });
        registry->requestRuntimeStop();
        QCOMPARE(stopCalls.load(), 0);
        QCOMPARE(capture.calls, 0);

        QVERIFY(
            operation->installMutationObservation({
                [&stopCalls, &terminalGate] {
                    stopCalls.fetch_add(1);
                    terminalGate.release();
                },
                [&terminalGate](
                    CodexRuntimeOperationState& state) {
                    terminalGate.wait();
                    state.finish(
                        Result<void>::failure(
                            failure(
                                QStringLiteral(
                                    "codex.operation_canceled"),
                                QStringLiteral(
                                    "The Codex operation was canceled."))));
                },
                failure(
                    QStringLiteral(
                        "codex.mutation_failed")),
            }));
        observer.join();

        QCOMPARE(stopCalls.load(), 1);
        QCOMPARE(capture.calls, 1);
        QCOMPARE(
            capture.errorCode,
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void preHandleFailureWakesObserverWithoutInvokingHandle()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        std::atomic_int observerCalls = 0;
        const auto operation =
            CodexRuntimeOperationState::createMutation(
                capture.completion(),
                6,
                7);
        QVERIFY(registry->registerOperation(operation) != 0);

        std::thread observer(
            [&operation] {
                operation->observeMutationTerminal();
            });
        QVERIFY(
            operation->finishBeforeMutationHandle(
                Result<void>::failure(
                    failure(
                        QStringLiteral(
                            "codex.mutation_failed")))));
        observer.join();

        QCOMPARE(observerCalls.load(), 0);
        QCOMPARE(capture.calls, 1);
        QCOMPARE(
            capture.errorCode,
            QStringLiteral(
                "codex.mutation_failed"));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void throwingStopCallbackStillUsesTerminalResult()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        std::atomic_int observerCalls = 0;
        const auto operation =
            CodexRuntimeOperationState::createMutation(
                capture.completion(),
                8,
                9);
        QVERIFY(registry->registerOperation(operation) != 0);

        std::thread observer(
            [&operation] {
                operation->observeMutationTerminal();
            });
        registry->requestRuntimeStop();
        QVERIFY(
            operation->installMutationObservation({
                [] {
                    throw std::runtime_error(
                        "private stop detail");
                },
                [&observerCalls](
                    CodexRuntimeOperationState& state) {
                    observerCalls.fetch_add(1);
                    state.finish(
                        Result<void>::success());
                },
                failure(
                    QStringLiteral(
                        "codex.mutation_failed")),
            }));
        observer.join();

        QVERIFY(operation->stopCallbackFailed());
        QCOMPARE(observerCalls.load(), 1);
        QCOMPARE(capture.calls, 1);
        QVERIFY(capture.succeeded);
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void malformedMutationObservationCanFinishBeforeHandle()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        const auto operation =
            CodexRuntimeOperationState::createMutation(
                capture.completion(),
                10,
                11);
        QVERIFY(registry->registerOperation(operation) != 0);

        std::thread observer(
            [&operation] {
                operation->observeMutationTerminal();
            });
        QVERIFY(
            !operation->installMutationObservation({
                {},
                [](
                    CodexRuntimeOperationState&) {},
                failure(
                    QStringLiteral(
                        "codex.mutation_failed")),
            }));
        QVERIFY(
            operation->finishBeforeMutationHandle(
                Result<void>::failure(
                    failure(
                        QStringLiteral(
                            "codex.mutation_failed")))));
        observer.join();

        QCOMPARE(capture.calls, 1);
        QCOMPARE(
            capture.errorCode,
            QStringLiteral(
                "codex.mutation_failed"));
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void throwingMutationObserverUsesSanitizedFailure()
    {
        const auto registry =
            CodexRuntimeOperationRegistry::create();
        CompletionCapture capture;
        const auto operation =
            CodexRuntimeOperationState::createMutation(
                capture.completion(),
                12,
                13);
        QVERIFY(registry->registerOperation(operation) != 0);

        std::thread observer(
            [&operation] {
                operation->observeMutationTerminal();
            });
        QVERIFY(
            operation->installMutationObservation({
                [] {},
                [](
                    CodexRuntimeOperationState&) {
                    throw std::runtime_error(
                        "private future detail");
                },
                {
                    QStringLiteral(
                        "codex.mutation_failed"),
                    QStringLiteral(
                        "Codex mutation failed."),
                    true,
                    {
                        {
                            QStringLiteral("secret"),
                            QStringLiteral("hidden"),
                        },
                    },
                },
            }));
        observer.join();

        QCOMPARE(capture.calls, 1);
        QCOMPARE(
            capture.errorCode,
            QStringLiteral(
                "codex.mutation_failed"));
        QCOMPARE(
            capture.errorMessage,
            QStringLiteral(
                "Codex mutation failed."));
        QVERIFY(capture.errorContext.isEmpty());
        QCOMPARE(registry->activeOperationCount(), 0);
    }

    void preCommitStopProducesExplicitCancellation()
    {
        const auto mutation =
            CommitAwareMutation<int>::create();
        QVERIFY(mutation);
        CommitAwareMutationHandle<int> handle =
            mutation->handle();
        QVERIFY(handle.terminalFuture.isValid());
        QVERIFY(handle.requestStopBeforeCommit);

        handle.requestStopBeforeCommit();
        QVERIFY(!mutation->tryCommit());
        handle.terminalFuture.waitForFinished();

        QVERIFY(!handle.terminalFuture.isCanceled());
        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<int> result =
            handle.terminalFuture.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QVERIFY(
            !mutation->finish(
                Result<int>::success(41)));
    }

    void postCommitStopCannotSuppressTerminalResult()
    {
        const auto mutation =
            CommitAwareMutation<int>::create();
        QVERIFY(mutation);
        CommitAwareMutationHandle<int> handle =
            mutation->handle();

        QVERIFY(mutation->tryCommit());
        handle.requestStopBeforeCommit();
        QVERIFY(
            mutation->finish(
                Result<int>::success(42)));
        handle.terminalFuture.waitForFinished();

        QVERIFY(!handle.terminalFuture.isCanceled());
        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<int> result =
            handle.terminalFuture.result();
        QVERIFY(result.hasValue());
        QCOMPARE(result.value(), 42);
    }

    void firstCommitOwnsEveryRemainingMutationStage()
    {
        const auto mutation =
            CommitAwareMutation<int>::create();
        QVERIFY(mutation);
        CommitAwareMutationHandle<int> handle =
            mutation->handle();

        QVERIFY(mutation->tryCommit());
        handle.requestStopBeforeCommit();
        QVERIFY(mutation->tryCommit());
        QVERIFY(
            mutation->finish(
                Result<int>::failure(
                    failure(
                        QStringLiteral(
                            "codex.service_failed")))));
        handle.terminalFuture.waitForFinished();

        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<int> result =
            handle.terminalFuture.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.service_failed"));
    }

    void validationFailureStillReturnsValidTerminalHandle()
    {
        const auto mutation =
            CommitAwareMutation<int>::create();
        QVERIFY(mutation);
        CommitAwareMutationHandle<int> handle =
            mutation->handle();
        QVERIFY(
            mutation->finish(
                Result<int>::failure(
                    failure(
                        QStringLiteral(
                            "codex.validation_failed")))));

        QVERIFY(handle.terminalFuture.isValid());
        QVERIFY(handle.requestStopBeforeCommit);
        handle.requestStopBeforeCommit();
        handle.terminalFuture.waitForFinished();
        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<int> result =
            handle.terminalFuture.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.validation_failed"));
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeOperationTests)

#include "CodexRuntimeOperationTests.moc"
