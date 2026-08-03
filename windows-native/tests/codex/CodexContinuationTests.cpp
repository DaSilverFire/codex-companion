#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/continuation/CodexAutomaticAccountContinuationCoordinator.h"
#include "codex/continuation/CodexAutomaticContinuationJournal.h"
#include "codex/continuation/CodexAutomaticContinuationPolicy.h"
#include "codex/continuation/CodexFailedTaskRetryService.h"
#include "codex/continuation/CodexQuotaInterruption.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace companion;

namespace {

BridgeUsageSnapshot usage(
    double shortRemaining,
    double weeklyRemaining)
{
    BridgeUsageSnapshot snapshot;
    BridgeUsageGroup group;
    group.id = QStringLiteral("codex");
    group.title = QStringLiteral("Codex");
    group.shortWindow = BridgeUsageWindow{
        shortRemaining,
        QStringLiteral("5h"),
        std::nullopt,
    };
    group.weeklyWindow = BridgeUsageWindow{
        weeklyRemaining,
        QStringLiteral("7d"),
        std::nullopt,
    };
    snapshot.groups.append(group);
    return snapshot;
}

BridgeGoal goal(GoalStatus status)
{
    BridgeGoal result;
    result.threadId =
        QStringLiteral("thread-1");
    result.objective =
        QStringLiteral(
            "Finish Windows Companion");
    result.status = status;
    result.updatedAt = 42;
    return result;
}

RpcResponse turnResponse(
    QString status,
    QString errorInfo)
{
    return {
        QJsonObject{
            {
                QStringLiteral("data"),
                QJsonArray{
                    QJsonObject{
                        {
                            QStringLiteral("id"),
                            QStringLiteral("turn-1"),
                        },
                        {
                            QStringLiteral("status"),
                            std::move(status),
                        },
                        {
                            QStringLiteral("completedAt"),
                            1700000123.0,
                        },
                        {
                            QStringLiteral("error"),
                            QJsonObject{
                                {
                                    QStringLiteral(
                                        "codexErrorInfo"),
                                    std::move(errorInfo),
                                },
                            },
                        },
                    },
                },
            },
        },
        {},
        false,
    };
}

struct AccountFixture final {
    explicit AccountFixture(
        const QString& root)
        : profiles(
              QDir(root).filePath(
                  QStringLiteral(
                      "profiles.json"))),
          bindings(
              QDir(root).filePath(
                  QStringLiteral(
                      "bindings.json"))),
          runtime(
              QDir(root).filePath(
                  QStringLiteral("homes")),
              QDir(root).filePath(
                  QStringLiteral("shared"))),
          router(
              QProcessEnvironment::
                  systemEnvironment(),
              profiles,
              runtime,
              bindings)
    {
        main = profiles.add(
            QStringLiteral("Main"))
                   .value();
        fallback = profiles.add(
            QStringLiteral("Account 2"))
                       .value();
        QVERIFY(
            profiles.select(main.id)
                .hasValue());
        QVERIFY(
            bindings.bind(
                        QStringLiteral(
                            "thread-1"),
                        main.id)
                .hasValue());
    }

    CodexAccountProfileStore profiles;
    CodexThreadAccountBindingStore bindings;
    CodexAccountRuntime runtime;
    CodexAccountRouter router;
    CodexAccountProfile main;
    CodexAccountProfile fallback;
};

} // namespace

class CodexContinuationTests final
    : public QObject {
    Q_OBJECT

private slots:
    void quotaParserAcceptsOnlyExplicitUsageLimitFailure()
    {
        const auto parsed =
            CodexQuotaInterruptionProtocol::parse(
                QStringLiteral(" thread-1 "),
                turnResponse(
                    QStringLiteral("failed"),
                    QStringLiteral(
                        "usageLimitExceeded")));
        QVERIFY(parsed.hasValue());
        QVERIFY(parsed.value().has_value());
        QCOMPARE(
            parsed.value()->threadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            parsed.value()->turnId,
            QStringLiteral("turn-1"));
        QCOMPARE(
            parsed.value()->eventKey(),
            QStringLiteral(
                "thread-1|turn-1"));

        const auto otherFailure =
            CodexQuotaInterruptionProtocol::parse(
                QStringLiteral("thread-1"),
                turnResponse(
                    QStringLiteral("failed"),
                    QStringLiteral(
                        "responseStreamDisconnected")));
        QVERIFY(otherFailure.hasValue());
        QVERIFY(
            !otherFailure.value()
                 .has_value());

        const auto completed =
            CodexQuotaInterruptionProtocol::parse(
                QStringLiteral("thread-1"),
                turnResponse(
                    QStringLiteral("completed"),
                    QStringLiteral(
                        "usageLimitExceeded")));
        QVERIFY(completed.hasValue());
        QVERIFY(
            !completed.value()
                 .has_value());
    }

    void quotaRequestMatchesAppServerContract()
    {
        const auto request =
            CodexQuotaInterruptionProtocol::
                latestTurnRequest(
                    2,
                    QStringLiteral(
                        " thread-1 "));
        QVERIFY(request.hasValue());
        QCOMPARE(request.value().id, 2);
        QCOMPARE(
            request.value().method,
            QStringLiteral(
                "thread/turns/list"));
        QCOMPARE(
            request.value().params.value(
                QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-1"));
        QCOMPARE(
            request.value().params.value(
                QStringLiteral("limit"))
                .toInt(),
            1);
        QCOMPARE(
            request.value().params.value(
                QStringLiteral(
                    "sortDirection"))
                .toString(),
            QStringLiteral("desc"));
        QCOMPARE(
            request.value().params.value(
                QStringLiteral("itemsView"))
                .toString(),
            QStringLiteral("notLoaded"));
    }

    void policyRequiresConfirmedExhaustionAndAvailableDestination()
    {
        QVERIFY(
            CodexAutomaticContinuationPolicy::
                hasConfirmedExhaustion(
                    usage(0.0, 65.0)));
        QVERIFY(
            CodexAutomaticContinuationPolicy::
                hasConfirmedExhaustion(
                    usage(35.0, 0.0)));
        QVERIFY(
            !CodexAutomaticContinuationPolicy::
                 hasConfirmedExhaustion(
                     usage(35.0, 65.0)));
        QVERIFY(
            CodexAutomaticContinuationPolicy::
                hasAvailableUsage(
                    usage(35.0, 65.0)));
        QVERIFY(
            !CodexAutomaticContinuationPolicy::
                 hasAvailableUsage(
                     usage(0.0, 65.0)));

        BridgeUsageSnapshot explicitLimit =
            usage(35.0, 65.0);
        explicitLimit.rateLimitReachedType =
            QStringLiteral("primary");
        QVERIFY(
            CodexAutomaticContinuationPolicy::
                hasConfirmedExhaustion(
                    explicitLimit));
        QVERIFY(
            !CodexAutomaticContinuationPolicy::
                 hasAvailableUsage(
                     explicitLimit));
    }

    void policyRotatesProfilesAndBuildsStableEventKeys()
    {
        const QVector<CodexAccountProfile>
            profiles{
                {
                    QUuid::createUuid(),
                    QStringLiteral("Main"),
                },
                {
                    QUuid::createUuid(),
                    QStringLiteral("Account 2"),
                },
                {
                    QUuid::createUuid(),
                    QStringLiteral("Account 3"),
                },
            };
        const auto ordered =
            CodexAutomaticContinuationPolicy::
                orderedTargets(
                    profiles,
                    profiles.at(1).id);
        QCOMPARE(ordered.size(), 2);
        QCOMPARE(
            ordered.at(0).id,
            profiles.at(2).id);
        QCOMPARE(
            ordered.at(1).id,
            profiles.at(0).id);
        QCOMPARE(
            CodexAutomaticContinuationPolicy::
                taskEventKey(
                    QStringLiteral("thread-1"),
                    QStringLiteral("turn-1")),
            QStringLiteral(
                "thread-1|turn-1"));
        QCOMPARE(
            CodexAutomaticContinuationPolicy::
                goalEventKey(
                    QStringLiteral("thread-1"),
                    QStringLiteral(
                        "Build Companion"),
                    1700000123),
            QStringLiteral(
                "thread-1|Build Companion|1700000123"));
    }

    void automaticJournalPersistsPlanBeforeCompletionAndTrims()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path =
            temporary.filePath(
                QStringLiteral(
                    "continuations.json"));
        CodexAutomaticContinuationJournal
            journal(path);
        QVERIFY(
            !journal.loadError()
                 .has_value());

        const auto planned =
            journal.plan(
                QStringLiteral("event-0"),
                QStringLiteral("thread-1"),
                QUuid::createUuid(),
                QUuid::createUuid(),
                QStringLiteral("Account 2"),
                QStringLiteral(
                    "codex-companion-quota-id"),
                QDateTime::fromMSecsSinceEpoch(
                    1700000000000,
                    QTimeZone::UTC));
        QVERIFY(planned.hasValue());
        QVERIFY(
            !planned.value()
                 .completedAt
                 .has_value());

        CodexAutomaticContinuationJournal
            reloaded(path);
        const auto persisted =
            reloaded.attempt(
                QStringLiteral("event-0"));
        QVERIFY(persisted.has_value());
        QVERIFY(
            !persisted->completedAt
                 .has_value());

        for (int index = 0;
             index < 121;
             ++index) {
            const QString key =
                QStringLiteral("event-%1")
                    .arg(index + 1);
            const auto attempt =
                reloaded.plan(
                    key,
                    QStringLiteral("thread-1"),
                    QUuid::createUuid(),
                    QUuid::createUuid(),
                    QStringLiteral(
                        "Account 2"),
                    QStringLiteral(
                        "message-%1")
                        .arg(index),
                    QDateTime::
                        fromMSecsSinceEpoch(
                            1700000001000
                                + index,
                            QTimeZone::UTC));
            QVERIFY(attempt.hasValue());
            QVERIFY(
                reloaded.complete(
                            key,
                            QDateTime::
                                fromMSecsSinceEpoch(
                                    1700001000000
                                        + index,
                                    QTimeZone::UTC))
                    .hasValue());
        }
        QCOMPARE(reloaded.size(), 120);
    }

    void manualRetryHandsOffReactivatesAndUsesStableMessageId()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AccountFixture accounts(
            temporary.path());
        QVERIFY(
            accounts.profiles
                .select(
                    accounts.fallback.id)
                .hasValue());

        int handoffCount = 0;
        int activationCount = 0;
        int sendCount = 0;
        QStringList messageIds;
        QStringList activatedThreads;
        QStringList sentThreads;
        QStringList sentTexts;
        bool activationRouteValid = true;
        bool sendRouteValid = true;
        bool failFirstSend = true;
        CodexContinuationCommands commands;
        commands.handoff =
            [&handoffCount](
                QString,
                QString,
                ThreadRuntimeStatus,
                QUuid,
                std::stop_token) {
                ++handoffCount;
                return Result<void>::success();
            };
        commands.activateGoal =
            [&activationCount,
             &activatedThreads,
             &activationRouteValid](
                const CodexAccountRoute& route,
                QString threadId,
                std::stop_token) {
                ++activationCount;
                activationRouteValid =
                    activationRouteValid
                    && route.profileId
                           .has_value();
                activatedThreads.append(
                    threadId);
                BridgeGoal resumed =
                    goal(GoalStatus::Active);
                resumed.threadId =
                    std::move(threadId);
                return Result<BridgeGoal>::
                    success(
                        std::move(resumed));
            };
        commands.send =
            [&sendCount,
             &messageIds,
             &sentThreads,
             &sentTexts,
             &sendRouteValid,
             &failFirstSend](
                const CodexAccountRoute& route,
                QString threadId,
                QString text,
                QString messageId,
                std::stop_token) {
                ++sendCount;
                sendRouteValid =
                    sendRouteValid
                    && route.profileId
                           .has_value();
                sentThreads.append(
                    std::move(threadId));
                sentTexts.append(
                    std::move(text));
                messageIds.append(
                    std::move(messageId));
                if (failFirstSend) {
                    failFirstSend = false;
                    return Result<void>::failure({
                        QStringLiteral(
                            "codex.send_failed"),
                        QStringLiteral(
                            "offline"),
                        true,
                        {},
                    });
                }
                return Result<void>::success();
            };

        CodexFailedTaskRetryJournal journal(
            temporary.filePath(
                QStringLiteral(
                    "failed-retries.json")));
        CodexFailedTaskRetryService service(
            accounts.profiles,
            accounts.bindings,
            accounts.router,
            std::move(commands),
            journal);
        CodexFailedTaskRetryRequest request{
            QStringLiteral("thread-1"),
            temporary.filePath(
                QStringLiteral(
                    "thread-1.jsonl")),
            ThreadRuntimeStatus::NotLoaded,
            1700000000000,
            goal(
                GoalStatus::UsageLimited),
        };

        const auto failed =
            service.retry(request);
        const auto retried =
            service.retry(request);
        const auto duplicate =
            service.retry(request);

        QCOMPARE(
            failed.disposition,
            CodexFailedTaskRetryDisposition::
                Failed);
        QCOMPARE(
            retried.disposition,
            CodexFailedTaskRetryDisposition::
                Continued);
        QCOMPARE(
            duplicate.disposition,
            CodexFailedTaskRetryDisposition::
                AlreadyContinued);
        QCOMPARE(handoffCount, 2);
        QCOMPARE(activationCount, 2);
        QCOMPARE(sendCount, 2);
        QVERIFY(activationRouteValid);
        QVERIFY(sendRouteValid);
        const QStringList expectedThreads{
            QStringLiteral("thread-1"),
            QStringLiteral("thread-1"),
        };
        const QStringList expectedTexts{
            QStringLiteral("continue"),
            QStringLiteral("continue"),
        };
        QCOMPARE(
            activatedThreads,
            expectedThreads);
        QCOMPARE(
            sentThreads,
            expectedThreads);
        QCOMPARE(
            sentTexts,
            expectedTexts);
        QCOMPARE(messageIds.size(), 2);
        QCOMPARE(
            messageIds.at(0),
            messageIds.at(1));
        QVERIFY(
            messageIds.at(0)
                .startsWith(
                    QStringLiteral(
                        "codex-companion-retry-")));
    }

    void unsafeManualRetryNeverMovesOrSends()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AccountFixture accounts(
            temporary.path());
        int calls = 0;
        CodexContinuationCommands commands;
        commands.handoff =
            [&calls](
                QString,
                QString,
                ThreadRuntimeStatus,
                QUuid,
                std::stop_token) {
                ++calls;
                return Result<void>::success();
            };
        commands.send =
            [&calls](
                const CodexAccountRoute&,
                QString,
                QString,
                QString,
                std::stop_token) {
                ++calls;
                return Result<void>::success();
            };
        CodexFailedTaskRetryJournal journal(
            temporary.filePath(
                QStringLiteral(
                    "failed-retries.json")));
        CodexFailedTaskRetryService service(
            accounts.profiles,
            accounts.bindings,
            accounts.router,
            std::move(commands),
            journal);

        const auto result =
            service.retry({
                QStringLiteral("thread-1"),
                temporary.filePath(
                    QStringLiteral(
                        "thread-1.jsonl")),
                ThreadRuntimeStatus::Active,
                1700000000000,
                std::nullopt,
            });

        QCOMPARE(
            result.disposition,
            CodexFailedTaskRetryDisposition::
                NotEligible);
        QCOMPARE(calls, 0);
    }

    void systemErrorManualRetryCanResumeTheStoppedTask()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AccountFixture accounts(
            temporary.path());
        QVERIFY(
            accounts.profiles
                .select(
                    accounts.fallback.id)
                .hasValue());

        int handoffs = 0;
        int sends = 0;
        QString sentText;
        CodexContinuationCommands commands;
        commands.handoff =
            [&handoffs](
                QString,
                QString,
                ThreadRuntimeStatus,
                QUuid,
                std::stop_token) {
                ++handoffs;
                return Result<void>::success();
            };
        commands.send =
            [&sends, &sentText](
                const CodexAccountRoute&,
                QString,
                QString text,
                QString,
                std::stop_token) {
                ++sends;
                sentText = std::move(text);
                return Result<void>::success();
            };
        CodexFailedTaskRetryJournal journal(
            temporary.filePath(
                QStringLiteral(
                    "failed-retries.json")));
        CodexFailedTaskRetryService service(
            accounts.profiles,
            accounts.bindings,
            accounts.router,
            std::move(commands),
            journal);

        const auto result = service.retry({
            QStringLiteral("thread-1"),
            temporary.filePath(
                QStringLiteral(
                    "thread-1.jsonl")),
            ThreadRuntimeStatus::SystemError,
            1700000000000,
            std::nullopt,
        });

        QCOMPARE(
            result.disposition,
            CodexFailedTaskRetryDisposition::
                Continued);
        QCOMPARE(handoffs, 1);
        QCOMPARE(sends, 1);
        QCOMPARE(
            sentText,
            QStringLiteral("continue"));
    }

    void automaticContinuationRequiresExplicitQuotaAndRunsOnce()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AccountFixture accounts(
            temporary.path());

        int quotaReads = 0;
        int handoffs = 0;
        int sends = 0;
        QString sentMessageId;
        QString sentText;
        CodexContinuationCommands commands;
        commands.readQuota =
            [&quotaReads](
                const CodexAccountRoute&,
                QString threadId,
                std::stop_token) {
                ++quotaReads;
                return Result<
                    std::optional<
                        CodexQuotaInterruption>>::
                    success(
                        CodexQuotaInterruption{
                            std::move(threadId),
                            QStringLiteral(
                                "turn-quota"),
                            QDateTime::
                                currentDateTimeUtc(),
                        });
            };
        commands.readUsage =
            [&accounts](
                const CodexAccountRoute& route,
                std::stop_token) {
                if (route.profileId
                    == std::optional<QUuid>(
                        accounts.main.id)) {
                    return Result<
                        BridgeUsageSnapshot>::
                        success(
                            usage(0.0, 40.0));
                }
                return Result<
                    BridgeUsageSnapshot>::
                    success(
                        usage(35.0, 40.0));
            };
        commands.readGoal =
            [](
                const CodexAccountRoute&,
                QString,
                std::stop_token) {
                return Result<
                    std::optional<
                        BridgeGoal>>::
                    success(std::nullopt);
            };
        commands.handoff =
            [&handoffs](
                QString,
                QString,
                ThreadRuntimeStatus,
                QUuid,
                std::stop_token) {
                ++handoffs;
                return Result<void>::success();
            };
        commands.send =
            [&sends,
             &sentMessageId,
             &sentText](
                const CodexAccountRoute&,
                QString,
                QString text,
                QString messageId,
                std::stop_token) {
                ++sends;
                sentText =
                    std::move(text);
                sentMessageId =
                    std::move(messageId);
                return Result<void>::success();
            };
        CodexAutomaticContinuationJournal
            journal(
                temporary.filePath(
                    QStringLiteral(
                        "automatic.json")));
        CodexAutomaticAccountContinuationCoordinator
            coordinator(
                accounts.profiles,
                accounts.bindings,
                accounts.router,
                std::move(commands),
                journal,
                [] {
                    return QDateTime::
                        fromMSecsSinceEpoch(
                            1700000000000,
                            QTimeZone::UTC);
                });
        const QVector<
            CodexAutomaticContinuationCandidate>
            candidates{
                {
                    QStringLiteral("thread-1"),
                    temporary.filePath(
                        QStringLiteral(
                            "thread-1.jsonl")),
                    ThreadRuntimeStatus::Idle,
                },
            };

        const auto disabled =
            coordinator.continueEligible(
                false,
                candidates);
        const auto first =
            coordinator.continueEligible(
                true,
                candidates);
        const auto repeated =
            coordinator.continueEligible(
                true,
                candidates);

        QVERIFY(disabled.isEmpty());
        QCOMPARE(first.size(), 1);
        QVERIFY(repeated.isEmpty());
        QCOMPARE(quotaReads, 2);
        QCOMPARE(handoffs, 1);
        QCOMPARE(sends, 1);
        QCOMPARE(
            sentText,
            QStringLiteral("continue"));
        QVERIFY(
            sentMessageId.startsWith(
                QStringLiteral(
                    "codex-companion-quota-")));
        QCOMPARE(
            first.first()
                .destinationProfileId,
            accounts.fallback.id);
    }

    void automaticContinuationRetriesPersistedPlanAfterHandoff()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AccountFixture accounts(
            temporary.path());

        int quotaReads = 0;
        int handoffs = 0;
        int sends = 0;
        bool failFirstSend = true;
        QStringList messageIds;
        CodexContinuationCommands commands;
        commands.readQuota =
            [&quotaReads](
                const CodexAccountRoute&,
                QString threadId,
                std::stop_token) {
                ++quotaReads;
                return Result<
                    std::optional<
                        CodexQuotaInterruption>>::
                    success(
                        CodexQuotaInterruption{
                            std::move(threadId),
                            QStringLiteral(
                                "turn-quota"),
                            QDateTime::
                                currentDateTimeUtc(),
                        });
            };
        commands.readUsage =
            [&accounts](
                const CodexAccountRoute& route,
                std::stop_token) {
                return Result<
                    BridgeUsageSnapshot>::
                    success(
                        route.profileId
                                == std::optional<
                                    QUuid>(
                                    accounts
                                        .main.id)
                            ? usage(0.0, 40.0)
                            : usage(35.0, 40.0));
            };
        commands.readGoal =
            [](
                const CodexAccountRoute&,
                QString,
                std::stop_token) {
                return Result<
                    std::optional<
                        BridgeGoal>>::
                    success(std::nullopt);
            };
        commands.handoff =
            [&accounts, &handoffs](
                QString threadId,
                QString,
                ThreadRuntimeStatus,
                QUuid destinationProfileId,
                std::stop_token) {
                ++handoffs;
                return accounts.bindings
                    .bind(
                        std::move(threadId),
                        destinationProfileId);
            };
        commands.send =
            [&failFirstSend,
             &messageIds,
             &sends](
                const CodexAccountRoute&,
                QString,
                QString,
                QString messageId,
                std::stop_token) {
                ++sends;
                messageIds.append(
                    std::move(messageId));
                if (failFirstSend) {
                    failFirstSend = false;
                    return Result<void>::failure({
                        QStringLiteral(
                            "codex.send_failed"),
                        QStringLiteral(
                            "offline"),
                        true,
                        {},
                    });
                }
                return Result<void>::success();
            };

        CodexAutomaticContinuationJournal
            journal(
                temporary.filePath(
                    QStringLiteral(
                        "automatic.json")));
        CodexAutomaticAccountContinuationCoordinator
            coordinator(
                accounts.profiles,
                accounts.bindings,
                accounts.router,
                std::move(commands),
                journal,
                [] {
                    return QDateTime::
                        fromMSecsSinceEpoch(
                            1700000000000,
                            QTimeZone::UTC);
                });
        const QVector<
            CodexAutomaticContinuationCandidate>
            candidates{
                {
                    QStringLiteral("thread-1"),
                    temporary.filePath(
                        QStringLiteral(
                            "thread-1.jsonl")),
                    ThreadRuntimeStatus::Idle,
                },
            };

        const auto failed =
            coordinator.continueEligible(
                true,
                candidates);
        const auto retried =
            coordinator.continueEligible(
                true,
                candidates);

        QVERIFY(failed.isEmpty());
        QCOMPARE(retried.size(), 1);
        QCOMPARE(quotaReads, 1);
        QCOMPARE(handoffs, 1);
        QCOMPARE(sends, 2);
        QCOMPARE(messageIds.size(), 2);
        QCOMPARE(
            messageIds.at(0),
            messageIds.at(1));
        QCOMPARE(
            accounts.bindings
                .profileIdFor(
                    QStringLiteral(
                        "thread-1")),
            std::optional<QUuid>(
                accounts.fallback.id));
    }
};

QTEST_GUILESS_MAIN(CodexContinuationTests)

#include "CodexContinuationTests.moc"
