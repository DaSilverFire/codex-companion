#include "mobile/MobileRequestDispatcher.h"

#include "codex/chat/ChatCatalog.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"

#include <QDir>
#include <QFuture>
#include <QHash>
#include <QPromise>
#include <QtTest>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

using namespace companion;

namespace {

const QString kPresencePackageId =
    QStringLiteral(
        "fixture-pet-mobile-presence-v1");
const QString kPresenceContentHash =
    QStringLiteral(
        "20f53b7f5c377426581f06c213c6f238ed3bbcaec70c468813295c9c96132804");

QString presenceFixtureDirectory()
{
    return QDir::cleanPath(
        QStringLiteral(
            COMPANION_MOBILE_PRESENCE_FIXTURE_ROOT));
}

template <typename T>
QFuture<Result<T>> readyFuture(Result<T> result)
{
    QPromise<Result<T>> promise;
    promise.start();
    QFuture<Result<T>> future = promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
}

BridgeResponse finished(QFuture<BridgeResponse> future)
{
    if (!future.isValid()) {
        qFatal("Mobile dispatcher returned an invalid future.");
    }
    future.waitForFinished();
    if (future.isCanceled() || future.resultCount() != 1) {
        qFatal("Mobile dispatcher future did not produce one response.");
    }
    return future.result();
}

CompanionError failure(
    const QString& code,
    const QString& message = QStringLiteral("dependency failed"))
{
    return {
        code,
        message,
        false,
        {},
    };
}

BridgeRequest request(BridgeOperation operation)
{
    BridgeRequest value;
    value.id = QUuid(
        QStringLiteral(
            "{99999999-8888-7777-6666-555555555555}"));
    value.operation = operation;
    return value;
}

BridgeTask task(const QString& id)
{
    BridgeTask value;
    value.id = id;
    value.title = QStringLiteral("Task ") + id;
    value.preview = QStringLiteral("Preview");
    value.updatedAt = {123.0};
    value.cwd = QStringLiteral("C:/work");
    value.status = TaskStatus::Running;
    value.activeTurnId = QStringLiteral("turn-1");
    value.model = QStringLiteral("gpt-5.6-sol");
    value.reasoningEffort = QStringLiteral("high");
    return value;
}

BridgeGoal goal(const QString& threadId)
{
    return {
        threadId,
        QStringLiteral("Goal objective"),
        GoalStatus::Active,
        5'000,
        125,
        90,
        100,
        200,
    };
}

BridgeAttachment attachment()
{
    return {
        QUuid(
            QStringLiteral(
                "{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}")),
        AttachmentKind::Image,
        QStringLiteral("capture.png"),
        QStringLiteral("image/png"),
        QByteArray("png-data"),
    };
}

HistorySnapshot history()
{
    BridgeMessage message;
    message.id = QStringLiteral("message-1");
    message.role = MessageRole::Assistant;
    message.text = QStringLiteral("Done");
    message.createdAt = BridgeDate{456.0};

    BridgeTimelineItem item;
    item.id = QStringLiteral("timeline-1");
    item.kind = TimelineKind::Reasoning;
    item.title = QStringLiteral("Thinking");

    BridgeSubagent subagent;
    subagent.id = QStringLiteral("subagent-1");
    subagent.name = QStringLiteral("Newton");
    subagent.title = QStringLiteral("Relay");
    subagent.updatedAt = {457.0};
    subagent.status = TaskStatus::Running;

    return {
        {message},
        QStringLiteral("messages-next"),
        {item},
        QStringLiteral("revision-1"),
        QStringLiteral("timeline-next"),
        {subagent},
        BridgeContextUsage{12, 100},
    };
}

BridgeUsageSnapshot usage()
{
    BridgeUsageSnapshot value;
    value.planType = QStringLiteral("pro");
    value.availableResetCount = 1;
    value.updatedAt = {789.0};
    return value;
}

BridgeCapabilities capabilities()
{
    BridgeCapabilities value;
    BridgeModel model;
    model.id = QStringLiteral("gpt-5.6-sol");
    model.model = QStringLiteral("gpt-5.6-sol");
    model.displayName = QStringLiteral("5.6 Sol");
    model.description = QStringLiteral("Highest capability");
    model.isDefault = true;
    model.defaultReasoningEffort = QStringLiteral("high");
    value.models = {model};
    value.chatAgents = ChatCatalog::agents();
    return value;
}

class RecordingFixture final {
public:
    MobileRequestDispatcher dispatcher()
    {
        MobileRequestReadDependencies reads;
        reads.taskPageLoader =
            [this](
                std::optional<QString> cursor,
                qint64 limit) {
                ++taskCalls;
                taskCursor = std::move(cursor);
                taskLimit = limit;
                if (taskError.has_value()) {
                    return readyFuture(
                        Result<MobileTaskPage>::failure(
                            *taskError));
                }
                return readyFuture(
                    Result<MobileTaskPage>::success(
                        taskPage));
            };
        reads.goalLoader =
            [this](QVector<QString> threadIds) {
                ++goalReadCalls;
                goalThreadIds = std::move(threadIds);
                if (goalReadError.has_value()) {
                    return readyFuture(
                        Result<MobileGoalMap>::failure(
                            *goalReadError));
                }
                return readyFuture(
                    Result<MobileGoalMap>::success(
                        goals));
            };
        reads.historyLoader =
            [this](MobileHistoryKey key) {
                ++historyCalls;
                historyKey = std::move(key);
                if (historyError.has_value()) {
                    return readyFuture(
                        Result<HistorySnapshot>::failure(
                            *historyError));
                }
                return readyFuture(
                    Result<HistorySnapshot>::success(
                        historyValue));
            };
        reads.capabilityLoader =
            [this](QString cwd) {
                ++capabilityCalls;
                capabilityCwd = std::move(cwd);
                if (capabilityError.has_value()) {
                    return readyFuture(
                        Result<BridgeCapabilities>::failure(
                            *capabilityError));
                }
                return readyFuture(
                    Result<BridgeCapabilities>::success(
                        capabilitiesValue));
            };
        reads.usageLoader =
            [this] {
                ++usageCalls;
                if (usageError.has_value()) {
                    return readyFuture(
                        Result<BridgeUsageSnapshot>::failure(
                            *usageError));
                }
                return readyFuture(
                    Result<BridgeUsageSnapshot>::success(
                        usageValue));
            };

        MobileRequestMutationDependencies mutations;
        mutations.sendMessage =
            [this](SendRequest value) {
                ++sendCalls;
                sentRequest = std::move(value);
                if (retainCurrentSettings
                    && sentRequest
                           ->executionState) {
                    sentRequest
                        ->executionState
                        ->retainedCurrentSettings
                        .store(true);
                }
                if (sendError.has_value()) {
                    return readyFuture(
                        Result<void>::failure(*sendError));
                }
                return readyFuture(
                    Result<void>::success());
            };
        mutations.respondToApproval =
            [this](
                QString threadId,
                ApprovalDecision decision) {
                ++approvalCalls;
                approvalThreadId = std::move(threadId);
                approvalDecision = decision;
                if (approvalError.has_value()) {
                    return readyFuture(
                        Result<void>::failure(
                            *approvalError));
                }
                return readyFuture(
                    Result<void>::success());
            };
        mutations.createTask =
            [this](RuntimeTaskCreateRequest value) {
                ++createTaskCalls;
                createTaskRequest = std::move(value);
                if (createTaskError.has_value()) {
                    return readyFuture(
                        Result<QString>::failure(
                            *createTaskError));
                }
                return readyFuture(
                    Result<QString>::success(
                        createdThreadId));
            };
        mutations.sendCasualChat =
            [this](ChatRequest value) {
                ++chatCalls;
                chatRequest = std::move(value);
                if (chatError.has_value()) {
                    return readyFuture(
                        Result<ChatResult>::failure(
                            *chatError));
                }
                return readyFuture(
                    Result<ChatResult>::success(
                        chatResult));
            };
        mutations.consumeUsageReset =
            [this](
                QString creditId,
                QUuid idempotencyKey) {
                ++resetCalls;
                resetCreditId = std::move(creditId);
                resetKeys.push_back(idempotencyKey);
                if (resetError.has_value()) {
                    return readyFuture(
                        Result<UsageResetOutcome>::failure(
                            *resetError));
                }
                return readyFuture(
                    Result<UsageResetOutcome>::success(
                        resetOutcome));
            };
        mutations.mutateGoal =
            [this](RuntimeGoalMutationRequest value) {
                ++goalMutationCalls;
                goalMutationRequest = std::move(value);
                if (goalMutationError.has_value()) {
                    return readyFuture(
                        Result<BridgeGoal>::failure(
                            *goalMutationError));
                }
                return readyFuture(
                    Result<BridgeGoal>::success(
                        goalValue));
            };

        MobileRequestDispatcherConfiguration configuration;
        configuration.hostName =
            QStringLiteral("Windows workstation");
        configuration.hostDeviceId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        configuration.relayUrlProvider = [this] {
            return relayUrl;
        };
        configuration.nowProvider = [] {
            return BridgeDate{999.0};
        };
        configuration
            .presencePetCatalogService =
            presencePetCatalogService;
        return {
            std::move(reads),
            std::move(mutations),
            std::move(configuration),
        };
    }

    MobileTaskPage taskPage;
    MobileGoalMap goals;
    HistorySnapshot historyValue = history();
    BridgeCapabilities capabilitiesValue = capabilities();
    BridgeUsageSnapshot usageValue = usage();
    BridgeGoal goalValue = goal(QStringLiteral("thread-1"));
    ChatResult chatResult{
        QStringLiteral("Answer"),
        12,
        4,
    };
    QString createdThreadId =
        QStringLiteral("created-thread");
    std::optional<QString> relayUrl =
        QStringLiteral("wss://relay.example.test/socket");
    std::shared_ptr<
        MobilePresencePetCatalogService>
        presencePetCatalogService;

    std::optional<CompanionError> taskError;
    std::optional<CompanionError> goalReadError;
    std::optional<CompanionError> historyError;
    std::optional<CompanionError> capabilityError;
    std::optional<CompanionError> usageError;
    std::optional<CompanionError> sendError;
    std::optional<CompanionError> approvalError;
    std::optional<CompanionError> createTaskError;
    std::optional<CompanionError> chatError;
    std::optional<CompanionError> resetError;
    std::optional<CompanionError> goalMutationError;
    bool retainCurrentSettings = false;

    int taskCalls = 0;
    int goalReadCalls = 0;
    int historyCalls = 0;
    int capabilityCalls = 0;
    int usageCalls = 0;
    int sendCalls = 0;
    int approvalCalls = 0;
    int createTaskCalls = 0;
    int chatCalls = 0;
    int resetCalls = 0;
    int goalMutationCalls = 0;

    std::optional<QString> taskCursor;
    qint64 taskLimit = 0;
    QVector<QString> goalThreadIds;
    MobileHistoryKey historyKey;
    QString capabilityCwd;
    std::optional<SendRequest> sentRequest;
    QString approvalThreadId;
    std::optional<ApprovalDecision> approvalDecision;
    std::optional<RuntimeTaskCreateRequest>
        createTaskRequest;
    std::optional<ChatRequest> chatRequest;
    QString resetCreditId;
    QVector<QUuid> resetKeys;
    UsageResetOutcome resetOutcome =
        UsageResetOutcome::Reset;
    std::optional<RuntimeGoalMutationRequest>
        goalMutationRequest;
};

} // namespace

class MobileRequestDispatcherTests final : public QObject {
    Q_OBJECT

private slots:
    void handshakeReturnsIdentityAndRejectsProtocolMismatch()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        const BridgeRequest handshake =
            request(BridgeOperation::Handshake);

        const BridgeResponse response =
            finished(dispatcher.handle(handshake));

        QVERIFY(response.succeeded);
        QCOMPARE(response.id, handshake.id);
        QCOMPARE(
            response.operation,
            BridgeOperation::Handshake);
        QCOMPARE(
            response.protocolVersion,
            kBridgeProtocolVersion);
        QCOMPARE(
            response.macName,
            std::optional<QString>(
                QStringLiteral("Windows workstation")));
        QCOMPARE(
            response.macDeviceId,
            std::optional<QString>(
                QStringLiteral(
                    "11111111-2222-3333-4444-555555555555")));
        QCOMPARE(
            response.relayUrlString,
            fixture.relayUrl);
        QVERIFY(!response.pairingSecret.has_value());

        BridgeRequest incompatible = handshake;
        incompatible.protocolVersion =
            kBridgeProtocolVersion + 1;
        const BridgeResponse rejected =
            finished(dispatcher.handle(incompatible));
        QVERIFY(!rejected.succeeded);
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("protocol_mismatch")));
        QCOMPARE(
            rejected.message,
            std::optional<QString>(
                QStringLiteral(
                    "Update Codex Companion on the Windows PC and iPhone.")));
        QCOMPARE(fixture.taskCalls, 0);
    }

    void handshakeAdvertisesPresencePetCatalog()
    {
        RecordingFixture fixture;
        fixture.presencePetCatalogService =
            std::make_shared<
                MobilePresencePetCatalogService>();
        const QVector<CompanionError>
            diagnostics =
                fixture
                    .presencePetCatalogService
                    ->replaceSnapshot({
                        QStringLiteral(
                            "fixture-pet"),
                        {
                            {
                                QStringLiteral(
                                    "fixture-pet"),
                                QStringLiteral(
                                    "Fixture Pet"),
                                presenceFixtureDirectory(),
                                kPresencePackageId,
                                kPresenceContentHash,
                            },
                        },
                    });
        QVERIFY(diagnostics.isEmpty());

        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        const BridgeResponse response =
            finished(
                dispatcher.handle(
                    request(
                        BridgeOperation::
                            Handshake)));

        QVERIFY(response.succeeded);
        QVERIFY(response.features.has_value());
        const QVector<BridgeFeature>
            expectedFeatures{
                BridgeFeature::
                    PresencePetPackageV1,
            };
        QCOMPARE(
            *response.features,
            expectedFeatures);
        QCOMPARE(
            response.selectedDesktopPetId,
            std::optional<QString>(
                QStringLiteral(
                    "fixture-pet")));
        QVERIFY(
            response.presencePetCatalog
                .has_value());
        QCOMPARE(
            response.presencePetCatalog
                ->size(),
            1);
        QCOMPARE(
            response.presencePetCatalog
                ->front()
                .packageId,
            kPresencePackageId);
        QCOMPARE(
            response.presencePetCatalog
                ->front()
                .contentHash,
            kPresenceContentHash);
    }

    void loadsPresencePetManifestAndChunks()
    {
        RecordingFixture fixture;
        fixture.presencePetCatalogService =
            std::make_shared<
                MobilePresencePetCatalogService>();
        const QVector<CompanionError>
            diagnostics =
                fixture
                    .presencePetCatalogService
                    ->replaceSnapshot({
                        QStringLiteral(
                            "fixture-pet"),
                        {
                            {
                                QStringLiteral(
                                    "fixture-pet"),
                                QStringLiteral(
                                    "Fixture Pet"),
                                presenceFixtureDirectory(),
                                kPresencePackageId,
                                kPresenceContentHash,
                            },
                        },
                    });
        QVERIFY(diagnostics.isEmpty());
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();

        BridgeRequest manifestRequest =
            request(
                BridgeOperation::
                    LoadPresencePetManifest);
        manifestRequest.presencePetPackageId =
            kPresencePackageId;
        manifestRequest.presencePetContentHash =
            kPresenceContentHash;
        const BridgeResponse manifestResponse =
            finished(
                dispatcher.handle(
                    manifestRequest));
        QVERIFY(manifestResponse.succeeded);
        QVERIFY(
            manifestResponse
                .presencePetManifest
                .has_value());
        QCOMPARE(
            manifestResponse
                .presencePetManifest
                ->packageId,
            kPresencePackageId);
        QCOMPARE(
            manifestResponse
                .presencePetManifest
                ->contentHash,
            kPresenceContentHash);

        BridgeRequest chunkRequest =
            request(
                BridgeOperation::
                    LoadPresencePetChunk);
        chunkRequest.presencePetPackageId =
            kPresencePackageId;
        chunkRequest.presencePetContentHash =
            kPresenceContentHash;
        chunkRequest.presencePetFileName =
            QStringLiteral("atlas.png");
        chunkRequest.presencePetOffset = 0;
        chunkRequest.presencePetLength =
            1024;
        const BridgeResponse chunkResponse =
            finished(
                dispatcher.handle(
                    chunkRequest));
        QVERIFY(chunkResponse.succeeded);
        QVERIFY(
            chunkResponse.presencePetChunk
                .has_value());
        QCOMPARE(
            chunkResponse
                .presencePetChunk
                ->data.size(),
            1024);
        QCOMPARE(
            chunkResponse
                .presencePetChunk
                ->nextOffset,
            1024);
        QVERIFY(
            !chunkResponse
                 .presencePetChunk
                 ->isComplete);

        BridgeRequest invalidRequest =
            chunkRequest;
        invalidRequest.presencePetLength =
            MobilePresencePetCatalogService::
                    kMaximumChunkLength
                + 1;
        const BridgeResponse invalidResponse =
            finished(
                dispatcher.handle(
                    invalidRequest));
        QVERIFY(!invalidResponse.succeeded);
        QCOMPARE(
            invalidResponse.errorCode,
            std::optional<QString>(
                QStringLiteral(
                    "invalid_presence_pet_request")));

        BridgeRequest staleRequest =
            manifestRequest;
        staleRequest.presencePetContentHash =
            QString(64, QLatin1Char('0'));
        const BridgeResponse staleResponse =
            finished(
                dispatcher.handle(
                    staleRequest));
        QVERIFY(!staleResponse.succeeded);
        QCOMPARE(
            staleResponse.errorCode,
            std::optional<QString>(
                QStringLiteral(
                    "stale_presence_pet")));
    }

    void listTasksBoundsLimitAndAttachesGoalsBestEffort()
    {
        RecordingFixture fixture;
        fixture.taskPage = {
            {
                task(QStringLiteral("thread-1")),
                task(QStringLiteral("thread-2")),
            },
            QStringLiteral("next"),
        };
        fixture.goals.insert(
            QStringLiteral("thread-1"),
            goal(QStringLiteral("thread-1")));
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        BridgeRequest list =
            request(BridgeOperation::ListTasks);
        list.cursor = QStringLiteral("cursor");
        list.limit = 500;

        const BridgeResponse response =
            finished(dispatcher.handle(list));

        QVERIFY(response.succeeded);
        QCOMPARE(fixture.taskCursor, list.cursor);
        QCOMPARE(
            fixture.taskLimit,
            kMaximumPageSize);
        QCOMPARE(
            fixture.goalThreadIds,
            QVector<QString>({
                QStringLiteral("thread-1"),
                QStringLiteral("thread-2"),
            }));
        QCOMPARE(
            response.nextCursor,
            fixture.taskPage.nextCursor);
        QVERIFY(response.tasks.has_value());
        QCOMPARE(response.tasks->size(), 2);
        QCOMPARE(
            response.tasks->at(0).goal,
            std::optional<BridgeGoal>(
                goal(QStringLiteral("thread-1"))));
        QVERIFY(
            !response.tasks->at(1).goal.has_value());

        fixture.goalReadError =
            failure(
                QStringLiteral("codex.goal_unavailable"),
                QStringLiteral("goals unavailable"));
        const BridgeResponse bestEffort =
            finished(dispatcher.handle(list));
        QVERIFY(bestEffort.succeeded);
        QVERIFY(bestEffort.tasks.has_value());
        QVERIFY(
            !bestEffort.tasks->at(0).goal.has_value());

        BridgeRequest minimum = list;
        minimum.limit = 0;
        finished(dispatcher.handle(minimum));
        QCOMPARE(fixture.taskLimit, qint64(1));

        fixture.taskError =
            failure(
                QStringLiteral("archive.failed"),
                QStringLiteral("tasks unavailable"));
        const BridgeResponse failed =
            finished(dispatcher.handle(list));
        QCOMPARE(
            failed.errorCode,
            std::optional<QString>(
                QStringLiteral("archive_error")));
        QCOMPARE(
            failed.message,
            std::optional<QString>(
                QStringLiteral("tasks unavailable")));
    }

    void loadMessagesValidatesAndReturnsFullSnapshot()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();

        BridgeRequest missing =
            request(BridgeOperation::LoadMessages);
        missing.threadId = QStringLiteral("   ");
        const BridgeResponse invalid =
            finished(dispatcher.handle(missing));
        QCOMPARE(
            invalid.errorCode,
            std::optional<QString>(
                QStringLiteral("missing_thread")));
        QCOMPARE(fixture.historyCalls, 0);

        BridgeRequest load =
            request(BridgeOperation::LoadMessages);
        load.threadId = QStringLiteral(" thread-1 ");
        load.cursor = QStringLiteral("history-cursor");
        load.limit = 0;
        const BridgeResponse response =
            finished(dispatcher.handle(load));

        QVERIFY(response.succeeded);
        QCOMPARE(
            fixture.historyKey.threadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            fixture.historyKey.cursor,
            load.cursor);
        QCOMPARE(fixture.historyKey.limit, 1);
        QCOMPARE(
            response.threadId,
            std::optional<QString>(
                QStringLiteral("thread-1")));
        QCOMPARE(
            response.messages,
            std::optional<QVector<BridgeMessage>>(
                fixture.historyValue.messages));
        QCOMPARE(
            response.timelineItems,
            std::optional<
                QVector<BridgeTimelineItem>>(
                fixture.historyValue.timelineItems));
        QCOMPARE(
            response.subagents,
            std::optional<QVector<BridgeSubagent>>(
                fixture.historyValue.subagents));
        QCOMPARE(
            response.contextUsage,
            fixture.historyValue.contextUsage);
        QCOMPARE(
            response.revision,
            std::optional<QString>(
                fixture.historyValue.revision));

        fixture.historyError =
            failure(
                QStringLiteral("history.failed"),
                QStringLiteral("history unavailable"));
        const BridgeResponse failed =
            finished(dispatcher.handle(load));
        QCOMPARE(
            failed.errorCode,
            std::optional<QString>(
                QStringLiteral("archive_error")));
        QCOMPARE(
            failed.message,
            std::optional<QString>(
                QStringLiteral("history unavailable")));
    }

    void sendMessageForwardsTaskContextActionAndAttachments()
    {
        RecordingFixture fixture;
        BridgeTask running =
            task(QStringLiteral("thread-1"));
        running.cwd = QStringLiteral("C:/task");
        running.activeTurnId =
            QStringLiteral("turn-active");
        fixture.taskPage = {{running}, std::nullopt};
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();

        BridgeRequest steer =
            request(BridgeOperation::SendMessage);
        steer.threadId = QStringLiteral(" thread-1 ");
        steer.text = QStringLiteral(" steer this ");
        steer.sendAction = SendAction::Steer;
        steer.model = QStringLiteral(" gpt-5.6-sol ");
        steer.reasoningEffort =
            QStringLiteral(" high ");
        steer.attachments =
            QVector<BridgeAttachment>{attachment()};
        const BridgeResponse steered =
            finished(dispatcher.handle(steer));

        QVERIFY(steered.succeeded);
        QCOMPARE(
            steered.message,
            std::optional<QString>(
                QStringLiteral("Steered task.")));
        QVERIFY(fixture.sentRequest.has_value());
        QCOMPARE(
            fixture.sentRequest->prompt,
            QStringLiteral("steer this"));
        QCOMPARE(
            fixture.sentRequest->threadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            fixture.sentRequest->cwd,
            QStringLiteral("C:/task"));
        QCOMPARE(
            fixture.sentRequest->action,
            SendAction::Steer);
        QCOMPARE(
            fixture.sentRequest->expectedTurnId,
            QStringLiteral("turn-active"));
        QCOMPARE(
            fixture.sentRequest->clientMessageId,
            QStringLiteral(
                "99999999-8888-7777-6666-555555555555"));
        QCOMPARE(
            fixture.sentRequest->model,
            QStringLiteral("gpt-5.6-sol"));
        QCOMPARE(
            fixture.sentRequest->reasoningEffort,
            QStringLiteral("high"));
        QCOMPARE(
            fixture.sentRequest->attachments,
            *steer.attachments);

        BridgeRequest reply = steer;
        reply.sendAction.reset();
        reply.cwd = QStringLiteral(" C:/override ");
        reply.attachments.reset();
        const BridgeResponse replied =
            finished(dispatcher.handle(reply));
        QVERIFY(replied.succeeded);
        QCOMPARE(
            replied.message,
            std::optional<QString>(
                QStringLiteral("Reply sent.")));
        QCOMPARE(
            fixture.sentRequest->action,
            SendAction::Reply);
        QCOMPARE(
            fixture.sentRequest->cwd,
            QStringLiteral("C:/override"));
        QVERIFY(
            fixture.sentRequest->attachments.isEmpty());

        BridgeRequest invalid = steer;
        invalid.text = QStringLiteral(" ");
        const BridgeResponse rejected =
            finished(dispatcher.handle(invalid));
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_message")));
        QCOMPARE(fixture.sendCalls, 2);
    }

    void sendMessageMapsNativeErrors()
    {
        struct Case final {
            const char* nativeCode;
            const char* bridgeCode;
            const char* message;
        };
        constexpr std::array cases{
            Case{
                "codex.no_active_turn",
                "no_active_turn",
                "This task is not currently running, so it cannot be steered.",
            },
            Case{
                "codex.thread_not_loaded",
                "thread_not_loaded",
                "The Windows PC could not load this task in the background. Your message was not sent.",
            },
            Case{
                "codex.shared_daemon_unavailable",
                "native_transport_unavailable",
                "ChatGPT's local task connection is unavailable. Your message was not lost.",
            },
            Case{
                "codex.send_timed_out",
                "timed_out",
                "Codex did not confirm the message in time.",
            },
            Case{
                "codex.send_failed",
                "send_failed",
                "Codex did not accept the message.",
            },
        };

        for (const Case& item : cases) {
            RecordingFixture fixture;
            fixture.sendError =
                failure(
                    QString::fromLatin1(
                        item.nativeCode));
            MobileRequestDispatcher dispatcher =
                fixture.dispatcher();
            BridgeRequest send =
                request(BridgeOperation::SendMessage);
            send.threadId =
                QStringLiteral("thread-1");
            send.text = QStringLiteral("message");

            const BridgeResponse response =
                finished(dispatcher.handle(send));

            QVERIFY(!response.succeeded);
            QCOMPARE(
                response.errorCode,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.bridgeCode)));
            QCOMPARE(
                response.message,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.message)));
        }
    }

    void sendMessageReportsRetainedCurrentSettings()
    {
        RecordingFixture fixture;
        fixture.retainCurrentSettings = true;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();

        struct Case final {
            std::optional<SendAction> action;
            const char* message;
        };
        const std::array cases{
            Case{
                std::nullopt,
                "Reply sent using the task's current model.",
            },
            Case{
                SendAction::Steer,
                "Steered task using its current model.",
            },
        };
        for (const Case& item : cases) {
            BridgeRequest send =
                request(
                    BridgeOperation::SendMessage);
            send.threadId =
                QStringLiteral("thread-1");
            send.text =
                QStringLiteral("Keep working");
            send.sendAction = item.action;
            send.model =
                QStringLiteral("gpt-selected");
            send.reasoningEffort =
                QStringLiteral("high");

            const BridgeResponse response =
                finished(
                    dispatcher.handle(send));

            QVERIFY(response.succeeded);
            QCOMPARE(
                response.message,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.message)));
        }
    }

    void approvalValidatesDecisionAndMapsNativeErrors()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        BridgeRequest approval =
            request(
                BridgeOperation::RespondToApproval);
        approval.threadId =
            QStringLiteral(" thread-1 ");
        approval.approvalDecision =
            ApprovalDecision::ApproveSimilar;

        const BridgeResponse accepted =
            finished(dispatcher.handle(approval));
        QVERIFY(accepted.succeeded);
        QCOMPARE(
            accepted.message,
            std::optional<QString>(
                QStringLiteral("Approval sent.")));
        QCOMPARE(
            fixture.approvalThreadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            fixture.approvalDecision,
            std::optional<ApprovalDecision>(
                ApprovalDecision::ApproveSimilar));

        approval.approvalDecision =
            ApprovalDecision::Decline;
        const BridgeResponse declined =
            finished(dispatcher.handle(approval));
        QVERIFY(declined.succeeded);
        QCOMPARE(
            declined.message,
            std::optional<QString>(
                QStringLiteral("Request declined.")));

        BridgeRequest invalid = approval;
        invalid.approvalDecision.reset();
        const BridgeResponse rejected =
            finished(dispatcher.handle(invalid));
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_approval")));
        QCOMPARE(fixture.approvalCalls, 2);

        struct Case final {
            const char* nativeCode;
            const char* bridgeCode;
            const char* message;
        };
        constexpr std::array cases{
            Case{
                "approval.request_not_found",
                "approval_gone",
                "That approval request is no longer active.",
            },
            Case{
                "approval.shared_daemon_unavailable",
                "native_transport_unavailable",
                "ChatGPT's native approval connection is unavailable. Refresh the request, then retry.",
            },
            Case{
                "approval.timed_out",
                "approval_timed_out",
                "The approval response could not be confirmed.",
            },
            Case{
                "approval.failed",
                "approval_failed",
                "Codex did not accept the approval response.",
            },
        };
        for (const Case& item : cases) {
            RecordingFixture failedFixture;
            failedFixture.approvalError =
                failure(
                    QString::fromLatin1(
                        item.nativeCode));
            MobileRequestDispatcher failedDispatcher =
                failedFixture.dispatcher();
            const BridgeResponse response =
                finished(
                    failedDispatcher.handle(
                        approval));
            QCOMPARE(
                response.errorCode,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.bridgeCode)));
            QCOMPARE(
                response.message,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.message)));
        }
    }

    void createTaskForwardsOptionsAttachmentsAndMapsErrors()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        BridgeRequest create =
            request(BridgeOperation::CreateTask);
        create.text = QStringLiteral(" Build it ");
        create.cwd = QStringLiteral(" C:/work ");
        create.model =
            QStringLiteral(" gpt-5.6-sol ");
        create.reasoningEffort =
            QStringLiteral(" high ");
        create.skillName =
            QStringLiteral(" design-and-build ");
        create.skillPath =
            QStringLiteral(" C:/skills/design/SKILL.md ");
        create.attachments =
            QVector<BridgeAttachment>{attachment()};

        const BridgeResponse response =
            finished(dispatcher.handle(create));

        QVERIFY(response.succeeded);
        QCOMPARE(
            response.message,
            std::optional<QString>(
                QStringLiteral(
                    "New Codex task started.")));
        QCOMPARE(
            response.threadId,
            std::optional<QString>(
                fixture.createdThreadId));
        QVERIFY(
            fixture.createTaskRequest.has_value());
        QCOMPARE(
            fixture.createTaskRequest->text,
            QStringLiteral("Build it"));
        QCOMPARE(
            fixture.createTaskRequest->cwd,
            QStringLiteral("C:/work"));
        QCOMPARE(
            fixture.createTaskRequest->model,
            QStringLiteral("gpt-5.6-sol"));
        QCOMPARE(
            fixture.createTaskRequest
                ->reasoningEffort,
            QStringLiteral("high"));
        QCOMPARE(
            fixture.createTaskRequest->skillName,
            QStringLiteral("design-and-build"));
        QCOMPARE(
            fixture.createTaskRequest->skillPath,
            QStringLiteral(
                "C:/skills/design/SKILL.md"));
        QCOMPARE(
            fixture.createTaskRequest
                ->clientMessageId,
            QStringLiteral(
                "99999999-8888-7777-6666-555555555555"));
        QCOMPARE(
            fixture.createTaskRequest->attachments,
            *create.attachments);

        BridgeRequest invalid = create;
        invalid.text = QStringLiteral(" ");
        const BridgeResponse rejected =
            finished(dispatcher.handle(invalid));
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_message")));
        QCOMPARE(fixture.createTaskCalls, 1);

        struct Case final {
            const char* nativeCode;
            const char* bridgeCode;
            const char* message;
        };
        constexpr std::array cases{
            Case{
                "codex.shared_daemon_unavailable",
                "native_transport_setup_required",
                "Restart ChatGPT once after native Companion transport is enabled. The task was not started.",
            },
            Case{
                "codex.task_worker_unavailable",
                "native_transport_setup_required",
                "Restart ChatGPT once after native Companion transport is enabled. The task was not started.",
            },
            Case{
                "codex.task_create_ambiguous",
                "timed_out",
                "Codex did not confirm the new task in time.",
            },
            Case{
                "codex.task_create_failed",
                "create_failed",
                "Codex did not start the new task.",
            },
        };
        for (const Case& item : cases) {
            RecordingFixture failedFixture;
            failedFixture.createTaskError =
                failure(
                    QString::fromLatin1(
                        item.nativeCode));
            MobileRequestDispatcher failedDispatcher =
                failedFixture.dispatcher();
            const BridgeResponse failed =
                finished(
                    failedDispatcher.handle(create));
            QCOMPARE(
                failed.errorCode,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.bridgeCode)));
            QCOMPARE(
                failed.message,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.message)));
        }
    }

    void capabilitiesAndCasualChatUseTypedDependencies()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        BridgeRequest load =
            request(
                BridgeOperation::LoadCapabilities);
        load.cwd = QStringLiteral(" C:/work ");

        const BridgeResponse loaded =
            finished(dispatcher.handle(load));

        QVERIFY(loaded.succeeded);
        QCOMPARE(
            fixture.capabilityCwd,
            QStringLiteral("C:/work"));
        QCOMPARE(
            loaded.capabilities,
            std::optional<BridgeCapabilities>(
                fixture.capabilitiesValue));

        BridgeRequest chat =
            request(
                BridgeOperation::SendCasualChat);
        chat.text = QStringLiteral(" Explain this ");
        chat.chatAgentId =
            QStringLiteral("explain");
        chat.chatProvider =
            ChatProvider::OpenAIAPI;
        chat.chatModelId =
            QStringLiteral("gpt56Sol");
        chat.attachments =
            QVector<BridgeAttachment>{attachment()};
        const BridgeResponse answered =
            finished(dispatcher.handle(chat));

        QVERIFY(answered.succeeded);
        QVERIFY(fixture.chatRequest.has_value());
        QCOMPARE(
            fixture.chatRequest->provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            fixture.chatRequest->modelId,
            QStringLiteral("gpt56Sol"));
        QCOMPARE(
            fixture.chatRequest->attachments,
            *chat.attachments);
        QVERIFY(
            fixture.chatRequest->prompt.contains(
                QStringLiteral("Mode: Explain")));
        QVERIFY(
            fixture.chatRequest->prompt.contains(
                QStringLiteral(
                    "User request:\nExplain this")));
        QVERIFY(answered.chatMessage.has_value());
        QCOMPARE(
            answered.chatMessage->role,
            MessageRole::Assistant);
        QCOMPARE(
            answered.chatMessage->text,
            fixture.chatResult.text);
        QCOMPARE(
            answered.chatMessage->createdAt,
            std::optional<BridgeDate>(
                BridgeDate{999.0}));

        BridgeRequest attachmentOnly = chat;
        attachmentOnly.text = QStringLiteral(" ");
        const BridgeResponse attachmentAnswer =
            finished(
                dispatcher.handle(
                    attachmentOnly));
        QVERIFY(attachmentAnswer.succeeded);
        QVERIFY(fixture.chatRequest.has_value());
        QVERIFY(fixture.chatRequest->prompt.contains(
            QStringLiteral("User request:\n")));
        QCOMPARE(
            fixture.chatRequest->attachments,
            *attachmentOnly.attachments);

        BridgeRequest invalid = attachmentOnly;
        invalid.attachments =
            QVector<BridgeAttachment>{};
        const BridgeResponse rejected =
            finished(dispatcher.handle(invalid));
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_message")));
        QCOMPARE(fixture.chatCalls, 2);

        fixture.capabilityError =
            failure(
                QStringLiteral(
                    "codex.capabilities_unavailable"),
                QStringLiteral(
                    "capabilities unavailable"));
        const BridgeResponse capabilityFailure =
            finished(dispatcher.handle(load));
        QCOMPARE(
            capabilityFailure.errorCode,
            std::optional<QString>(
                QStringLiteral("archive_error")));
        QCOMPARE(
            capabilityFailure.message,
            std::optional<QString>(
                QStringLiteral(
                    "capabilities unavailable")));

        struct ChatFailureCase final {
            ChatProvider provider;
            const char* bridgeCode;
        };
        constexpr std::array chatFailureCases{
            ChatFailureCase{
                ChatProvider::OnDevice,
                "on_device_chat_unavailable",
            },
            ChatFailureCase{
                ChatProvider::OpenAIAPI,
                "openai_chat_unavailable",
            },
            ChatFailureCase{
                ChatProvider::LumoAPI,
                "lumo_chat_unavailable",
            },
        };
        fixture.chatError =
            failure(
                QStringLiteral("chat.provider_failed"),
                QStringLiteral("chat unavailable"));
        for (const ChatFailureCase& item :
             chatFailureCases) {
            BridgeRequest failedChat = chat;
            failedChat.chatProvider = item.provider;
            const BridgeResponse chatFailure =
                finished(
                    dispatcher.handle(failedChat));
            QCOMPARE(
                chatFailure.errorCode,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.bridgeCode)));
            QCOMPARE(
                chatFailure.message,
                std::optional<QString>(
                    QStringLiteral(
                        "chat unavailable")));
        }
    }

    void usageAndResetPreserveIdempotencyAndMessages()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();
        const BridgeResponse loaded =
            finished(
                dispatcher.handle(
                    request(
                        BridgeOperation::LoadUsage)));
        QVERIFY(loaded.succeeded);
        QCOMPARE(
            loaded.usageSnapshot,
            std::optional<BridgeUsageSnapshot>(
                fixture.usageValue));

        struct Case final {
            UsageResetOutcome outcome;
            const char* message;
        };
        constexpr std::array cases{
            Case{
                UsageResetOutcome::Reset,
                "Codex usage reset applied.",
            },
            Case{
                UsageResetOutcome::NothingToReset,
                "There is currently no Codex limit to reset.",
            },
            Case{
                UsageResetOutcome::NoCredit,
                "That Codex reset is no longer available.",
            },
            Case{
                UsageResetOutcome::AlreadyRedeemed,
                "That Codex reset was already used.",
            },
        };
        const QUuid key(
            QStringLiteral(
                "{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"));
        BridgeRequest reset =
            request(
                BridgeOperation::ConsumeUsageReset);
        reset.resetCreditId =
            QStringLiteral(" credit-1 ");
        reset.idempotencyKey = key;
        for (const Case& item : cases) {
            fixture.resetOutcome = item.outcome;
            const BridgeResponse response =
                finished(dispatcher.handle(reset));
            QVERIFY(response.succeeded);
            QCOMPARE(
                response.message,
                std::optional<QString>(
                    QString::fromLatin1(
                        item.message)));
            QCOMPARE(
                response.usageSnapshot,
                std::optional<BridgeUsageSnapshot>(
                    fixture.usageValue));
            QCOMPARE(
                fixture.resetCreditId,
                QStringLiteral("credit-1"));
            QCOMPARE(fixture.resetKeys.back(), key);
        }
        QCOMPARE(fixture.resetKeys.size(), 4);
        QVERIFY(std::all_of(
            fixture.resetKeys.cbegin(),
            fixture.resetKeys.cend(),
            [&key](const QUuid& value) {
                return value == key;
            }));

        BridgeRequest invalid = reset;
        invalid.idempotencyKey.reset();
        const BridgeResponse rejected =
            finished(dispatcher.handle(invalid));
        QCOMPARE(
            rejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_reset")));
        QCOMPARE(fixture.resetCalls, 4);

        fixture.usageError =
            failure(
                QStringLiteral("usage.unavailable"),
                QStringLiteral("usage unavailable"));
        const BridgeResponse usageFailure =
            finished(
                dispatcher.handle(
                    request(
                        BridgeOperation::LoadUsage)));
        QCOMPARE(
            usageFailure.errorCode,
            std::optional<QString>(
                QStringLiteral("usage_unavailable")));
        QCOMPARE(
            usageFailure.message,
            std::optional<QString>(
                QStringLiteral("usage unavailable")));

        fixture.resetError =
            failure(
                QStringLiteral("usage.unavailable"),
                QStringLiteral("reset unavailable"));
        const BridgeResponse resetFailure =
            finished(dispatcher.handle(reset));
        QCOMPARE(
            resetFailure.errorCode,
            std::optional<QString>(
                QStringLiteral("reset_failed")));
        QCOMPARE(
            resetFailure.message,
            std::optional<QString>(
                QStringLiteral("reset unavailable")));
    }

    void goalOperationsValidateForwardAndMapFailures()
    {
        RecordingFixture fixture;
        MobileRequestDispatcher dispatcher =
            fixture.dispatcher();

        BridgeRequest create =
            request(BridgeOperation::CreateGoal);
        create.threadId =
            QStringLiteral(" thread-1 ");
        create.goalObjective =
            QStringLiteral(" Ship parity ");
        create.goalTokenBudget = 4'000;
        const BridgeResponse created =
            finished(dispatcher.handle(create));
        QVERIFY(created.succeeded);
        QCOMPARE(
            created.message,
            std::optional<QString>(
                QStringLiteral("Goal created.")));
        QCOMPARE(
            created.goal,
            std::optional<BridgeGoal>(
                fixture.goalValue));
        QVERIFY(
            fixture.goalMutationRequest.has_value());
        QCOMPARE(
            fixture.goalMutationRequest->kind,
            RuntimeGoalMutationKind::Create);
        QCOMPARE(
            fixture.goalMutationRequest->threadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            fixture.goalMutationRequest->objective,
            std::optional<QString>(
                QStringLiteral("Ship parity")));
        QCOMPARE(
            fixture.goalMutationRequest->tokenBudget,
            std::optional<qint64>(4'000));

        BridgeRequest resume =
            request(BridgeOperation::ResumeGoal);
        resume.threadId =
            QStringLiteral("thread-1");
        const BridgeResponse resumed =
            finished(dispatcher.handle(resume));
        QVERIFY(resumed.succeeded);
        QCOMPARE(
            resumed.message,
            std::optional<QString>(
                QStringLiteral("Goal resumed.")));
        QCOMPARE(
            fixture.goalMutationRequest->kind,
            RuntimeGoalMutationKind::Resume);
        QVERIFY(
            !fixture.goalMutationRequest
                 ->objective.has_value());

        BridgeRequest update =
            request(BridgeOperation::UpdateGoal);
        update.threadId =
            QStringLiteral("thread-1");
        update.goalObjective =
            QStringLiteral(" Finish relay ");
        update.goalTokenBudget = 9'000;
        const BridgeResponse updated =
            finished(dispatcher.handle(update));
        QVERIFY(updated.succeeded);
        QCOMPARE(
            updated.message,
            std::optional<QString>(
                QStringLiteral("Goal updated.")));
        QCOMPARE(
            fixture.goalMutationRequest->kind,
            RuntimeGoalMutationKind::Update);
        QCOMPARE(
            fixture.goalMutationRequest->objective,
            std::optional<QString>(
                QStringLiteral("Finish relay")));
        QVERIFY(
            !fixture.goalMutationRequest
                 ->tokenBudget.has_value());

        BridgeRequest invalidCreate = create;
        invalidCreate.goalTokenBudget = 0;
        const BridgeResponse createRejected =
            finished(
                dispatcher.handle(invalidCreate));
        QCOMPARE(
            createRejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_goal")));

        BridgeRequest invalidResume = resume;
        invalidResume.threadId =
            QStringLiteral(" ");
        const BridgeResponse resumeRejected =
            finished(
                dispatcher.handle(invalidResume));
        QCOMPARE(
            resumeRejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_goal")));

        BridgeRequest invalidUpdate = update;
        invalidUpdate.goalObjective =
            QStringLiteral(" ");
        const BridgeResponse updateRejected =
            finished(
                dispatcher.handle(invalidUpdate));
        QCOMPARE(
            updateRejected.errorCode,
            std::optional<QString>(
                QStringLiteral("invalid_goal")));
        QCOMPARE(fixture.goalMutationCalls, 3);

        fixture.goalMutationError =
            failure(
                QStringLiteral("codex.goal_unavailable"),
                QStringLiteral("goal unavailable"));
        const BridgeResponse createFailed =
            finished(dispatcher.handle(create));
        QCOMPARE(
            createFailed.errorCode,
            std::optional<QString>(
                QStringLiteral(
                    "goal_create_failed")));
        QCOMPARE(
            createFailed.message,
            std::optional<QString>(
                QStringLiteral("goal unavailable")));
        const BridgeResponse resumeFailed =
            finished(dispatcher.handle(resume));
        QCOMPARE(
            resumeFailed.errorCode,
            std::optional<QString>(
                QStringLiteral(
                    "goal_resume_failed")));
        const BridgeResponse updateFailed =
            finished(dispatcher.handle(update));
        QCOMPARE(
            updateFailed.errorCode,
            std::optional<QString>(
                QStringLiteral(
                    "goal_update_failed")));
    }
};

QTEST_GUILESS_MAIN(MobileRequestDispatcherTests)

#include "MobileRequestDispatcherTests.moc"
