#include "app/CompanionRuntimeHost.h"
#include "codex/runtime/ProcessListModel.h"
#include "codex/runtime/CodexRuntime.h"
#include "core/ChatCredentialService.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"
#include "core/CredentialStore.h"
#include "ui/CompanionShellViewModel.h"

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPromise>
#include <QSignalSpy>
#include <QtTest>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace companion::detail {

struct CodexRuntimeTestAccess final {
    static void publishProcessSnapshot(
        CodexRuntime& runtime,
        CodexProcessSnapshot snapshot)
    {
        runtime.processSnapshot_ =
            std::move(snapshot);
        if (!runtime.state_.isNull()) {
            runtime.state_->tasks()->setSnapshot(
                runtime.processSnapshot_.tasks);
        }
        emit runtime.processSnapshotChanged();
    }

    static void publishUsage(
        CodexRuntime& runtime,
        BridgeUsageSnapshot snapshot)
    {
        runtime.usageLoading_ = false;
        runtime.usageErrorCode_.clear();
        runtime.usageErrorMessage_.clear();
        runtime.usageSnapshot_ =
            std::move(snapshot);
        emit runtime.usageChanged();
    }
};

} // namespace companion::detail

namespace {

using namespace companion;

CompanionError missingCredential(
    const QString& service)
{
    return {
        QStringLiteral("credential.not_found"),
        QStringLiteral(
            "The requested credential was not found."),
        false,
        {{QStringLiteral("service"), service}},
    };
}

class MemoryCredentialStore final
    : public CredentialStore {
public:
    Result<QByteArray> read(
        const QString& service) const override
    {
        const QMutexLocker lock(&mutex_);
        const auto value = values_.constFind(service);
        if (value == values_.constEnd()) {
            return Result<QByteArray>::failure(
                missingCredential(service));
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

class FakeStatusSubscription final
    : public WindowsOnDeviceChatStatusSubscription {
public:
    explicit FakeStatusSubscription(
        std::function<void()> onDestroy)
        : onDestroy_(std::move(onDestroy))
    {
    }

    ~FakeStatusSubscription() override
    {
        if (onDestroy_) {
            onDestroy_();
        }
    }

private:
    std::function<void()> onDestroy_;
};

class FakeOnDeviceBackend final
    : public WindowsOnDeviceChatBackend,
      public std::enable_shared_from_this<
          FakeOnDeviceBackend> {
public:
    WindowsOnDeviceChatStatus status()
        const override
    {
        return status_;
    }

    Result<void> setDownloadConsent(
        bool granted) override
    {
        status_.downloadConsentGranted = granted;
        return Result<void>::success();
    }

    QFuture<Result<void>> prepare() override
    {
        return readyFuture(
            Result<void>::success());
    }

    std::shared_ptr<
        WindowsOnDeviceChatStatusSubscription>
    subscribeStatus(
        std::function<void(
            WindowsOnDeviceChatStatus)>
            observer) override
    {
        observer_ = std::move(observer);
        const std::weak_ptr<FakeOnDeviceBackend>
            weakBackend = weak_from_this();
        return std::make_shared<
            FakeStatusSubscription>(
                [weakBackend] {
                    if (const auto backend =
                            weakBackend.lock()) {
                        backend->observer_ = {};
                    }
                });
    }

    Result<ChatResult> send(
        const ChatRequest& request) override
    {
        lastRequest_ = request;
        return Result<ChatResult>::success({
            QStringLiteral("On-device reply"),
            std::nullopt,
            std::nullopt,
        });
    }

    void publish(
        WindowsOnDeviceChatStatus status)
    {
        status_ = status;
        const auto observer = observer_;
        if (observer) {
            observer(status_);
        }
    }

    WindowsOnDeviceChatStatus status_;
    std::optional<ChatRequest> lastRequest_;

private:
    template <typename T>
    static QFuture<T> readyFuture(T result)
    {
        QPromise<T> promise;
        promise.start();
        QFuture<T> future = promise.future();
        promise.addResult(std::move(result));
        promise.finish();
        return future;
    }

    std::function<void(
        WindowsOnDeviceChatStatus)>
        observer_;
};

template <typename T>
QFuture<T> readyFuture(T result)
{
    QPromise<T> promise;
    promise.start();
    QFuture<T> future = promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
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
        return QDateTime::fromSecsSinceEpoch(
            1'721'737'600,
            QTimeZone::UTC);
    };
}

BridgeDate currentBridgeDate()
{
    constexpr double swiftReferenceDateUnixSeconds =
        978307200.0;
    return {
        static_cast<double>(
            QDateTime::currentMSecsSinceEpoch())
            / 1000.0
        - swiftReferenceDateUnixSeconds,
    };
}

std::unique_ptr<CompanionRuntimeHost>
createHost(
    CompanionShellViewModel& shell,
    std::shared_ptr<CredentialStore> credentials,
    std::shared_ptr<WindowsOnDeviceChatBackend>
        onDeviceBackend,
    CompanionRuntimeHost::ChatRequestSender
        chatSender,
    bool validRuntime = true,
    TaskListModel** attachedModel = nullptr,
    CompanionCommandBus** attachedCommandBus = nullptr,
    CodexRuntime** attachedRuntime = nullptr)
{
    auto state =
        std::make_unique<CompanionState>();
    auto commandBus =
        std::make_unique<CompanionCommandBus>();
    if (attachedModel != nullptr) {
        *attachedModel = state->tasks();
    }

    RuntimeTaskLoader taskLoader;
    if (validRuntime) {
        taskLoader = [](
                         const QHash<
                             QString,
                             BridgeGoal>&,
                         std::stop_token) {
            return Result<
                CodexProcessSnapshot>::success(
                    {});
        };
    }
    RuntimeExecutor executor =
        [](std::function<void()>) {
        };
    auto runtime =
        std::make_unique<CodexRuntime>(
            *state,
            *commandBus,
            std::move(taskLoader),
            emptyGoalLoader(),
            std::move(executor),
            fixedNow());
    if (attachedCommandBus != nullptr) {
        *attachedCommandBus = commandBus.get();
    }
    if (attachedRuntime != nullptr) {
        *attachedRuntime = runtime.get();
    }

    return detail::
        CompanionRuntimeHostTestAccess::create(
            shell,
            std::move(state),
            std::move(commandBus),
            std::move(runtime),
            std::move(credentials),
            std::move(onDeviceBackend),
            std::move(chatSender));
}

class CompanionRuntimeHostTests final
    : public QObject {
    Q_OBJECT

private slots:
    void attachesProcessModelAndSurfacesStartFailure()
    {
        CompanionShellViewModel shell;
        TaskListModel* attachedModel = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            false,
            &attachedModel);

        QVERIFY(host != nullptr);
        QVERIFY(
            qobject_cast<ProcessListModel*>(
                shell.processModel())
            != nullptr);
        QVERIFY(shell.processModel() != attachedModel);
        QCOMPARE(host->taskModel(), attachedModel);

        const auto started = host->start();

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(
            shell.processErrorMessage(),
            QStringLiteral(
                "Codex runtime is unavailable."));
    }

    void publishesJobsToTheDesktopProcessModel()
    {
        CompanionShellViewModel shell;
        TaskListModel* taskModel = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            &taskModel,
            nullptr,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(taskModel != nullptr);
        QVERIFY(runtime != nullptr);
        auto* processModel =
            qobject_cast<ProcessListModel*>(
                shell.processModel());
        QVERIFY(processModel != nullptr);

        BridgeTask task;
        task.id = QStringLiteral("thread-active");
        task.title =
            QStringLiteral("Active thread");
        task.status = TaskStatus::Running;
        task.updatedAt = currentBridgeDate();

        CodexJobRecord job;
        job.id = QStringLiteral("build");
        job.name = QStringLiteral("Native build");
        job.status = QStringLiteral("running");
        job.instruction =
            QStringLiteral("Build the Windows app");
        job.threadId =
            QStringLiteral("thread-active");
        job.updatedAt =
            QDateTime::currentDateTimeUtc();
        job.startedAt =
            job.updatedAt.addSecs(-20);

        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(task);
        snapshot.jobs.append(job);
        detail::CodexRuntimeTestAccess::
            publishProcessSnapshot(
                *runtime,
                std::move(snapshot));

        QCOMPARE(taskModel->rowCount(), 1);
        QCOMPARE(processModel->rowCount(), 2);
        bool foundJob = false;
        for (int row = 0;
             row < processModel->rowCount();
             ++row) {
            const QModelIndex index =
                processModel->index(row, 0);
            if (processModel
                    ->data(
                        index,
                        ProcessListModel::
                            ProcessIdRole)
                    .toString()
                != QStringLiteral("job-build")) {
                continue;
            }
            foundJob = true;
            QCOMPARE(
                processModel
                    ->data(
                        index,
                        ProcessListModel::
                            ThreadIdRole)
                    .toString(),
                QStringLiteral("thread-active"));
        }
        QVERIFY(foundJob);
    }

    void mapsOpenAIAndLumoSelectionsIntoRequests()
    {
        auto credentials =
            std::make_shared<
                MemoryCredentialStore>();
        QVERIFY(
            ChatCredentialService::save(
                *credentials,
                ChatCredentialKind::OpenAI,
                QStringLiteral("openai-key"))
                .hasValue());
        QVERIFY(
            ChatCredentialService::save(
                *credentials,
                ChatCredentialKind::Lumo,
                QStringLiteral("lumo-key"))
                .hasValue());
        CompanionShellViewModel shell(
            QStringLiteral(
                "openai:gpt56Terra"),
            {});
        QVector<ChatRequest> requests;
        auto host = createHost(
            shell,
            credentials,
            {},
            [&requests](
                const ChatRequest& request) {
                requests.append(request);
                return readyFuture(
                    Result<ChatResult>::
                        success({
                            QStringLiteral(
                                "Mapped reply"),
                            12,
                            34,
                        }));
            });
        QVERIFY(host != nullptr);
        QSignalSpy animationSpy(
            host.get(),
            &CompanionRuntimeHost::
                petAnimationRequested);
        QVERIFY(animationSpy.isValid());
        QVERIFY(shell.chatSendEnabled());

        shell.submitLocalChat(
            QStringLiteral("  OpenAI prompt  "));

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            1,
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.chatBusy(),
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            animationSpy.count(),
            2,
            1000);
        QCOMPARE(
            animationSpy.at(0).at(0).toString(),
            QStringLiteral("waving"));
        QCOMPARE(
            animationSpy.at(1).at(0).toString(),
            QStringLiteral("review"));
        QCOMPARE(
            requests.at(0).provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            requests.at(0).modelId,
            QStringLiteral("gpt56Terra"));
        QCOMPARE(
            requests.at(0).prompt,
            QStringLiteral("OpenAI prompt"));
        QCOMPARE(
            shell.chatResponseTitle(),
            QStringLiteral("5.6 Terra"));
        QCOMPARE(
            shell.chatResponsePrompt(),
            QStringLiteral("OpenAI prompt"));
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral("Mapped reply"));
        QCOMPARE(
            shell.chatResponseUsageSummary(),
            QStringLiteral("12 in \u00b7 34 out"));

        shell.setSelectedChatModelId(
            QStringLiteral("lumo:thinking"));
        QVERIFY(shell.chatSendEnabled());
        shell.submitLocalChat(
            QStringLiteral("Lumo prompt"));

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            2,
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.chatBusy(),
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            animationSpy.count(),
            4,
            1000);
        QCOMPARE(
            animationSpy.at(2).at(0).toString(),
            QStringLiteral("waving"));
        QCOMPARE(
            animationSpy.at(3).at(0).toString(),
            QStringLiteral("review"));
        QCOMPARE(
            requests.at(1).provider,
            ChatProvider::LumoAPI);
        QCOMPARE(
            requests.at(1).modelId,
            QStringLiteral("thinking"));
        QCOMPARE(
            requests.at(1).prompt,
            QStringLiteral("Lumo prompt"));
        QCOMPARE(
            shell.chatResponseTitle(),
            QStringLiteral("Lumo Thinking"));
        QCOMPARE(
            shell.chatResponsePrompt(),
            QStringLiteral("Lumo prompt"));
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral("Mapped reply"));
        QCOMPARE(
            shell.chatResponseUsageSummary(),
            QStringLiteral("12 in \u00b7 34 out"));
    }

    void refreshesCredentialAndOnDeviceAvailability()
    {
        auto credentials =
            std::make_shared<
                MemoryCredentialStore>();
        CompanionShellViewModel cloudShell(
            QStringLiteral(
                "openai:gpt56Luna"),
            {});
        auto cloudHost = createHost(
            cloudShell,
            credentials,
            {},
            {});
        QVERIFY(cloudHost != nullptr);
        QVERIFY(cloudShell.chatSendEnabled());
        QCOMPARE(
            cloudShell.chatStatusMessage(),
            QStringLiteral(
                "Add an OpenAI API key in Settings"));

        QVERIFY(
            ChatCredentialService::save(
                *credentials,
                ChatCredentialKind::OpenAI,
                QStringLiteral("openai-key"))
                .hasValue());
        cloudHost->refreshChatAvailability();
        QVERIFY(cloudShell.chatSendEnabled());
        QCOMPARE(
            cloudShell.chatStatusMessage(),
            QStringLiteral("OpenAI API ready"));

        auto backend =
            std::make_shared<
                FakeOnDeviceBackend>();
        backend->status_.phase =
            WindowsOnDeviceChatPhase::
                ConsentRequired;
        CompanionShellViewModel localShell;
        auto localHost = createHost(
            localShell,
            credentials,
            backend,
            {});
        QVERIFY(localHost != nullptr);
        QVERIFY(
            localShell.chatPreparationEnabled());
        QVERIFY(!localShell.chatSendEnabled());

        WindowsOnDeviceChatStatus ready;
        ready.phase =
            WindowsOnDeviceChatPhase::Ready;
        ready.available = true;
        backend->publish(ready);

        QTRY_VERIFY_WITH_TIMEOUT(
            localShell.chatSendEnabled(),
            1000);
        QCOMPARE(
            localShell.chatStatusMessage(),
            QStringLiteral(
                "Ready on this Windows PC"));
    }

    void missingCloudCredentialShowsGuidanceWithoutStartingRequest()
    {
        auto credentials =
            std::make_shared<
                MemoryCredentialStore>();
        CompanionShellViewModel shell(
            QStringLiteral(
                "openai:gpt56Terra"),
            {});
        int requestCount = 0;
        auto host = createHost(
            shell,
            credentials,
            {},
            [&requestCount](const ChatRequest&) {
                ++requestCount;
                return readyFuture(
                    Result<ChatResult>::success({
                        QStringLiteral("Unexpected response"),
                        std::nullopt,
                        std::nullopt,
                    }));
            });
        QVERIFY(host != nullptr);
        QSignalSpy animationSpy(
            host.get(),
            &CompanionRuntimeHost::
                petAnimationRequested);
        QVERIFY(animationSpy.isValid());
        QVERIFY(shell.chatSendEnabled());

        shell.submitLocalChat(
            QStringLiteral("Explain this"));

        QCOMPARE(requestCount, 0);
        QVERIFY(!shell.chatBusy());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral(
                "Add an OpenAI API key in Settings to answer inside Companion."));
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral(
                "I did not open ChatGPT. To answer here, paste your OpenAI API key into Codex Companion Settings. ChatGPT Pro and API billing are separate."));
        QCOMPARE(
            shell.chatResponseTitle(),
            QStringLiteral("5.6 Terra"));
        QCOMPARE(
            shell.chatResponsePrompt(),
            QStringLiteral("Explain this"));
        QVERIFY(
            shell.chatResponseUsageSummary().isEmpty());
        QCOMPARE(animationSpy.count(), 1);
        QCOMPARE(
            animationSpy.at(0).at(0).toString(),
            QStringLiteral("waiting"));
    }

    void failedChatEmitsTypedErrorAndVisibleStatus()
    {
        auto credentials =
            std::make_shared<
                MemoryCredentialStore>();
        QVERIFY(
            ChatCredentialService::save(
                *credentials,
                ChatCredentialKind::OpenAI,
                QStringLiteral("openai-key"))
                .hasValue());
        CompanionShellViewModel shell(
            QStringLiteral(
                "openai:gpt56Sol"),
            {});
        auto host = createHost(
            shell,
            credentials,
            {},
            [](const ChatRequest&) {
                return readyFuture(
                    Result<ChatResult>::
                        failure({
                            QStringLiteral(
                                "chat.synthetic_failure"),
                            QStringLiteral(
                                "Synthetic chat failure."),
                            false,
                            {},
                        }));
            });
        QVERIFY(host != nullptr);
        QSignalSpy errorSpy(
            host.get(),
            &CompanionRuntimeHost::
                runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());
        QSignalSpy animationSpy(
            host.get(),
            &CompanionRuntimeHost::
                petAnimationRequested);
        QVERIFY(animationSpy.isValid());

        shell.submitLocalChat(
            QStringLiteral("Fail now"));

        QTRY_COMPARE_WITH_TIMEOUT(
            errorSpy.count(),
            1,
            1000);
        QVERIFY(!shell.chatBusy());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral(
                "Synthetic chat failure."));
        QCOMPARE(
            shell.chatResponseTitle(),
            QStringLiteral("5.6 Sol"));
        QCOMPARE(
            shell.chatResponsePrompt(),
            QStringLiteral("Fail now"));
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral(
                "Synthetic chat failure."));
        QVERIFY(
            shell.chatResponseUsageSummary().isEmpty());
        QCOMPARE(
            qvariant_cast<CompanionError>(
                errorSpy.takeFirst().at(0))
                .code,
            QStringLiteral(
                "chat.synthetic_failure"));
        QCOMPARE(animationSpy.count(), 2);
        QCOMPARE(
            animationSpy.at(0).at(0).toString(),
            QStringLiteral("waving"));
        QCOMPARE(
            animationSpy.at(1).at(0).toString(),
            QStringLiteral("failed"));
    }

    void pendingChatCannotPublishAfterHostDestruction()
    {
        auto credentials =
            std::make_shared<
                MemoryCredentialStore>();
        QVERIFY(
            ChatCredentialService::save(
                *credentials,
                ChatCredentialKind::OpenAI,
                QStringLiteral("openai-key"))
                .hasValue());
        CompanionShellViewModel shell(
            QStringLiteral(
                "openai:gpt56Luna"),
            {});
        auto promise =
            std::make_shared<
                QPromise<Result<ChatResult>>>();
        promise->start();
        const QFuture<Result<ChatResult>>
            future = promise->future();
        auto host = createHost(
            shell,
            credentials,
            {},
            [future](
                const ChatRequest&) {
                return future;
            });
        QVERIFY(host != nullptr);
        QSignalSpy statusSpy(
            &shell,
            &CompanionShellViewModel::
                chatStatusChanged);
        QVERIFY(statusSpy.isValid());

        shell.submitLocalChat(
            QStringLiteral("Pending"));
        QVERIFY(shell.chatBusy());
        QCOMPARE(
            shell.chatResponseTitle(),
            QStringLiteral("5.6 Luna"));
        QCOMPARE(
            shell.chatResponsePrompt(),
            QStringLiteral("Pending"));
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral("Thinking..."));
        const int statusCountBeforeDestroy =
            statusSpy.count();

        host.reset();
        promise->addResult(
            Result<ChatResult>::success({
                QStringLiteral("Late reply"),
                std::nullopt,
                std::nullopt,
            }));
        promise->finish();
        QTest::qWait(50);

        QCOMPARE(
            statusSpy.count(),
            statusCountBeforeDestroy);
        QCOMPARE(
            shell.chatResponse(),
            QStringLiteral("Thinking..."));
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral("Thinking..."));
    }

    void routesGoalControlsThroughRuntimeCommands()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);
        QVERIFY(runtime != nullptr);

        QVector<QPair<QString, QVariantMap>> requests;
        const auto registerCommand =
            [commandBus, runtime, &requests](
                const QString& command) {
                return commandBus->registerHandler(
                    command,
                    [runtime, &requests, command](
                        const QVariantMap& arguments,
                        CompanionCommandBus::Completion
                            completion) {
                        requests.append({
                            command,
                            arguments,
                        });
                        BridgeGoal result;
                        result.threadId =
                            arguments.value(
                                QStringLiteral("threadId"))
                                .toString();
                        result.objective =
                            QStringLiteral(
                                "Normalized objective");
                        result.elapsedSeconds = 20;
                        if (command
                            == QStringLiteral(
                                "codex.goal.pause")) {
                            result.status =
                                GoalStatus::Blocked;
                        } else {
                            result.status =
                                GoalStatus::Active;
                        }
                        emit runtime->goalChanged(result);
                        completion(
                            Result<void>::success());
                    });
            };
        QVERIFY(
            registerCommand(
                QStringLiteral(
                    "codex.goal.update"))
                .hasValue());
        QVERIFY(
            registerCommand(
                QStringLiteral(
                    "codex.goal.pause"))
                .hasValue());
        QVERIFY(
            registerCommand(
                QStringLiteral(
                    "codex.goal.resume"))
                .hasValue());

        QVariantMap currentGoal{
            {QStringLiteral("threadId"),
             QStringLiteral("thread-goal")},
            {QStringLiteral("objective"),
             QStringLiteral("Initial objective")},
            {QStringLiteral("status"),
             QStringLiteral("active")},
            {QStringLiteral("elapsedSeconds"), 15},
        };
        shell.openGoalControls(
            QStringLiteral("Goal task"),
            currentGoal);
        shell.beginGoalEditing();
        shell.setGoalDraftObjective(
            QStringLiteral("Updated objective"));
        shell.saveGoalEdit();

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            1,
            1000);
        QCOMPARE(
            requests.at(0).first,
            QStringLiteral("codex.goal.update"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("threadId")),
            QStringLiteral("thread-goal"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("goalObjective")),
            QStringLiteral("Updated objective"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.goalMutationPending(),
            1000);
        QCOMPARE(
            shell.goalObjective(),
            QStringLiteral("Normalized objective"));
        QVERIFY(!shell.goalEditing());

        shell.pauseGoal();
        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            2,
            1000);
        QCOMPARE(
            requests.at(1).first,
            QStringLiteral("codex.goal.pause"));
        QCOMPARE(
            requests.at(1).second.value(
                QStringLiteral("threadId")),
            QStringLiteral("thread-goal"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.goalMutationPending(),
            1000);
        QCOMPARE(
            shell.goalStatus(),
            QStringLiteral("blocked"));
        QVERIFY(shell.goalCanResume());

        shell.resumeGoal();
        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            3,
            1000);
        QCOMPARE(
            requests.at(2).first,
            QStringLiteral("codex.goal.resume"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.goalMutationPending(),
            1000);
        QVERIFY(shell.goalCanPause());
    }

    void routesUsageRefreshIntoShellPresentationState()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);
        QVERIFY(runtime != nullptr);

        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.usage.load"),
                [runtime](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    BridgeUsageSnapshot snapshot;
                    snapshot.planType =
                        QStringLiteral("plus");
                    BridgeUsageGroup group;
                    group.id =
                        QStringLiteral("codex");
                    group.title =
                        QStringLiteral("Codex");
                    group.shortWindow =
                        BridgeUsageWindow{
                            72.5,
                            QStringLiteral("5h"),
                            BridgeDate{1000},
                        };
                    group.weeklyWindow =
                        BridgeUsageWindow{
                            41.0,
                            QStringLiteral("Weekly"),
                            std::nullopt,
                        };
                    snapshot.groups.append(
                        std::move(group));
                    snapshot.availableResetCount = 2;
                    snapshot.updatedAt =
                        BridgeDate{2000};
                    detail::CodexRuntimeTestAccess::
                        publishUsage(
                            *runtime,
                            std::move(snapshot));
                    completion(
                        Result<void>::success());
                })
                .hasValue());

        shell.refreshUsage();

        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.usageLoading(),
            1000);
        QVERIFY(
            shell.usageErrorMessage().isEmpty());
        const QVariantMap snapshot =
            shell.usageSnapshot();
        QCOMPARE(
            snapshot.value(
                QStringLiteral("planType"))
                .toString(),
            QStringLiteral("plus"));
        QCOMPARE(
            snapshot.value(
                QStringLiteral(
                    "availableResetCount"))
                .toLongLong(),
            2);
        const QVariantList groups =
            snapshot.value(
                QStringLiteral("groups"))
                .toList();
        QCOMPARE(groups.size(), 1);
        const QVariantMap firstGroup =
            groups.front().toMap();
        QCOMPARE(
            firstGroup.value(
                QStringLiteral("title"))
                .toString(),
            QStringLiteral("Codex"));
        QCOMPARE(
            firstGroup.value(
                QStringLiteral("shortWindow"))
                .toMap()
                .value(
                    QStringLiteral(
                        "remainingPercent"))
                .toDouble(),
            72.5);
        QVERIFY(
            firstGroup.value(
                QStringLiteral("shortWindow"))
                .toMap()
                .value(
                    QStringLiteral("resetsAt"))
                .toLongLong()
            > 0);
    }

    void routesConfirmedUsageResetAndPresentsItsOutcome()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);
        QVERIFY(runtime != nullptr);

        QVariantMap receivedArguments;
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.usage.consume-reset"),
                [&receivedArguments,
                 runtime](
                    const QVariantMap& arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    receivedArguments = arguments;
                    emit runtime->usageResetFinished(
                        UsageResetOutcome::Reset);
                    completion(
                        Result<void>::success());
                })
                .hasValue());

        const QVariantMap credit{
            {QStringLiteral("id"),
             QStringLiteral("credit-weekly")},
            {QStringLiteral("displayTitle"),
             QStringLiteral("Weekly Codex reset")},
        };
        shell.setUsageStatus(
            false,
            {
                {
                    QStringLiteral(
                        "availableResetCount"),
                    1,
                },
                {
                    QStringLiteral(
                        "availableResetCredits"),
                    QVariantList{credit},
                },
            },
            {});

        shell.prepareUsageReset(credit);
        shell.confirmUsageReset();

        QTRY_VERIFY_WITH_TIMEOUT(
            !receivedArguments.isEmpty(),
            1000);
        QCOMPARE(
            receivedArguments.value(
                QStringLiteral(
                    "resetCreditId"))
                .toString(),
            QStringLiteral("credit-weekly"));
        QVERIFY(
            !QUuid(
                 receivedArguments.value(
                     QStringLiteral(
                         "idempotencyKey"))
                     .toString())
                 .isNull());
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.usageResetBusy(),
            1000);
        QCOMPARE(
            shell.usageResetStatusMessage(),
            QStringLiteral(
                "Codex usage reset applied."));
    }

    void refreshesAndRemovalsUpdateOpenGoalControls()
    {
        CompanionShellViewModel shell;
        TaskListModel* model = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            &model,
            nullptr,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(runtime != nullptr);

        BridgeGoal currentGoal;
        currentGoal.threadId =
            QStringLiteral("thread-goal");
        currentGoal.objective =
            QStringLiteral("Initial objective");
        currentGoal.status = GoalStatus::Active;
        currentGoal.elapsedSeconds = 15;
        BridgeTask task;
        task.id = currentGoal.threadId;
        task.title = QStringLiteral("Goal task");
        task.goal = currentGoal;
        model->setSnapshot({task});
        shell.openGoalControls(
            task.title,
            model->data(
                model->index(0, 0),
                TaskListModel::GoalRole)
                .toMap());
        shell.beginGoalEditing();

        currentGoal.objective =
            QStringLiteral("Completed remotely");
        currentGoal.status = GoalStatus::Complete;
        currentGoal.elapsedSeconds = 45;
        task.goal = currentGoal;
        model->setSnapshot({task});
        emit runtime->processSnapshotChanged();

        QCOMPARE(
            shell.goalObjective(),
            QStringLiteral("Completed remotely"));
        QCOMPARE(
            shell.goalStatus(),
            QStringLiteral("complete"));
        QVERIFY(!shell.goalEditing());

        task.goal.reset();
        model->setSnapshot({task});
        emit runtime->processSnapshotChanged();

        QVERIFY(!shell.goalControlVisible());
    }

    void goalCommandFailureStaysVisibleInControls()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.goal.pause"),
                [](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    completion(
                        Result<void>::failure({
                            QStringLiteral(
                                "goal.synthetic_failure"),
                            QStringLiteral(
                                "Synthetic goal failure."),
                            false,
                            {},
                        }));
                })
                .hasValue());

        shell.openGoalControls(
            QStringLiteral("Goal task"),
            {
                {QStringLiteral("threadId"),
                 QStringLiteral("thread-goal")},
                {QStringLiteral("objective"),
                 QStringLiteral("Initial objective")},
                {QStringLiteral("status"),
                 QStringLiteral("active")},
            });
        shell.pauseGoal();

        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.goalMutationPending(),
            1000);
        QCOMPARE(
            shell.goalErrorMessage(),
            QStringLiteral("Synthetic goal failure."));
        QVERIFY(shell.goalCanPause());
    }

    void routesProcessMessagesAndApprovalsThroughTheCommandBus()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);

        QVector<QPair<QString, QVariantMap>> requests;
        const auto registerCommand =
            [commandBus, &requests](
                const QString& command) {
                return commandBus->registerHandler(
                    command,
                    [&requests, command](
                        const QVariantMap& arguments,
                        CompanionCommandBus::Completion
                            completion) {
                        requests.append({
                            command,
                            arguments,
                        });
                        completion(
                            Result<void>::success());
                    });
            };
        QVERIFY(
            registerCommand(
                QStringLiteral("codex.reply"))
                .hasValue());
        QVERIFY(
            registerCommand(
                QStringLiteral("codex.steer"))
                .hasValue());
        QVERIFY(
            registerCommand(
                QStringLiteral(
                    "codex.approval.respond"))
                .hasValue());

        const QVariantMap target{
            {QStringLiteral("id"),
             QStringLiteral("thread-process")},
            {QStringLiteral("threadId"),
             QStringLiteral("thread-process")},
            {QStringLiteral("title"),
             QStringLiteral("Process task")},
            {QStringLiteral("status"),
             QStringLiteral("running")},
            {QStringLiteral("needsApproval"), false},
            {QStringLiteral("cwd"),
             QStringLiteral("C:\\worktree")},
            {QStringLiteral("activeTurnId"),
             QStringLiteral("turn-active")},
            {QStringLiteral("model"),
             QStringLiteral("gpt-test")},
            {QStringLiteral("reasoningEffort"),
             QStringLiteral("high")},
        };

        shell.beginProcessAction(
            target,
            QStringLiteral("steer"));
        shell.setProcessDraft(
            QStringLiteral("Redirect this task"));
        shell.submitProcessMessage();

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            1,
            1000);
        QCOMPARE(
            requests.at(0).first,
            QStringLiteral("codex.steer"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("threadId")),
            QStringLiteral("thread-process"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("text")),
            QStringLiteral("Redirect this task"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("cwd")),
            QStringLiteral("C:\\worktree"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("expectedTurnId")),
            QStringLiteral("turn-active"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("model")),
            QStringLiteral("gpt-test"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("reasoningEffort")),
            QStringLiteral("high"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.processTargetActive(),
            1000);

        QVariantMap approvalTarget = target;
        approvalTarget.insert(
            QStringLiteral("status"),
            QStringLiteral("waiting"));
        approvalTarget.insert(
            QStringLiteral("needsApproval"),
            true);
        shell.respondToProcessApproval(
            approvalTarget,
            QStringLiteral("approveOnce"));

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            2,
            1000);
        QCOMPARE(
            requests.at(1).first,
            QStringLiteral(
                "codex.approval.respond"));
        QCOMPARE(
            requests.at(1).second.value(
                QStringLiteral("approvalDecision")),
            QStringLiteral("approveOnce"));
        QTRY_VERIFY_WITH_TIMEOUT(
            shell.approvingProcessId().isEmpty(),
            1000);
    }

    void cancelingPendingSteerIgnoresItsLateCompletion()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);

        QVector<CompanionCommandBus::Completion>
            completions;
        QVector<QVariantMap> requests;
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral("codex.steer"),
                [&completions, &requests](
                    const QVariantMap& arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    requests.append(arguments);
                    completions.append(
                        std::move(completion));
                })
                .hasValue());

        const QVariantMap target{
            {QStringLiteral("id"),
             QStringLiteral("thread-process")},
            {QStringLiteral("threadId"),
             QStringLiteral("thread-process")},
            {QStringLiteral("title"),
             QStringLiteral("Process task")},
            {QStringLiteral("status"),
             QStringLiteral("running")},
            {QStringLiteral("needsApproval"), false},
            {QStringLiteral("activeTurnId"),
             QStringLiteral("turn-active")},
        };

        shell.beginProcessAction(
            target,
            QStringLiteral("steer"));
        shell.setProcessDraft(
            QStringLiteral("First steer"));
        shell.submitProcessMessage();
        QTRY_COMPARE_WITH_TIMEOUT(
            completions.size(),
            1,
            1000);
        QVERIFY(shell.processSending());

        shell.cancelProcessTarget();
        QVERIFY(!shell.processSending());
        QVERIFY(!shell.processTargetActive());

        shell.beginProcessAction(
            target,
            QStringLiteral("steer"));
        shell.setProcessDraft(
            QStringLiteral("Second steer"));
        shell.submitProcessMessage();
        QTRY_COMPARE_WITH_TIMEOUT(
            completions.size(),
            2,
            1000);
        QVERIFY(shell.processSending());
        QVERIFY(shell.processTargetActive());

        const QString firstOperationKey =
            requests.at(0)
                .value(QStringLiteral(
                    "_companionOperationKey"))
                .toString();
        const QString secondOperationKey =
            requests.at(1)
                .value(QStringLiteral(
                    "_companionOperationKey"))
                .toString();
        QVERIFY(!firstOperationKey.isEmpty());
        QVERIFY(!secondOperationKey.isEmpty());
        QVERIFY(firstOperationKey
                != secondOperationKey);

        completions.at(0)(
            Result<void>::success());
        QCoreApplication::processEvents();
        QVERIFY(shell.processSending());
        QVERIFY(shell.processTargetActive());
        QCOMPARE(
            shell.processDraft(),
            QStringLiteral("Second steer"));

        completions.at(1)(
            Result<void>::success());
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.processSending(),
            1000);
        QVERIFY(!shell.processTargetActive());
    }

    void processMessageAndApprovalCompleteIndependently()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);

        QVector<CompanionCommandBus::Completion>
            messageCompletions;
        QVector<CompanionCommandBus::Completion>
            approvalCompletions;
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral("codex.reply"),
                [&messageCompletions](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    messageCompletions.append(
                        std::move(completion));
                })
                .hasValue());
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.approval.respond"),
                [&approvalCompletions](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    approvalCompletions.append(
                        std::move(completion));
                })
                .hasValue());

        const QVariantMap replyTarget{
            {QStringLiteral("id"),
             QStringLiteral("thread-reply")},
            {QStringLiteral("threadId"),
             QStringLiteral("thread-reply")},
            {QStringLiteral("title"),
             QStringLiteral("Reply task")},
            {QStringLiteral("status"),
             QStringLiteral("running")},
            {QStringLiteral("needsApproval"), false},
        };
        const QVariantMap approvalTarget{
            {QStringLiteral("id"),
             QStringLiteral("thread-approval")},
            {QStringLiteral("threadId"),
             QStringLiteral("thread-approval")},
            {QStringLiteral("title"),
             QStringLiteral("Approval task")},
            {QStringLiteral("status"),
             QStringLiteral("waiting")},
            {QStringLiteral("needsApproval"), true},
        };

        shell.beginProcessAction(
            replyTarget,
            QStringLiteral("reply"));
        shell.setProcessDraft(
            QStringLiteral("Continue"));
        shell.submitProcessMessage();
        QTRY_COMPARE_WITH_TIMEOUT(
            messageCompletions.size(),
            1,
            1000);

        shell.respondToProcessApproval(
            approvalTarget,
            QStringLiteral("approveOnce"));
        QTRY_COMPARE_WITH_TIMEOUT(
            approvalCompletions.size(),
            1,
            1000);
        QVERIFY(shell.processSending());
        QCOMPARE(
            shell.approvingProcessId(),
            QStringLiteral("thread-approval"));

        approvalCompletions.at(0)(
            Result<void>::success());
        QTRY_VERIFY_WITH_TIMEOUT(
            shell.approvingProcessId().isEmpty(),
            1000);
        QVERIFY(shell.processSending());

        messageCompletions.at(0)(
            Result<void>::success());
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.processSending(),
            1000);
    }

    void successfulReplyMarksTheDisplayedFailureHandled()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        CodexRuntime* runtime = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus,
            &runtime);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);
        QVERIFY(runtime != nullptr);
        auto* processModel =
            qobject_cast<ProcessListModel*>(
                shell.processModel());
        QVERIFY(processModel != nullptr);

        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral("codex.reply"),
                [](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    completion(
                        Result<void>::success());
                })
                .hasValue());

        BridgeTask failed;
        failed.id =
            QStringLiteral("thread-failed");
        failed.title =
            QStringLiteral("Failed task");
        failed.preview =
            QStringLiteral("The build failed");
        failed.status = TaskStatus::Failed;
        failed.updatedAt = currentBridgeDate();
        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(failed);
        detail::CodexRuntimeTestAccess::
            publishProcessSnapshot(
                *runtime,
                snapshot);
        QCOMPARE(processModel->rowCount(), 1);

        shell.beginProcessAction(
            {
                {QStringLiteral("id"),
                 failed.id},
                {QStringLiteral("threadId"),
                 failed.id},
                {QStringLiteral("title"),
                 failed.title},
                {QStringLiteral("status"),
                 QStringLiteral("failed")},
                {QStringLiteral("needsApproval"),
                 false},
            },
            QStringLiteral("reply"));
        shell.setProcessDraft(
            QStringLiteral("Retry with the fix"));
        shell.submitProcessMessage();

        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.processTargetActive(),
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            processModel->rowCount(),
            0,
            1000);

        detail::CodexRuntimeTestAccess::
            publishProcessSnapshot(
                *runtime,
                snapshot);
        QCOMPARE(processModel->rowCount(), 0);
    }

    void cancelingApprovalFeedbackPreventsFollowUpReply()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);

        CompanionCommandBus::Completion
            approvalCompletion;
        int replyCalls = 0;
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.approval.respond"),
                [&approvalCompletion](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    approvalCompletion =
                        std::move(completion);
                })
                .hasValue());
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral("codex.reply"),
                [&replyCalls](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    ++replyCalls;
                    completion(
                        Result<void>::success());
                })
                .hasValue());

        shell.beginProcessAction(
            {
                {QStringLiteral("id"),
                 QStringLiteral(
                     "thread-approval")},
                {QStringLiteral("threadId"),
                 QStringLiteral(
                     "thread-approval")},
                {QStringLiteral("title"),
                 QStringLiteral("Approval task")},
                {QStringLiteral("status"),
                 QStringLiteral("waiting")},
                {QStringLiteral("needsApproval"),
                 true},
            },
            QStringLiteral(
                "approval-feedback"));
        shell.setProcessDraft(
            QStringLiteral(
                "Use the safer command instead"));
        shell.submitProcessMessage();
        QTRY_VERIFY_WITH_TIMEOUT(
            static_cast<bool>(
                approvalCompletion),
            1000);

        shell.cancelProcessTarget();
        QVERIFY(!shell.processSending());
        QVERIFY(!shell.processTargetActive());

        approvalCompletion(
            Result<void>::success());
        QCoreApplication::processEvents();

        QCOMPARE(replyCalls, 0);
        QVERIFY(!shell.processSending());
        QVERIFY(!shell.processTargetActive());
    }

    void tellCodexDeclinesApprovalBeforeSendingGuidance()
    {
        CompanionShellViewModel shell;
        CompanionCommandBus* commandBus = nullptr;
        auto host = createHost(
            shell,
            std::make_shared<
                MemoryCredentialStore>(),
            {},
            {},
            true,
            nullptr,
            &commandBus);
        QVERIFY(host != nullptr);
        QVERIFY(commandBus != nullptr);

        QVector<QPair<QString, QVariantMap>> requests;
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral(
                    "codex.approval.respond"),
                [&requests](
                    const QVariantMap& arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    requests.append({
                        QStringLiteral(
                            "codex.approval.respond"),
                        arguments,
                    });
                    completion(
                        Result<void>::success());
                })
                .hasValue());
        QVERIFY(
            commandBus->registerHandler(
                QStringLiteral("codex.reply"),
                [&requests](
                    const QVariantMap& arguments,
                    CompanionCommandBus::Completion
                        completion) {
                    requests.append({
                        QStringLiteral("codex.reply"),
                        arguments,
                    });
                    completion(
                        Result<void>::success());
                })
                .hasValue());

        shell.beginProcessAction(
            {
                {QStringLiteral("id"),
                 QStringLiteral("thread-approval")},
                {QStringLiteral("threadId"),
                 QStringLiteral("thread-approval")},
                {QStringLiteral("title"),
                 QStringLiteral("Approval task")},
                {QStringLiteral("status"),
                 QStringLiteral("waiting")},
                {QStringLiteral("needsApproval"), true},
                {QStringLiteral("cwd"),
                 QStringLiteral("C:\\approval")},
            },
            QStringLiteral("approval-feedback"));
        shell.setProcessDraft(
            QStringLiteral("Use the safer command instead"));
        shell.submitProcessMessage();

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            2,
            1000);
        QCOMPARE(
            requests.at(0).first,
            QStringLiteral(
                "codex.approval.respond"));
        QCOMPARE(
            requests.at(0).second.value(
                QStringLiteral("approvalDecision")),
            QStringLiteral("decline"));
        QCOMPARE(
            requests.at(1).first,
            QStringLiteral("codex.reply"));
        QCOMPARE(
            requests.at(1).second.value(
                QStringLiteral("text")),
            QStringLiteral(
                "Use the safer command instead"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell.processTargetActive(),
            1000);
    }
};

} // namespace

QTEST_GUILESS_MAIN(CompanionRuntimeHostTests)
#include "CompanionRuntimeHostTests.moc"
