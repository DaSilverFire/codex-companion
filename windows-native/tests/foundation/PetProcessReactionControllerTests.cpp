#include "app/PetProcessReactionController.h"

#include "codex/runtime/ProcessListModel.h"
#include "ui/PetViewModel.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

namespace {

constexpr double kSwiftReferenceDateUnixSeconds =
    978307200.0;

companion::BridgeTask task(
    QString id,
    companion::TaskStatus status,
    QString preview = {})
{
    companion::BridgeTask result;
    result.id = std::move(id);
    result.title =
        QStringLiteral("Companion task");
    result.status = status;
    result.preview = std::move(preview);
    result.updatedAt.secondsSinceReferenceDate =
        static_cast<double>(
            QDateTime::currentDateTimeUtc()
                .toMSecsSinceEpoch())
        / 1000.0
        - kSwiftReferenceDateUnixSeconds;
    return result;
}

companion::CodexJobRecord job(
    QString id,
    QString status,
    std::optional<QString> threadId =
        std::nullopt)
{
    const QDateTime now =
        QDateTime::currentDateTimeUtc();
    return {
        std::move(id),
        QStringLiteral("Companion job"),
        std::move(status),
        QStringLiteral("Job instructions"),
        std::nullopt,
        std::move(threadId),
        now,
        now.addSecs(-10),
    };
}

companion::BridgeTask goalTask(
    QString id,
    companion::TaskStatus status,
    companion::GoalStatus goalStatus,
    qint64 goalCreatedAt = 1)
{
    companion::BridgeTask result =
        task(std::move(id), status);
    result.goal = companion::BridgeGoal{
        result.id,
        QStringLiteral("Finish Companion parity"),
        goalStatus,
        std::nullopt,
        0,
        10,
        goalCreatedAt,
        goalCreatedAt,
    };
    return result;
}

void publish(
    companion::ProcessListModel& model,
    QVector<companion::BridgeTask> tasks,
    QVector<companion::CodexJobRecord> jobs = {},
    QHash<QString, companion::PendingApproval>
        approvals = {})
{
    companion::CodexProcessSnapshot snapshot;
    snapshot.tasks = std::move(tasks);
    snapshot.jobs = std::move(jobs);
    snapshot.pendingApprovals =
        std::move(approvals);
    model.setSnapshot(
        snapshot,
        QDateTime::currentDateTimeUtc());
}

} // namespace

class PetProcessReactionControllerTests final
    : public QObject {
    Q_OBJECT

private slots:
    void firstTaskPublicationReactsAfterEmptySeed()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 60});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);

        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Waiting),
        });

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("attention"));
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("first"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("waiting"));
    }

    void runningToWaitingPublishesApprovalReaction()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Working")),
        });

        auto waiting = task(
            QStringLiteral("first"),
            companion::TaskStatus::Waiting,
            QStringLiteral("Approve the command"));
        waiting.needsApproval = true;
        publish(model, {waiting});

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("attention"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("waiting"));

        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("first"));
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("attention"));

        controller.dismissAttention();
        QVERIFY(!controller.hasAttention());
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("first"));
    }

    void failureWinsOverLowerPriorityTransitions()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("done"),
                companion::TaskStatus::Running),
            task(
                QStringLiteral("failed"),
                companion::TaskStatus::Running),
        });

        publish(model, {
            task(
                QStringLiteral("done"),
                companion::TaskStatus::Completed,
                QStringLiteral("Finished")),
            task(
                QStringLiteral("failed"),
                companion::TaskStatus::Failed,
                QStringLiteral("Build failed")),
        });

        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("failure"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("failed"));
    }

    void noticeRowsNeverDrivePetReactions()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString failureStatePath =
            directory.filePath(
                QStringLiteral(
                    "process-failures.v1.json"));
        const QDateTime now =
            QDateTime::currentDateTimeUtc();
        const QJsonObject notice{
            {QStringLiteral("processId"),
             QStringLiteral("notice-failure")},
            {QStringLiteral("threadId"),
             QString()},
            {QStringLiteral("kind"),
             QStringLiteral("notice")},
            {QStringLiteral("title"),
             QStringLiteral("Synthetic notice")},
            {QStringLiteral("preview"),
             QStringLiteral("This is not a process")},
            {QStringLiteral("updatedAt"),
             static_cast<double>(
                 now.toMSecsSinceEpoch())
                 / 1000.0
                 - kSwiftReferenceDateUnixSeconds},
            {QStringLiteral("needsApproval"), false},
            {QStringLiteral("activityAtMs"),
             static_cast<double>(
                 now.toMSecsSinceEpoch())},
        };
        QFile stateFile(failureStatePath);
        QVERIFY(stateFile.open(QIODevice::WriteOnly));
        const QByteArray state =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("version"), 1},
                    {QStringLiteral("unresolved"),
                     QJsonArray{notice}},
                    {QStringLiteral("handled"),
                     QJsonArray{}},
                })
                .toJson(QJsonDocument::Compact);
        QCOMPARE(stateFile.write(state), state.size());
        stateFile.close();

        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200});
        companion::ProcessListModel model(
            {},
            failureStatePath);
        controller.setProcessModel(&model);

        publish(model, {
            task(
                QStringLiteral("real-completion"),
                companion::TaskStatus::Completed,
                QStringLiteral("Finished real work")),
        });

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("real-completion"));
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("completion"));
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("real-completion"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));
    }

    void firstCompletedGoalCelebratesOnce()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200, 35});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);

        auto completed = goalTask(
            QStringLiteral("goal"),
            companion::TaskStatus::Completed,
            companion::GoalStatus::Complete,
            42);
        publish(model, {completed});

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("completion"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("running"),
            200);

        publish(model, {completed});
        QTest::qWait(70);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("running"));
    }

    void completedGoalsPublishMonotonicConfettiTriggers()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200, 35});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);

        QVERIFY(
            controller.property(
                "goalConfettiTrigger")
                .isValid());
        QSignalSpy confettiSpy(
            &controller,
            SIGNAL(goalConfettiTriggerChanged()));
        QVERIFY(confettiSpy.isValid());

        publish(model, {
            goalTask(
                QStringLiteral("first-goal"),
                companion::TaskStatus::Running,
                companion::GoalStatus::Active,
                101),
        });
        QCOMPARE(
            controller.property(
                "goalConfettiTrigger")
                .toInt(),
            0);

        const auto firstCompleted = goalTask(
            QStringLiteral("first-goal"),
            companion::TaskStatus::Completed,
            companion::GoalStatus::Complete,
            101);
        publish(model, {firstCompleted});

        QCOMPARE(
            controller.property(
                "goalConfettiTrigger")
                .toInt(),
            1);
        QCOMPARE(confettiSpy.count(), 1);

        publish(model, {firstCompleted});

        QCOMPARE(
            controller.property(
                "goalConfettiTrigger")
                .toInt(),
            1);
        QCOMPARE(confettiSpy.count(), 1);

        publish(model, {
            goalTask(
                QStringLiteral("second-goal"),
                companion::TaskStatus::Completed,
                companion::GoalStatus::Complete,
                202),
        });

        QCOMPARE(
            controller.property(
                "goalConfettiTrigger")
                .toInt(),
            2);
        QCOMPARE(confettiSpy.count(), 2);
    }

    void unchangedActiveGoalDoesNotRestartAttention()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        const auto activeGoal = goalTask(
            QStringLiteral("goal"),
            companion::TaskStatus::Running,
            companion::GoalStatus::Active,
            84);

        publish(model, {activeGoal});
        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("goal"));
        controller.dismissAttention();
        QVERIFY(!controller.hasAttention());

        publish(model, {activeGoal});

        QVERIFY(!controller.hasAttention());
    }

    void completedGoalCelebrationPersistsAcrossControllers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            directory.filePath(
                QStringLiteral(
                    "companion.ini"));
        const auto completed = goalTask(
            QStringLiteral("goal"),
            companion::TaskStatus::Completed,
            companion::GoalStatus::Complete,
            314);

        {
            companion::PetViewModel pet;
            companion::PetProcessReactionController
                controller(
                    pet,
                    {20, 200, 35},
                    settingsPath);
            companion::ProcessListModel model;
            controller.setProcessModel(&model);

            publish(model, {completed});
            QCOMPARE(
                pet.renderedAnimation(),
                QStringLiteral(
                    "goal-complete"));
        }

        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200, 35},
                settingsPath);
        companion::ProcessListModel model;
        controller.setProcessModel(&model);

        publish(model, {completed});
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));
        QVERIFY(controller.hasAttention());
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("completion"));
        QVERIFY(
            !pet.goalCelebrationActive());
    }

    void goalCelebrationOwnsAnimationUntilItsTimerEnds()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 250, 70});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            goalTask(
                QStringLiteral("goal"),
                companion::TaskStatus::Running,
                companion::GoalStatus::Active,
                84),
            task(
                QStringLiteral("other"),
                companion::TaskStatus::Running),
        });

        publish(model, {
            goalTask(
                QStringLiteral("goal"),
                companion::TaskStatus::Completed,
                companion::GoalStatus::Complete,
                84),
            task(
                QStringLiteral("other"),
                companion::TaskStatus::Running),
        });
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));

        publish(model, {
            goalTask(
                QStringLiteral("goal"),
                companion::TaskStatus::Completed,
                companion::GoalStatus::Complete,
                84),
            task(
                QStringLiteral("other"),
                companion::TaskStatus::Failed,
                QStringLiteral("Build failed")),
        });

        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("failure"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));

        controller.dismissAttention();
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("running"),
            250);
    }

    void openProcessMenuSettlesRunningAnimationToIdle()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("running"),
                companion::TaskStatus::Running),
        });

        pet.setMenuOpen(true);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("running"));
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("idle"),
            200);
    }

    void chatSurfaceKeepsItsAnimationUntilProcessesAreShown()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        pet.setMenuOpen(true);
        controller.setProcessSurfaceVisible(false);
        pet.setSelectedAnimation(
            QStringLiteral("review"));

        publish(model, {
            task(
                QStringLiteral("failed"),
                companion::TaskStatus::Failed,
                QStringLiteral("Build failed")),
        });

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("review"));

        controller.setProcessSurfaceVisible(true);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("failed"));
    }

    void openingMenuPreservesActiveAttentionAnimation()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("completed"),
                companion::TaskStatus::Running),
        });
        publish(model, {
            task(
                QStringLiteral("completed"),
                companion::TaskStatus::Completed),
        });

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));

        pet.setMenuOpen(true);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("goal-complete"));
        controller.dismissAttention();
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("review"));
    }

    void menuOpenTransitionStillUpdatesLatestHighlight()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 200});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("completed"),
                companion::TaskStatus::Running),
        });
        pet.setMenuOpen(true);

        publish(model, {
            task(
                QStringLiteral("completed"),
                companion::TaskStatus::Completed),
        });

        QVERIFY(!controller.hasAttention());
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("completed"));
        QCOMPARE(
            controller.latestAttentionHighlight()
                .value(QStringLiteral("kind"))
                .toString(),
            QStringLiteral("completion"));
    }

    void reopeningProcessMenuRestartsRunningAnimation()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("running"),
                companion::TaskStatus::Running),
        });

        pet.setMenuOpen(true);
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("idle"),
            200);

        pet.setMenuOpen(false);
        pet.setMenuOpen(true);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("running"));
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("idle"),
            200);
    }

    void attentionAutoDismissesAndRestoresRoaming()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 20});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running),
        });
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Completed,
                QStringLiteral("Done")),
        });

        QVERIFY(controller.hasAttention());
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.hasAttention(),
            200);
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("running"));
    }

    void attentionAutoDismissWhileHoveredPreservesHoverAnimation()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 20});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Working")),
        });
        pet.setPointerHovered(true);
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("jumping"));

        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Fresh update")),
        });

        QVERIFY(controller.hasAttention());
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("jumping"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.hasAttention(),
            200);
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("jumping"));
        QCOMPARE(pet.frameRow(), 4);
    }

    void stagedReactionPublishesOnlyLatestSnapshot()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 250, 6200, 40});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Initial update")),
        });

        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Stale update")),
        });
        QVERIFY(!controller.hasAttention());

        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Latest update")),
        });
        QVERIFY(!controller.hasAttention());

        QTRY_VERIFY_WITH_TIMEOUT(
            controller.hasAttention(),
            200);
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("detail"))
                .toString(),
            QStringLiteral("Latest update"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("talking"));
    }

    void openingMenuCancelsStagedReaction()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 250, 6200, 40});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Initial update")),
        });
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Pending update")),
        });

        pet.setMenuOpen(true);
        QTest::qWait(80);

        QVERIFY(!controller.hasAttention());
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("idle"));
    }

    void autoDismissDoesNotCancelNewerStagedReaction()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 50, 6200, 80});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Initial update")),
        });
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("First reaction")),
        });
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.hasAttention(),
            250);
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("detail"))
                .toString(),
            QStringLiteral("First reaction"));

        QTest::qWait(30);
        publish(model, {
            task(
                QStringLiteral("first"),
                companion::TaskStatus::Running,
                QStringLiteral("Newest reaction")),
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.hasAttention(),
            150);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.hasAttention(),
            250);
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("detail"))
                .toString(),
            QStringLiteral("Newest reaction"));
    }

    void jobOnlyActivityDrivesTheMenuAnimation()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(
            model,
            {},
            {
                job(
                    QStringLiteral("running"),
                    QStringLiteral("running")),
            });

        pet.setMenuOpen(true);

        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("running"));
        QTRY_COMPARE_WITH_TIMEOUT(
            pet.renderedAnimation(),
            QStringLiteral("idle"),
            200);
    }

    void assignedJobReactionPreservesCardAndThreadIdentity()
    {
        companion::PetViewModel pet;
        companion::PetProcessReactionController
            controller(
                pet,
                {20, 100});
        companion::ProcessListModel model;
        controller.setProcessModel(&model);
        publish(
            model,
            {},
            {
                job(
                    QStringLiteral("build"),
                    QStringLiteral("running"),
                    QStringLiteral("thread-build")),
            });
        auto failed = job(
            QStringLiteral("build"),
            QStringLiteral("failed"),
            QStringLiteral("thread-build"));
        failed.error =
            QStringLiteral("Build failed");

        publish(model, {}, {failed});

        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("processId"))
                .toString(),
            QStringLiteral("job-build"));
        QCOMPARE(
            controller.attentionMessage()
                .value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-build"));
        QCOMPARE(
            pet.renderedAnimation(),
            QStringLiteral("failed"));
    }
};

QTEST_GUILESS_MAIN(
    PetProcessReactionControllerTests)

#include "PetProcessReactionControllerTests.moc"
