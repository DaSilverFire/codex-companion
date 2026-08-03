#include "codex/chat/WindowsOnDeviceChatBackend.h"
#include "codex/runtime/CodexRuntimeDependencyFactory.h"
#include "core/CredentialStore.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFuture>
#include <QMutex>
#include <QMutexLocker>
#include <QPromise>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

using namespace companion;

namespace {

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
        std::function<void()> worker;
        {
            const std::scoped_lock lock(
                mutex_);
            QVERIFY(!workers_.empty());
            worker =
                std::move(workers_.front());
            workers_.pop_front();
        }
        worker();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>>
        workers_;
};

class MemoryCredentialStore final
    : public CredentialStore {
public:
    Result<QByteArray> read(
        const QString& service) const override
    {
        const QMutexLocker lock(&mutex_);
        const auto value =
            values_.constFind(service);
        if (value == values_.constEnd()) {
            return Result<QByteArray>::failure({
                QStringLiteral(
                    "credential.not_found"),
                QStringLiteral(
                    "Credential was not found."),
                false,
                {},
            });
        }
        return Result<QByteArray>::success(
            value.value());
    }

    Result<void> write(
        const QString& service,
        QByteArrayView secret) override
    {
        const QMutexLocker lock(&mutex_);
        values_.insert(
            service,
            QByteArray(
                secret.data(),
                secret.size()));
        return Result<void>::success();
    }

    Result<void> remove(
        const QString& service) override
    {
        const QMutexLocker lock(&mutex_);
        values_.remove(service);
        return Result<void>::success();
    }

    bool contains(
        const QString& service) const override
    {
        const QMutexLocker lock(&mutex_);
        return values_.contains(service);
    }

private:
    mutable QMutex mutex_;
    QHash<QString, QByteArray> values_;
};

class NullChatTransport final
    : public ChatHttpTransport {
public:
    Result<ChatHttpResponse> post(
        const ChatHttpRequest&) override
    {
        return Result<ChatHttpResponse>::failure({
            QStringLiteral(
                "chat.transport_unavailable"),
            QStringLiteral(
                "Transport is unavailable."),
            true,
            {},
        });
    }
};

class FakeBackend final
    : public WindowsOnDeviceChatBackend {
private:
    struct SharedState final {
        mutable QMutex mutex;
        WindowsOnDeviceChatStatus status;
        quint64 nextSubscriptionId = 0;
        QHash<
            quint64,
            std::function<void(
                WindowsOnDeviceChatStatus)>>
            observers;
    };

    class Subscription final
        : public WindowsOnDeviceChatStatusSubscription {
    public:
        Subscription(
            std::weak_ptr<SharedState> state,
            quint64 subscriptionId)
            : state_(std::move(state)),
              subscriptionId_(subscriptionId)
        {
        }

        ~Subscription() override
        {
            const auto state = state_.lock();
            if (state == nullptr) {
                return;
            }
            const QMutexLocker lock(
                &state->mutex);
            state->observers.remove(
                subscriptionId_);
        }

    private:
        std::weak_ptr<SharedState> state_;
        quint64 subscriptionId_ = 0;
    };

public:
    FakeBackend()
        : state_(
              std::make_shared<SharedState>())
    {
        state_->status.phase =
            WindowsOnDeviceChatPhase::Ready;
        state_->status.downloadConsentGranted =
            true;
        state_->status.available = true;
        state_->status.supportsAttachments =
            false;
        state_->status.progressPercent = 100.0;
        state_->status.revision = 1;
    }

    WindowsOnDeviceChatStatus status()
        const override
    {
        const QMutexLocker lock(
            &state_->mutex);
        return state_->status;
    }

    Result<void> setDownloadConsent(
        bool granted) override
    {
        publish([granted](
                    WindowsOnDeviceChatStatus& status) {
            status.downloadConsentGranted =
                granted;
            status.available = granted;
            status.phase = granted
                ? WindowsOnDeviceChatPhase::Ready
                : WindowsOnDeviceChatPhase::
                      ConsentRequired;
        });
        return Result<void>::success();
    }

    QFuture<Result<void>> prepare() override
    {
        QPromise<Result<void>> promise;
        promise.start();
        QFuture<Result<void>> future =
            promise.future();
        promise.addResult(
            Result<void>::success());
        promise.finish();
        return future;
    }

    std::shared_ptr<
        WindowsOnDeviceChatStatusSubscription>
    subscribeStatus(
        std::function<void(
            WindowsOnDeviceChatStatus)>
            observer) override
    {
        const QMutexLocker lock(
            &state_->mutex);
        const quint64 id =
            ++state_->nextSubscriptionId;
        state_->observers.insert(
            id,
            std::move(observer));
        return std::make_shared<Subscription>(
            state_,
            id);
    }

    Result<ChatResult> send(
        const ChatRequest&) override
    {
        return Result<ChatResult>::success({
            QStringLiteral("local answer"),
            std::nullopt,
            std::nullopt,
        });
    }

    void setAvailable(bool available)
    {
        publish([available](
                    WindowsOnDeviceChatStatus& status) {
            status.available = available;
            status.phase = available
                ? WindowsOnDeviceChatPhase::Ready
                : WindowsOnDeviceChatPhase::Failed;
        });
    }

    qsizetype observerCount() const
    {
        const QMutexLocker lock(
            &state_->mutex);
        return state_->observers.size();
    }

private:
    void publish(
        const std::function<void(
            WindowsOnDeviceChatStatus&)>&
            update)
    {
        QVector<
            std::function<void(
                WindowsOnDeviceChatStatus)>>
            observers;
        WindowsOnDeviceChatStatus status;
        {
            const QMutexLocker lock(
                &state_->mutex);
            update(state_->status);
            ++state_->status.revision;
            status = state_->status;
            observers =
                state_->observers.values();
        }
        for (const auto& observer :
             observers) {
            observer(status);
        }
    }

    std::shared_ptr<SharedState> state_;
};

CodexEnvironment environment(
    const QString& root)
{
    CodexEnvironment result;
    result.homeDirectory = root;
    result.localAppData = root;
    result.codexHome =
        root + QStringLiteral("/.codex");
    result.stateDatabase =
        result.codexHome
        + QStringLiteral("/state_5.sqlite");
    result.sessionIndex =
        result.codexHome
        + QStringLiteral(
            "/session_index.jsonl");
    result.rolloutRoot =
        result.codexHome
        + QStringLiteral("/sessions");
    result.configToml =
        result.codexHome
        + QStringLiteral("/config.toml");
    result.petRoot =
        root + QStringLiteral("/pets");
    result.codexBinRoot =
        root + QStringLiteral("/bin");
    return result;
}

CodexRuntimeProductionServices services(
    const CodexEnvironment& environment,
    const QString& attachmentRoot,
    const std::shared_ptr<FakeBackend>&
        backend)
{
    CodexRuntimeProductionServices result;
    result.attachmentStore =
        std::make_shared<AttachmentStore>(
            attachmentRoot);
    result.taskCommandService =
        std::make_shared<TaskCommandService>(
            environment);
    result.approvalService =
        std::make_shared<ApprovalService>(
            environment);
    result.taskCreator =
        std::make_shared<TaskCreator>(
            environment);
    result.credentialStore =
        std::make_shared<
            MemoryCredentialStore>();
    result.openAITransport =
        std::make_shared<
            NullChatTransport>();
    result.lumoTransport =
        std::make_shared<
            NullChatTransport>();
    result.onDeviceBackend = backend;
    result.goalService =
        std::make_shared<GoalService>(
            environment);
    result.usageService =
        std::make_shared<UsageService>(
            environment);
    return result;
}

} // namespace

class CodexRuntimeDependencyFactoryTests final
    : public QObject {
    Q_OBJECT

private slots:
    void missingProductionOwnerIsRejected()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const auto backend =
            std::make_shared<FakeBackend>();
        const CodexRuntimeProductionServices
            valid = services(
                discovered,
                directory.filePath(
                    QStringLiteral(
                        "attachments")),
                backend);
        const QVector<std::function<void(
            CodexRuntimeProductionServices&)>>
            removals{
                [](auto& value) {
                    value.attachmentStore.reset();
                },
                [](auto& value) {
                    value.taskCommandService.reset();
                },
                [](auto& value) {
                    value.approvalService.reset();
                },
                [](auto& value) {
                    value.taskCreator.reset();
                },
                [](auto& value) {
                    value.credentialStore.reset();
                },
                [](auto& value) {
                    value.openAITransport.reset();
                },
                [](auto& value) {
                    value.lumoTransport.reset();
                },
                [](auto& value) {
                    value.goalService.reset();
                },
                [](auto& value) {
                    value.usageService.reset();
                },
            };

        ManualExecutor executor;
        for (const auto& remove : removals) {
            CodexRuntimeProductionServices
                candidate = valid;
            remove(candidate);
            const auto built =
                CodexRuntimeDependencyFactory::
                    build(
                        discovered,
                        std::move(candidate),
                        executor.executor(),
                        [] {
                            return QDateTime(
                                QDate(2026, 7, 23),
                                QTime(12, 0),
                                QTimeZone::UTC);
                        });
            QVERIFY(!built.hasValue());
            QCOMPARE(
                built.error().code,
                QStringLiteral(
                    "codex.runtime_unavailable"));
            QVERIFY(
                built.error().context.isEmpty());
        }
    }

    void missingOnDeviceBackendStillBuildsCloudRuntime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const auto backend =
            std::make_shared<FakeBackend>();
        CodexRuntimeProductionServices
            owners =
                services(
                    discovered,
                    directory.filePath(
                        QStringLiteral(
                            "attachments")),
                    backend);
        owners.onDeviceBackend.reset();

        const auto built =
            CodexRuntimeDependencyFactory::
                build(
                    discovered,
                    std::move(owners));

        QVERIFY(built.hasValue());
        QVERIFY(
            built.value()
                .onDeviceBackend
            == nullptr);
        QVERIFY(
            built.value()
                .dependencies
                .mutations
                .has_value());
    }

    void validFactoryBuildsCompleteD3Bundle()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const auto backend =
            std::make_shared<FakeBackend>();
        ManualExecutor executor;

        const auto built =
            CodexRuntimeDependencyFactory::build(
                discovered,
                services(
                    discovered,
                    directory.filePath(
                        QStringLiteral(
                            "attachments")),
                    backend),
                executor.executor(),
                [] {
                    return QDateTime(
                        QDate(2026, 7, 23),
                        QTime(12, 0),
                        QTimeZone::UTC);
                });

        QVERIFY(built.hasValue());
        const CodexRuntimeDependencies&
            dependencies =
                built.value().dependencies;
        QVERIFY(dependencies.taskLoader);
        QVERIFY(dependencies.goalLoader);
        QVERIFY(dependencies.executor);
        QVERIFY(dependencies.nowProvider);
        QVERIFY(dependencies.history.has_value());
        QVERIFY(
            dependencies.history
                ->historyLoader);
        QVERIFY(
            dependencies.history
                ->historyCoordinator
            != nullptr);
        QVERIFY(dependencies.reads.has_value());
        QVERIFY(
            dependencies.reads
                ->capabilityLoader);
        QVERIFY(
            dependencies.reads
                ->usageReadStarter);
        QVERIFY(
            dependencies.mutations.has_value());
        QVERIFY(
            dependencies.mutations
                ->sendMutationStarter);
        QVERIFY(
            dependencies.mutations
                ->approvalMutationStarter);
        QVERIFY(
            dependencies.mutations
                ->taskCreateMutationStarter);
        QVERIFY(
            dependencies.mutations
                ->chatMutationStarter);
        QVERIFY(
            dependencies.mutations
                ->goalMutationStarter);
        QVERIFY(
            dependencies.mutations
                ->usageResetMutationStarter);
        QCOMPARE(
            built.value().onDeviceBackend,
            backend);
    }

    void lifetimeRetainsOwnersAndInvalidatesRevisions()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        auto backend =
            std::make_shared<FakeBackend>();
        CodexRuntimeProductionServices owners =
            services(
                discovered,
                directory.filePath(
                    QStringLiteral(
                        "attachments")),
                backend);
        const std::weak_ptr<AttachmentStore>
            weakAttachments =
                owners.attachmentStore;
        const std::weak_ptr<TaskCommandService>
            weakTaskCommands =
                owners.taskCommandService;
        const std::weak_ptr<ApprovalService>
            weakApprovals =
                owners.approvalService;
        const std::weak_ptr<TaskCreator>
            weakCreator =
                owners.taskCreator;
        const std::weak_ptr<CredentialStore>
            weakCredentials =
                owners.credentialStore;
        const std::weak_ptr<ChatHttpTransport>
            weakOpenAI =
                owners.openAITransport;
        const std::weak_ptr<ChatHttpTransport>
            weakLumo =
                owners.lumoTransport;
        const std::weak_ptr<
            WindowsOnDeviceChatBackend>
            weakBackend =
                owners.onDeviceBackend;
        const std::weak_ptr<GoalService>
            weakGoals =
                owners.goalService;
        const std::weak_ptr<UsageService>
            weakUsage =
                owners.usageService;

        ManualExecutor executor;
        auto created =
            CodexRuntimeDependencyFactory::create(
                discovered,
                std::move(owners),
                executor.executor(),
                [] {
                    return QDateTime(
                        QDate(2026, 7, 23),
                        QTime(12, 0),
                        QTimeZone::UTC);
                });
        QVERIFY(created.hasValue());
        std::unique_ptr<CodexRuntimeLifetime>
            lifetime =
                std::move(created.value());
        QVERIFY(
            lifetime->runtime()
                .start()
                .hasValue());
        QCOMPARE(executor.pendingCount(), 1);
        QCOMPARE(backend->observerCount(), 1);

        QVERIFY(!weakAttachments.expired());
        QVERIFY(!weakTaskCommands.expired());
        QVERIFY(!weakApprovals.expired());
        QVERIFY(!weakCreator.expired());
        QVERIFY(!weakCredentials.expired());
        QVERIFY(!weakOpenAI.expired());
        QVERIFY(!weakLumo.expired());
        QVERIFY(!weakBackend.expired());
        QVERIFY(!weakGoals.expired());
        QVERIFY(!weakUsage.expired());

        const quint64 beforeBackend =
            lifetime->runtime()
                .capabilityRevision();
        backend->setAvailable(false);
        QVERIFY(waitUntil([&] {
            return lifetime->runtime()
                       .capabilityRevision()
                == beforeBackend + 1;
        }));

        const quint64 beforeCredential =
            lifetime->runtime()
                .capabilityRevision();
        lifetime
            ->notifyCredentialStateChanged();
        QVERIFY(waitUntil([&] {
            return lifetime->runtime()
                       .capabilityRevision()
                == beforeCredential + 1;
        }));

        lifetime.reset();
        QCOMPARE(backend->observerCount(), 0);
        backend.reset();
        QVERIFY(weakAttachments.expired());
        QVERIFY(weakTaskCommands.expired());
        QVERIFY(weakApprovals.expired());
        QVERIFY(weakCreator.expired());
        QVERIFY(weakCredentials.expired());
        QVERIFY(weakOpenAI.expired());
        QVERIFY(weakLumo.expired());
        QVERIFY(weakBackend.expired());
        QVERIFY(weakGoals.expired());
        QVERIFY(weakUsage.expired());
    }

    void taskCreateCancellationRollsBackOwnedBatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const QString attachmentRoot =
            directory.filePath(
                QStringLiteral(
                    "attachments"));
        const auto backend =
            std::make_shared<FakeBackend>();
        ManualExecutor executor;
        auto built =
            CodexRuntimeDependencyFactory::build(
                discovered,
                services(
                    discovered,
                    attachmentRoot,
                    backend),
                executor.executor());
        QVERIFY(built.hasValue());

        BridgeAttachment attachment;
        attachment.id = QUuid(
            QStringLiteral(
                "{0955FB1D-9828-456F-954F-F53DF900B8DE}"));
        attachment.kind =
            AttachmentKind::File;
        attachment.filename =
            QStringLiteral("notes.txt");
        attachment.mimeType =
            QStringLiteral("text/plain");
        attachment.data =
            QByteArray("notes");
        RuntimeTaskCreateRequest request;
        request.text =
            QStringLiteral("create");
        request.clientMessageId =
            QStringLiteral(
                "7f46c6d8-074e-4ec2-b927-a858c198a8c1");
        request.attachments.append(
            attachment);

        CommitAwareMutationHandle<QString>
            handle =
                built.value()
                    .dependencies
                    .mutations
                    ->taskCreateMutationStarter(
                        request);
        QVERIFY(handle.terminalFuture.isValid());
        QVERIFY(
            handle.requestStopBeforeCommit);
        QCOMPARE(executor.pendingCount(), 1);
        handle.requestStopBeforeCommit();
        executor.runNext();
        handle.terminalFuture.waitForFinished();

        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<QString> result =
            handle.terminalFuture.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        const QDir root(attachmentRoot);
        QCOMPARE(
            root.entryList(
                    QDir::Dirs
                        | QDir::NoDotAndDotDot)
                .size(),
            0);
    }

    void customTaskCreatePerformerKeepsAttachmentStaging()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const QString attachmentRoot =
            directory.filePath(
                QStringLiteral(
                    "attachments"));
        const auto backend =
            std::make_shared<FakeBackend>();
        ManualExecutor executor;
        CodexRuntimeProductionServices owners =
            services(
                discovered,
                attachmentRoot,
                backend);
        owners.taskCreator.reset();
        CreateTaskRequest captured;
        bool called = false;
        owners.taskCreatePerformer =
            [&captured, &called](
                const CreateTaskRequest&
                    request) {
                captured = request;
                called = true;
                return Result<QString>::success(
                    QStringLiteral(
                        "thread-profiled"));
            };

        auto built =
            CodexRuntimeDependencyFactory::build(
                discovered,
                std::move(owners),
                executor.executor());
        QVERIFY(built.hasValue());

        BridgeAttachment attachment;
        attachment.id = QUuid(
            QStringLiteral(
                "{ACEC51FB-C05B-47E7-A0B4-B594ED13F51C}"));
        attachment.kind =
            AttachmentKind::File;
        attachment.filename =
            QStringLiteral("profile.txt");
        attachment.mimeType =
            QStringLiteral("text/plain");
        attachment.data =
            QByteArray("profile");
        RuntimeTaskCreateRequest request;
        request.text =
            QStringLiteral(
                "create with profile");
        request.clientMessageId =
            QStringLiteral(
                "eb5f2297-60d7-446c-889c-aa3323835a48");
        request.attachments.append(
            attachment);

        auto handle =
            built.value()
                .dependencies
                .mutations
                ->taskCreateMutationStarter(
                    request);
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        handle.terminalFuture.waitForFinished();

        QVERIFY(called);
        QCOMPARE(
            handle.terminalFuture
                .result()
                .value(),
            QStringLiteral(
                "thread-profiled"));
        QCOMPARE(
            captured.prompt,
            request.text);
        QCOMPARE(
            captured.attachments.size(),
            1);
        QVERIFY(
            QFileInfo(
                captured.attachments
                    .constFirst()
                    .fsPath)
                .isFile());
    }

    void chatAdapterCommitsBeforeProviderSend()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const CodexEnvironment discovered =
            environment(directory.path());
        const auto backend =
            std::make_shared<FakeBackend>();
        ManualExecutor executor;
        auto built =
            CodexRuntimeDependencyFactory::build(
                discovered,
                services(
                    discovered,
                    directory.filePath(
                        QStringLiteral(
                            "attachments")),
                    backend),
                executor.executor());
        QVERIFY(built.hasValue());

        CommitAwareMutationHandle<ChatResult>
            handle =
                built.value()
                    .dependencies
                    .mutations
                    ->chatMutationStarter({
                        ChatProvider::OnDevice,
                        QStringLiteral(
                            "on-device"),
                        QStringLiteral("prompt"),
                        {},
                    });
        QVERIFY(handle.terminalFuture.isValid());
        QVERIFY(
            handle.requestStopBeforeCommit);
        QCOMPARE(executor.pendingCount(), 1);

        handle.requestStopBeforeCommit();
        executor.runNext();
        handle.terminalFuture.waitForFinished();

        QCOMPARE(
            handle.terminalFuture.resultCount(),
            1);
        const Result<ChatResult> result =
            handle.terminalFuture.result();
        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().text,
            QStringLiteral("local answer"));
    }
};

QTEST_GUILESS_MAIN(
    CodexRuntimeDependencyFactoryTests)

#include "CodexRuntimeDependencyFactoryTests.moc"
