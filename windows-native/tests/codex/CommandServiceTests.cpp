#include "codex/commands/ApprovalService.h"
#include "codex/commands/GoalService.h"
#include "codex/commands/TaskCommandService.h"
#include "codex/commands/UsageService.h"
#include "codex/ipc/FollowerRequestFactory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPromise>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QFutureWatcher>
#include <QtTest>

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

using namespace companion;

namespace companion::detail {
struct GoalServiceTestAccess;
}

template <typename Type, typename = void>
struct HasProductionGoalServiceTestFactory
    : std::false_type {
};

template <typename Type>
struct HasProductionGoalServiceTestFactory<
    Type,
    std::void_t<decltype(
        Type::create(
            std::declval<CancellableGoalRpcPerformer>(),
            std::declval<std::function<void(int, qsizetype)>>()))>>
    : std::true_type {
};

static_assert(
    !HasProductionGoalServiceTestFactory<
        companion::detail::GoalServiceTestAccess>::value,
    "The production goal-service header must not expose "
    "a callable test-access factory.");

namespace companion::detail {

struct GoalServiceTestAccess final {
    using Phase = GoalService::ReadPhase;
    using Probe = GoalService::ReadPhaseProbe;

    static std::unique_ptr<GoalService> create(
        CancellableGoalRpcPerformer performer,
        Probe probe)
    {
        return std::unique_ptr<GoalService>(
            new GoalService(
                std::move(performer),
                GoalCommitProbe{},
                std::move(probe)));
    }
};

} // namespace companion::detail

namespace {

template <typename T>
QFuture<T> readyFuture(T value)
{
    QPromise<T> promise;
    promise.start();
    QFuture<T> future = promise.future();
    promise.addResult(std::move(value));
    promise.finish();
    return future;
}

template <typename T>
QFuture<T> globalPoolPromiseFuture(T value)
{
    auto promise = std::make_shared<QPromise<T>>();
    promise->start();
    QFuture<T> future = promise->future();
    QThreadPool::globalInstance()->start(
        [promise, value] {
            promise->addResult(value);
            promise->finish();
        });
    return future;
}

template <typename T>
QFuture<T> exceptionalFuture()
{
    auto promise = std::make_shared<QPromise<T>>();
    promise->start();
    QFuture<T> future = promise->future();
    promise->setException(
        std::make_exception_ptr(
            std::runtime_error("sensitive exception text")));
    promise->finish();
    return future;
}

template <typename T>
QFuture<T> noResultFuture()
{
    QPromise<T> promise;
    promise.start();
    QFuture<T> future = promise.future();
    promise.finish();
    return future;
}

template <typename T>
QFuture<T> canceledFuture()
{
    QPromise<T> promise;
    promise.start();
    QFuture<T> future = promise.future();
    future.cancel();
    promise.finish();
    return future;
}

template <typename T>
T waitFor(QFuture<T> future)
{
    while (!future.isFinished()) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QTest::qWait(1);
    }
    future.waitForFinished();
    return future.result();
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QTest::qWait(10);
    }
    return predicate();
}

template <typename Predicate>
bool waitUntilNoEvents(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        if (predicate()) {
            return true;
        }
        QThread::msleep(1);
    }
    return predicate();
}

void cancelFutureWhenFlagIsSet(
    QFuture<Result<void>> future,
    std::atomic_bool& flag,
    std::atomic_bool& release)
{
    QElapsedTimer timer;
    timer.start();
    while (!flag.load() && timer.elapsed() < 1500) {
        QThread::msleep(1);
    }
    if (flag.load()) {
        future.cancel();
    }
    release.store(true);
}

QString errorBlob(const CompanionError& error)
{
    const QJsonObject object{
        {QStringLiteral("code"), error.code},
        {QStringLiteral("message"), error.message},
        {
            QStringLiteral("context"),
            QJsonValue::fromVariant(error.context),
        },
    };
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void verifySanitized(
    const CompanionError& error,
    const QVector<QString>& forbidden)
{
    const QString blob = errorBlob(error);
    for (const QString& secret : forbidden) {
        QVERIFY2(
            !blob.contains(secret),
            qPrintable(
                QStringLiteral("leaked `%1` in %2")
                    .arg(secret, blob)));
    }
}

CompanionError testError(QString code)
{
    return {
        std::move(code),
        QStringLiteral("test error"),
        false,
        {},
    };
}

QJsonObject fixtureObject(const QString& name)
{
    QFile file(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral("/codex-v034/")
        + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

BridgeAttachment bridgeAttachment(
    const QString& filename = QStringLiteral("notes.txt"))
{
    return {
        QUuid(QStringLiteral(
            "10000000-0000-0000-0000-000000000001")),
        AttachmentKind::File,
        filename,
        QStringLiteral("text/plain"),
        QByteArray("hello"),
    };
}

StagedAttachment stagedAttachment()
{
    return {
        QUuid(QStringLiteral(
            "20000000-0000-0000-0000-000000000001")),
        AttachmentKind::File,
        QStringLiteral("notes.txt"),
        QStringLiteral("C:/staged/notes.txt"),
        QStringLiteral("C:/staged/notes.txt"),
        QStringLiteral("text/plain"),
    };
}

SendRequest replyRequest()
{
    return {
        QStringLiteral("  Continue working  "),
        QStringLiteral(" thread-a "),
        QStringLiteral(" C:/repo "),
        SendAction::Reply,
        {},
        QStringLiteral(
            "30000000-0000-0000-0000-000000000001"),
    };
}

SendRequest steerRequest()
{
    SendRequest request = replyRequest();
    request.action = SendAction::Steer;
    request.expectedTurnId = QStringLiteral("turn-active");
    return request;
}

class ManualFollowerOutcome final {
public:
    ManualFollowerOutcome()
        : promise_(std::make_shared<QPromise<FollowerSendOutcome>>())
    {
        promise_->start();
        future_ = promise_->future();
    }

    QFuture<FollowerSendOutcome> future() const
    {
        return future_;
    }

    bool isCanceled() const
    {
        return future_.isCanceled();
    }

    void finish(FollowerSendOutcome outcome)
    {
        promise_->addResult(outcome);
        promise_->finish();
    }

private:
    std::shared_ptr<QPromise<FollowerSendOutcome>> promise_;
    QFuture<FollowerSendOutcome> future_;
};

class ManualApprovalOutcome final {
public:
    ManualApprovalOutcome()
        : promise_(std::make_shared<QPromise<FollowerApprovalOutcome>>())
    {
        promise_->start();
        future_ = promise_->future();
    }

    QFuture<FollowerApprovalOutcome> future() const
    {
        return future_;
    }

    bool isCanceled() const
    {
        return future_.isCanceled();
    }

    void finish(FollowerApprovalOutcome outcome)
    {
        promise_->addResult(outcome);
        promise_->finish();
    }

private:
    std::shared_ptr<QPromise<FollowerApprovalOutcome>> promise_;
    QFuture<FollowerApprovalOutcome> future_;
};

class ScopedGlobalThreadLimit final {
public:
    explicit ScopedGlobalThreadLimit(int threadCount)
        : originalCount_(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(threadCount);
    }

    ~ScopedGlobalThreadLimit()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(originalCount_);
    }

    ScopedGlobalThreadLimit(const ScopedGlobalThreadLimit&) = delete;
    ScopedGlobalThreadLimit& operator=(const ScopedGlobalThreadLimit&) = delete;

private:
    int originalCount_;
};

struct RecordingTaskBackend final {
    std::atomic_int validateCalls = 0;
    std::atomic_int stageCalls = 0;
    std::atomic_int settingsCalls = 0;
    std::atomic_int submitCalls = 0;
    std::atomic_int queueCalls = 0;
    QVector<QString> order;
    QVector<QString> stagedMessageIds;
    QVector<QString> transportedMessageIds;
    QVector<QVector<StagedAttachment>> transportedAttachments;
    QVector<CommandDiagnostic> diagnostics;
    FollowerSendOutcome settingsOutcome =
        FollowerSendOutcome::Sent;
    FollowerSendOutcome submitOutcome =
        FollowerSendOutcome::Sent;
    FollowerSendOutcome queueOutcome =
        FollowerSendOutcome::Sent;
    ManualFollowerOutcome* manualSettings = nullptr;
    ManualFollowerOutcome* manualQueue = nullptr;
    bool settingsDefaultFuture = false;
    bool settingsNoResultFuture = false;
    bool settingsCanceledFuture = false;
    bool settingsExceptionalFuture = false;
    bool submitDefaultFuture = false;
    bool submitCanceledFuture = false;
    bool queueDefaultFuture = false;
    bool queueNoResultFuture = false;
    bool queueCanceledFuture = false;
    bool queueOnGlobalThreadPool = false;
    bool queueExceptionalFuture = false;
    bool throwSettings = false;
    bool throwQueue = false;
    std::function<void()> settingsHook;
    std::function<void()> queueHook;
    std::function<void(const CommandDiagnostic&)> diagnosticHook;
    std::function<void(CommandCompletionPhase)> completionHook;

    Result<void> validate(
        const QVector<BridgeAttachment>& attachments)
    {
        validateCalls.fetch_add(1);
        order.append(QStringLiteral("validate"));
        if (attachments.size() > 10) {
            return Result<void>::failure(
                testError(QStringLiteral("attachment.too_many")));
        }
        return Result<void>::success();
    }

    Result<QVector<StagedAttachment>> stage(
        const QVector<BridgeAttachment>&,
        const QString& clientMessageId)
    {
        ++stageCalls;
        order.append(QStringLiteral("stage"));
        stagedMessageIds.append(clientMessageId);
        return Result<QVector<StagedAttachment>>::success(
            QVector<StagedAttachment>{stagedAttachment()});
    }

    Result<StagedAttachmentBatch> stageOwned(
        const QVector<BridgeAttachment>&,
        const QUuid& requestId)
    {
        ++stageCalls;
        order.append(QStringLiteral("stage"));
        stagedMessageIds.append(
            requestId.toString(QUuid::WithoutBraces));
        return Result<StagedAttachmentBatch>::success({
            QVector<StagedAttachment>{stagedAttachment()},
            StagedAttachmentCleanupLease::retainedInert(),
        });
    }

    QFuture<FollowerSendOutcome> settings(
        QString,
        QString,
        QString)
    {
        ++settingsCalls;
        order.append(QStringLiteral("settings"));
        if (settingsHook) {
            settingsHook();
        }
        if (throwSettings) {
            throw std::runtime_error("settings leaked secret");
        }
        if (manualSettings != nullptr) {
            return manualSettings->future();
        }
        if (settingsDefaultFuture) {
            return {};
        }
        if (settingsNoResultFuture) {
            return noResultFuture<FollowerSendOutcome>();
        }
        if (settingsCanceledFuture) {
            return canceledFuture<FollowerSendOutcome>();
        }
        if (settingsExceptionalFuture) {
            return exceptionalFuture<FollowerSendOutcome>();
        }
        return readyFuture(settingsOutcome);
    }

    QFuture<FollowerSendOutcome> submit(
        QString,
        QString,
        SendAction,
        QString clientMessageId,
        QString,
        QVector<StagedAttachment> attachments)
    {
        ++submitCalls;
        order.append(QStringLiteral("submit"));
        transportedMessageIds.append(clientMessageId);
        transportedAttachments.append(attachments);
        if (submitDefaultFuture) {
            return {};
        }
        if (submitCanceledFuture) {
            return canceledFuture<FollowerSendOutcome>();
        }
        return readyFuture(submitOutcome);
    }

    QFuture<FollowerSendOutcome> queue(
        QString,
        QString,
        QString clientMessageId,
        QString,
        QVector<StagedAttachment> attachments)
    {
        queueCalls.fetch_add(1);
        order.append(QStringLiteral("queue"));
        transportedMessageIds.append(clientMessageId);
        transportedAttachments.append(attachments);
        if (queueHook) {
            queueHook();
        }
        if (throwQueue) {
            throw std::runtime_error("queue leaked secret");
        }
        if (manualQueue != nullptr) {
            return manualQueue->future();
        }
        if (queueDefaultFuture) {
            return {};
        }
        if (queueNoResultFuture) {
            return noResultFuture<FollowerSendOutcome>();
        }
        if (queueCanceledFuture) {
            return canceledFuture<FollowerSendOutcome>();
        }
        if (queueExceptionalFuture) {
            return exceptionalFuture<FollowerSendOutcome>();
        }
        if (queueOnGlobalThreadPool) {
            return globalPoolPromiseFuture(queueOutcome);
        }
        return readyFuture(queueOutcome);
    }

    std::unique_ptr<TaskCommandService> service()
    {
        return std::make_unique<TaskCommandService>(
            [this](const QVector<BridgeAttachment>& attachments) {
                return validate(attachments);
            },
            [this](
                const QVector<BridgeAttachment>& attachments,
                const QString& clientMessageId) {
                return stage(attachments, clientMessageId);
            },
            [this](
                QString threadId,
                QString model,
                QString effort) {
                return settings(
                    std::move(threadId),
                    std::move(model),
                    std::move(effort));
            },
            [this](
                QString prompt,
                QString threadId,
                SendAction action,
                QString clientMessageId,
                QString cwd,
                QVector<StagedAttachment> attachments) {
                return submit(
                    std::move(prompt),
                    std::move(threadId),
                    action,
                    std::move(clientMessageId),
                    std::move(cwd),
                    std::move(attachments));
            },
            [this](
                QString prompt,
                QString threadId,
                QString clientMessageId,
                QString cwd,
                QVector<StagedAttachment> attachments) {
                return queue(
                    std::move(prompt),
                    std::move(threadId),
                    std::move(clientMessageId),
                    std::move(cwd),
                    std::move(attachments));
            },
            [this](CommandDiagnostic diagnostic) {
                if (diagnosticHook) {
                    diagnosticHook(diagnostic);
                }
                diagnostics.append(std::move(diagnostic));
            },
            completionHook);
    }

    std::unique_ptr<TaskCommandService> ownedService()
    {
        return std::make_unique<TaskCommandService>(
            [this](const QVector<BridgeAttachment>& attachments) {
                return validate(attachments);
            },
            [this](
                const QVector<BridgeAttachment>& attachments,
                const QUuid& requestId) {
                return stageOwned(attachments, requestId);
            },
            [this](
                QString threadId,
                QString model,
                QString effort) {
                return settings(
                    std::move(threadId),
                    std::move(model),
                    std::move(effort));
            },
            [this](
                QString prompt,
                QString threadId,
                SendAction action,
                QString clientMessageId,
                QString cwd,
                QVector<StagedAttachment> attachments) {
                return submit(
                    std::move(prompt),
                    std::move(threadId),
                    action,
                    std::move(clientMessageId),
                    std::move(cwd),
                    std::move(attachments));
            },
            [this](
                QString prompt,
                QString threadId,
                QString clientMessageId,
                QString cwd,
                QVector<StagedAttachment> attachments) {
                return queue(
                    std::move(prompt),
                    std::move(threadId),
                    std::move(clientMessageId),
                    std::move(cwd),
                    std::move(attachments));
            },
            [this](CommandDiagnostic diagnostic) {
                if (diagnosticHook) {
                    diagnosticHook(diagnostic);
                }
                diagnostics.append(std::move(diagnostic));
            },
            completionHook);
    }
};

QJsonObject goalObject(
    const QString& threadId,
    const QString& status,
    std::optional<qint64> tokenBudget = std::nullopt)
{
    QJsonObject object{
        {QStringLiteral("threadId"), threadId},
        {QStringLiteral("objective"), QStringLiteral("Ship it")},
        {QStringLiteral("status"), status},
        {QStringLiteral("tokensUsed"), 42},
        {QStringLiteral("timeUsedSeconds"), 90},
        {QStringLiteral("createdAt"), 100},
        {QStringLiteral("updatedAt"), 200},
    };
    object.insert(
        QStringLiteral("tokenBudget"),
        tokenBudget.has_value()
            ? QJsonValue(*tokenBudget)
            : QJsonValue(QJsonValue::Null));
    return object;
}

RpcResponse goalResponse(
    const QString& threadId,
    const QString& status = QStringLiteral("active"),
    std::optional<qint64> tokenBudget = std::nullopt)
{
    return {
        QJsonObject{
            {
                QStringLiteral("goal"),
                goalObject(threadId, status, tokenBudget),
            },
        },
        {},
        false,
    };
}

struct RecordingRpc final {
    QVector<QVector<RpcRequest>> batches;
    QHash<int, RpcResponse> responses;
    Result<QHash<int, RpcResponse>> result =
        Result<QHash<int, RpcResponse>>::success({});

    Result<QHash<int, RpcResponse>> perform(
        const QVector<RpcRequest>& requests)
    {
        batches.append(requests);
        if (!responses.isEmpty()) {
            return Result<QHash<int, RpcResponse>>::success(
                responses);
        }
        return result;
    }
};

QJsonObject usageWindow(double usedPercent, double durationMins)
{
    return {
        {QStringLiteral("usedPercent"), usedPercent},
        {QStringLiteral("windowDurationMins"), durationMins},
        {QStringLiteral("resetsAt"), 1'700'000'000.0},
    };
}

QJsonObject usageCredit(
    const QString& status = QStringLiteral("available"))
{
    return {
        {QStringLiteral("id"), QStringLiteral("credit-a")},
        {QStringLiteral("resetType"), QStringLiteral("codexRateLimits")},
        {QStringLiteral("status"), status},
        {QStringLiteral("grantedAt"), 1'700'000'000.0},
        {QStringLiteral("expiresAt"), 1'800'000'000.0},
        {QStringLiteral("title"), QStringLiteral("Reset")},
        {QStringLiteral("description"), QStringLiteral("Banked reset")},
    };
}

QJsonObject validUsageObject()
{
    return {
        {
            QStringLiteral("rateLimits"),
            QJsonObject{
                {QStringLiteral("limitId"), QStringLiteral("codex")},
                {QStringLiteral("planType"), QStringLiteral("pro")},
                {QStringLiteral("primary"), usageWindow(20.0, 60.0)},
            },
        },
        {
            QStringLiteral("rateLimitResetCredits"),
            QJsonObject{
                {QStringLiteral("availableCount"), 1},
                {
                    QStringLiteral("credits"),
                    QJsonArray{usageCredit()},
                },
            },
        },
    };
}

Result<BridgeUsageSnapshot> readUsageObject(
    const QJsonObject& object)
{
    RecordingRpc rpc;
    rpc.responses = {{2, {object, {}, false}}};
    UsageService service(
        [&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        },
        [] {
            return BridgeDate{500.0};
        });
    return waitFor(service.read());
}

std::unique_ptr<TaskCommandService> taskServiceWithDiagnosticHook(
    RecordingTaskBackend& backend,
    std::function<void(const CommandDiagnostic&)> diagnosticHook)
{
    return std::make_unique<TaskCommandService>(
        [&backend](const QVector<BridgeAttachment>& attachments) {
            return backend.validate(attachments);
        },
        [&backend](
            const QVector<BridgeAttachment>& attachments,
            const QString& clientMessageId) {
            return backend.stage(attachments, clientMessageId);
        },
        [&backend](
            QString threadId,
            QString model,
            QString effort) {
            return backend.settings(
                std::move(threadId),
                std::move(model),
                std::move(effort));
        },
        [&backend](
            QString prompt,
            QString threadId,
            SendAction action,
            QString clientMessageId,
            QString cwd,
            QVector<StagedAttachment> attachments) {
            return backend.submit(
                std::move(prompt),
                std::move(threadId),
                action,
                std::move(clientMessageId),
                std::move(cwd),
                std::move(attachments));
        },
        [&backend](
            QString prompt,
            QString threadId,
            QString clientMessageId,
            QString cwd,
            QVector<StagedAttachment> attachments) {
            return backend.queue(
                std::move(prompt),
                std::move(threadId),
                std::move(clientMessageId),
                std::move(cwd),
                std::move(attachments));
        },
        [diagnosticHook = std::move(diagnosticHook)](
            CommandDiagnostic diagnostic) {
            if (diagnosticHook) {
                diagnosticHook(diagnostic);
            }
        });
}

QJsonObject validUsageByIdGroups()
{
    return {
        {
            QStringLiteral("model-2"),
            QJsonObject{
                {
                    QStringLiteral("primary"),
                    usageWindow(10.0, 60.0),
                },
            },
        },
    };
}

void expectUsageUnavailable(QJsonObject object)
{
    const Result<BridgeUsageSnapshot> result =
        readUsageObject(object);
    QVERIFY(!result.hasValue());
    QCOMPARE(
        result.error().code,
        QStringLiteral("usage.unavailable"));
}

class CommandServiceTests final : public QObject {
    Q_OBJECT

private slots:
    void blankPromptAndThreadFailBeforeStagingOrTransport()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();

        SendRequest blankPrompt = replyRequest();
        blankPrompt.prompt = QStringLiteral(" \n ");
        const Result<void> promptResult =
            waitFor(service->send(blankPrompt));

        QVERIFY(!promptResult.hasValue());
        QCOMPARE(
            promptResult.error().code,
            QStringLiteral("codex.prompt_empty"));
        QCOMPARE(backend.validateCalls.load(), 0);
        QCOMPARE(backend.stageCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 0);

        SendRequest blankThread = replyRequest();
        blankThread.threadId = QStringLiteral(" \t ");
        const Result<void> threadResult =
            waitFor(service->send(blankThread));

        QVERIFY(!threadResult.hasValue());
        QCOMPARE(
            threadResult.error().code,
            QStringLiteral("codex.thread_id_empty"));
        QCOMPARE(backend.validateCalls.load(), 0);
        QCOMPARE(backend.stageCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 0);
    }

    void attachmentsValidateAndStageOnceBeforeQueuedReply()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.attachments = {bridgeAttachment()};

        const Result<void> result =
            waitFor(service->send(request));

        QVERIFY(result.hasValue());
        QCOMPARE(backend.validateCalls.load(), 1);
        QCOMPARE(backend.stageCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
        QCOMPARE(
            backend.order,
            QVector<QString>({
                QStringLiteral("validate"),
                QStringLiteral("stage"),
                QStringLiteral("queue"),
            }));
        QCOMPARE(backend.transportedAttachments.size(), 1);
        QCOMPARE(backend.transportedAttachments.first().size(), 1);
    }

    void attachmentBoundaryErrorsAreSanitized()
    {
        const QVector<QString> forbidden{
            QStringLiteral("SECRET_FILENAME.txt"),
            QStringLiteral("SECRET_PROMPT"),
            QStringLiteral("C:/secret/SECRET_FILENAME.txt"),
            QStringLiteral("raw-secret-bytes"),
        };

        TaskCommandService validateService(
            [](const QVector<BridgeAttachment>&) {
                return Result<void>::failure({
                    QStringLiteral("attachment.SECRET_FILENAME"),
                    QStringLiteral(
                        "failed SECRET_FILENAME.txt from SECRET_PROMPT"),
                    false,
                    {
                        {
                            QStringLiteral("filename"),
                            QStringLiteral("SECRET_FILENAME.txt"),
                        },
                        {
                            QStringLiteral("path"),
                            QStringLiteral(
                                "C:/secret/SECRET_FILENAME.txt"),
                        },
                    },
                });
            },
            [](
                const QVector<BridgeAttachment>&,
                const QString&) {
                return Result<QVector<StagedAttachment>>::success(
                    QVector<StagedAttachment>{});
            },
            CommandSettingsSender{},
            CommandSubmitter{},
            [](
                QString,
                QString,
                QString,
                QString,
                QVector<StagedAttachment>) {
                return readyFuture(FollowerSendOutcome::Sent);
            });
        SendRequest validateRequest = replyRequest();
        validateRequest.prompt = QStringLiteral("SECRET_PROMPT");
        validateRequest.attachments = {
            bridgeAttachment(QStringLiteral("SECRET_FILENAME.txt")),
        };

        const Result<void> validateResult =
            waitFor(validateService.send(validateRequest));

        QVERIFY(!validateResult.hasValue());
        QCOMPARE(
            validateResult.error().code,
            QStringLiteral("codex.attachments_unavailable"));
        QCOMPARE(
            validateResult.error().message,
            QStringLiteral(
                "Codex command attachments are unavailable."));
        QCOMPARE(
            validateResult.error().context.value(
                QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-a"));
        verifySanitized(validateResult.error(), forbidden);

        TaskCommandService stageService(
            [](const QVector<BridgeAttachment>&) {
                return Result<void>::success();
            },
            [](
                const QVector<BridgeAttachment>&,
                const QString&) {
                return Result<QVector<StagedAttachment>>::failure({
                    QStringLiteral("attachment.stage_failed"),
                    QStringLiteral(
                        "staged path C:/secret/SECRET_FILENAME.txt"),
                    false,
                    {
                        {
                            QStringLiteral("bytes"),
                            QStringLiteral("raw-secret-bytes"),
                        },
                    },
                });
            },
            CommandSettingsSender{},
            CommandSubmitter{},
            [](
                QString,
                QString,
                QString,
                QString,
                QVector<StagedAttachment>) {
                return readyFuture(FollowerSendOutcome::Sent);
            });

        const Result<void> stageResult =
            waitFor(stageService.send(validateRequest));

        QVERIFY(!stageResult.hasValue());
        QCOMPARE(
            stageResult.error().code,
            QStringLiteral("codex.attachments_unavailable"));
        QCOMPARE(
            stageResult.error().message,
            QStringLiteral(
                "Codex command attachments are unavailable."));
        verifySanitized(stageResult.error(), forbidden);
    }

    void settingsFailureIsRecordedButActionStillSends()
    {
        RecordingTaskBackend backend;
        backend.settingsOutcome = FollowerSendOutcome::Failed;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.model = QStringLiteral(" gpt-5.6 ");
        request.reasoningEffort = QStringLiteral(" high ");
        request.executionState =
            std::make_shared<SendExecutionState>();

        const Result<void> result =
            waitFor(service->send(request));

        QVERIFY(result.hasValue());
        QVERIFY(
            request.executionState
                ->retainedCurrentSettings.load());
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
        QCOMPARE(
            backend.order,
            QVector<QString>({
                QStringLiteral("validate"),
                QStringLiteral("stage"),
                QStringLiteral("settings"),
                QStringLiteral("queue"),
            }));
        QVERIFY(!backend.diagnostics.isEmpty());
        const auto settingsDiagnostic =
            std::find_if(
                backend.diagnostics.cbegin(),
                backend.diagnostics.cend(),
                [](const CommandDiagnostic& diagnostic) {
                    return diagnostic.method
                        == QStringLiteral("settings")
                        && diagnostic.outcome
                            == QStringLiteral("failed");
                });
        QVERIFY(settingsDiagnostic != backend.diagnostics.cend());
        QCOMPARE(
            settingsDiagnostic->outcome,
            QStringLiteral("failed"));
        QCOMPARE(
            settingsDiagnostic->threadId,
            QStringLiteral("thread-a"));
        QCOMPARE(settingsDiagnostic->attachmentCount, 1);
    }

    void requestedSettingsWithoutSenderRecordsFailureAndStillSends()
    {
        RecordingTaskBackend backend;
        std::atomic_int settingsFailures = 0;
        TaskCommandService service(
            [&backend](const QVector<BridgeAttachment>& attachments) {
                return backend.validate(attachments);
            },
            [&backend](
                const QVector<BridgeAttachment>& attachments,
                const QString& clientMessageId) {
                return backend.stage(attachments, clientMessageId);
            },
            CommandSettingsSender{},
            CommandSubmitter{},
            [&backend](
                QString prompt,
                QString threadId,
                QString clientMessageId,
                QString cwd,
                QVector<StagedAttachment> attachments) {
                return backend.queue(
                    std::move(prompt),
                    std::move(threadId),
                    std::move(clientMessageId),
                    std::move(cwd),
                    std::move(attachments));
            },
            [&settingsFailures](CommandDiagnostic diagnostic) {
                if (diagnostic.method == QStringLiteral("settings")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    settingsFailures.fetch_add(1);
                }
            });
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        const Result<void> result =
            waitFor(service.send(request));

        QVERIFY(result.hasValue());
        QCOMPARE(settingsFailures.load(), 1);
        QCOMPARE(backend.settingsCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void settingsSenderThrowRecordsFailureAndStillSends()
    {
        RecordingTaskBackend backend;
        backend.throwSettings = true;
        std::atomic_int settingsFailures = 0;
        backend.diagnosticHook =
            [&settingsFailures](const CommandDiagnostic& diagnostic) {
                if (diagnostic.method == QStringLiteral("settings")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    settingsFailures.fetch_add(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("SECRET_MODEL");

        const Result<void> result =
            waitFor(service->send(request));

        QVERIFY(result.hasValue());
        QCOMPARE(settingsFailures.load(), 1);
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void settingsExceptionalFutureRecordsFailureAndStillSends()
    {
        RecordingTaskBackend backend;
        backend.settingsExceptionalFuture = true;
        std::atomic_int settingsFailures = 0;
        backend.diagnosticHook =
            [&settingsFailures](const CommandDiagnostic& diagnostic) {
                if (diagnostic.method == QStringLiteral("settings")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    settingsFailures.fetch_add(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.reasoningEffort = QStringLiteral("SECRET_EFFORT");

        const Result<void> result =
            waitFor(service->send(request));

        QVERIFY(result.hasValue());
        QCOMPARE(settingsFailures.load(), 1);
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void malformedSettingsFutureRecordsFailureAndStillSends_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("default") << QStringLiteral("default");
        QTest::newRow("no-result") << QStringLiteral("no-result");
        QTest::newRow("canceled") << QStringLiteral("canceled");
    }

    void malformedSettingsFutureRecordsFailureAndStillSends()
    {
        QFETCH(QString, kind);
        RecordingTaskBackend backend;
        backend.settingsDefaultFuture =
            kind == QStringLiteral("default");
        backend.settingsNoResultFuture =
            kind == QStringLiteral("no-result");
        backend.settingsCanceledFuture =
            kind == QStringLiteral("canceled");
        std::atomic_int settingsFailures = 0;
        backend.diagnosticHook =
            [&settingsFailures](const CommandDiagnostic& diagnostic) {
                if (diagnostic.method == QStringLiteral("settings")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    settingsFailures.fetch_add(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        QFuture<Result<void>> future = service->send(request);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        const Result<void> result = future.result();

        QVERIFY(result.hasValue());
        QCOMPARE(settingsFailures.load(), 1);
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingAfterSettingsCompletionSkipsActionAndClearsInFlight()
    {
        RecordingTaskBackend backend;
        std::atomic_bool inSettingsDiagnostic = false;
        std::atomic_bool releaseDiagnostic = false;
        backend.diagnosticHook =
            [&inSettingsDiagnostic, &releaseDiagnostic](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("settings")) {
                    return;
                }
                if (diagnostic.outcome != QStringLiteral("sent")) {
                    return;
                }
                inSettingsDiagnostic.store(true);
                while (!releaseDiagnostic.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        auto handle =
            service->sendMutation(request);
        QTRY_VERIFY_WITH_TIMEOUT(
            inSettingsDiagnostic.load(),
            1000);

        handle.requestStopBeforeCommit();
        releaseDiagnostic.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingBeforeActionCommitSkipsLaunchAndClearsInFlight()
    {
        RecordingTaskBackend backend;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        backend.diagnosticHook =
            [&inCommit, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();
        SendRequest request = replyRequest();

        auto handle = service->sendMutation(request);
        QTRY_VERIFY_WITH_TIMEOUT(inCommit.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(backend.settingsCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 0);
        backend.diagnosticHook = {};
        request.model.clear();
        QVERIFY(waitFor(service->send(request)).hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingAfterActionClaimBeforePostCheckSkipsLaunch()
    {
        RecordingTaskBackend backend;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        backend.diagnosticHook =
            [&claimEstablished, &releaseClaim](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();

        auto handle =
            service->sendMutation(replyRequest());
        QTRY_VERIFY_WITH_TIMEOUT(claimEstablished.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(backend.queueCalls.load(), 0);
        backend.diagnosticHook = {};
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingAfterActionPostCheckKeepsLaunchCommitted()
    {
        RecordingTaskBackend backend;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        backend.diagnosticHook =
            [&committed, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("committed")) {
                    return;
                }
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();

        auto handle =
            service->sendMutation(replyRequest());
        QTRY_VERIFY_WITH_TIMEOUT(committed.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingAfterActionCommitKeepsLaunchCommittedAndFinishesOnce()
    {
        RecordingTaskBackend backend;
        std::atomic_bool inQueue = false;
        std::atomic_bool releaseQueue = false;
        backend.queueHook = [&inQueue, &releaseQueue] {
            inQueue.store(true);
            while (!releaseQueue.load()) {
                QThread::msleep(1);
            }
        };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();

        auto handle =
            service->sendMutation(replyRequest());
        QTRY_VERIFY_WITH_TIMEOUT(inQueue.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseQueue.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingBeforeSettingsCommitSkipsSettingsAndAction()
    {
        RecordingTaskBackend backend;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        backend.diagnosticHook =
            [&inCommit, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("settings")
                    || diagnostic.outcome
                        != QStringLiteral("commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        auto handle = service->sendMutation(request);
        const bool reached =
            waitUntil(
                [&inCommit] {
                    return inCommit.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(backend.settingsCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 0);
        backend.diagnosticHook = {};
        request.model.clear();
        QVERIFY(waitFor(service->send(request)).hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void cancellingAfterSettingsClaimBeforePostCheckSkipsSettingsAndAction()
    {
        RecordingTaskBackend backend;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        backend.diagnosticHook =
            [&claimEstablished, &releaseClaim](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("settings")
                    || diagnostic.outcome
                        != QStringLiteral("claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        auto handle = service->sendMutation(request);
        const bool reached =
            waitUntil(
                [&claimEstablished] {
                    return claimEstablished.load();
                },
                1000);
        if (!reached) {
            releaseClaim.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(backend.settingsCalls.load(), 0);
        QCOMPARE(backend.queueCalls.load(), 0);
    }

    void cancellingAfterSettingsCommitKeepsSettingsLaunchCommitted()
    {
        RecordingTaskBackend backend;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        backend.diagnosticHook =
            [&committed, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("settings")
                    || diagnostic.outcome
                        != QStringLiteral("committed")) {
                    return;
                }
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.ownedService();
        SendRequest request = replyRequest();
        request.model = QStringLiteral("gpt-5");

        auto handle = service->sendMutation(request);
        const bool reached =
            waitUntil(
                [&committed] {
                    return committed.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(backend.settingsCalls.load(), 1);
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void sendMutationPreCommitStopRollsBackOwnedAttachments()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto store =
            std::make_shared<AttachmentStore>(
                temporary.path());
        std::atomic_int queueCalls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        TaskCommandService service(
            [](const QVector<BridgeAttachment>& attachments) {
                return AttachmentStore::validate(attachments);
            },
            [store](
                const QVector<BridgeAttachment>& attachments,
                const QUuid& requestId) {
                return store->stageOwned(
                    attachments,
                    requestId);
            },
            {},
            [](QString,
               QString,
               SendAction,
               QString,
               QString,
               QVector<StagedAttachment>) {
                return readyFuture(
                    FollowerSendOutcome::Sent);
            },
            [&queueCalls](
                QString,
                QString,
                QString,
                QString,
                QVector<StagedAttachment>) {
                queueCalls.fetch_add(1);
                return readyFuture(
                    FollowerSendOutcome::Sent);
            },
            [&inCommit, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        SendRequest request = replyRequest();
        request.attachments = {bridgeAttachment()};
        const QUuid requestId(request.clientMessageId);
        const QString requestDirectory =
            QDir(temporary.path()).filePath(
                requestId.toString(
                    QUuid::WithoutBraces));

        auto handle = service.sendMutation(request);
        QTRY_VERIFY_WITH_TIMEOUT(inCommit.load(), 1000);
        QVERIFY(QFileInfo::exists(requestDirectory));

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(queueCalls.load(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(
            !QFileInfo::exists(requestDirectory),
            1000);
    }

    void sendMutationTransportFailureRollsBackOwnedAttachments()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto store =
            std::make_shared<AttachmentStore>(
                temporary.path());
        TaskCommandService service(
            [](const QVector<BridgeAttachment>& attachments) {
                return AttachmentStore::validate(attachments);
            },
            [store](
                const QVector<BridgeAttachment>& attachments,
                const QUuid& requestId) {
                return store->stageOwned(
                    attachments,
                    requestId);
            },
            {},
            {},
            {},
            {});
        SendRequest request = replyRequest();
        request.attachments = {bridgeAttachment()};
        const QUuid requestId(request.clientMessageId);
        const QString requestDirectory =
            QDir(temporary.path()).filePath(
                requestId.toString(
                    QUuid::WithoutBraces));

        const Result<void> result =
            waitFor(
                service.sendMutation(request)
                    .terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.send_failed"));
        QVERIFY(
            !QFileInfo::exists(requestDirectory));
    }

    void sendMutationCommitRetainsOwnedAttachments()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto store =
            std::make_shared<AttachmentStore>(
                temporary.path());
        std::atomic_int queueCalls = 0;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        TaskCommandService service(
            [](const QVector<BridgeAttachment>& attachments) {
                return AttachmentStore::validate(attachments);
            },
            [store](
                const QVector<BridgeAttachment>& attachments,
                const QUuid& requestId) {
                return store->stageOwned(
                    attachments,
                    requestId);
            },
            {},
            [](QString,
               QString,
               SendAction,
               QString,
               QString,
               QVector<StagedAttachment>) {
                return readyFuture(
                    FollowerSendOutcome::Sent);
            },
            [&queueCalls](
                QString,
                QString,
                QString,
                QString,
                QVector<StagedAttachment>) {
                queueCalls.fetch_add(1);
                return readyFuture(
                    FollowerSendOutcome::Sent);
            },
            [&committed, &releaseCommit](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("committed")) {
                    return;
                }
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        SendRequest request = replyRequest();
        request.attachments = {bridgeAttachment()};
        const QUuid requestId(request.clientMessageId);
        const QString requestDirectory =
            QDir(temporary.path()).filePath(
                requestId.toString(
                    QUuid::WithoutBraces));

        auto handle = service.sendMutation(request);
        QTRY_VERIFY_WITH_TIMEOUT(committed.load(), 1000);
        QVERIFY(QFileInfo::exists(requestDirectory));

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(queueCalls.load(), 1);
        QVERIFY(QFileInfo::exists(requestDirectory));
    }

    void steerWithoutActiveLifecycleFailsBeforeTransport()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = steerRequest();
        request.expectedTurnId.clear();

        const Result<void> result =
            waitFor(service->send(request));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.no_active_turn"));
        QCOMPARE(backend.validateCalls.load(), 0);
        QCOMPARE(backend.stageCalls.load(), 0);
        QCOMPARE(backend.submitCalls.load(), 0);
    }

    void queuedReplyEmitsOnlyAfterFollowerAcceptance()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTest::qWait(25);
        QCOMPARE(queuedSpy.size(), 0);

        manual.finish(FollowerSendOutcome::Sent);
        const Result<void> result = waitFor(future);
        QCoreApplication::processEvents();

        QVERIFY(result.hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
        QCOMPARE(
            queuedSpy.takeFirst().at(0).toString(),
            QStringLiteral("thread-a"));
    }

    void queuedReplyEmitsOnServiceThread()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QThread* const serviceThread = service->thread();
        QThread* observedThread = nullptr;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            service.get(),
            [&observedThread] {
                observedThread = QThread::currentThread();
            },
            Qt::DirectConnection);

        QVERIFY(waitFor(service->send(replyRequest())).hasValue());

        QTRY_VERIFY_WITH_TIMEOUT(observedThread != nullptr, 1000);
        QCOMPARE(observedThread, serviceThread);
    }

    void replyDispatcherDeferredDeleteStress()
    {
        for (int index = 0; index < 50; ++index) {
            auto service =
                std::make_unique<TaskCommandService>(
                    [](const QVector<BridgeAttachment>&) {
                        return Result<void>::success();
                    },
                    [](
                        const QVector<BridgeAttachment>&,
                        const QString&) {
                        return Result<QVector<StagedAttachment>>::success(
                            QVector<StagedAttachment>{
                                stagedAttachment(),
                            });
                    },
                    CommandSettingsSender{},
                    [](
                        QString,
                        QString,
                        SendAction,
                        QString,
                        QString,
                        QVector<StagedAttachment>) {
                        return readyFuture(FollowerSendOutcome::Sent);
                    },
                    [](
                        QString,
                        QString,
                        QString,
                        QString,
                        QVector<StagedAttachment>) {
                        return readyFuture(FollowerSendOutcome::Sent);
                    });
            QObject receiver;
            int signalCount = 0;
            QObject::connect(
                service.get(),
                &TaskCommandService::replyQueued,
                &receiver,
                [&signalCount] {
                    ++signalCount;
                });

            QFuture<Result<void>> future =
                service->send(replyRequest());

            QVERIFY(waitFor(future).hasValue());
            QCoreApplication::processEvents();
            QCOMPARE(signalCount, 1);
            service.reset();
            QCoreApplication::sendPostedEvents(
                nullptr,
                QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
    }

    void concurrentDuplicateForSameThreadIsRejectedUntilExit()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> first =
            service->send(replyRequest());
        QTest::qWait(25);
        const Result<void> duplicate =
            waitFor(service->send(replyRequest()));

        QVERIFY(!duplicate.hasValue());
        QCOMPARE(
            duplicate.error().code,
            QStringLiteral("codex.send_in_flight"));
        QCOMPARE(backend.queueCalls.load(), 1);

        manual.finish(FollowerSendOutcome::Sent);
        QVERIFY(waitFor(first).hasValue());
        backend.manualQueue = nullptr;

        const Result<void> recovered =
            waitFor(service->send(replyRequest()));
        QVERIFY(recovered.hasValue());
        QCOMPARE(backend.queueCalls.load(), 2);
    }

    void noFallbackOccursAfterAcceptedOrAmbiguousSteer()
    {
        RecordingTaskBackend accepted;
        accepted.submitOutcome = FollowerSendOutcome::Sent;
        std::unique_ptr<TaskCommandService> acceptedService =
            accepted.service();

        QVERIFY(waitFor(
                    acceptedService->send(steerRequest()))
                    .hasValue());
        QCOMPARE(accepted.submitCalls, 1);
        QCOMPARE(accepted.queueCalls, 0);

        RecordingTaskBackend ambiguous;
        ambiguous.submitOutcome = FollowerSendOutcome::TimedOut;
        std::unique_ptr<TaskCommandService> ambiguousService =
            ambiguous.service();
        const Result<void> timedOut =
            waitFor(ambiguousService->send(steerRequest()));

        QVERIFY(!timedOut.hasValue());
        QCOMPARE(
            timedOut.error().code,
            QStringLiteral("codex.send_timed_out"));
        QCOMPARE(ambiguous.submitCalls, 1);
        QCOMPARE(ambiguous.queueCalls, 0);
    }

    void clientMessageIdStaysIdenticalAcrossStagingAndTransport()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        SendRequest request = replyRequest();
        request.clientMessageId = QStringLiteral(
            "40000000-0000-0000-0000-000000000001");
        request.attachments = {bridgeAttachment()};

        QVERIFY(waitFor(service->send(request)).hasValue());

        QCOMPARE(backend.stagedMessageIds.size(), 1);
        QCOMPARE(backend.transportedMessageIds.size(), 1);
        QCOMPARE(backend.stagedMessageIds.first(), request.clientMessageId);
        QCOMPARE(
            backend.transportedMessageIds.first(),
            request.clientMessageId);
    }

    void queuedReplyCompletesWhenGlobalThreadPoolHasOneThread()
    {
        ScopedGlobalThreadLimit globalLimit(1);
        RecordingTaskBackend backend;
        backend.queueOnGlobalThreadPool = true;
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> future =
            service->send(replyRequest());

        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.result().hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void acceptedReplyCompletesWhenOriginalDispatcherThreadStops()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool completionObserved = false;
        std::atomic_int diagnosticsAfterCompletion = 0;
        std::unique_ptr<TaskCommandService> service =
            taskServiceWithDiagnosticHook(
                backend,
                [&completionObserved,
                 &diagnosticsAfterCompletion](
                    const CommandDiagnostic&) {
                    if (completionObserved.load()) {
                        diagnosticsAfterCompletion.fetch_add(1);
                    }
                });
        TaskCommandService* rawService = service.get();
        QThread originalThread;
        QThread targetThread;
        originalThread.start();
        targetThread.start();
        QVERIFY(originalThread.isRunning());
        QVERIFY(targetThread.isRunning());
        rawService->moveToThread(&originalThread);

        QFuture<Result<void>> future =
            rawService->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(
            backend.queueCalls.load(),
            1,
            1000);

        bool moved = false;
        const bool moveInvoked =
            QMetaObject::invokeMethod(
                rawService,
                [rawService, &targetThread, &moved] {
                    rawService->moveToThread(&targetThread);
                    moved = true;
                },
                Qt::BlockingQueuedConnection);
        originalThread.quit();
        const bool originalStopped =
            originalThread.wait(1000);

        manual.finish(FollowerSendOutcome::Sent);
        const bool finished =
            waitUntil(
                [&future] {
                    return future.isFinished();
                },
                1000);

        std::optional<Result<void>> result;
        std::optional<Result<void>> recovered;
        int diagnosticsAfterFirstCompletion = -1;
        if (finished) {
            result = future.result();
            completionObserved.store(true);
            QCoreApplication::processEvents();
            QTest::qWait(25);
            diagnosticsAfterFirstCompletion =
                diagnosticsAfterCompletion.load();
            completionObserved.store(false);
            backend.manualQueue = nullptr;
            recovered =
                waitFor(rawService->send(replyRequest()));
        }

        TaskCommandService* owned = service.release();
        bool deleted = false;
        const bool deleteInvoked =
            QMetaObject::invokeMethod(
                owned,
                [owned, &deleted] {
                    delete owned;
                    deleted = true;
                },
                Qt::BlockingQueuedConnection);
        targetThread.quit();
        const bool targetStopped = targetThread.wait(1000);

        QVERIFY(moveInvoked);
        QVERIFY(moved);
        QVERIFY(originalStopped);
        QVERIFY(finished);
        QVERIFY(result.has_value());
        QVERIFY(result->hasValue());
        QVERIFY(recovered.has_value());
        QVERIFY(recovered->hasValue());
        QCOMPARE(diagnosticsAfterFirstCompletion, 0);
        QVERIFY(deleteInvoked);
        QVERIFY(deleted);
        QVERIFY(targetStopped);
    }

    void cancelingLegacyQueuedReplyDoesNotCancelFollower()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool replyFinished = false;
        backend.diagnosticHook =
            [&replyFinished](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method == QStringLiteral("reply")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    replyFinished.store(true);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);

        future.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.isCanceled());
        QVERIFY(!manual.isCanceled());

        manual.finish(FollowerSendOutcome::Failed);
        QTRY_VERIFY_WITH_TIMEOUT(replyFinished.load(), 1000);

        QVERIFY(!manual.isCanceled());
        backend.manualQueue = nullptr;
        backend.diagnosticHook = {};
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCOMPARE(backend.queueCalls.load(), 2);
    }

    void cancelingLegacyObserverDoesNotWaitForFollower()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool replyFinished = false;
        backend.diagnosticHook =
            [&replyFinished](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method == QStringLiteral("reply")
                    && diagnostic.outcome
                        == QStringLiteral("failed")) {
                    replyFinished.store(true);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);

        future.cancel();
        const bool outerFinished =
            waitUntil(
                [&future] {
                    return future.isFinished();
                },
                1000);

        QVERIFY(outerFinished);
        QVERIFY(future.isCanceled());
        QVERIFY(!manual.isCanceled());

        manual.finish(FollowerSendOutcome::Failed);
        QTRY_VERIFY_WITH_TIMEOUT(replyFinished.load(), 1000);

        backend.manualQueue = nullptr;
        backend.diagnosticHook = {};
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCOMPARE(backend.queueCalls.load(), 2);
    }

    void cancelingLegacyAfterReplyAcceptanceKeepsQueuedSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool inReplyDiagnostic = false;
        std::atomic_bool releaseDiagnostic = false;
        backend.diagnosticHook =
            [&inReplyDiagnostic, &releaseDiagnostic](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method != QStringLiteral("reply")
                    || diagnostic.outcome
                        != QStringLiteral("sent")) {
                    return;
                }
                inReplyDiagnostic.store(true);
                while (!releaseDiagnostic.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        QTRY_VERIFY_WITH_TIMEOUT(
            inReplyDiagnostic.load(),
            1000);

        future.cancel();
        releaseDiagnostic.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);

        QCOMPARE(future.resultCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
        backend.manualQueue = nullptr;
        backend.diagnosticHook = {};
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            2,
            1000);
    }

    void cancelingLegacyBeforeReplyDispatchKeepsQueuedSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool inPost = false;
        std::atomic_bool releasePost = false;
        backend.diagnosticHook =
            [&inPost, &releasePost](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome
                        != QStringLiteral("postPending")) {
                    return;
                }
                inPost.store(true);
                while (!releasePost.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        std::jthread canceler(
            cancelFutureWhenFlagIsSet,
            future,
            std::ref(inPost),
            std::ref(releasePost));
        QTRY_VERIFY_WITH_TIMEOUT(inPost.load(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QCoreApplication::processEvents();

        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
        backend.manualQueue = nullptr;
        backend.diagnosticHook = {};
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            2,
            1000);
    }

    void cancelingLegacyAfterReplyDispatchKeepsQueuedSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool posted = false;
        std::atomic_bool releasePosted = false;
        backend.diagnosticHook =
            [&posted, &releasePosted](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome
                        != QStringLiteral("postDeferred")) {
                    return;
                }
                posted.store(true);
                while (!releasePosted.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        const bool reached =
            waitUntilNoEvents(
                [&posted] {
                    return posted.load();
                },
                1000);
        if (!reached) {
            releasePosted.store(true);
        }
        QVERIFY(reached);
        future.cancel();
        releasePosted.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QCoreApplication::processEvents();

        QCOMPARE(future.resultCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
    }

    void cancelingLegacyAfterReplyEligibilityKeepsQueuedSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool dispatchDecision = false;
        std::atomic_bool releaseDispatchDecision = false;
        backend.diagnosticHook =
            [&dispatchDecision, &releaseDispatchDecision](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome
                        == QStringLiteral("postPending")) {
                    return;
                }
                dispatchDecision.store(true);
                while (!releaseDispatchDecision.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        const bool reached =
            waitUntilNoEvents(
                [&dispatchDecision] {
                    return dispatchDecision.load();
                },
                1000);
        if (!reached) {
            releaseDispatchDecision.store(true);
        }
        QVERIFY(reached);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        future.cancel();
        releaseDispatchDecision.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(future.resultCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
    }

    void cancelingLegacyAtReplyCompletionKeepsQueuedSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool hookEnabled = true;
        std::atomic_bool beforeAddResult = false;
        std::atomic_bool releaseBeforeAddResult = false;
        backend.completionHook =
            [&hookEnabled,
             &beforeAddResult,
             &releaseBeforeAddResult](CommandCompletionPhase phase) {
                if (!hookEnabled.load()
                    || phase
                        != CommandCompletionPhase::BeforeAddResult) {
                    return;
                }
                beforeAddResult.store(true);
                while (!releaseBeforeAddResult.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        const bool reached =
            waitUntilNoEvents(
                [&beforeAddResult] {
                    return beforeAddResult.load();
                },
                1000);
        if (!reached) {
            releaseBeforeAddResult.store(true);
        }
        QVERIFY(reached);

        future.cancel();
        releaseBeforeAddResult.store(true);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(future.resultCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);

        hookEnabled.store(false);
        backend.manualQueue = nullptr;
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCoreApplication::processEvents();
        QCOMPARE(backend.queueCalls.load(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            2,
            1000);
    }

    void cancelingLegacyAfterTerminalCompletionKeepsSingleSignal()
    {
        RecordingTaskBackend backend;
        ManualFollowerOutcome manual;
        backend.manualQueue = &manual;
        std::atomic_bool afterAddResult = false;
        std::atomic_bool releaseAfterAddResult = false;
        std::atomic_int afterAddResultCalls = 0;
        backend.completionHook =
            [&afterAddResult,
             &releaseAfterAddResult,
             &afterAddResultCalls](CommandCompletionPhase phase) {
                if (phase
                    != CommandCompletionPhase::AfterAddResultBeforeFinish) {
                    return;
                }
                afterAddResultCalls.fetch_add(1);
                afterAddResult.store(true);
                while (!releaseAfterAddResult.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);
        QTRY_COMPARE_WITH_TIMEOUT(backend.queueCalls.load(), 1, 1000);
        manual.finish(FollowerSendOutcome::Sent);
        const bool reached =
            waitUntilNoEvents(
                [&afterAddResult] {
                    return afterAddResult.load();
                },
                1000);
        if (!reached) {
            releaseAfterAddResult.store(true);
        }
        QVERIFY(reached);

        future.cancel();
        releaseAfterAddResult.store(true);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(afterAddResultCalls.load(), 1);
        QVERIFY(
            future.resultCount() == 0
            || future.resultCount() == 1);
        if (future.resultCount() == 1) {
            QVERIFY(future.result().hasValue());
        }
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);

        future.cancel();
        QCoreApplication::processEvents();
        QTest::qWait(25);
        QCOMPARE(finishedSpy.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            queuedSpy.size(),
            1,
            1000);
    }

    void cancellingAfterReplyDispatchCompletionDoesNotDuplicateSignalOrCompletion()
    {
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QSignalSpy queuedSpy(
            service.get(),
            &TaskCommandService::replyQueued);

        QFuture<Result<void>> future = service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        future.cancel();
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(future.resultCount(), 1);
        QVERIFY(future.result().hasValue());
        QCOMPARE(queuedSpy.size(), 1);
    }

    void replyQueuedCompletesWhenServiceDestroyedBeforeDispatchPost()
    {
        RecordingTaskBackend backend;
        std::atomic_bool postPending = false;
        std::atomic_bool releasePost = false;
        backend.diagnosticHook =
            [&postPending, &releasePost](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome
                        != QStringLiteral("postPending")) {
                    return;
                }
                postPending.store(true);
                while (!releasePost.load()) {
                    QThread::msleep(1);
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QObject receiver;
        int signalCount = 0;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            &receiver,
            [&signalCount] {
                ++signalCount;
            });

        QFuture<Result<void>> future = service->send(replyRequest());
        const bool reached =
            waitUntil(
                [&postPending] {
                    return postPending.load();
                },
                1000);
        if (!reached) {
            releasePost.store(true);
        }
        QVERIFY(reached);

        service.reset();
        releasePost.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QCoreApplication::processEvents();

        QCOMPARE(signalCount, 0);
    }

    void replyQueuedCompletesWhenServiceDestroyedAfterDispatchPost()
    {
        RecordingTaskBackend backend;
        std::atomic_bool posted = false;
        std::atomic_bool releasePosted = false;
        std::unique_ptr<TaskCommandService> service;
        backend.diagnosticHook =
            [&posted, &releasePosted](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome
                        != QStringLiteral("postDeferred")) {
                    return;
                }
                posted.store(true);
                while (!releasePosted.load()) {
                    QThread::msleep(1);
                }
            };
        service =
            backend.service();
        QObject receiver;
        int signalCount = 0;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            &receiver,
            [&signalCount] {
                ++signalCount;
            });

        QFuture<Result<void>> future = service->send(replyRequest());
        const bool reached =
            waitUntilNoEvents(
                [&posted] {
                    return posted.load();
                },
                1000);
        if (!reached) {
            releasePosted.store(true);
        }
        QVERIFY(reached);

        service.reset();
        releasePosted.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QCoreApplication::processEvents();

        QCOMPARE(signalCount, 0);
    }

    void replyQueuedDispatchDiagnosticsCompleteBeforePublicFuture()
    {
        RecordingTaskBackend backend;
        std::atomic_bool completionObserved = false;
        std::atomic_int diagnosticsAfterCompletion = 0;
        auto capture = std::make_shared<int>(1);
        std::weak_ptr<int> weakCapture = capture;
        std::mutex mutex;
        QVector<QString> order;
        QThread* postedThread = nullptr;
        backend.diagnosticHook =
            [&completionObserved,
             &diagnosticsAfterCompletion,
             weakCapture,
             &mutex,
             &order,
             &postedThread](const CommandDiagnostic& diagnostic) {
                if (completionObserved.load()
                    || weakCapture.expired()) {
                    diagnosticsAfterCompletion.fetch_add(1);
                }
                const QString entry =
                    diagnostic.method
                    + QLatin1Char(':')
                    + diagnostic.outcome;
                const std::scoped_lock lock(mutex);
                order.append(entry);
                if (diagnostic.method
                        == QStringLiteral("replyQueuedDispatch")
                    && diagnostic.outcome
                        == QStringLiteral("postDeferred")) {
                    postedThread = QThread::currentThread();
                }
            };
        std::unique_ptr<TaskCommandService> service =
            backend.service();
        QThread* const ownerThread = service->thread();

        const Result<void> result =
            waitFor(service->send(replyRequest()));
        completionObserved.store(true);
        capture.reset();
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QVERIFY(result.hasValue());
        QCOMPARE(diagnosticsAfterCompletion.load(), 0);
        QVector<QString> observed;
        QThread* observedPostedThread = nullptr;
        {
            const std::scoped_lock lock(mutex);
            observed = order;
            observedPostedThread = postedThread;
        }
        QVERIFY(observedPostedThread != nullptr);
        QVERIFY(observedPostedThread != ownerThread);
        const int postedIndex =
            observed.indexOf(
                QStringLiteral("replyQueuedDispatch:postDeferred"));
        const int commitIndex =
            observed.indexOf(
                QStringLiteral("reply:sent"));
        QVERIFY(postedIndex >= 0);
        QVERIFY(commitIndex >= 0);
        QVERIFY(commitIndex < postedIndex);
    }

    void replyQueuedCommitDiagnosticDestroysServiceSuppressesSignalAndCompletes_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("postDeferred")
            << QStringLiteral("postDeferred");
    }

    void replyQueuedCommitDiagnosticDestroysServiceSuppressesSignalAndCompletes()
    {
        QFETCH(QString, phase);
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service;
        std::atomic_bool posted = false;
        std::atomic_bool releasePosted = false;
        std::function<void(const CommandDiagnostic&)> diagnosticHook =
            [&posted, &releasePosted, &phase](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome != phase) {
                    return;
                }
                posted.store(true);
                while (!releasePosted.load()) {
                    QThread::msleep(1);
                }
            };
        service = taskServiceWithDiagnosticHook(
            backend,
            std::move(diagnosticHook));
        QObject receiver;
        int signalCount = 0;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            &receiver,
            [&signalCount] {
                ++signalCount;
            },
            Qt::DirectConnection);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);

        const bool reached =
            waitUntilNoEvents(
                [&posted] {
                    return posted.load();
                },
                1000);
        if (!reached) {
            releasePosted.store(true);
        }
        QVERIFY(reached);
        service.reset();
        releasePosted.store(true);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(future.result().hasValue());
        QCOMPARE(signalCount, 0);
    }

    void replyQueuedCommitDiagnosticMovesServiceSuppressesStaleThreadSignal_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("postDeferred")
            << QStringLiteral("postDeferred");
    }

    void replyQueuedCommitDiagnosticMovesServiceSuppressesStaleThreadSignal()
    {
        QFETCH(QString, phase);
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service;
        QThread targetThread;
        targetThread.start();
        QVERIFY(targetThread.isRunning());
        std::atomic_bool posted = false;
        std::atomic_bool releasePosted = false;
        std::function<void(const CommandDiagnostic&)> diagnosticHook =
            [&posted, &releasePosted, &phase](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome != phase) {
                    return;
                }
                posted.store(true);
                while (!releasePosted.load()) {
                    QThread::msleep(1);
                }
            };
        service = taskServiceWithDiagnosticHook(
            backend,
            std::move(diagnosticHook));
        QThread* const originalThread = service->thread();
        QObject receiver;
        int signalCount = 0;
        QThread* observedThread = nullptr;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            &receiver,
            [&signalCount, &observedThread] {
                ++signalCount;
                observedThread = QThread::currentThread();
            },
            Qt::DirectConnection);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);

        const bool reached =
            waitUntilNoEvents(
                [&posted] {
                    return posted.load();
                },
                1000);
        if (!reached) {
            releasePosted.store(true);
        }
        QVERIFY(reached);
        service->moveToThread(&targetThread);
        releasePosted.store(true);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(future.result().hasValue());
        const int staleSignalCount = signalCount;
        QThread* const staleObservedThread = observedThread;

        backend.diagnosticHook = {};
        bool movedBack = false;
        const bool invoked = QMetaObject::invokeMethod(
            service.get(),
            [service = service.get(),
             originalThread,
             &movedBack] {
                service->moveToThread(originalThread);
                movedBack = true;
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(invoked);
        QVERIFY(movedBack);
        targetThread.quit();
        QVERIFY(targetThread.wait(1000));

        QCOMPARE(staleSignalCount, 0);
        QCOMPARE(staleObservedThread, nullptr);

        const Result<void> recovered =
            waitFor(service->send(replyRequest()));
        QVERIFY(recovered.hasValue());
        QCoreApplication::processEvents();
        QCOMPARE(backend.queueCalls.load(), 2);
        QCOMPARE(signalCount, 1);
        QCOMPARE(observedThread, originalThread);
    }

    void replyQueuedCommitDiagnosticMovesThenDeletesServiceSuppressesSignalAndCompletes_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("postDeferred")
            << QStringLiteral("postDeferred");
    }

    void replyQueuedCommitDiagnosticMovesThenDeletesServiceSuppressesSignalAndCompletes()
    {
        QFETCH(QString, phase);
        RecordingTaskBackend backend;
        std::unique_ptr<TaskCommandService> service;
        QThread targetThread;
        targetThread.start();
        QVERIFY(targetThread.isRunning());
        std::atomic_bool posted = false;
        std::atomic_bool releasePosted = false;
        std::atomic_bool moved = false;
        std::atomic_bool destroyed = false;
        std::atomic_bool deletePosted = false;
        QObject destroyedReceiver;
        std::function<void(const CommandDiagnostic&)> diagnosticHook =
            [&posted, &releasePosted, &phase](
                const CommandDiagnostic& diagnostic) {
                if (diagnostic.method
                        != QStringLiteral("replyQueuedDispatch")
                    || diagnostic.outcome != phase) {
                    return;
                }
                posted.store(true);
                while (!releasePosted.load()) {
                    QThread::msleep(1);
                }
            };
        service = taskServiceWithDiagnosticHook(
            backend,
            std::move(diagnosticHook));
        QObject receiver;
        int signalCount = 0;
        QObject::connect(
            service.get(),
            &TaskCommandService::replyQueued,
            &receiver,
            [&signalCount] {
                ++signalCount;
            },
            Qt::DirectConnection);

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QFutureWatcher<Result<void>> watcher;
        QSignalSpy finishedSpy(&watcher, SIGNAL(finished()));
        watcher.setFuture(future);

        const bool reached =
            waitUntilNoEvents(
                [&posted] {
                    return posted.load();
                },
                1000);
        if (!reached) {
            releasePosted.store(true);
        }
        QVERIFY(reached);
        TaskCommandService* raw = service.release();
        QObject::connect(
            raw,
            &QObject::destroyed,
            &destroyedReceiver,
            [&destroyed] {
                destroyed.store(true);
            },
            Qt::DirectConnection);
        raw->moveToThread(&targetThread);
        moved.store(true);
        deletePosted.store(
            QMetaObject::invokeMethod(
                raw,
                [raw] {
                    delete raw;
                },
                Qt::QueuedConnection));
        releasePosted.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(moved.load(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(deletePosted.load(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(destroyed.load(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(future.result().hasValue());
        QCOMPARE(signalCount, 0);
        targetThread.quit();
        QVERIFY(targetThread.wait(1000));
    }

    void taskExceptionsCompleteWithStableFailureAndClearInFlight()
    {
        std::atomic_int validateCalls = 0;
        std::atomic_int queueCalls = 0;
        TaskCommandService service(
            [&validateCalls](const QVector<BridgeAttachment>&) {
                validateCalls.fetch_add(1);
                if (validateCalls == 1) {
                    throw std::runtime_error("validator leaked secret");
                }
                return Result<void>::success();
            },
            [](
                const QVector<BridgeAttachment>&,
                const QString&) {
                return Result<QVector<StagedAttachment>>::success(
                    QVector<StagedAttachment>{stagedAttachment()});
            },
            CommandSettingsSender{},
            [](
                QString,
                QString,
                SendAction,
                QString,
                QString,
                QVector<StagedAttachment>) {
                return readyFuture(FollowerSendOutcome::Sent);
            },
            [&queueCalls](
                QString,
                QString,
                QString,
                QString,
                QVector<StagedAttachment>) {
                queueCalls.fetch_add(1);
                if (queueCalls == 1) {
                    throw std::runtime_error("sender leaked secret");
                }
                return readyFuture(FollowerSendOutcome::Sent);
            });

        const Result<void> validatorResult =
            waitFor(service.send(replyRequest()));
        QVERIFY(!validatorResult.hasValue());
        QCOMPARE(
            validatorResult.error().code,
            QStringLiteral("codex.send_failed"));
        QVERIFY(!validatorResult.error().message.contains(
            QStringLiteral("secret")));

        const Result<void> senderResult =
            waitFor(service.send(replyRequest()));
        QVERIFY(!senderResult.hasValue());
        QCOMPARE(
            senderResult.error().code,
            QStringLiteral("codex.send_failed"));
        QVERIFY(!senderResult.error().message.contains(
            QStringLiteral("secret")));

        QVERIFY(waitFor(service.send(replyRequest())).hasValue());
        QCOMPARE(validateCalls.load(), 3);
        QCOMPARE(queueCalls.load(), 2);
    }

    void taskFollowerFutureExceptionCompletesWithStableFailure()
    {
        RecordingTaskBackend backend;
        backend.queueExceptionalFuture = true;
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        const Result<void> failed =
            waitFor(service->send(replyRequest()));

        QVERIFY(!failed.hasValue());
        QCOMPARE(
            failed.error().code,
            QStringLiteral("codex.send_failed"));
        QVERIFY(!failed.error().message.contains(
            QStringLiteral("sensitive")));

        backend.queueExceptionalFuture = false;
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCOMPARE(backend.queueCalls.load(), 2);
    }

    void malformedActionFutureFailsAndClearsInFlight_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("default") << QStringLiteral("default");
        QTest::newRow("no-result") << QStringLiteral("no-result");
        QTest::newRow("canceled") << QStringLiteral("canceled");
    }

    void malformedActionFutureFailsAndClearsInFlight()
    {
        QFETCH(QString, kind);
        RecordingTaskBackend backend;
        backend.queueDefaultFuture =
            kind == QStringLiteral("default");
        backend.queueNoResultFuture =
            kind == QStringLiteral("no-result");
        backend.queueCanceledFuture =
            kind == QStringLiteral("canceled");
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> future =
            service->send(replyRequest());
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        const Result<void> failed = future.result();

        QVERIFY(!failed.hasValue());
        QCOMPARE(
            failed.error().code,
            QStringLiteral("codex.send_failed"));
        backend.queueDefaultFuture = false;
        backend.queueNoResultFuture = false;
        backend.queueCanceledFuture = false;
        QVERIFY(waitFor(service->send(replyRequest())).hasValue());
        QCOMPARE(backend.queueCalls.load(), 2);
    }

    void malformedSteerFutureFailsWithStableSendFailure()
    {
        RecordingTaskBackend backend;
        backend.submitDefaultFuture = true;
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        QFuture<Result<void>> future =
            service->send(steerRequest());
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        const Result<void> failed = future.result();

        QVERIFY(!failed.hasValue());
        QCOMPARE(
            failed.error().code,
            QStringLiteral("codex.send_failed"));
        backend.submitDefaultFuture = false;
        QVERIFY(waitFor(service->send(steerRequest())).hasValue());
        QCOMPARE(backend.submitCalls.load(), 2);
    }

    void taskDiagnosticExceptionsDoNotBreakCompletion()
    {
        RecordingTaskBackend backend;
        backend.diagnosticHook = [](const CommandDiagnostic&) {
            throw std::runtime_error("diagnostic leaked secret");
        };
        std::unique_ptr<TaskCommandService> service =
            backend.service();

        const Result<void> result =
            waitFor(service->send(replyRequest()));

        QVERIFY(result.hasValue());
        QCOMPARE(backend.queueCalls.load(), 1);
    }

    void approvalParsesMethodsAndMapsDecisions()
    {
        const std::optional<PendingApproval> command =
            ApprovalService::pendingApprovalFromNotification(
                QJsonObject{
                    {QStringLiteral("id"), 91},
                    {
                        QStringLiteral("method"),
                        QStringLiteral(
                            "item/commandExecution/requestApproval"),
                    },
                    {
                        QStringLiteral("params"),
                        QJsonObject{
                            {
                                QStringLiteral("threadId"),
                                QStringLiteral("thread-a"),
                            },
                            {
                                QStringLiteral(
                                    "proposedExecpolicyAmendment"),
                                QJsonArray{
                                    QStringLiteral("git"),
                                    QStringLiteral("status"),
                                },
                            },
                        },
                    },
                });
        QVERIFY(command.has_value());
        QCOMPARE(
            command->method,
            PendingApprovalMethod::CommandExecution);
        QCOMPARE(command->requestId, 91);
        QCOMPARE(
            command->proposedExecpolicyAmendment.value(),
            QVector<QString>({
                QStringLiteral("git"),
                QStringLiteral("status"),
            }));

        const std::optional<PendingApproval> file =
            ApprovalService::pendingApprovalFromNotification(
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("17")},
                    {
                        QStringLiteral("method"),
                        QStringLiteral(
                            "item/fileChange/requestApproval"),
                    },
                    {
                        QStringLiteral("params"),
                        QJsonObject{
                            {
                                QStringLiteral("threadId"),
                                QStringLiteral("thread-b"),
                            },
                        },
                    },
                });
        QVERIFY(file.has_value());
        QCOMPARE(file->method, PendingApprovalMethod::FileChange);

        const QJsonValue once =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                *command,
                ApprovalDecision::ApproveOnce)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"));
        QCOMPARE(once.toString(), QStringLiteral("accept"));

        const QJsonObject similar =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                *command,
                ApprovalDecision::ApproveSimilar)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"))
                .toObject()
                .value(QStringLiteral(
                    "acceptWithExecpolicyAmendment"))
                .toObject();
        QCOMPARE(
            similar.value(QStringLiteral("execpolicy_amendment"))
                .toArray()
                .at(0)
                .toString(),
            QStringLiteral("git"));

        const QJsonValue decline =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                *file,
                ApprovalDecision::Decline)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"));
        QCOMPARE(decline.toString(), QStringLiteral("decline"));
    }

    void approvalRejectsMixedAmendmentArrayForApproveSimilarFallback()
    {
        const std::optional<PendingApproval> approval =
            ApprovalService::pendingApprovalFromNotification(
                QJsonObject{
                    {QStringLiteral("id"), 91},
                    {
                        QStringLiteral("method"),
                        QStringLiteral(
                            "item/commandExecution/requestApproval"),
                    },
                    {
                        QStringLiteral("params"),
                        QJsonObject{
                            {
                                QStringLiteral("threadId"),
                                QStringLiteral("thread-a"),
                            },
                            {
                                QStringLiteral(
                                    "proposedExecpolicyAmendment"),
                                QJsonArray{
                                    QStringLiteral("git"),
                                    5,
                                    QStringLiteral("status"),
                                },
                            },
                        },
                    },
                });

        QVERIFY(approval.has_value());
        QVERIFY(!approval->proposedExecpolicyAmendment.has_value());
        const QJsonValue decision =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                *approval,
                ApprovalDecision::ApproveSimilar)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"));
        QCOMPARE(
            decision.toString(),
            QStringLiteral("acceptForSession"));
    }

    void approvalEmptyCamelAmendmentWinsOverSnake()
    {
        const std::optional<PendingApproval> approval =
            ApprovalService::pendingApprovalFromNotification(
                QJsonObject{
                    {QStringLiteral("id"), 91},
                    {
                        QStringLiteral("method"),
                        QStringLiteral(
                            "item/commandExecution/requestApproval"),
                    },
                    {
                        QStringLiteral("params"),
                        QJsonObject{
                            {
                                QStringLiteral("threadId"),
                                QStringLiteral("thread-a"),
                            },
                            {
                                QStringLiteral(
                                    "proposedExecpolicyAmendment"),
                                QJsonArray{},
                            },
                            {
                                QStringLiteral(
                                    "proposed_execpolicy_amendment"),
                                QJsonArray{
                                    QStringLiteral("dangerous"),
                                },
                            },
                        },
                    },
                });

        QVERIFY(approval.has_value());
        QVERIFY(!approval->proposedExecpolicyAmendment.has_value());
        const QJsonValue decision =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                *approval,
                ApprovalDecision::ApproveSimilar)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"));
        QCOMPARE(
            decision.toString(),
            QStringLiteral("acceptForSession"));
    }

    void approvalInvalidCamelAmendmentFallsBackToSnake()
    {
        const std::optional<PendingApproval> approval =
            ApprovalService::pendingApprovalFromNotification(
                QJsonObject{
                    {QStringLiteral("id"), 91},
                    {
                        QStringLiteral("method"),
                        QStringLiteral(
                            "item/commandExecution/requestApproval"),
                    },
                    {
                        QStringLiteral("params"),
                        QJsonObject{
                            {
                                QStringLiteral("threadId"),
                                QStringLiteral("thread-a"),
                            },
                            {
                                QStringLiteral(
                                    "proposedExecpolicyAmendment"),
                                QJsonArray{
                                    QStringLiteral("git"),
                                    5,
                                },
                            },
                            {
                                QStringLiteral(
                                    "proposed_execpolicy_amendment"),
                                QJsonArray{
                                    QStringLiteral("fallback"),
                                },
                            },
                        },
                    },
                });

        QVERIFY(approval.has_value());
        QVERIFY(approval->proposedExecpolicyAmendment.has_value());
        QCOMPARE(
            approval->proposedExecpolicyAmendment.value(),
            QVector<QString>({QStringLiteral("fallback")}));
    }

    void approvalRequestNotFoundMapsAndRemovalWaitsForAcceptance()
    {
        QVector<FollowerApprovalOutcome> outcomes{
            FollowerApprovalOutcome::RequestNotFound,
            FollowerApprovalOutcome::Approved,
        };
        QVector<PendingApproval> removed;
        ApprovalService service(
            [&outcomes](
                PendingApproval,
                ApprovalDecision) {
                const FollowerApprovalOutcome outcome =
                    outcomes.takeFirst();
                return readyFuture(outcome);
            },
            [&removed](PendingApproval request) {
                removed.append(std::move(request));
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        const Result<void> missing = waitFor(
            service.respond(
                request,
                ApprovalDecision::ApproveOnce));

        QVERIFY(!missing.hasValue());
        QCOMPARE(
            missing.error().code,
            QStringLiteral("approval.request_not_found"));
        QCOMPARE(removed.size(), 0);

        const Result<void> accepted = waitFor(
            service.respond(
                request,
                ApprovalDecision::ApproveOnce));

        QVERIFY(accepted.hasValue());
        QCOMPARE(removed, QVector<PendingApproval>({request}));
    }

    void approvalCompletesWhenGlobalThreadPoolHasOneThread()
    {
        ScopedGlobalThreadLimit globalLimit(1);
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };
        ApprovalService service(
            [](PendingApproval, ApprovalDecision) {
                return globalPoolPromiseFuture(
                    FollowerApprovalOutcome::Approved);
            });

        QFuture<Result<void>> future =
            service.respond(
                request,
                ApprovalDecision::ApproveOnce);

        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.result().hasValue());
    }

    void cancellingBeforeApprovalResponderCommitSkipsResponder()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        ApprovalService service(
            [&calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            {},
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral("responder.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        const bool reached =
            waitUntil(
                [&inCommit] {
                    return inCommit.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterApprovalResponderClaimSkipsResponder()
    {
        std::atomic_int calls = 0;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        ApprovalService service(
            [&calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            {},
            [&claimEstablished, &releaseClaim](
                const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "responder.claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        const bool reached =
            waitUntil(
                [&claimEstablished] {
                    return claimEstablished.load();
                },
                1000);
        if (!reached) {
            releaseClaim.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterApprovalResponderCommitKeepsLaunchCommitted()
    {
        std::atomic_int calls = 0;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        ApprovalService service(
            [&calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            {},
            [&committed, &releaseCommit](
                const QString& phase) {
                if (phase
                    != QStringLiteral("responder.committed")) {
                    return;
                }
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        const bool reached =
            waitUntil(
                [&committed] {
                    return committed.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(calls.load(), 1);
    }

    void cancelingLegacyApprovalFutureDoesNotStopMutation()
    {
        ManualApprovalOutcome manual;
        std::atomic_int calls = 0;
        QVector<PendingApproval> removed;
        ApprovalService service(
            [&manual, &calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return manual.future();
            },
            [&removed](PendingApproval request) {
                removed.append(std::move(request));
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        QFuture<Result<void>> future =
            service.respond(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);

        future.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.isCanceled());
        QVERIFY(!manual.isCanceled());

        manual.finish(FollowerApprovalOutcome::Approved);
        QTRY_COMPARE_WITH_TIMEOUT(removed.size(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);

        QVERIFY(!manual.isCanceled());
        QCOMPARE(removed, QVector<PendingApproval>({request}));
    }

    void cancellingNonCooperativeApprovalDoesNotWaitForFollower()
    {
        ManualApprovalOutcome manual;
        ManualApprovalOutcome* activeManual = &manual;
        std::atomic_int calls = 0;
        QVector<PendingApproval> removed;
        ApprovalService service(
            [&activeManual, &calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                if (activeManual != nullptr) {
                    return activeManual->future();
                }
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            [&removed](PendingApproval request) {
                removed.append(std::move(request));
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        QFuture<Result<void>> future =
            service.respond(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);

        future.cancel();
        const bool outerFinished =
            waitUntil(
                [&future] {
                    return future.isFinished();
                },
                1000);

        QVERIFY(outerFinished);
        QVERIFY(future.isCanceled());
        QVERIFY(!manual.isCanceled());
        QCOMPARE(removed.size(), 0);

        manual.finish(FollowerApprovalOutcome::Approved);
        QTRY_COMPARE_WITH_TIMEOUT(removed.size(), 1, 1000);
        QVERIFY(!manual.isCanceled());

        activeManual = nullptr;
        QVERIFY(waitFor(
                    service.respond(
                        request,
                        ApprovalDecision::ApproveOnce))
                    .hasValue());
        QCOMPARE(calls.load(), 2);
        QCOMPARE(removed.size(), 2);
    }

    void cancellingBeforeApprovalRemovalCommitSkipsRemoval()
    {
        ManualApprovalOutcome manual;
        std::atomic_int calls = 0;
        QVector<PendingApproval> removed;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        ApprovalService service(
            [&manual, &calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return manual.future();
            },
            [&removed](PendingApproval request) {
                removed.append(std::move(request));
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase != QStringLiteral("commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);
        manual.finish(FollowerApprovalOutcome::Approved);
        QTRY_VERIFY_WITH_TIMEOUT(inCommit.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(removed.size(), 1);
    }

    void cancellingAfterApprovalRemovalClaimBeforePostCheckSkipsRemoval()
    {
        ManualApprovalOutcome manual;
        std::atomic_int calls = 0;
        QVector<PendingApproval> removed;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        ApprovalService service(
            [&manual, &calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                return manual.future();
            },
            [&removed](PendingApproval request) {
                removed.append(std::move(request));
            },
            [&claimEstablished, &releaseClaim](
                const QString& phase) {
                if (phase
                    != QStringLiteral("claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);
        manual.finish(FollowerApprovalOutcome::Approved);
        QTRY_VERIFY_WITH_TIMEOUT(
            claimEstablished.load(),
            1000);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(removed.size(), 1);
    }

    void cancellingAfterApprovalRemovalPostCheckRemovesAndFinishesOnce()
    {
        int removals = 0;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        ApprovalService service(
            [](
                PendingApproval,
                ApprovalDecision) {
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            [&removals](PendingApproval) {
                ++removals;
            },
            [&committed, &releaseCommit](
                const QString& phase) {
                if (phase != QStringLiteral("committed")) {
                    return;
                }
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_VERIFY_WITH_TIMEOUT(committed.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(removals, 1);
    }

    void cancellingAfterApprovalRemovalCommitRemovesAndFinishesOnce()
    {
        int removals = 0;
        std::atomic_bool inRemoval = false;
        std::atomic_bool releaseRemoval = false;
        ApprovalService service(
            [](
                PendingApproval,
                ApprovalDecision) {
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            [&removals, &inRemoval, &releaseRemoval](
                PendingApproval) {
                ++removals;
                inRemoval.store(true);
                while (!releaseRemoval.load()) {
                    QThread::msleep(1);
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        auto handle =
            service.respondMutation(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_VERIFY_WITH_TIMEOUT(inRemoval.load(), 1000);

        handle.requestStopBeforeCommit();
        releaseRemoval.store(true);
        const Result<void> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(removals, 1);
    }

    void approvalExceptionsCompleteWithStableFailure()
    {
        int responderCalls = 0;
        int removals = 0;
        ApprovalService service(
            [&responderCalls](
                PendingApproval,
                ApprovalDecision) {
                ++responderCalls;
                if (responderCalls == 1) {
                    throw std::runtime_error(
                        "responder leaked secret");
                }
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            },
            [&removals](PendingApproval) {
                ++removals;
                if (removals == 1) {
                    throw std::runtime_error(
                        "removal leaked secret");
                }
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        const Result<void> responderResult =
            waitFor(service.respond(
                request,
                ApprovalDecision::ApproveOnce));
        QVERIFY(!responderResult.hasValue());
        QCOMPARE(
            responderResult.error().code,
            QStringLiteral("approval.failed"));
        QVERIFY(!responderResult.error().message.contains(
            QStringLiteral("secret")));

        const Result<void> removalResult =
            waitFor(service.respond(
                request,
                ApprovalDecision::ApproveOnce));
        QVERIFY(!removalResult.hasValue());
        QCOMPARE(
            removalResult.error().code,
            QStringLiteral("approval.failed"));
        QVERIFY(!removalResult.error().message.contains(
            QStringLiteral("secret")));

        QVERIFY(waitFor(
                    service.respond(
                        request,
                        ApprovalDecision::ApproveOnce))
                    .hasValue());
        QCOMPARE(responderCalls, 3);
        QCOMPARE(removals, 2);
    }

    void approvalFollowerFutureExceptionCompletesWithStableFailure()
    {
        std::atomic_int calls = 0;
        ApprovalService service(
            [&calls](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                if (calls == 1) {
                    return exceptionalFuture<FollowerApprovalOutcome>();
                }
                return readyFuture(
                    FollowerApprovalOutcome::Approved);
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        const Result<void> failed =
            waitFor(service.respond(
                request,
                ApprovalDecision::ApproveOnce));

        QVERIFY(!failed.hasValue());
        QCOMPARE(
            failed.error().code,
            QStringLiteral("approval.failed"));
        QVERIFY(!failed.error().message.contains(
            QStringLiteral("sensitive")));

        QVERIFY(waitFor(
                    service.respond(
                        request,
                        ApprovalDecision::ApproveOnce))
                    .hasValue());
        QCOMPARE(calls.load(), 2);
    }

    void malformedApprovalFutureFailsWithoutRemoval_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("default") << QStringLiteral("default");
        QTest::newRow("no-result") << QStringLiteral("no-result");
        QTest::newRow("canceled") << QStringLiteral("canceled");
    }

    void malformedApprovalFutureFailsWithoutRemoval()
    {
        QFETCH(QString, kind);
        std::atomic_int calls = 0;
        std::atomic_int removals = 0;
        ApprovalService service(
            [&calls, &kind](
                PendingApproval,
                ApprovalDecision) {
                calls.fetch_add(1);
                if (kind == QStringLiteral("default")) {
                    return QFuture<FollowerApprovalOutcome>();
                }
                if (kind == QStringLiteral("canceled")) {
                    return canceledFuture<FollowerApprovalOutcome>();
                }
                return noResultFuture<FollowerApprovalOutcome>();
            },
            [&removals](PendingApproval) {
                removals.fetch_add(1);
            });
        const PendingApproval request{
            QStringLiteral("thread-a"),
            42,
            PendingApprovalMethod::CommandExecution,
            std::nullopt,
        };

        QFuture<Result<void>> future =
            service.respond(
                request,
                ApprovalDecision::ApproveOnce);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        const Result<void> failed = future.result();

        QVERIFY(!failed.hasValue());
        QCOMPARE(
            failed.error().code,
            QStringLiteral("approval.failed"));
        QCOMPARE(calls.load(), 1);
        QCOMPARE(removals.load(), 0);
    }

    void goalReadDeduplicatesTrimsSortsAndCorrelatesById()
    {
        RecordingRpc rpc;
        rpc.responses = {
            {2, goalResponse(QStringLiteral("thread-a"))},
            {3, goalResponse(
                    QStringLiteral("thread-b"),
                    QStringLiteral("paused"),
                    50'000)},
        };
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            waitFor(service.read({
                QStringLiteral(" thread-b "),
                QStringLiteral("thread-a"),
                QStringLiteral("thread-b"),
                QStringLiteral(" \n "),
            }));

        QVERIFY(result.hasValue());
        QCOMPARE(rpc.batches.size(), 1);
        QCOMPARE(rpc.batches.first().size(), 2);
        QCOMPARE(rpc.batches.first().at(0).id, 2);
        QCOMPARE(
            rpc.batches.first().at(0).method,
            QStringLiteral("thread/goal/get"));
        QCOMPARE(
            rpc.batches.first()
                .at(0)
                .params
                .value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-a"));
        QCOMPARE(rpc.batches.first().at(1).id, 3);
        QCOMPARE(
            rpc.batches.first()
                .at(1)
                .params
                .value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-b"));
        QVERIFY(result.value().value(QStringLiteral("thread-a")).has_value());
        QCOMPARE(
            result.value()
                .value(QStringLiteral("thread-b"))
                ->status,
            GoalStatus::Paused);
        QCOMPARE(
            result.value()
                .value(QStringLiteral("thread-b"))
                ->tokenBudget.value(),
            50'000);
    }

    void goalReadSyncForwardsExactTokenAndRequests()
    {
        QVector<RpcRequest> captured;
        std::stop_source stopSource;
        const std::stop_token expectedToken =
            stopSource.get_token();
        bool tokenMatched = false;
        GoalService service(
            [&captured,
             &expectedToken,
             &tokenMatched](
                const QVector<RpcRequest>& requests,
                std::stop_token stopToken) {
                captured = requests;
                tokenMatched =
                    stopToken == expectedToken;
                return Result<QHash<int, RpcResponse>>::success({
                    {2, goalResponse(QStringLiteral("thread-a"))},
                    {3, goalResponse(QStringLiteral("thread-b"))},
                });
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync(
                {
                    QStringLiteral(" thread-b "),
                    QStringLiteral("thread-a"),
                    QStringLiteral("thread-b"),
                    QStringLiteral(" "),
                },
                expectedToken);

        QVERIFY(result.hasValue());
        QVERIFY(tokenMatched);
        QCOMPARE(captured.size(), 2);
        QCOMPARE(captured.at(0).id, 2);
        QCOMPARE(
            captured.at(0).method,
            QStringLiteral("thread/goal/get"));
        const QJsonObject threadAParams{
            {
                QStringLiteral("threadId"),
                QStringLiteral("thread-a"),
            },
        };
        QCOMPARE(
            captured.at(0).params,
            threadAParams);
        QCOMPARE(captured.at(1).id, 3);
        const QJsonObject threadBParams{
            {
                QStringLiteral("threadId"),
                QStringLiteral("thread-b"),
            },
        };
        QCOMPARE(
            captured.at(1).params,
            threadBParams);
    }

    void goalReadSyncCancellationBeforePerformerIsExact()
    {
        std::atomic_int calls = 0;
        GoalService service(
            [&calls](
                const QVector<RpcRequest>&,
                std::stop_token) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({});
            });
        std::stop_source stopSource;
        stopSource.request_stop();

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync(
                {QStringLiteral("thread-a")},
                stopSource.get_token());

        QVERIFY(!result.hasValue());
        QCOMPARE(calls.load(), 0);
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(
            result.error().message,
            QStringLiteral("The Codex operation was canceled."));
        QVERIFY(!result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
    }

    void goalReadSyncCancellationAfterPerformerIsExact()
    {
        std::stop_source stopSource;
        bool tokenMatched = false;
        GoalService service(
            [&stopSource,
             &tokenMatched](
                const QVector<RpcRequest>&,
                std::stop_token stopToken) {
                tokenMatched =
                    stopToken == stopSource.get_token();
                stopSource.request_stop();
                return Result<QHash<int, RpcResponse>>::success({
                    {2, goalResponse(QStringLiteral("thread-a"))},
                });
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync(
                {QStringLiteral("thread-a")},
                stopSource.get_token());

        QVERIFY(!result.hasValue());
        QVERIFY(tokenMatched);
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(
            result.error().message,
            QStringLiteral("The Codex operation was canceled."));
        QVERIFY(!result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
    }

    void goalReadSyncCancellationFromThrowingPerformerWins()
    {
        std::stop_source stopSource;
        GoalService service(
            [&stopSource](
                const QVector<RpcRequest>&,
                std::stop_token)
                -> Result<QHash<int, RpcResponse>> {
                stopSource.request_stop();
                throw std::runtime_error(
                    "SECRET_THROWING_GOAL_PERFORMER");
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync(
                {QStringLiteral("thread-a")},
                stopSource.get_token());

        QVERIFY(stopSource.stop_requested());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(
            result.error().message,
            QStringLiteral("The Codex operation was canceled."));
        QVERIFY(!result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
    }

    void goalReadSyncCancellationDuringNormalizationSkipsPerformer()
    {
        using Access =
            companion::detail::GoalServiceTestAccess;
        using Phase = Access::Phase;
        constexpr int kThreadCount = 20'000;
        constexpr qsizetype kCancelIndex = 12'345;
        QVector<QString> threadIds;
        threadIds.reserve(kThreadCount);
        for (int index = 0; index < kThreadCount; ++index) {
            threadIds.append(
                QStringLiteral(" thread-%1 ")
                    .arg(index));
        }

        std::stop_source stopSource;
        std::atomic_int calls = 0;
        bool reached = false;
        bool phaseObservedAfterStop = false;
        qsizetype lastIndex = -1;
        std::unique_ptr<GoalService> service =
            Access::create(
                [&calls](
                    const QVector<RpcRequest>&,
                    std::stop_token) {
                    calls.fetch_add(1);
                    return Result<QHash<int, RpcResponse>>::success({});
                },
                [&stopSource,
                 &reached,
                 &phaseObservedAfterStop,
                 &lastIndex](
                    Phase phase,
                    qsizetype index) {
                    if (stopSource.stop_requested()) {
                        phaseObservedAfterStop = true;
                    }
                    if (phase != Phase::AfterNormalizeItem) {
                        return;
                    }
                    lastIndex = index;
                    if (index == kCancelIndex) {
                        reached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service->readSync(
                threadIds,
                stopSource.get_token());

        QVERIFY(reached);
        QVERIFY(stopSource.stop_requested());
        QVERIFY(!phaseObservedAfterStop);
        QCOMPARE(lastIndex, kCancelIndex);
        QVERIFY(!result.hasValue());
        QCOMPARE(calls.load(), 0);
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
    }

    void goalReadSyncCancellationDuringRequestBuildingSkipsPerformer()
    {
        using Access =
            companion::detail::GoalServiceTestAccess;
        using Phase = Access::Phase;
        constexpr int kThreadCount = 20'000;
        constexpr qsizetype kCancelIndex = 12'345;
        QVector<QString> threadIds;
        threadIds.reserve(kThreadCount);
        for (int index = 0; index < kThreadCount; ++index) {
            threadIds.append(
                QStringLiteral("thread-%1")
                    .arg(index, 5, 10, QLatin1Char('0')));
        }

        std::stop_source stopSource;
        std::atomic_int calls = 0;
        bool reached = false;
        bool phaseObservedAfterStop = false;
        qsizetype lastIndex = -1;
        std::unique_ptr<GoalService> service =
            Access::create(
                [&calls](
                    const QVector<RpcRequest>&,
                    std::stop_token) {
                    calls.fetch_add(1);
                    return Result<QHash<int, RpcResponse>>::success({});
                },
                [&stopSource,
                 &reached,
                 &phaseObservedAfterStop,
                 &lastIndex](
                    Phase phase,
                    qsizetype index) {
                    if (stopSource.stop_requested()) {
                        phaseObservedAfterStop = true;
                    }
                    if (phase != Phase::AfterRequestBuilt) {
                        return;
                    }
                    lastIndex = index;
                    if (index == kCancelIndex) {
                        reached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service->readSync(
                threadIds,
                stopSource.get_token());

        QVERIFY(reached);
        QVERIFY(stopSource.stop_requested());
        QVERIFY(!phaseObservedAfterStop);
        QCOMPARE(lastIndex, kCancelIndex);
        QVERIFY(!result.hasValue());
        QCOMPARE(calls.load(), 0);
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
    }

    void goalReadSyncCancellationDuringResponseParsingWins()
    {
        using Access =
            companion::detail::GoalServiceTestAccess;
        using Phase = Access::Phase;
        constexpr int kResponseCount = 20'000;
        constexpr qsizetype kCancelIndex = 12'345;
        QVector<QString> threadIds;
        threadIds.reserve(kResponseCount);
        QHash<int, RpcResponse> responses;
        responses.reserve(kResponseCount);
        const QString objective(512, QLatin1Char('x'));
        for (int index = 0; index < kResponseCount; ++index) {
            const QString threadId =
                QStringLiteral("thread-%1")
                    .arg(index, 5, 10, QLatin1Char('0'));
            threadIds.append(threadId);
            QJsonObject goal =
                goalObject(
                    threadId,
                    QStringLiteral("active"));
            goal.insert(
                QStringLiteral("objective"),
                objective);
            responses.insert(
                index + 2,
                {
                    QJsonObject{
                        {
                            QStringLiteral("goal"),
                            goal,
                        },
                    },
                    {},
                    false,
                });
        }

        std::stop_source stopSource;
        bool performerReturned = false;
        bool reached = false;
        bool phaseObservedAfterStop = false;
        qsizetype lastIndex = -1;
        std::unique_ptr<GoalService> service =
            Access::create(
                [&responses,
                 &performerReturned](
                    const QVector<RpcRequest>&,
                    std::stop_token) {
                    performerReturned = true;
                    return Result<QHash<int, RpcResponse>>::success(
                        responses);
                },
                [&stopSource,
                 &reached,
                 &phaseObservedAfterStop,
                 &lastIndex](
                    Phase phase,
                    qsizetype index) {
                    if (stopSource.stop_requested()) {
                        phaseObservedAfterStop = true;
                    }
                    if (phase != Phase::AfterResponseParsed) {
                        return;
                    }
                    lastIndex = index;
                    if (index == kCancelIndex) {
                        reached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service->readSync(
                threadIds,
                stopSource.get_token());

        QVERIFY(performerReturned);
        QVERIFY(reached);
        QVERIFY(stopSource.stop_requested());
        QVERIFY(!phaseObservedAfterStop);
        QCOMPARE(lastIndex, kCancelIndex);
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
    }

    void throwingGoalReadPhaseProbeDoesNotEscape()
    {
        using Access =
            companion::detail::GoalServiceTestAccess;
        using Phase = Access::Phase;
        int probeCalls = 0;
        std::unique_ptr<GoalService> service =
            Access::create(
                [](
                    const QVector<RpcRequest>&,
                    std::stop_token) {
                    return Result<QHash<int, RpcResponse>>::success({
                        {
                            2,
                            goalResponse(
                                QStringLiteral("thread-a")),
                        },
                    });
                },
                [&probeCalls](Phase, qsizetype) {
                    ++probeCalls;
                    throw std::runtime_error(
                        "SECRET_GOAL_READ_PROBE");
                });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service->readSync(
                {QStringLiteral("thread-a")});

        QVERIFY(result.hasValue());
        QVERIFY(probeCalls >= 3);
    }

    void goalReadSyncPreservesPerformerCancellation()
    {
        const CompanionError canceled{
            QStringLiteral("codex.operation_canceled"),
            QStringLiteral("The Codex operation was canceled."),
            false,
            {},
        };
        GoalService service(
            [canceled](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<QHash<int, RpcResponse>>::failure(
                    canceled);
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync({QStringLiteral("thread-a")});

        QVERIFY(!result.hasValue());
        QVERIFY(result.error() == canceled);
    }

    void goalLegacyConstructorAndAsyncReadRemainCompatible()
    {
        RecordingRpc rpc;
        rpc.responses = {
            {2, goalResponse(QStringLiteral("thread-a"))},
        };
        GoalRpcPerformer legacy =
            [&rpc](const QVector<RpcRequest>& requests) {
                return rpc.perform(requests);
            };
        GoalService service(legacy);

        const Result<QHash<QString, std::optional<BridgeGoal>>> sync =
            service.readSync({QStringLiteral("thread-a")});
        const Result<QHash<QString, std::optional<BridgeGoal>>> async =
            waitFor(service.read({QStringLiteral("thread-a")}));

        QVERIFY(sync.hasValue());
        QVERIFY(async.hasValue());
        QCOMPARE(sync.value(), async.value());
        QCOMPARE(rpc.batches.size(), 2);
    }

    void goalReadSyncExceptionsAreSanitized()
    {
        GoalService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error(
                    "SECRET_SYNC_GOAL_EXCEPTION");
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            service.readSync({QStringLiteral("thread-a")});

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.goal_unavailable"));
        verifySanitized(
            result.error(),
            {QStringLiteral("SECRET_SYNC_GOAL_EXCEPTION")});
    }

    void goalCreateRejectsInvalidInputsBeforeTransport()
    {
        RecordingRpc rpc;
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        QVERIFY(!waitFor(
                     service.create(
                         QStringLiteral("thread-a"),
                         QStringLiteral(" \n "),
                         std::nullopt))
                     .hasValue());
        QVERIFY(!waitFor(
                     service.create(
                         QStringLiteral("thread-a"),
                         QStringLiteral("Build"),
                         0))
                     .hasValue());
        QVERIFY(!waitFor(
                     service.create(
                         QStringLiteral("thread-a"),
                         QStringLiteral("Build"),
                         -1))
                     .hasValue());
        QCOMPARE(rpc.batches.size(), 0);
    }

    void goalMutationsUseGoalSetWithoutResettingOmittedFields()
    {
        RecordingRpc rpc;
        rpc.responses = {{2, goalResponse(QStringLiteral("thread-a"))}};
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        QVERIFY(waitFor(service.pause(QStringLiteral(" thread-a "))).hasValue());
        QVERIFY(waitFor(service.resume(QStringLiteral("thread-a"))).hasValue());
        QVERIFY(waitFor(
                    service.update({
                        QStringLiteral("thread-a"),
                        QStringLiteral(" Updated objective "),
                        std::nullopt,
                    }))
                    .hasValue());
        QVERIFY(waitFor(
                    service.update({
                        QStringLiteral("thread-a"),
                        QStringLiteral("Updated objective"),
                        75'000,
                    }))
                    .hasValue());

        QCOMPARE(rpc.batches.size(), 4);
        const QJsonObject pauseParams = rpc.batches.at(0).first().params;
        QCOMPARE(
            pauseParams.value(QStringLiteral("status")).toString(),
            QStringLiteral("paused"));
        QVERIFY(!pauseParams.contains(QStringLiteral("objective")));
        QVERIFY(!pauseParams.contains(QStringLiteral("tokenBudget")));

        const QJsonObject resumeParams = rpc.batches.at(1).first().params;
        QCOMPARE(
            resumeParams.value(QStringLiteral("status")).toString(),
            QStringLiteral("active"));
        QVERIFY(!resumeParams.contains(QStringLiteral("objective")));
        QVERIFY(!resumeParams.contains(QStringLiteral("tokenBudget")));

        const QJsonObject omittedBudgetParams =
            rpc.batches.at(2).first().params;
        QCOMPARE(
            omittedBudgetParams.value(QStringLiteral("objective"))
                .toString(),
            QStringLiteral("Updated objective"));
        QVERIFY(!omittedBudgetParams.contains(QStringLiteral("status")));
        QVERIFY(!omittedBudgetParams.contains(QStringLiteral("tokenBudget")));

        const QJsonObject budgetParams =
            rpc.batches.at(3).first().params;
        QCOMPARE(
            budgetParams.value(QStringLiteral("tokenBudget")).toInt(),
            75'000);

        QVERIFY(!waitFor(
                     service.update({
                         QStringLiteral("thread-a"),
                         QStringLiteral("Updated objective"),
                         0,
                     }))
                     .hasValue());
        QVERIFY(!waitFor(
                     service.update({
                         QStringLiteral("thread-a"),
                         QStringLiteral("Updated objective"),
                         -10,
                     }))
                     .hasValue());
        QCOMPARE(rpc.batches.size(), 4);
    }

    void goalDecodePreservesEveryStatus()
    {
        const QVector<std::pair<QString, GoalStatus>> cases{
            {QStringLiteral("active"), GoalStatus::Active},
            {QStringLiteral("paused"), GoalStatus::Paused},
            {QStringLiteral("blocked"), GoalStatus::Blocked},
            {QStringLiteral("usageLimited"), GoalStatus::UsageLimited},
            {QStringLiteral("budgetLimited"), GoalStatus::BudgetLimited},
            {QStringLiteral("complete"), GoalStatus::Complete},
        };

        for (const auto& [statusText, expected] : cases) {
            RecordingRpc rpc;
            rpc.responses = {
                {2, goalResponse(QStringLiteral("thread-a"), statusText)},
            };
            GoalService service([&rpc](const QVector<RpcRequest>& requests) {
                return rpc.perform(requests);
            });

            const Result<BridgeGoal> result = waitFor(
                service.resume(QStringLiteral("thread-a")));

            QVERIFY(result.hasValue());
            QCOMPARE(result.value().status, expected);
        }
    }

    void goalDecodePreservesResponseStringsVerbatim()
    {
        RecordingRpc rpc;
        QJsonObject goal =
            goalObject(
                QStringLiteral("  thread-a  "),
                QStringLiteral("active"));
        goal.insert(
            QStringLiteral("objective"),
            QStringLiteral("  Ship it with spaces  "));
        rpc.responses = {
            {
                2,
                {
                    QJsonObject{
                        {QStringLiteral("goal"), goal},
                    },
                    {},
                    false,
                },
            },
        };
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        const Result<BridgeGoal> result =
            waitFor(service.resume(QStringLiteral("thread-a")));

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().threadId,
            QStringLiteral("  thread-a  "));
        QCOMPARE(
            result.value().objective,
            QStringLiteral("  Ship it with spaces  "));
    }

    void goalDecodeRejectsAliasAndCoercedIntegerFields()
    {
        auto expectUnreadable =
            [](QJsonObject goal) {
                RecordingRpc rpc;
                rpc.responses = {
                    {
                        2,
                        {
                            QJsonObject{
                                {QStringLiteral("goal"), goal},
                            },
                            {},
                            false,
                        },
                    },
                };
                GoalService service(
                    [&rpc](const QVector<RpcRequest>& requests) {
                        return rpc.perform(requests);
                    });

                const Result<BridgeGoal> result =
                    waitFor(service.resume(QStringLiteral("thread-a")));
                QVERIFY(!result.hasValue());
                QCOMPARE(
                    result.error().code,
                    QStringLiteral("codex.goal_unavailable"));
            };

        QJsonObject alias =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        alias.remove(QStringLiteral("timeUsedSeconds"));
        alias.insert(QStringLiteral("elapsedSeconds"), 90);
        expectUnreadable(alias);

        QJsonObject stringNumber =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        stringNumber.insert(
            QStringLiteral("tokensUsed"),
            QStringLiteral("42"));
        expectUnreadable(stringNumber);

        QJsonObject fractional =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        fractional.insert(
            QStringLiteral("timeUsedSeconds"),
            90.5);
        expectUnreadable(fractional);

        QJsonObject stringBudget =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        stringBudget.insert(
            QStringLiteral("tokenBudget"),
            QStringLiteral("50000"));
        expectUnreadable(stringBudget);
    }

    void goalDecodePreservesFullRangeIntegerFields()
    {
        constexpr qint64 beyondSafeDouble =
            9'007'199'254'740'993LL;
        const qint64 qint64Min =
            std::numeric_limits<qint64>::min();
        const qint64 qint64Max =
            std::numeric_limits<qint64>::max();
        QJsonObject goal =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"),
                qint64Max);
        goal.insert(
            QStringLiteral("tokensUsed"),
            QJsonValue(beyondSafeDouble));
        goal.insert(
            QStringLiteral("timeUsedSeconds"),
            QJsonValue(qint64Min));
        goal.insert(
            QStringLiteral("createdAt"),
            QJsonValue(qint64Max));
        goal.insert(
            QStringLiteral("updatedAt"),
            QJsonValue(beyondSafeDouble));

        RecordingRpc rpc;
        rpc.responses = {
            {
                2,
                {
                    QJsonObject{
                        {QStringLiteral("goal"), goal},
                    },
                    {},
                    false,
                },
            },
        };
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        const Result<BridgeGoal> result =
            waitFor(service.resume(QStringLiteral("thread-a")));

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tokensUsed, beyondSafeDouble);
        QCOMPARE(result.value().elapsedSeconds, qint64Min);
        QCOMPARE(result.value().createdAt, qint64Max);
        QCOMPARE(result.value().updatedAt, beyondSafeDouble);
        QCOMPARE(result.value().tokenBudget.value(), qint64Max);
    }

    void goalDecodeRejectsNonFiniteAndOutOfRangeIntegerFields()
    {
        auto expectUnreadable =
            [](QJsonObject goal) {
                RecordingRpc rpc;
                rpc.responses = {
                    {
                        2,
                        {
                            QJsonObject{
                                {QStringLiteral("goal"), goal},
                            },
                            {},
                            false,
                        },
                    },
                };
                GoalService service(
                    [&rpc](const QVector<RpcRequest>& requests) {
                        return rpc.perform(requests);
                    });

                const Result<BridgeGoal> result =
                    waitFor(service.resume(QStringLiteral("thread-a")));
                QVERIFY(!result.hasValue());
                QCOMPARE(
                    result.error().code,
                    QStringLiteral("codex.goal_unavailable"));
            };

        QJsonObject infinity =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        infinity.insert(
            QStringLiteral("tokensUsed"),
            QJsonValue(
                std::numeric_limits<double>::infinity()));
        expectUnreadable(infinity);

        QJsonObject outOfRange =
            goalObject(
                QStringLiteral("thread-a"),
                QStringLiteral("active"));
        outOfRange.insert(
            QStringLiteral("tokensUsed"),
            QJsonValue(
                static_cast<double>(
                    std::numeric_limits<qint64>::max())));
        expectUnreadable(outOfRange);
    }

    void goalReadRejectsNonObjectResponseEnvelope()
    {
        RecordingRpc rpc;
        rpc.responses = {
            {
                2,
                {
                    QJsonArray{},
                    {},
                    false,
                },
            },
        };
        GoalService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        const Result<QHash<QString, std::optional<BridgeGoal>>> result =
            waitFor(service.read({QStringLiteral("thread-a")}));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.goal_unavailable"));
    }

    void goalPublicErrorsAreSanitized()
    {
        const QVector<QString> forbidden{
            QStringLiteral("SECRET_OBJECTIVE"),
            QStringLiteral("SECRET_BODY"),
            QStringLiteral("SECRET_STDERR"),
        };

        RecordingRpc transportRpc;
        transportRpc.result =
            Result<QHash<int, RpcResponse>>::failure({
                QStringLiteral("codex.SECRET_BODY"),
                QStringLiteral(
                    "transport failed SECRET_OBJECTIVE SECRET_STDERR"),
                false,
                {
                    {
                        QStringLiteral("stderr"),
                        QStringLiteral("SECRET_STDERR"),
                    },
                    {
                        QStringLiteral("response"),
                        QStringLiteral("SECRET_BODY"),
                    },
                },
            });
        GoalService transportService(
            [&transportRpc](const QVector<RpcRequest>& requests) {
                return transportRpc.perform(requests);
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> read =
            waitFor(transportService.read({QStringLiteral("thread-a")}));

        QVERIFY(!read.hasValue());
        QCOMPARE(
            read.error().code,
            QStringLiteral("codex.goal_unavailable"));
        QCOMPARE(
            read.error().message,
            QStringLiteral("Codex goal is unavailable."));
        verifySanitized(read.error(), forbidden);

        RecordingRpc serverRpc;
        serverRpc.responses = {
            {
                2,
                {
                    {},
                    QStringLiteral(
                        "server failed SECRET_OBJECTIVE SECRET_BODY"),
                    true,
                },
            },
        };
        GoalService serverService(
            [&serverRpc](const QVector<RpcRequest>& requests) {
                return serverRpc.perform(requests);
            });

        const Result<QHash<QString, std::optional<BridgeGoal>>> server =
            waitFor(serverService.read({QStringLiteral("thread-a")}));

        QVERIFY(!server.hasValue());
        QCOMPARE(
            server.error().code,
            QStringLiteral("codex.goal_unavailable"));
        verifySanitized(server.error(), forbidden);

        RecordingRpc mutationRpc;
        mutationRpc.result =
            Result<QHash<int, RpcResponse>>::failure({
                QStringLiteral("codex.transport"),
                QStringLiteral(
                    "mutation failed SECRET_OBJECTIVE SECRET_BODY"),
                false,
                {
                    {
                        QStringLiteral("objective"),
                        QStringLiteral("SECRET_OBJECTIVE"),
                    },
                },
            });
        GoalService mutationService(
            [&mutationRpc](const QVector<RpcRequest>& requests) {
                return mutationRpc.perform(requests);
            });

        const Result<BridgeGoal> mutation =
            waitFor(mutationService.update({
                QStringLiteral("thread-a"),
                QStringLiteral("SECRET_OBJECTIVE"),
                std::nullopt,
            }));

        QVERIFY(!mutation.hasValue());
        QCOMPARE(
            mutation.error().code,
            QStringLiteral("codex.goal_unavailable"));
        verifySanitized(mutation.error(), forbidden);
    }

    void goalExceptionsMapToStableFailures()
    {
        GoalService readService(
            [](const QVector<RpcRequest>&)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error("SECRET_OBJECTIVE");
            });

        std::optional<
            Result<QHash<QString, std::optional<BridgeGoal>>>>
            readResult;
        try {
            readResult = waitFor(
                readService.read({QStringLiteral("thread-a")}));
        } catch (...) {
            QFAIL("goal read future propagated an exception");
        }
        QVERIFY(readResult.has_value());
        QVERIFY(!readResult->hasValue());
        QCOMPARE(
            readResult->error().code,
            QStringLiteral("codex.goal_unavailable"));
        verifySanitized(
            readResult->error(),
            {QStringLiteral("SECRET_OBJECTIVE")});

        GoalService mutationService(
            [](const QVector<RpcRequest>&)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error("SECRET_BODY");
            });

        std::optional<Result<BridgeGoal>> mutationResult;
        try {
            mutationResult = waitFor(mutationService.update({
                QStringLiteral("thread-a"),
                QStringLiteral("SECRET_OBJECTIVE"),
                std::nullopt,
            }));
        } catch (...) {
            QFAIL("goal mutation future propagated an exception");
        }
        QVERIFY(mutationResult.has_value());
        QVERIFY(!mutationResult->hasValue());
        QCOMPARE(
            mutationResult->error().code,
            QStringLiteral("codex.goal_unavailable"));
        verifySanitized(
            mutationResult->error(),
            {
                QStringLiteral("SECRET_OBJECTIVE"),
                QStringLiteral("SECRET_BODY"),
            });
    }

    void cancellingBeforeGoalMutationCommitSkipsRpc()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        GoalService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase != QStringLiteral("mutation.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle = service.updateMutation({
            QStringLiteral("thread-a"),
            QStringLiteral("Updated objective"),
            std::nullopt,
        });
        const bool reached =
            waitUntil(
                [&inCommit] {
                    return inCommit.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<BridgeGoal> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterGoalMutationClaimSkipsRpc()
    {
        std::atomic_int calls = 0;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        GoalService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&claimEstablished, &releaseClaim](
                const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "mutation.claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle = service.updateMutation({
            QStringLiteral("thread-a"),
            QStringLiteral("Updated objective"),
            std::nullopt,
        });
        const bool reached =
            waitUntil(
                [&claimEstablished] {
                    return claimEstablished.load();
                },
                1000);
        if (!reached) {
            releaseClaim.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<BridgeGoal> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterGoalMutationCommitKeepsRpcLaunchCommitted()
    {
        std::atomic_int calls = 0;
        std::atomic_int committedCount = 0;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        GoalService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&committed,
             &committedCount,
             &releaseCommit](const QString& phase) {
                if (phase != QStringLiteral("mutation.committed")) {
                    return;
                }
                committedCount.fetch_add(1);
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle = service.updateMutation({
            QStringLiteral("thread-a"),
            QStringLiteral("Updated objective"),
            std::nullopt,
        });
        const bool reached =
            waitUntil(
                [&committed] {
                    return committed.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<BridgeGoal> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(calls.load(), 1);
        QCOMPARE(committedCount.load(), 1);
    }

    void cancellingAfterGoalMutationPerformerLaunchFinishesAndKeepsCommit()
    {
        std::atomic_int calls = 0;
        std::atomic_int committedCount = 0;
        std::atomic_bool performerStarted = false;
        std::atomic_bool releasePerformer = false;
        GoalService service(
            [&calls,
             &performerStarted,
             &releasePerformer](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                performerStarted.store(true);
                while (!releasePerformer.load()) {
                    QThread::msleep(1);
                }
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&committedCount](const QString& phase) {
                if (phase == QStringLiteral("mutation.committed")) {
                    committedCount.fetch_add(1);
                }
            });

        auto handle = service.updateMutation({
            QStringLiteral("thread-a"),
            QStringLiteral("Updated objective"),
            std::nullopt,
        });
        QTRY_VERIFY_WITH_TIMEOUT(performerStarted.load(), 1000);

        handle.requestStopBeforeCommit();
        releasePerformer.store(true);
        const Result<BridgeGoal> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(calls.load(), 1);
        QCOMPARE(committedCount.load(), 1);
    }

    void goalMutationHandleStopsBeforeCommitWithExplicitResult()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        GoalService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "mutation.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle = service.updateMutation({
            QStringLiteral("thread-a"),
            QStringLiteral("Updated objective"),
            std::nullopt,
        });
        const bool reached = waitUntil(
            [&inCommit] {
                return inCommit.load();
            },
            1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<BridgeGoal> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancelingLegacyGoalFutureDoesNotStopMutation()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        GoalService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success(
                    {{2, goalResponse(QStringLiteral("thread-a"))}});
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "mutation.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        QFuture<Result<BridgeGoal>> future =
            service.update({
                QStringLiteral("thread-a"),
                QStringLiteral("Updated objective"),
                std::nullopt,
            });
        const bool reached = waitUntil(
            [&inCommit] {
                return inCommit.load();
            },
            1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        future.cancel();
        releaseCommit.store(true);

        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.isCanceled());
    }

    void usageReadUsesRateLimitEndpointAndParsesFixture()
    {
        RecordingRpc rpc;
        rpc.responses = {
            {
                2,
                {
                    fixtureObject(QStringLiteral("usage.json")),
                    {},
                    false,
                },
            },
        };
        UsageService service(
            [&rpc](const QVector<RpcRequest>& requests) {
                return rpc.perform(requests);
            },
            [] {
                return BridgeDate{500.0};
            });

        const Result<BridgeUsageSnapshot> result =
            waitFor(service.read());

        QVERIFY(result.hasValue());
        QCOMPARE(rpc.batches.size(), 1);
        QCOMPARE(rpc.batches.first().size(), 1);
        QCOMPARE(
            rpc.batches.first().first().method,
            QStringLiteral("account/rateLimits/read"));
        QCOMPARE(result.value().planType.value(), QStringLiteral("pro"));
        QCOMPARE(result.value().groups.size(), 2);
        QCOMPARE(result.value().groups.first().id, QStringLiteral("codex"));
        QCOMPARE(
            result.value().groups.first().shortWindow->remainingPercent,
            82.0);
        QCOMPARE(result.value().availableResetCount, 2);
        QCOMPARE(result.value().availableResetCredits.size(), 1);
        QCOMPARE(
            result.value().availableResetCredits.first().id,
            QStringLiteral("credit-a"));
        QCOMPARE(result.value().updatedAt.secondsSinceReferenceDate, 500.0);
    }

    void usageConsumeValidatesInputsAndReturnsEveryOutcome()
    {
        RecordingRpc rpc;
        UsageService service([&rpc](const QVector<RpcRequest>& requests) {
            return rpc.perform(requests);
        });

        QVERIFY(!waitFor(
                     service.consumeReset(
                         QStringLiteral(" \t "),
                         QUuid(QStringLiteral(
                             "50000000-0000-0000-0000-000000000001"))))
                     .hasValue());
        QVERIFY(!waitFor(
                     service.consumeReset(
                         QStringLiteral("credit-a"),
                         QUuid()))
                     .hasValue());
        QCOMPARE(rpc.batches.size(), 0);

        const QUuid observableKey(QStringLiteral(
            "50000000-0000-0000-0000-000000abcdef"));
        const QVector<std::pair<QString, UsageResetOutcome>> cases{
            {QStringLiteral("reset"), UsageResetOutcome::Reset},
            {
                QStringLiteral("nothingToReset"),
                UsageResetOutcome::NothingToReset,
            },
            {QStringLiteral("noCredit"), UsageResetOutcome::NoCredit},
            {
                QStringLiteral("alreadyRedeemed"),
                UsageResetOutcome::AlreadyRedeemed,
            },
        };
        for (const auto& [rawOutcome, expected] : cases) {
            rpc.responses = {
                {
                    2,
                    {
                        QJsonObject{
                            {QStringLiteral("outcome"), rawOutcome},
                        },
                        {},
                        false,
                    },
                },
            };
            const Result<UsageResetOutcome> result =
                waitFor(service.consumeReset(
                    QStringLiteral(" credit-a "),
                    observableKey));

            QVERIFY(result.hasValue());
            QCOMPARE(result.value(), expected);
            QCOMPARE(
                rpc.batches.last().first().method,
                QStringLiteral(
                    "account/rateLimitResetCredit/consume"));
            QCOMPARE(
                rpc.batches.last()
                    .first()
                    .params
                    .value(QStringLiteral("creditId"))
                    .toString(),
                QStringLiteral("credit-a"));
            QCOMPARE(
                rpc.batches.last()
                    .first()
                    .params
                    .value(QStringLiteral("idempotencyKey"))
                    .toString(),
                QStringLiteral(
                    "50000000-0000-0000-0000-000000ABCDEF"));
        }
    }

    void usageMalformedDataReturnsUnavailable()
    {
        RecordingRpc readRpc;
        readRpc.responses = {{2, {QJsonObject{}, {}, false}}};
        UsageService readService(
            [&readRpc](const QVector<RpcRequest>& requests) {
                return readRpc.perform(requests);
            });

        const Result<BridgeUsageSnapshot> read =
            waitFor(readService.read());

        QVERIFY(!read.hasValue());
        QCOMPARE(
            read.error().code,
            QStringLiteral("usage.unavailable"));

        RecordingRpc consumeRpc;
        consumeRpc.responses = {{2, {QJsonObject{}, {}, false}}};
        UsageService consumeService(
            [&consumeRpc](const QVector<RpcRequest>& requests) {
                return consumeRpc.perform(requests);
            });

        const Result<UsageResetOutcome> consume =
            waitFor(consumeService.consumeReset(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001"))));

        QVERIFY(!consume.hasValue());
        QCOMPARE(
            consume.error().code,
            QStringLiteral("usage.unavailable"));
    }

    void usagePublicErrorsAreSanitized()
    {
        const QVector<QString> forbidden{
            QStringLiteral("SECRET_STDERR"),
            QStringLiteral("SECRET_BODY"),
            QStringLiteral("SECRET_CREDENTIAL"),
        };

        RecordingRpc readRpc;
        readRpc.result =
            Result<QHash<int, RpcResponse>>::failure({
                QStringLiteral("usage.SECRET_BODY"),
                QStringLiteral(
                    "process failed SECRET_STDERR SECRET_BODY"),
                false,
                {
                    {
                        QStringLiteral("stderr"),
                        QStringLiteral("SECRET_STDERR"),
                    },
                    {
                        QStringLiteral("credential"),
                        QStringLiteral("SECRET_CREDENTIAL"),
                    },
                },
            });
        UsageService readService(
            [&readRpc](const QVector<RpcRequest>& requests) {
                return readRpc.perform(requests);
            });

        const Result<BridgeUsageSnapshot> read =
            waitFor(readService.read());

        QVERIFY(!read.hasValue());
        QCOMPARE(
            read.error().code,
            QStringLiteral("usage.unavailable"));
        QCOMPARE(
            read.error().message,
            QStringLiteral("Codex usage is unavailable."));
        verifySanitized(read.error(), forbidden);

        RecordingRpc consumeRpc;
        consumeRpc.result =
            Result<QHash<int, RpcResponse>>::failure({
                QStringLiteral("usage.transport"),
                QStringLiteral(
                    "consume failed SECRET_BODY SECRET_STDERR"),
                false,
                {
                    {
                        QStringLiteral("response"),
                        QStringLiteral("SECRET_BODY"),
                    },
                },
            });
        UsageService consumeService(
            [&consumeRpc](const QVector<RpcRequest>& requests) {
                return consumeRpc.perform(requests);
            });

        const Result<UsageResetOutcome> consume =
            waitFor(consumeService.consumeReset(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001"))));

        QVERIFY(!consume.hasValue());
        QCOMPARE(
            consume.error().code,
            QStringLiteral("usage.unavailable"));
        verifySanitized(consume.error(), forbidden);
    }

    void usageExceptionsMapToStableFailures()
    {
        UsageService readService(
            [](const QVector<RpcRequest>&)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error("SECRET_STDERR");
            });

        std::optional<Result<BridgeUsageSnapshot>> readResult;
        try {
            readResult = waitFor(readService.read());
        } catch (...) {
            QFAIL("usage read future propagated an exception");
        }
        QVERIFY(readResult.has_value());
        QVERIFY(!readResult->hasValue());
        QCOMPARE(
            readResult->error().code,
            QStringLiteral("usage.unavailable"));
        verifySanitized(
            readResult->error(),
            {QStringLiteral("SECRET_STDERR")});

        RecordingRpc clockRpc;
        clockRpc.responses = {{2, {validUsageObject(), {}, false}}};
        UsageService clockService(
            [&clockRpc](const QVector<RpcRequest>& requests) {
                return clockRpc.perform(requests);
            },
            []() -> BridgeDate {
                throw std::runtime_error("SECRET_CLOCK");
            });
        std::optional<Result<BridgeUsageSnapshot>> clockResult;
        try {
            clockResult = waitFor(clockService.read());
        } catch (...) {
            QFAIL("usage clock exception propagated");
        }
        QVERIFY(clockResult.has_value());
        QVERIFY(!clockResult->hasValue());
        QCOMPARE(
            clockResult->error().code,
            QStringLiteral("usage.unavailable"));
        verifySanitized(
            clockResult->error(),
            {QStringLiteral("SECRET_CLOCK")});

        UsageService consumeService(
            [](const QVector<RpcRequest>&)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error("SECRET_BODY");
            });
        std::optional<Result<UsageResetOutcome>> consumeResult;
        try {
            consumeResult = waitFor(consumeService.consumeReset(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001"))));
        } catch (...) {
            QFAIL("usage consume future propagated an exception");
        }
        QVERIFY(consumeResult.has_value());
        QVERIFY(!consumeResult->hasValue());
        QCOMPARE(
            consumeResult->error().code,
            QStringLiteral("usage.unavailable"));
        verifySanitized(
            consumeResult->error(),
            {QStringLiteral("SECRET_BODY")});
    }

    void cancellingBeforeUsageConsumeCommitSkipsRpc()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        UsageService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "consumeReset.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle =
            service.consumeResetMutation(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001")));
        const bool reached =
            waitUntil(
                [&inCommit] {
                    return inCommit.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<UsageResetOutcome> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterUsageConsumeClaimSkipsRpc()
    {
        std::atomic_int calls = 0;
        std::atomic_bool claimEstablished = false;
        std::atomic_bool releaseClaim = false;
        UsageService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&claimEstablished, &releaseClaim](
                const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "consumeReset.claimEstablished")) {
                    return;
                }
                claimEstablished.store(true);
                while (!releaseClaim.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle =
            service.consumeResetMutation(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001")));
        const bool reached =
            waitUntil(
                [&claimEstablished] {
                    return claimEstablished.load();
                },
                1000);
        if (!reached) {
            releaseClaim.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseClaim.store(true);
        const Result<UsageResetOutcome> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancellingAfterUsageConsumeCommitKeepsRpcLaunchCommitted()
    {
        std::atomic_int calls = 0;
        std::atomic_int committedCount = 0;
        std::atomic_bool committed = false;
        std::atomic_bool releaseCommit = false;
        UsageService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&committed,
             &committedCount,
             &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral("consumeReset.committed")) {
                    return;
                }
                committedCount.fetch_add(1);
                committed.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle =
            service.consumeResetMutation(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001")));
        const bool reached =
            waitUntil(
                [&committed] {
                    return committed.load();
                },
                1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<UsageResetOutcome> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(calls.load(), 1);
        QCOMPARE(committedCount.load(), 1);
    }

    void cancellingAfterUsageConsumePerformerLaunchFinishesAndKeepsCommit()
    {
        std::atomic_int calls = 0;
        std::atomic_int committedCount = 0;
        std::atomic_bool performerStarted = false;
        std::atomic_bool releasePerformer = false;
        UsageService service(
            [&calls,
             &performerStarted,
             &releasePerformer](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                performerStarted.store(true);
                while (!releasePerformer.load()) {
                    QThread::msleep(1);
                }
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&committedCount](const QString& phase) {
                if (phase == QStringLiteral("consumeReset.committed")) {
                    committedCount.fetch_add(1);
                }
            });

        auto handle =
            service.consumeResetMutation(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001")));
        QTRY_VERIFY_WITH_TIMEOUT(performerStarted.load(), 1000);

        handle.requestStopBeforeCommit();
        releasePerformer.store(true);
        const Result<UsageResetOutcome> result =
            waitFor(handle.terminalFuture);

        QVERIFY(result.hasValue());
        QCOMPARE(calls.load(), 1);
        QCOMPARE(committedCount.load(), 1);
    }

    void usageMutationHandleStopsBeforeCommitWithExplicitResult()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        UsageService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "consumeReset.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        auto handle = service.consumeResetMutation(
            QStringLiteral("credit-a"),
            QUuid(QStringLiteral(
                "50000000-0000-0000-0000-000000000001")));
        const bool reached = waitUntil(
            [&inCommit] {
                return inCommit.load();
            },
            1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        handle.requestStopBeforeCommit();
        releaseCommit.store(true);
        const Result<UsageResetOutcome> result =
            waitFor(handle.terminalFuture);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.operation_canceled"));
        QCOMPARE(calls.load(), 0);
    }

    void cancelingLegacyUsageFutureDoesNotStopMutation()
    {
        std::atomic_int calls = 0;
        std::atomic_bool inCommit = false;
        std::atomic_bool releaseCommit = false;
        UsageService service(
            [&calls](const QVector<RpcRequest>&) {
                calls.fetch_add(1);
                return Result<QHash<int, RpcResponse>>::success({
                    {
                        2,
                        {
                            QJsonObject{
                                {
                                    QStringLiteral("outcome"),
                                    QStringLiteral("reset"),
                                },
                            },
                            {},
                            false,
                        },
                    },
                });
            },
            [] {
                return BridgeDate{500.0};
            },
            [&inCommit, &releaseCommit](const QString& phase) {
                if (phase
                    != QStringLiteral(
                        "consumeReset.commitPending")) {
                    return;
                }
                inCommit.store(true);
                while (!releaseCommit.load()) {
                    QThread::msleep(1);
                }
            });

        QFuture<Result<UsageResetOutcome>> future =
            service.consumeReset(
                QStringLiteral("credit-a"),
                QUuid(QStringLiteral(
                    "50000000-0000-0000-0000-000000000001")));
        const bool reached = waitUntil(
            [&inCommit] {
                return inCommit.load();
            },
            1000);
        if (!reached) {
            releaseCommit.store(true);
        }
        QVERIFY(reached);

        future.cancel();
        releaseCommit.store(true);

        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.isCanceled());
    }

    void usageMalformedNestedDataFailsWholeRead()
    {
        auto expectUnavailable =
            [](QJsonObject object) {
                const Result<BridgeUsageSnapshot> result =
                    readUsageObject(object);
                QVERIFY(!result.hasValue());
                QCOMPARE(
                    result.error().code,
                    QStringLiteral("usage.unavailable"));
            };

        QJsonObject badWindow = validUsageObject();
        QJsonObject rateLimits =
            badWindow.value(QStringLiteral("rateLimits")).toObject();
        QJsonObject primary =
            rateLimits.value(QStringLiteral("primary")).toObject();
        primary.insert(
            QStringLiteral("usedPercent"),
            QStringLiteral("20"));
        rateLimits.insert(QStringLiteral("primary"), primary);
        badWindow.insert(QStringLiteral("rateLimits"), rateLimits);
        expectUnavailable(badWindow);

        QJsonObject badById = validUsageObject();
        badById.insert(
            QStringLiteral("rateLimitsByLimitId"),
            QJsonObject{
                {QStringLiteral("codex"), true},
            });
        expectUnavailable(badById);

        QJsonObject missingAvailable = validUsageObject();
        QJsonObject credits =
            missingAvailable
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.remove(QStringLiteral("availableCount"));
        missingAvailable.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(missingAvailable);

        QJsonObject stringAvailable = validUsageObject();
        credits =
            stringAvailable
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            QStringLiteral("1"));
        stringAvailable.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(stringAvailable);

        QJsonObject badCreditRow = validUsageObject();
        credits =
            badCreditRow
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("credits"),
            QJsonArray{true});
        badCreditRow.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(badCreditRow);

        QJsonObject badCreditStatus = validUsageObject();
        credits =
            badCreditStatus
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("credits"),
            QJsonArray{
                usageCredit(QStringLiteral("mystery")),
            });
        badCreditStatus.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(badCreditStatus);

        QJsonObject badCreditData = validUsageObject();
        credits =
            badCreditData
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        QJsonObject credit = usageCredit();
        credit.insert(
            QStringLiteral("grantedAt"),
            QStringLiteral("1700000000"));
        credits.insert(
            QStringLiteral("credits"),
            QJsonArray{credit});
        badCreditData.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(badCreditData);

        QJsonObject fractionalAvailable = validUsageObject();
        credits =
            fractionalAvailable
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            1.5);
        fractionalAvailable.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(fractionalAvailable);

        QJsonObject infiniteAvailable = validUsageObject();
        credits =
            infiniteAvailable
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            QJsonValue(
                std::numeric_limits<double>::infinity()));
        infiniteAvailable.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        expectUnavailable(infiniteAvailable);
    }

    void usageMalformedBaseRateLimitsFailsEvenWhenByIdGroupsExist()
    {
        QJsonObject malformedPrimary = validUsageObject();
        QJsonObject rateLimits =
            malformedPrimary
                .value(QStringLiteral("rateLimits"))
                .toObject();
        QJsonObject primary =
            rateLimits.value(QStringLiteral("primary")).toObject();
        primary.insert(
            QStringLiteral("usedPercent"),
            QStringLiteral("20"));
        rateLimits.insert(QStringLiteral("primary"), primary);
        malformedPrimary.insert(
            QStringLiteral("rateLimits"),
            rateLimits);
        malformedPrimary.insert(
            QStringLiteral("rateLimitsByLimitId"),
            validUsageByIdGroups());
        expectUsageUnavailable(malformedPrimary);

        QJsonObject malformedLimitId = validUsageObject();
        rateLimits =
            malformedLimitId
                .value(QStringLiteral("rateLimits"))
                .toObject();
        rateLimits.insert(QStringLiteral("limitId"), true);
        malformedLimitId.insert(
            QStringLiteral("rateLimits"),
            rateLimits);
        malformedLimitId.insert(
            QStringLiteral("rateLimitsByLimitId"),
            validUsageByIdGroups());
        expectUsageUnavailable(malformedLimitId);

        QJsonObject malformedLimitName = validUsageObject();
        rateLimits =
            malformedLimitName
                .value(QStringLiteral("rateLimits"))
                .toObject();
        rateLimits.insert(QStringLiteral("limitName"), 42);
        malformedLimitName.insert(
            QStringLiteral("rateLimits"),
            rateLimits);
        malformedLimitName.insert(
            QStringLiteral("rateLimitsByLimitId"),
            validUsageByIdGroups());
        expectUsageUnavailable(malformedLimitName);
    }

    void usageNullOptionalSectionsRemainEmpty()
    {
        const Result<BridgeUsageSnapshot> result =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {
                            QStringLiteral("limitId"),
                            QJsonValue(QJsonValue::Null),
                        },
                        {
                            QStringLiteral("limitName"),
                            QJsonValue(QJsonValue::Null),
                        },
                        {
                            QStringLiteral("planType"),
                            QJsonValue(QJsonValue::Null),
                        },
                        {
                            QStringLiteral("primary"),
                            QJsonValue(QJsonValue::Null),
                        },
                        {
                            QStringLiteral("secondary"),
                            usageWindow(25.0, 60.0),
                        },
                    },
                },
                {
                    QStringLiteral("rateLimitsByLimitId"),
                    QJsonValue(QJsonValue::Null),
                },
                {
                    QStringLiteral("rateLimitResetCredits"),
                    QJsonValue(QJsonValue::Null),
                },
            });

        QVERIFY(result.hasValue());
        QVERIFY(!result.value().planType.has_value());
        QCOMPARE(result.value().groups.size(), 1);
        QCOMPARE(
            result.value().groups.first().id,
            QStringLiteral("codex"));
        QVERIFY(result.value().groups.first().shortWindow.has_value());
        QCOMPARE(result.value().availableResetCount, 0);
        QCOMPARE(result.value().availableResetCredits.size(), 0);
    }

    void usagePreservesPlanTypeAndGroupIdentityVerbatim()
    {
        const Result<BridgeUsageSnapshot> byIdResult =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {QStringLiteral("limitId"), QStringLiteral("codex")},
                        {QStringLiteral("planType"), QStringLiteral("")},
                        {QStringLiteral("primary"), usageWindow(20.0, 60.0)},
                    },
                },
                {
                    QStringLiteral("rateLimitsByLimitId"),
                    QJsonObject{
                        {
                            QStringLiteral("dictionary-key"),
                            QJsonObject{
                                {
                                    QStringLiteral("limitId"),
                                    QStringLiteral("nested-id"),
                                },
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                    },
                },
            });

        QVERIFY(byIdResult.hasValue());
        QVERIFY(byIdResult.value().planType.has_value());
        QCOMPARE(byIdResult.value().planType.value(), QStringLiteral(""));
        QCOMPARE(byIdResult.value().groups.size(), 1);
        QCOMPARE(
            byIdResult.value().groups.first().id,
            QStringLiteral("dictionary-key"));

        const Result<BridgeUsageSnapshot> fallbackResult =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {
                            QStringLiteral("limitId"),
                            QStringLiteral("  fallback-id  "),
                        },
                        {
                            QStringLiteral("planType"),
                            QStringLiteral("  pro  "),
                        },
                        {
                            QStringLiteral("primary"),
                            usageWindow(20.0, 60.0),
                        },
                    },
                },
            });

        QVERIFY(fallbackResult.hasValue());
        QCOMPARE(
            fallbackResult.value().planType.value(),
            QStringLiteral("  pro  "));
        QCOMPARE(fallbackResult.value().groups.size(), 1);
        QCOMPARE(
            fallbackResult.value().groups.first().id,
            QStringLiteral("  fallback-id  "));
    }

    void usageTitlesSplitHyphenOnlyAndSortNaturally()
    {
        const Result<BridgeUsageSnapshot> result =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {QStringLiteral("limitId"), QStringLiteral("codex")},
                        {QStringLiteral("primary"), usageWindow(20.0, 60.0)},
                    },
                },
                {
                    QStringLiteral("rateLimitsByLimitId"),
                    QJsonObject{
                        {
                            QStringLiteral("model-10"),
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                        {
                            QStringLiteral("codex"),
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                        {
                            QStringLiteral("foo_bar"),
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                        {
                            QStringLiteral("model-2"),
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                    },
                },
            });

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().groups.size(), 4);
        QCOMPARE(
            result.value().groups.at(0).id,
            QStringLiteral("codex"));
        QCOMPARE(
            result.value().groups.at(1).id,
            QStringLiteral("foo_bar"));
        QCOMPARE(
            result.value().groups.at(1).title,
            QStringLiteral("Foo_bar"));
        QCOMPARE(
            result.value().groups.at(2).id,
            QStringLiteral("model-2"));
        QCOMPARE(
            result.value().groups.at(2).title,
            QStringLiteral("Model 2"));
        QCOMPARE(
            result.value().groups.at(3).id,
            QStringLiteral("model-10"));
        QCOMPARE(
            result.value().groups.at(3).title,
            QStringLiteral("Model 10"));
    }

    void usageDisplayTitleUppercasesFirstGrapheme()
    {
        const QString sharpSId =
            QString::fromUtf8("\xC3\x9F" "eta-model");
        const QString combiningId =
            QStringLiteral("e")
            + QChar(0x0301)
            + QStringLiteral("clair-mode");
        const QString emoji =
            QString::fromUcs4(
                std::array<char32_t, 4>{
                    0x1F469,
                    0x200D,
                    0x1F4BB,
                    0,
                }.data());
        const QString emojiId =
            emoji + QStringLiteral("-mode");

        const Result<BridgeUsageSnapshot> result =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {QStringLiteral("limitId"), QStringLiteral("codex")},
                        {QStringLiteral("primary"), usageWindow(20.0, 60.0)},
                    },
                },
                {
                    QStringLiteral("rateLimitsByLimitId"),
                    QJsonObject{
                        {
                            sharpSId,
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                        {
                            combiningId,
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                        {
                            emojiId,
                            QJsonObject{
                                {
                                    QStringLiteral("primary"),
                                    usageWindow(10.0, 60.0),
                                },
                            },
                        },
                    },
                },
            });

        QVERIFY(result.hasValue());
        QHash<QString, QString> titles;
        for (const BridgeUsageGroup& group : result.value().groups) {
            titles.insert(group.id, group.title);
        }
        QCOMPARE(
            titles.value(sharpSId),
            QStringLiteral("SSeta Model"));
        QCOMPARE(
            titles.value(combiningId),
            QStringLiteral("E")
                + QChar(0x0301)
                + QStringLiteral("clair Mode"));
        QCOMPARE(
            titles.value(emojiId),
            emoji + QStringLiteral(" Mode"));
    }

    void usagePreservesFullRangeAvailableCount()
    {
        constexpr qint64 beyondSafeDouble =
            9'007'199'254'740'993LL;
        const qint64 qint64Min =
            std::numeric_limits<qint64>::min();
        const qint64 qint64Max =
            std::numeric_limits<qint64>::max();

        QJsonObject highCount = validUsageObject();
        QJsonObject credits =
            highCount
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            QJsonValue(beyondSafeDouble));
        highCount.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        const Result<BridgeUsageSnapshot> highResult =
            readUsageObject(highCount);
        QVERIFY(highResult.hasValue());
        QCOMPARE(
            highResult.value().availableResetCount,
            beyondSafeDouble);

        QJsonObject maxCount = validUsageObject();
        credits =
            maxCount
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            QJsonValue(qint64Max));
        maxCount.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        const Result<BridgeUsageSnapshot> maxResult =
            readUsageObject(maxCount);
        QVERIFY(maxResult.hasValue());
        QCOMPARE(maxResult.value().availableResetCount, qint64Max);

        QJsonObject minCount = validUsageObject();
        credits =
            minCount
                .value(QStringLiteral("rateLimitResetCredits"))
                .toObject();
        credits.insert(
            QStringLiteral("availableCount"),
            QJsonValue(qint64Min));
        minCount.insert(
            QStringLiteral("rateLimitResetCredits"),
            credits);
        const Result<BridgeUsageSnapshot> minResult =
            readUsageObject(minCount);
        QVERIFY(minResult.hasValue());
        QCOMPARE(minResult.value().availableResetCount, qint64(0));
    }

    void usageClassifiesShortWindowsFromRawDuration()
    {
        const Result<BridgeUsageSnapshot> result =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {QStringLiteral("limitId"), QStringLiteral("codex")},
                        {QStringLiteral("primary"), usageWindow(20.0, 60.0)},
                        {
                            QStringLiteral("secondary"),
                            usageWindow(40.0, 1439.0),
                        },
                    },
                },
            });

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().groups.size(), 1);
        const BridgeUsageGroup group =
            result.value().groups.first();
        QVERIFY(group.shortWindow.has_value());
        QCOMPARE(
            group.shortWindow->durationLabel,
            QStringLiteral("24h"));
        QCOMPARE(group.shortWindow->remainingPercent, 60.0);
        QVERIFY(!group.weeklyWindow.has_value());
    }

    void usageDurationLabelUsesChecked64BitSeconds()
    {
        constexpr qint64 seconds =
            qint64(std::numeric_limits<int>::max()) + 1;
        const qint64 expectedWeeks =
            std::max<qint64>(
                1,
                static_cast<qint64>(
                    std::ceil(
                        static_cast<long double>(seconds)
                        / static_cast<long double>(
                            7 * 24 * 60 * 60))));
        const Result<BridgeUsageSnapshot> result =
            readUsageObject({
                {
                    QStringLiteral("rateLimits"),
                    QJsonObject{
                        {QStringLiteral("limitId"), QStringLiteral("codex")},
                        {
                            QStringLiteral("primary"),
                            usageWindow(
                                20.0,
                                static_cast<double>(seconds) / 60.0),
                        },
                    },
                },
            });

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().groups.size(), 1);
        const BridgeUsageGroup group =
            result.value().groups.first();
        QVERIFY(group.weeklyWindow.has_value());
        QCOMPARE(
            group.weeklyWindow->durationLabel,
            QStringLiteral("%1 Week").arg(expectedWeeks));
        QVERIFY(!group.shortWindow.has_value());
    }

    void usageRejectsOutOfSupportedRangeDuration()
    {
        QJsonObject object = validUsageObject();
        QJsonObject rateLimits =
            object.value(QStringLiteral("rateLimits")).toObject();
        rateLimits.insert(
            QStringLiteral("primary"),
            usageWindow(
                20.0,
                static_cast<double>(
                    std::numeric_limits<qint64>::max())
                    / 30.0));
        object.insert(QStringLiteral("rateLimits"), rateLimits);

        expectUsageUnavailable(object);
    }
};

} // namespace

QTEST_GUILESS_MAIN(CommandServiceTests)

#include "CommandServiceTests.moc"
