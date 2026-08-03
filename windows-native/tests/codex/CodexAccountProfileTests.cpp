#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountLoginService.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "codex/accounts/CodexThreadAccountHandoffService.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/accounts/ProfiledCodexControlService.h"
#include "codex/accounts/ProfiledTaskCreator.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

using namespace companion;

namespace {

const QUuid kMainProfileId(
    QStringLiteral(
        "{5774A03C-CB05-4809-9054-464F3E25D6A0}"));
const QUuid kBackupProfileId(
    QStringLiteral(
        "{A7E4430B-DFD1-424C-A1ED-26AF90B520A8}"));

QString readText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

template <typename T>
QFuture<Result<T>> readyFuture(
    Result<T> result)
{
    QPromise<Result<T>> promise;
    promise.start();
    promise.addResult(
        std::move(result));
    promise.finish();
    return promise.future();
}

template <typename T>
CommitAwareMutationHandle<T>
readyMutation(Result<T> result)
{
    const auto mutation =
        CommitAwareMutation<T>::create();
    auto handle = mutation->handle();
    mutation->finish(
        std::move(result));
    return handle;
}

} // namespace

class CodexAccountProfileTests final
    : public QObject {
    Q_OBJECT

private slots:
    void storePersistsProfilesAndSelectionWithoutCredentials()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path =
            temporary.filePath(
                QStringLiteral("profiles.json"));
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore store(
            path,
            [&generatedIds] {
                return generatedIds.takeFirst();
            });

        const auto main =
            store.add(QStringLiteral("  Main  "));
        QVERIFY(main.hasValue());
        const auto backup =
            store.add(QStringLiteral("Backup"));
        QVERIFY(backup.hasValue());
        QVERIFY(store.select(kBackupProfileId)
                    .hasValue());

        CodexAccountProfileStore restored(path);

        QVERIFY(!restored.loadError().has_value());
        QCOMPARE(restored.profiles().size(), 2);
        const CodexAccountProfile
            expectedMain{
                kMainProfileId,
                QStringLiteral("Main"),
            };
        const CodexAccountProfile
            expectedBackup{
                kBackupProfileId,
                QStringLiteral("Backup"),
            };
        QCOMPARE(
            restored.profiles().at(0),
            expectedMain);
        QCOMPARE(
            restored.profiles().at(1),
            expectedBackup);
        QCOMPARE(
            restored.selectedProfileId(),
            std::optional<QUuid>(
                kBackupProfileId));

        const QString persisted =
            readText(path);
        QVERIFY(persisted.contains(
            QStringLiteral("\"version\": 1")));
        QVERIFY(persisted.contains(
            QStringLiteral("\"profiles\"")));
        QVERIFY(persisted.contains(
            QStringLiteral("\"selectedProfileId\"")));
        QVERIFY(persisted.contains(
            QStringLiteral("\"label\": \"Main\"")));
        QVERIFY(!persisted.contains(
            QStringLiteral("token"),
            Qt::CaseInsensitive));
        QVERIFY(!persisted.contains(
            QStringLiteral("cookie"),
            Qt::CaseInsensitive));
        QVERIFY(!persisted.contains(
            QStringLiteral("secret"),
            Qt::CaseInsensitive));
    }

    void storePersistsCurrentCodexAccountSelection()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path =
            temporary.filePath(
                QStringLiteral("profiles.json"));
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore store(
            path,
            [&generatedIds] {
                return generatedIds.takeFirst();
            });

        QVERIFY(store.add(QStringLiteral("Main"))
                    .hasValue());
        QVERIFY(store.selectCurrentAccount()
                    .hasValue());
        QVERIFY(!store.selectedProfileId()
                     .has_value());

        QVERIFY(store.add(QStringLiteral("Backup"))
                    .hasValue());
        QVERIFY(!store.selectedProfileId()
                     .has_value());

        CodexAccountProfileStore restored(path);
        QVERIFY(!restored.loadError().has_value());
        QCOMPARE(restored.profiles().size(), 2);
        QVERIFY(!restored.selectedProfileId()
                     .has_value());

        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared")));
        QProcessEnvironment base;
        base.insert(
            QStringLiteral("CURRENT_ACCOUNT"),
            QStringLiteral("yes"));
        CodexAccountRouter router(
            base,
            restored,
            runtime,
            bindings);

        const CodexAccountRoute route =
            router.routeNewWork();
        QVERIFY(!route.profileId.has_value());
        QCOMPARE(
            route.environment.value(
                QStringLiteral("CURRENT_ACCOUNT")),
            QStringLiteral("yes"));
        QVERIFY(!route.environment.contains(
            QStringLiteral("CODEX_HOME")));
    }

    void runtimeUsesIsolatedCodexHomeAndSharedTaskCatalog()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("Codex Profiles")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        QProcessEnvironment base;
        base.insert(
            QStringLiteral("PRESERVED"),
            QStringLiteral("yes"));

        const auto route =
            runtime.forProfile(
                base,
                CodexAccountProfile{
                    kMainProfileId,
                    QStringLiteral("Main"),
                });

        QVERIFY(route.hasValue());
        QCOMPARE(
            route.value().value(
                QStringLiteral("CODEX_HOME")),
            QDir::cleanPath(
                temporary.filePath(
                    QStringLiteral(
                        "Codex Profiles/"
                        "5774a03ccb0548099054464f3e25d6a0"))));
        QCOMPARE(
            route.value().value(
                QStringLiteral(
                    "CODEX_SQLITE_HOME")),
            QDir::cleanPath(
                temporary.filePath(
                    QStringLiteral(
                        "shared-codex"))));
        QCOMPARE(
            route.value().value(
                QStringLiteral("PRESERVED")),
            QStringLiteral("yes"));
    }

    void routerUsesSelectionForNewWorkButOriginBindingForExistingTask()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [&generatedIds] {
                return generatedIds.takeFirst();
            });
        QVERIFY(profiles.add(
                            QStringLiteral("Main"))
                    .hasValue());
        QVERIFY(profiles.add(
                            QStringLiteral("Backup"))
                    .hasValue());
        QVERIFY(profiles.select(
                            kBackupProfileId)
                    .hasValue());

        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(bindings.bind(
                             QStringLiteral(
                                 "thread-main"),
                             kMainProfileId)
                    .hasValue());

        QProcessEnvironment base;
        base.insert(
            QStringLiteral("BASE_ROUTE"),
            QStringLiteral("true"));
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            base,
            profiles,
            runtime,
            bindings);

        const CodexAccountRoute newWork =
            router.routeNewWork();
        const CodexAccountRoute bound =
            router.routeThread(
                QStringLiteral("thread-main"));
        const CodexAccountRoute legacy =
            router.routeThread(
                QStringLiteral(
                    "legacy-thread"));

        QCOMPARE(
            newWork.profileId,
            std::optional<QUuid>(
                kBackupProfileId));
        QVERIFY(newWork.environment
                    .value(
                        QStringLiteral(
                            "CODEX_HOME"))
                    .endsWith(
                        QStringLiteral(
                            "a7e4430bdfd1424ca1ed26af90b520a8")));
        QCOMPARE(
            bound.profileId,
            std::optional<QUuid>(
                kMainProfileId));
        QVERIFY(bound.environment
                    .value(
                        QStringLiteral(
                            "CODEX_HOME"))
                    .endsWith(
                        QStringLiteral(
                            "5774a03ccb0548099054464f3e25d6a0")));
        QVERIFY(!legacy.profileId.has_value());
        QCOMPARE(
            legacy.environment.value(
                QStringLiteral("BASE_ROUTE")),
            QStringLiteral("true"));
        QVERIFY(!legacy.environment.contains(
            QStringLiteral("CODEX_HOME")));
    }

    void profiledTaskCreationUsesSelectionAndBindsOnlySuccessfulThreads()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [&generatedIds] {
                return generatedIds.takeFirst();
            });
        QVERIFY(
            profiles
                .add(QStringLiteral("Main"))
                .hasValue());
        QVERIFY(
            profiles
                .add(QStringLiteral("Backup"))
                .hasValue());
        QVERIFY(
            profiles.select(kBackupProfileId)
                .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            {},
            profiles,
            runtime,
            bindings);
        QVector<CodexAccountRoute> routes;
        int calls = 0;
        ProfiledTaskCreator creator(
            router,
            [&routes, &calls](
                const CreateTaskRequest&,
                const CodexAccountRoute& route) {
                routes.append(route);
                ++calls;
                if (calls == 1) {
                    return Result<QString>::success(
                        QStringLiteral(
                            "thread-profiled"));
                }
                return Result<QString>::failure({
                    QStringLiteral(
                        "codex.synthetic-failure"),
                    QStringLiteral(
                        "Synthetic create failure."),
                    false,
                    {},
                });
            });

        const auto created =
            creator.create({
                QStringLiteral("Build it"),
                {},
                {},
                {},
                {},
                {},
                {},
                QStringLiteral(
                    "11111111-1111-1111-1111-111111111111"),
            });
        const auto failed =
            creator.create({
                QStringLiteral("Fail it"),
                {},
                {},
                {},
                {},
                {},
                {},
                QStringLiteral(
                    "22222222-2222-2222-2222-222222222222"),
            });

        QVERIFY(created.hasValue());
        QVERIFY(!failed.hasValue());
        QCOMPARE(routes.size(), 2);
        QCOMPARE(
            routes.constFirst().profileId,
            std::optional<QUuid>(
                kBackupProfileId));
        QVERIFY(
            routes.constFirst()
                .environment.value(
                    QStringLiteral(
                        "CODEX_HOME"))
                .endsWith(
                    QStringLiteral(
                        "a7e4430bdfd1424ca1ed26af90b520a8")));
        QCOMPARE(
            bindings.profileIdFor(
                QStringLiteral(
                    "thread-profiled")),
            std::optional<QUuid>(
                kBackupProfileId));
        QVERIFY(
            !bindings
                 .profileIdFor(
                     QStringLiteral(
                         "thread-failed"))
                 .has_value());
    }

    void profiledControlRoutesUsageToSelectionAndGoalsToThreadOrigin()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [&generatedIds] {
                return generatedIds.takeFirst();
            });
        QVERIFY(
            profiles
                .add(QStringLiteral("Main"))
                .hasValue());
        QVERIFY(
            profiles
                .add(QStringLiteral("Backup"))
                .hasValue());
        QVERIFY(
            profiles.select(kBackupProfileId)
                .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(
            bindings
                .bind(
                    QStringLiteral("thread-main"),
                    kMainProfileId)
                .hasValue());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            {},
            profiles,
            runtime,
            bindings);
        std::optional<QUuid> usageProfile;
        QVector<std::optional<QUuid>>
            goalReadProfiles;
        QVector<QVector<QString>>
            goalReadThreadGroups;
        std::optional<QUuid>
            goalMutationProfile;
        ProfiledCodexControlCommands commands;
        commands.readUsage =
            [&usageProfile](
                const CodexAccountRoute& route) {
                usageProfile =
                    route.profileId;
                return readyFuture(
                    Result<
                        BridgeUsageSnapshot>::success(
                        {}));
            };
        commands.readGoals =
            [&goalReadProfiles,
             &goalReadThreadGroups](
                const CodexAccountRoute& route,
                const QVector<QString>& threadIds,
                std::stop_token) {
                goalReadProfiles.append(
                    route.profileId);
                goalReadThreadGroups.append(
                    threadIds);
                QHash<
                    QString,
                    std::optional<BridgeGoal>>
                    result;
                for (const QString& threadId :
                     threadIds) {
                    result.insert(
                        threadId,
                        std::nullopt);
                }
                return Result<
                    QHash<
                        QString,
                        std::optional<
                            BridgeGoal>>>::
                    success(
                        std::move(result));
            };
        commands.mutateGoal =
            [&goalMutationProfile](
                const CodexAccountRoute& route,
                RuntimeGoalMutationRequest) {
                goalMutationProfile =
                    route.profileId;
                return readyMutation(
                    Result<BridgeGoal>::
                        success({}));
            };
        ProfiledCodexControlService service(
            router,
            std::move(commands));

        auto usage = service.readUsage();
        usage.waitForFinished();
        QVERIFY(usage.result().hasValue());
        QCOMPARE(
            usageProfile,
            std::optional<QUuid>(
                kBackupProfileId));

        const auto goals =
            service.readGoalsSync(
                {
                    QStringLiteral(
                        "thread-main"),
                    QStringLiteral(
                        "legacy-thread"),
                });
        QVERIFY(goals.hasValue());
        QCOMPARE(goalReadProfiles.size(), 2);
        QVERIFY(
            goalReadProfiles.contains(
                std::optional<QUuid>(
                    kMainProfileId)));
        QVERIFY(
            goalReadProfiles.contains(
                std::nullopt));
        QCOMPARE(
            goalReadThreadGroups.size(),
            2);

        auto mutation =
            service.mutateGoal({
                RuntimeGoalMutationKind::Pause,
                QStringLiteral("thread-main"),
                std::nullopt,
                std::nullopt,
            });
        mutation.terminalFuture
            .waitForFinished();
        QVERIFY(
            mutation.terminalFuture
                .result()
                .hasValue());
        QCOMPARE(
            goalMutationProfile,
            std::optional<QUuid>(
                kMainProfileId));
    }

    void handoffResumesExactThreadBeforeChangingBinding()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [&generatedIds] {
                return generatedIds.takeFirst();
            });
        QVERIFY(
            profiles
                .add(QStringLiteral("Main"))
                .hasValue());
        QVERIFY(
            profiles
                .add(QStringLiteral("Backup"))
                .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(
            bindings.bind(
                        QStringLiteral(
                            "thread-1"),
                        kMainProfileId)
                .hasValue());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            {},
            profiles,
            runtime,
            bindings);
        QVector<CodexAccountRoute> routes;
        QVector<RpcRequest> requests;
        CodexThreadAccountHandoffService
            handoff(
                router,
                [&routes, &requests](
                    const CodexAccountRoute&
                        route,
                    const QVector<RpcRequest>&
                        batch,
                    std::stop_token) {
                    routes.append(route);
                    requests += batch;
                    const QString path =
                        batch.constFirst()
                            .params.value(
                                QStringLiteral(
                                    "path"))
                            .toString();
                    return Result<
                        QHash<
                            int,
                            RpcResponse>>::success(
                        {
                            {
                                batch
                                    .constFirst()
                                    .id,
                                {
                                    QJsonObject{
                                        {
                                            QStringLiteral(
                                                "thread"),
                                            QJsonObject{
                                                {
                                                    QStringLiteral(
                                                        "id"),
                                                    QStringLiteral(
                                                        "thread-1"),
                                                },
                                                {
                                                    QStringLiteral(
                                                        "path"),
                                                    path,
                                                },
                                            },
                                        },
                                    },
                                    {},
                                    false,
                                },
                            },
                        });
                });
        const QString rolloutPath =
            temporary.filePath(
                QStringLiteral(
                    "sessions/thread-1.jsonl"));

        const auto result =
            handoff.handoff(
                QStringLiteral(
                    " thread-1 "),
                rolloutPath,
                ThreadRuntimeStatus::Idle,
                kBackupProfileId);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().threadId,
            QStringLiteral("thread-1"));
        QCOMPARE(
            result.value().profileId,
            kBackupProfileId);
        QCOMPARE(routes.size(), 1);
        QCOMPARE(
            routes.constFirst().profileId,
            std::optional<QUuid>(
                kBackupProfileId));
        QVERIFY(
            routes.constFirst()
                .environment.value(
                    QStringLiteral(
                        "CODEX_HOME"))
                .endsWith(
                    QStringLiteral(
                        "a7e4430bdfd1424ca1ed26af90b520a8")));
        QCOMPARE(requests.size(), 1);
        QCOMPARE(
            requests.constFirst().method,
            QStringLiteral(
                "thread/resume"));
        QCOMPARE(
            requests.constFirst()
                .params.value(
                    QStringLiteral(
                        "threadId"))
                .toString(),
            QStringLiteral("thread-1"));
        QCOMPARE(
            QDir::cleanPath(
                requests.constFirst()
                    .params.value(
                        QStringLiteral(
                            "path"))
                    .toString()),
            QDir::cleanPath(
                QFileInfo(rolloutPath)
                    .absoluteFilePath()));
        QCOMPARE(
            bindings.profileIdFor(
                QStringLiteral("thread-1")),
            std::optional<QUuid>(
                kBackupProfileId));
    }

    void unsafeOrMismatchedHandoffPreservesExistingBinding()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVector<QUuid> generatedIds{
            kMainProfileId,
            kBackupProfileId,
        };
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [&generatedIds] {
                return generatedIds.takeFirst();
            });
        QVERIFY(
            profiles
                .add(QStringLiteral("Main"))
                .hasValue());
        QVERIFY(
            profiles
                .add(QStringLiteral("Backup"))
                .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(
            bindings.bind(
                        QStringLiteral(
                            "thread-1"),
                        kMainProfileId)
                .hasValue());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            {},
            profiles,
            runtime,
            bindings);
        int calls = 0;
        CodexThreadAccountHandoffService
            handoff(
                router,
                [&calls](
                    const CodexAccountRoute&,
                    const QVector<RpcRequest>&
                        batch,
                    std::stop_token) {
                    ++calls;
                    return Result<
                        QHash<
                            int,
                            RpcResponse>>::success(
                        {
                            {
                                batch
                                    .constFirst()
                                    .id,
                                {
                                    QJsonObject{
                                        {
                                            QStringLiteral(
                                                "thread"),
                                            QJsonObject{
                                                {
                                                    QStringLiteral(
                                                        "id"),
                                                    QStringLiteral(
                                                        "different-thread"),
                                                },
                                                {
                                                    QStringLiteral(
                                                        "path"),
                                                    batch
                                                        .constFirst()
                                                        .params.value(
                                                            QStringLiteral(
                                                                "path")),
                                                },
                                            },
                                        },
                                    },
                                    {},
                                    false,
                                },
                            },
                        });
                });
        const QString rolloutPath =
            temporary.filePath(
                QStringLiteral(
                    "sessions/thread-1.jsonl"));

        for (const ThreadRuntimeStatus
                 status : {
                     ThreadRuntimeStatus::Active,
                     ThreadRuntimeStatus::
                         WaitingOnApproval,
                     ThreadRuntimeStatus::
                         WaitingOnUserInput,
                 }) {
            const auto blocked =
                handoff.handoff(
                    QStringLiteral("thread-1"),
                    rolloutPath,
                    status,
                    kBackupProfileId);
            QVERIFY(!blocked.hasValue());
        }
        QCOMPARE(calls, 0);

        const auto mismatched =
            handoff.handoff(
                QStringLiteral("thread-1"),
                rolloutPath,
                ThreadRuntimeStatus::
                    NotLoaded,
                kBackupProfileId);
        QVERIFY(!mismatched.hasValue());
        QCOMPARE(calls, 1);
        QCOMPARE(
            bindings.profileIdFor(
                QStringLiteral("thread-1")),
            std::optional<QUuid>(
                kMainProfileId));
    }

    void terminalSystemErrorAllowsExplicitHandoff()
    {
        QVERIFY(
            CodexThreadAccountHandoffService::
                canHandoff(
                    ThreadRuntimeStatus::
                        SystemError));
        QVERIFY(
            !CodexThreadAccountHandoffService::
                 canHandoff(
                     ThreadRuntimeStatus::
                         Active));
        QVERIFY(
            !CodexThreadAccountHandoffService::
                 canHandoff(
                     ThreadRuntimeStatus::
                         WaitingOnApproval));
        QVERIFY(
            !CodexThreadAccountHandoffService::
                 canHandoff(
                     ThreadRuntimeStatus::
                         WaitingOnUserInput));
    }

    void loginCommandsUseOfficialCliAndPreserveProfileEnvironment()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("profiles")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        QProcessEnvironment base;
        base.insert(
            QStringLiteral("BASE"),
            QStringLiteral("kept"));
        QVector<CodexLoginProcessRequest>
            started;
        QVector<CodexLoginProcessRequest>
            executed;
        CodexAccountLoginService service(
            QStringLiteral(
                "C:/Codex/codex.exe"),
            base,
            runtime,
            [&started](
                const CodexLoginProcessRequest&
                    request) {
                started.append(request);
                return Result<void>::success();
            },
            [&executed](
                const CodexLoginProcessRequest&
                    request) {
                executed.append(request);
                return Result<
                    CodexLoginProcessResult>::
                    success({
                        0,
                        QByteArray(
                            "Logged in using ChatGPT"),
                        {},
                    });
            });
        const CodexAccountProfile profile{
            kMainProfileId,
            QStringLiteral("Main"),
        };

        QVERIFY(service.beginLogin(profile)
                    .hasValue());
        const auto status =
            service.readStatus(profile);

        QVERIFY(status.hasValue());
        QCOMPARE(
            status.value().state,
            CodexAccountAuthenticationState::
                SignedIn);
        QCOMPARE(
            status.value().message,
            QStringLiteral(
                "Logged in using ChatGPT"));
        QCOMPARE(started.size(), 1);
        QCOMPARE(executed.size(), 1);
        QCOMPARE(
            started.constFirst().program,
            QStringLiteral(
                "C:/Codex/codex.exe"));
        const QStringList
            expectedLoginArguments{
                QStringLiteral("login"),
            };
        const QStringList
            expectedStatusArguments{
                QStringLiteral("login"),
                QStringLiteral("status"),
            };
        QCOMPARE(
            started.constFirst().arguments,
            expectedLoginArguments);
        QVERIFY(started.constFirst()
                    .interactive);
        QVERIFY(
            started.constFirst()
                .createVisibleConsole);
        QCOMPARE(
            executed.constFirst()
                .arguments,
            expectedStatusArguments);
        QVERIFY(!executed.constFirst()
                     .interactive);
        QVERIFY(
            !executed.constFirst()
                 .createVisibleConsole);
        QCOMPARE(
            started.constFirst()
                .environment.value(
                    QStringLiteral(
                        "CODEX_HOME")),
            QDir::cleanPath(
                temporary.filePath(
                    QStringLiteral(
                        "profiles/"
                        "5774a03ccb0548099054464f3e25d6a0"))));
        QCOMPARE(
            executed.constFirst()
                .environment.value(
                    QStringLiteral(
                        "CODEX_SQLITE_HOME")),
            QDir::cleanPath(
                temporary.filePath(
                    QStringLiteral(
                        "shared-codex"))));
        QCOMPARE(
            executed.constFirst()
                .environment.value(
                    QStringLiteral("BASE")),
            QStringLiteral("kept"));
    }

    void currentAccountLoginUsesUnmodifiedBaseEnvironment()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("profiles")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        QProcessEnvironment base;
        base.insert(
            QStringLiteral("CURRENT_ACCOUNT"),
            QStringLiteral("native"));
        QVector<CodexLoginProcessRequest>
            requests;
        CodexAccountLoginService service(
            QStringLiteral("codex.exe"),
            base,
            runtime,
            [&requests](
                const CodexLoginProcessRequest&
                    request) {
                requests.append(request);
                return Result<void>::success();
            },
            [&requests](
                const CodexLoginProcessRequest&
                    request) {
                requests.append(request);
                return Result<
                    CodexLoginProcessResult>::
                    success({
                        0,
                        QByteArray(
                            "Logged in using ChatGPT"),
                        {},
                    });
            });

        QVERIFY(service
                    .beginLoginCurrentAccount()
                    .hasValue());
        const auto status =
            service
                .readCurrentAccountStatus();

        QVERIFY(status.hasValue());
        QCOMPARE(requests.size(), 2);
        for (const auto& request : requests) {
            QCOMPARE(
                request.environment.value(
                    QStringLiteral(
                        "CURRENT_ACCOUNT")),
                QStringLiteral("native"));
            QVERIFY(!request.environment
                         .contains(
                             QStringLiteral(
                                 "CODEX_HOME")));
            QVERIFY(!request.environment
                         .contains(
                             QStringLiteral(
                                 "CODEX_SQLITE_HOME")));
        }
    }

    void loginPreparesProfileHomeBeforeLaunchingOfficialCli()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("profiles")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        bool observedPreparedHome = false;
        CodexAccountLoginService service(
            QStringLiteral(
                "C:/Codex/codex.exe"),
            {},
            runtime,
            [&observedPreparedHome](
                const CodexLoginProcessRequest&
                    request) {
                observedPreparedHome =
                    QFileInfo(
                        request.environment.value(
                            QStringLiteral(
                                "CODEX_HOME")))
                        .isDir();
                if (!observedPreparedHome) {
                    return Result<void>::failure({
                        QStringLiteral(
                            "codex.synthetic-home-missing"),
                        QStringLiteral(
                            "The profile home was not prepared before login."),
                        false,
                        {},
                    });
                }
                return Result<void>::success();
            });
        const CodexAccountProfile profile{
            kMainProfileId,
            QStringLiteral("Main"),
        };

        const auto result =
            service.beginLogin(profile);

        QVERIFY(result.hasValue());
        QVERIFY(observedPreparedHome);
    }

    void loginStatusDoesNotSurfaceCredentialLikeOutput()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("profiles")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountLoginService signedIn(
            QStringLiteral(
                "C:/Codex/codex.exe"),
            {},
            runtime,
            {},
            [](
                const CodexLoginProcessRequest&) {
                return Result<
                    CodexLoginProcessResult>::
                    success({
                        0,
                        QByteArray(
                            "Bearer token diagnostic"),
                        {},
                    });
            });
        CodexAccountLoginService signedOut(
            QStringLiteral(
                "C:/Codex/codex.exe"),
            {},
            runtime,
            {},
            [](
                const CodexLoginProcessRequest&) {
                return Result<
                    CodexLoginProcessResult>::
                    success({
                        1,
                        {},
                        QByteArray(
                            "authorization cookie unavailable"),
                    });
            });
        const CodexAccountProfile profile{
            kMainProfileId,
            QStringLiteral("Main"),
        };

        const auto signedInStatus =
            signedIn.readStatus(profile);
        const auto signedOutStatus =
            signedOut.readStatus(profile);

        QVERIFY(signedInStatus.hasValue());
        QCOMPARE(
            signedInStatus.value().message,
            QStringLiteral(
                "Signed in with the official Codex CLI."));
        QVERIFY(signedOutStatus.hasValue());
        QCOMPARE(
            signedOutStatus.value().state,
            CodexAccountAuthenticationState::
                SignedOut);
        QCOMPARE(
            signedOutStatus.value().message,
            QStringLiteral(
                "Codex sign-in status is unavailable."));
    }

    void missingBoundProfileFallsBackToBaseInsteadOfCurrentSelection()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [] { return kBackupProfileId; });
        QVERIFY(profiles.add(
                            QStringLiteral("Backup"))
                    .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(bindings.bind(
                             QStringLiteral(
                                 "orphan-thread"),
                             kMainProfileId)
                    .hasValue());
        QProcessEnvironment base;
        base.insert(
            QStringLiteral("BASE_ONLY"),
            QStringLiteral("yes"));
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            base,
            profiles,
            runtime,
            bindings);

        const CodexAccountRoute route =
            router.routeThread(
                QStringLiteral(
                    "orphan-thread"));

        QVERIFY(!route.profileId.has_value());
        QCOMPARE(
            route.environment.value(
                QStringLiteral("BASE_ONLY")),
            QStringLiteral("yes"));
        QVERIFY(!route.environment.contains(
            QStringLiteral("CODEX_HOME")));
    }

    void profileRemovalIsRejectedWhileBindingsExist()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CodexAccountProfileStore profiles(
            temporary.filePath(
                QStringLiteral("profiles.json")),
            [] { return kMainProfileId; });
        QVERIFY(profiles.add(
                            QStringLiteral("Main"))
                    .hasValue());
        CodexThreadAccountBindingStore bindings(
            temporary.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(bindings.bind(
                             QStringLiteral("thread-1"),
                             kMainProfileId)
                    .hasValue());
        CodexAccountRuntime runtime(
            temporary.filePath(
                QStringLiteral("homes")),
            temporary.filePath(
                QStringLiteral("shared-codex")));
        CodexAccountRouter router(
            {},
            profiles,
            runtime,
            bindings);

        const auto blocked =
            router.removeProfile(
                kMainProfileId);

        QVERIFY(!blocked.hasValue());
        QCOMPARE(
            blocked.error().code,
            QStringLiteral(
                "codex.account_profile_in_use"));
        QVERIFY(profiles.profile(
                              kMainProfileId)
                    .has_value());

        QVERIFY(bindings.remove(
                             QStringLiteral("thread-1"))
                    .hasValue());
        const auto removed =
            router.removeProfile(
                kMainProfileId);
        QVERIFY(removed.hasValue());
        QVERIFY(removed.value());
        QVERIFY(!profiles.profile(
                               kMainProfileId)
                     .has_value());
    }

    void malformedStoresFailClosedWithoutOverwritingTheFiles()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profilesPath =
            temporary.filePath(
                QStringLiteral("profiles.json"));
        const QString bindingsPath =
            temporary.filePath(
                QStringLiteral("bindings.json"));
        {
            QFile profilesFile(profilesPath);
            QVERIFY(profilesFile.open(
                QIODevice::WriteOnly));
            QCOMPARE(
                profilesFile.write("{invalid"),
                qint64(8));
        }
        {
            QFile bindingsFile(bindingsPath);
            QVERIFY(bindingsFile.open(
                QIODevice::WriteOnly));
            QCOMPARE(
                bindingsFile.write("[1,2,3]"),
                qint64(7));
        }

        CodexAccountProfileStore profiles(
            profilesPath);
        CodexThreadAccountBindingStore bindings(
            bindingsPath);

        QVERIFY(profiles.loadError().has_value());
        QVERIFY(bindings.loadError().has_value());
        QCOMPARE(
            readText(profilesPath),
            QStringLiteral("{invalid"));
        QCOMPARE(
            readText(bindingsPath),
            QStringLiteral("[1,2,3]"));
    }
};

QTEST_GUILESS_MAIN(
    CodexAccountProfileTests)

#include "CodexAccountProfileTests.moc"
