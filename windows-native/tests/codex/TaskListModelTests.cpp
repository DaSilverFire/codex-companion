#include "codex/runtime/TaskListModel.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QAbstractItemModel>
#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaProperty>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

using namespace companion;

template <typename Type, typename = void>
struct HasProductionCommandBusTestFactory
    : std::false_type {
};

template <typename Type>
struct HasProductionCommandBusTestFactory<
    Type,
    std::void_t<decltype(
        Type::create(
            std::declval<
                detail::CommandBusDeliveryHook>(),
            static_cast<QObject*>(nullptr)))>>
    : std::true_type {
};

static_assert(
    !HasProductionCommandBusTestFactory<
        detail::CompanionCommandBusTestAccess>::
        value,
    "The production command-bus header must not expose "
    "a callable test-access factory.");

namespace companion::detail {

struct CompanionCommandBusTestAccess final {
    static std::unique_ptr<CompanionCommandBus> create(
        CommandBusDeliveryHook deliveryHook,
        QObject* parent = nullptr)
    {
        return std::unique_ptr<CompanionCommandBus>(
            new CompanionCommandBus(
                std::move(deliveryHook),
                parent));
    }

    static Result<void> registerHandlerTransaction(
        CompanionCommandBus& bus,
        const QString& command,
        CompanionCommandBus::Handler handler,
        quint64& registrationId)
    {
        return bus.registerHandlerTransaction(
            command,
            std::move(handler),
            registrationId);
    }

    static void rollbackHandlerRegistration(
        CompanionCommandBus& bus,
        const QString& command,
        quint64 registrationId)
    {
        bus.rollbackHandlerRegistration(
            command,
            registrationId);
    }

    static bool commitHandlerRegistration(
        CompanionCommandBus& bus,
        const QString& command,
        quint64 registrationId)
    {
        return bus.commitHandlerRegistration(
            command,
            registrationId);
    }
};

} // namespace companion::detail

namespace {

BridgeTask task(QString id, QString title = {})
{
    BridgeTask result;
    result.id = std::move(id);
    result.title = title.isNull()
        ? QStringLiteral("Title ") + result.id
        : std::move(title);
    result.preview = QStringLiteral("Preview ") + result.id;
    result.updatedAt.secondsSinceReferenceDate = 10.5;
    result.status = TaskStatus::Waiting;
    return result;
}

BridgeTask richTask(QString id)
{
    BridgeTask result = task(std::move(id));
    result.cwd = QStringLiteral("C:/work/project");
    result.status = TaskStatus::Running;
    result.needsApproval = true;
    result.activeTurnId = QStringLiteral("turn-1");
    result.model = QStringLiteral("gpt-5");
    result.reasoningEffort = QStringLiteral("high");
    result.taskGroup = BridgeTaskGroup{
        TaskGroupKind::Project,
        QStringLiteral("Project"),
        QStringLiteral("C:/work/project"),
    };
    result.goal = BridgeGoal{
        result.id,
        QStringLiteral("Ship the boundary"),
        GoalStatus::Active,
        50000,
        1200,
        45,
        100,
        200,
    };
    return result;
}

QStringList modelIds(const TaskListModel& model)
{
    QStringList ids;
    ids.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row) {
        ids.append(
            model.data(
                     model.index(row, 0),
                     TaskListModel::IdRole)
                .toString());
    }
    return ids;
}

QVector<BridgeTask> tasksForIds(const QStringList& ids)
{
    QVector<BridgeTask> tasks;
    tasks.reserve(ids.size());
    for (const QString& id : ids) {
        tasks.append(task(id));
    }
    return tasks;
}

void appendUniqueSequences(
    const QStringList& remaining,
    const QStringList& current,
    QVector<QStringList>& sequences)
{
    sequences.append(current);
    for (const QString& id : remaining) {
        QStringList nextRemaining = remaining;
        nextRemaining.removeOne(id);
        QStringList next = current;
        next.append(id);
        appendUniqueSequences(
            nextRemaining,
            next,
            sequences);
    }
}

QVector<QStringList> allUniqueSequences()
{
    QVector<QStringList> sequences;
    appendUniqueSequences(
        {
            QStringLiteral("a"),
            QStringLiteral("b"),
            QStringLiteral("c"),
            QStringLiteral("d"),
        },
        {},
        sequences);
    return sequences;
}

QString roleListText(const QList<int>& roles)
{
    QStringList values;
    values.reserve(roles.size());
    for (const int role : roles) {
        values.append(QString::number(role));
    }
    return values.join(QStringLiteral(","));
}

struct DataChange final {
    int firstRow = -1;
    int lastRow = -1;
    QList<int> roles;
};

struct MoveCheck final {
    int sourceStart = -1;
    int sourceEnd = -1;
    int destinationRow = -1;
    QStringList expectedAfter;
    QHash<QString, QPersistentModelIndex>
        persistentBefore;
    QStringList errors;
    bool completed = false;
};

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

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()
           && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(
        QEventLoop::AllEvents,
        10);
    return predicate();
}

CompanionCommandBus::Handler successHandler()
{
    return [](
               const QVariantMap&,
               CompanionCommandBus::Completion completion) {
        completion(Result<void>::success());
    };
}

std::unique_ptr<CompanionCommandBus>
commandBusWithDeliveryHook(
    detail::CommandBusDeliveryHook hook)
{
    return detail::CompanionCommandBusTestAccess::
        create(std::move(hook));
}

} // namespace

class TaskListModelTests final : public QObject {
    Q_OBJECT

private slots:
    void roleNamesAndIdsAreExact()
    {
        TaskListModel model;
        const QVector<QPair<int, QByteArray>> expected{
            {Qt::UserRole + 1, QByteArrayLiteral("id")},
            {Qt::UserRole + 2, QByteArrayLiteral("title")},
            {Qt::UserRole + 3, QByteArrayLiteral("preview")},
            {Qt::UserRole + 4, QByteArrayLiteral("updatedAt")},
            {Qt::UserRole + 5, QByteArrayLiteral("cwd")},
            {Qt::UserRole + 6, QByteArrayLiteral("status")},
            {Qt::UserRole + 7, QByteArrayLiteral("needsApproval")},
            {Qt::UserRole + 8, QByteArrayLiteral("activeTurnId")},
            {Qt::UserRole + 9, QByteArrayLiteral("model")},
            {Qt::UserRole + 10, QByteArrayLiteral("reasoningEffort")},
            {Qt::UserRole + 11, QByteArrayLiteral("groupKind")},
            {Qt::UserRole + 12, QByteArrayLiteral("groupTitle")},
            {Qt::UserRole + 13, QByteArrayLiteral("goal")},
            {Qt::UserRole + 14, QByteArrayLiteral("processId")},
        };

        const QHash<int, QByteArray> actual = model.roleNames();

        QCOMPARE(actual.size(), expected.size());
        for (const auto& [role, name] : expected) {
            QCOMPARE(actual.value(role), name);
        }
        QCOMPARE(
            static_cast<int>(TaskListModel::IdRole),
            Qt::UserRole + 1);
        QCOMPARE(
            static_cast<int>(TaskListModel::GoalRole),
            Qt::UserRole + 13);
        QCOMPARE(
            static_cast<int>(TaskListModel::ProcessIdRole),
            Qt::UserRole + 14);

        model.setSnapshot({
            task(QStringLiteral("thread-process")),
        });
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     TaskListModel::ProcessIdRole)
                .toString(),
            QStringLiteral("thread-process"));
    }

    void initialInsertionPreservesSuppliedOrder()
    {
        TaskListModel model;
        int resetCount = 0;
        int insertedRows = 0;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&insertedRows](
                const QModelIndex&,
                int first,
                int last) {
                insertedRows += last - first + 1;
            });

        const QVector<BridgeTask> supplied{
            task(QStringLiteral("third")),
            task(QStringLiteral("first")),
            task(QStringLiteral("second")),
        };
        model.setSnapshot(supplied);

        QCOMPARE(resetCount, 0);
        QCOMPARE(insertedRows, 3);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.snapshot(), supplied);
        const QStringList expectedIds{
            QStringLiteral("third"),
            QStringLiteral("first"),
            QStringLiteral("second"),
        };
        QCOMPARE(modelIds(model), expectedIds);
    }

    void snapshotChangedPublishesOnceAfterEachCompleteUpdate()
    {
        TaskListModel model;
        QSignalSpy snapshotSpy(
            &model,
            &TaskListModel::snapshotChanged);
        QVERIFY(snapshotSpy.isValid());

        model.setSnapshot({
            task(QStringLiteral("first")),
            task(QStringLiteral("second")),
        });
        QCOMPARE(snapshotSpy.count(), 1);

        QVector<BridgeTask> updated =
            model.snapshot();
        updated[0].title =
            QStringLiteral("Updated");
        model.setSnapshot(updated);

        QCOMPARE(snapshotSpy.count(), 2);
        QCOMPARE(
            model.snapshot().at(0).title,
            QStringLiteral("Updated"));
    }

    void stableIdUpdateDoesNotResetUnchangedRows()
    {
        TaskListModel model;
        QVector<BridgeTask> initial{
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
        };
        model.setSnapshot(initial);

        int resetCount = 0;
        int insertedRows = 0;
        int removedRows = 0;
        int movedRows = 0;
        QVector<DataChange> changes;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&insertedRows](
                const QModelIndex&,
                int first,
                int last) {
                insertedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsRemoved,
            &model,
            [&removedRows](
                const QModelIndex&,
                int first,
                int last) {
                removedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsMoved,
            &model,
            [&movedRows](
                const QModelIndex&,
                int first,
                int last,
                const QModelIndex&,
                int) {
                movedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::dataChanged,
            &model,
            [&changes](
                const QModelIndex& first,
                const QModelIndex& last,
                const QList<int>& roles) {
                changes.append(
                    {first.row(), last.row(), roles});
            });

        QVector<BridgeTask> updated = initial;
        updated[1].title = QStringLiteral("Updated B");
        model.setSnapshot(updated);

        QCOMPARE(resetCount, 0);
        QCOMPARE(insertedRows, 0);
        QCOMPARE(removedRows, 0);
        QCOMPARE(movedRows, 0);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().firstRow, 1);
        QCOMPARE(changes.first().lastRow, 1);
        QCOMPARE(
            changes.first().roles,
            QList<int>{TaskListModel::TitleRole});
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     TaskListModel::TitleRole)
                .toString(),
            initial[0].title);
        QCOMPARE(
            model.data(
                     model.index(1, 0),
                     TaskListModel::TitleRole)
                .toString(),
            updated[1].title);
    }

    void changedRowsEmitOnlyChangedRoles()
    {
        TaskListModel model;
        QVector<BridgeTask> initial{
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
        };
        model.setSnapshot(initial);

        QVector<DataChange> changes;
        QObject::connect(
            &model,
            &QAbstractItemModel::dataChanged,
            &model,
            [&changes](
                const QModelIndex& first,
                const QModelIndex& last,
                const QList<int>& roles) {
                changes.append(
                    {first.row(), last.row(), roles});
            });

        QVector<BridgeTask> updated = initial;
        updated[1].preview = QStringLiteral("Fresh preview");
        updated[1].model = QStringLiteral("gpt-5.6");
        updated[1].goal = BridgeGoal{
            QStringLiteral("b"),
            QStringLiteral("Ship Task 12A"),
            GoalStatus::Active,
            std::nullopt,
            120,
            30,
            400,
            500,
        };
        model.setSnapshot(updated);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().firstRow, 1);
        QCOMPARE(changes.first().lastRow, 1);
        const QList<int> expectedRoles{
            TaskListModel::PreviewRole,
            TaskListModel::ModelRole,
            TaskListModel::GoalRole,
        };
        QCOMPARE(changes.first().roles, expectedRoles);

        model.setSnapshot(updated);
        QCOMPARE(changes.size(), 1);
    }

    void everyMutableRoleEmitsOnlyItsExactChange()
    {
        struct RoleChangeCase final {
            QString name;
            BridgeTask initial;
            BridgeTask updated;
            QList<int> expectedRoles;
        };

        QVector<RoleChangeCase> cases;
        const BridgeTask baseline =
            richTask(QStringLiteral("role-task"));
        const auto addCase =
            [&cases](
                QString name,
                BridgeTask initial,
                BridgeTask updated,
                QList<int> expectedRoles) {
                cases.append({
                    std::move(name),
                    std::move(initial),
                    std::move(updated),
                    std::move(expectedRoles),
                });
            };

        {
            BridgeTask updated = baseline;
            updated.title = QStringLiteral("Changed title");
            addCase(
                QStringLiteral("title"),
                baseline,
                updated,
                {TaskListModel::TitleRole});
        }
        {
            BridgeTask updated = baseline;
            updated.preview =
                QStringLiteral("Changed preview");
            addCase(
                QStringLiteral("preview"),
                baseline,
                updated,
                {TaskListModel::PreviewRole});
        }
        {
            BridgeTask updated = baseline;
            updated.updatedAt.secondsSinceReferenceDate =
                123.25;
            addCase(
                QStringLiteral("updatedAt"),
                baseline,
                updated,
                {TaskListModel::UpdatedAtRole});
        }
        {
            BridgeTask updated = baseline;
            updated.cwd = QStringLiteral("C:/other");
            addCase(
                QStringLiteral("cwd value"),
                baseline,
                updated,
                {TaskListModel::CwdRole});
        }
        {
            BridgeTask updated = baseline;
            updated.status = TaskStatus::Completed;
            addCase(
                QStringLiteral("status"),
                baseline,
                updated,
                {TaskListModel::StatusRole});
        }
        {
            BridgeTask updated = baseline;
            updated.needsApproval = false;
            addCase(
                QStringLiteral("needsApproval"),
                baseline,
                updated,
                {TaskListModel::NeedsApprovalRole});
        }
        {
            BridgeTask updated = baseline;
            updated.activeTurnId =
                QStringLiteral("turn-2");
            addCase(
                QStringLiteral("activeTurnId value"),
                baseline,
                updated,
                {TaskListModel::ActiveTurnIdRole});
        }
        {
            BridgeTask updated = baseline;
            updated.model = QStringLiteral("gpt-5.6");
            addCase(
                QStringLiteral("model value"),
                baseline,
                updated,
                {TaskListModel::ModelRole});
        }
        {
            BridgeTask updated = baseline;
            updated.reasoningEffort =
                QStringLiteral("medium");
            addCase(
                QStringLiteral("reasoningEffort value"),
                baseline,
                updated,
                {TaskListModel::ReasoningEffortRole});
        }
        {
            BridgeTask updated = baseline;
            updated.taskGroup->kind =
                TaskGroupKind::Chats;
            addCase(
                QStringLiteral("groupKind"),
                baseline,
                updated,
                {TaskListModel::GroupKindRole});
        }
        {
            BridgeTask updated = baseline;
            updated.taskGroup->title =
                QStringLiteral("Changed group");
            addCase(
                QStringLiteral("groupTitle"),
                baseline,
                updated,
                {TaskListModel::GroupTitleRole});
        }
        {
            BridgeTask updated = baseline;
            updated.goal->objective =
                QStringLiteral("Changed objective");
            addCase(
                QStringLiteral("goal value"),
                baseline,
                updated,
                {TaskListModel::GoalRole});
        }

        const auto addOptionalTransitions =
            [&addCase, &baseline](
                const QString& name,
                const std::function<void(BridgeTask&)>& clear,
                const std::function<void(BridgeTask&)>& set,
                int role) {
                BridgeTask absent = baseline;
                clear(absent);
                BridgeTask present = absent;
                set(present);
                addCase(
                    name
                        + QStringLiteral(
                            " absent-to-present"),
                    absent,
                    present,
                    {role});
                addCase(
                    name
                        + QStringLiteral(
                            " present-to-absent"),
                    present,
                    absent,
                    {role});
            };
        addOptionalTransitions(
            QStringLiteral("cwd"),
            [](BridgeTask& item) {
                item.cwd.reset();
            },
            [](BridgeTask& item) {
                item.cwd =
                    QStringLiteral("C:/present");
            },
            TaskListModel::CwdRole);
        addOptionalTransitions(
            QStringLiteral("activeTurnId"),
            [](BridgeTask& item) {
                item.activeTurnId.reset();
            },
            [](BridgeTask& item) {
                item.activeTurnId =
                    QStringLiteral("turn-present");
            },
            TaskListModel::ActiveTurnIdRole);
        addOptionalTransitions(
            QStringLiteral("model"),
            [](BridgeTask& item) {
                item.model.reset();
            },
            [](BridgeTask& item) {
                item.model =
                    QStringLiteral("model-present");
            },
            TaskListModel::ModelRole);
        addOptionalTransitions(
            QStringLiteral("reasoningEffort"),
            [](BridgeTask& item) {
                item.reasoningEffort.reset();
            },
            [](BridgeTask& item) {
                item.reasoningEffort =
                    QStringLiteral("low");
            },
            TaskListModel::ReasoningEffortRole);

        {
            BridgeTask absent = baseline;
            absent.taskGroup.reset();
            addCase(
                QStringLiteral(
                    "group absent-to-present"),
                absent,
                baseline,
                {
                    TaskListModel::GroupKindRole,
                    TaskListModel::GroupTitleRole,
                });
            addCase(
                QStringLiteral(
                    "group present-to-absent"),
                baseline,
                absent,
                {
                    TaskListModel::GroupKindRole,
                    TaskListModel::GroupTitleRole,
                });
        }
        {
            BridgeTask absent = baseline;
            absent.goal.reset();
            addCase(
                QStringLiteral(
                    "goal absent-to-present"),
                absent,
                baseline,
                {TaskListModel::GoalRole});
            addCase(
                QStringLiteral(
                    "goal present-to-absent"),
                baseline,
                absent,
                {TaskListModel::GoalRole});
        }
        addCase(
            QStringLiteral("unchanged"),
            baseline,
            baseline,
            {});
        {
            BridgeTask updated = baseline;
            updated.taskGroup->path =
                QStringLiteral("C:/unexposed-path");
            addCase(
                QStringLiteral(
                    "unexposed group path"),
                baseline,
                updated,
                {});
        }

        for (const RoleChangeCase& testCase : cases) {
            TaskListModel model;
            model.setSnapshot({testCase.initial});

            int resetCount = 0;
            QVector<DataChange> changes;
            QObject::connect(
                &model,
                &QAbstractItemModel::modelReset,
                &model,
                [&resetCount] {
                    ++resetCount;
                });
            QObject::connect(
                &model,
                &QAbstractItemModel::dataChanged,
                &model,
                [&changes](
                    const QModelIndex& first,
                    const QModelIndex& last,
                    const QList<int>& roles) {
                    changes.append({
                        first.row(),
                        last.row(),
                        roles,
                    });
                });

            model.setSnapshot({testCase.updated});

            const QString context =
                QStringLiteral("role case '%1'")
                    .arg(testCase.name);
            const QVector<BridgeTask> expectedSnapshot{
                testCase.updated,
            };
            QVERIFY2(
                resetCount == 0,
                qPrintable(
                    context
                    + QStringLiteral(
                        " unexpectedly reset")));
            QVERIFY2(
                model.snapshot() == expectedSnapshot,
                qPrintable(
                    context
                    + QStringLiteral(
                        " did not retain the update")));
            if (testCase.expectedRoles.isEmpty()) {
                QVERIFY2(
                    changes.isEmpty(),
                    qPrintable(
                        context
                        + QStringLiteral(
                            " emitted roles ")
                        + roleListText(
                            changes.isEmpty()
                                ? QList<int>{}
                                : changes.first().roles)));
                continue;
            }

            QVERIFY2(
                changes.size() == 1,
                qPrintable(
                    context
                    + QStringLiteral(
                        " emitted %1 dataChanged signals")
                          .arg(changes.size())));
            const DataChange& change = changes.first();
            QVERIFY2(
                change.firstRow == 0
                    && change.lastRow == 0,
                qPrintable(
                    context
                    + QStringLiteral(
                        " changed the wrong rows")));
            QVERIFY2(
                change.roles
                    == testCase.expectedRoles,
                qPrintable(
                    context
                    + QStringLiteral(
                        " expected roles [%1], got [%2]")
                          .arg(
                              roleListText(
                                  testCase.expectedRoles),
                              roleListText(
                                  change.roles))));
        }
    }

    void exhaustiveUniqueSnapshotTransitionsAreModelValid()
    {
        const QVector<QStringList> sequences =
            allUniqueSequences();
        QCOMPARE(sequences.size(), 65);

        for (const QStringList& initialIds : sequences) {
            for (const QStringList& targetIds : sequences) {
                TaskListModel model;
                QAbstractItemModelTester tester(
                    &model,
                    QAbstractItemModelTester::
                        FailureReportingMode::QtTest);
                tester.setUseFetchMore(false);

                const QVector<BridgeTask> initial =
                    tasksForIds(initialIds);
                const QVector<BridgeTask> target =
                    tasksForIds(targetIds);
                model.setSnapshot(initial);

                QHash<QString, QPersistentModelIndex>
                    persistentIndexes;
                for (int row = 0;
                     row < initialIds.size();
                     ++row) {
                    persistentIndexes.insert(
                        initialIds.at(row),
                        QPersistentModelIndex(
                            model.index(row, 0)));
                }

                int resetCount = 0;
                QVector<MoveCheck> moveChecks;
                QString unexpectedMoveCompletion;
                QObject::connect(
                    &model,
                    &QAbstractItemModel::modelReset,
                    &model,
                    [&resetCount] {
                        ++resetCount;
                    });
                QObject::connect(
                    &model,
                    &QAbstractItemModel::
                        rowsAboutToBeMoved,
                    &model,
                    [&model, &moveChecks](
                        const QModelIndex& sourceParent,
                        int sourceStart,
                        int sourceEnd,
                        const QModelIndex&
                            destinationParent,
                        int destinationRow) {
                        MoveCheck check;
                        check.sourceStart = sourceStart;
                        check.sourceEnd = sourceEnd;
                        check.destinationRow =
                            destinationRow;

                        const int beforeCount =
                            model.rowCount();
                        for (int row = 0;
                             row < beforeCount;
                             ++row) {
                            const QModelIndex index =
                                model.index(row, 0);
                            check.persistentBefore.insert(
                                index
                                    .data(
                                        TaskListModel::
                                            IdRole)
                                    .toString(),
                                QPersistentModelIndex(
                                    index));
                        }
                        if (sourceParent.isValid()
                            || destinationParent.isValid()) {
                            check.errors.append(
                                QStringLiteral(
                                    "move parent was valid"));
                        }
                        if (sourceStart < 0
                            || sourceEnd < sourceStart
                            || sourceEnd >= beforeCount) {
                            check.errors.append(
                                QStringLiteral(
                                    "source range was invalid"));
                        }
                        if (destinationRow < 0
                            || destinationRow
                                > beforeCount) {
                            check.errors.append(
                                QStringLiteral(
                                    "destination was out of range"));
                        }
                        if (destinationRow
                                >= sourceStart
                            && destinationRow
                                <= sourceEnd + 1) {
                            check.errors.append(
                                QStringLiteral(
                                    "destination intersected source"));
                        }
                        if (sourceStart != sourceEnd) {
                            check.errors.append(
                                QStringLiteral(
                                    "algorithm moved multiple rows"));
                        }
                        if (destinationRow
                            >= sourceStart) {
                            check.errors.append(
                                QStringLiteral(
                                    "algorithm did not move upward"));
                        }

                        if (sourceStart >= 0
                            && sourceEnd >= sourceStart
                            && sourceEnd < beforeCount
                            && destinationRow >= 0
                            && destinationRow
                                <= beforeCount
                            && !(destinationRow
                                     >= sourceStart
                                 && destinationRow
                                     <= sourceEnd + 1)) {
                            QStringList expected =
                                modelIds(model);
                            QStringList moved;
                            for (int row = sourceStart;
                                 row <= sourceEnd;
                                 ++row) {
                                moved.append(
                                    expected.at(row));
                            }
                            for (int row = sourceEnd;
                                 row >= sourceStart;
                                 --row) {
                                expected.removeAt(row);
                            }
                            int insertionRow =
                                destinationRow;
                            if (destinationRow
                                > sourceEnd) {
                                insertionRow -=
                                    moved.size();
                            }
                            for (int row = 0;
                                 row < moved.size();
                                 ++row) {
                                expected.insert(
                                    insertionRow + row,
                                    moved.at(row));
                            }
                            check.expectedAfter =
                                std::move(expected);
                        }
                        moveChecks.append(
                            std::move(check));
                    });
                QObject::connect(
                    &model,
                    &QAbstractItemModel::rowsMoved,
                    &model,
                    [&model,
                     &moveChecks,
                     &unexpectedMoveCompletion](
                        const QModelIndex& sourceParent,
                        int sourceStart,
                        int sourceEnd,
                        const QModelIndex&
                            destinationParent,
                        int destinationRow) {
                        if (moveChecks.isEmpty()
                            || moveChecks.last()
                                   .completed) {
                            unexpectedMoveCompletion =
                                QStringLiteral(
                                    "rowsMoved had no pending move");
                            return;
                        }
                        MoveCheck& check =
                            moveChecks.last();
                        if (sourceParent.isValid()
                            || destinationParent.isValid()
                            || sourceStart
                                != check.sourceStart
                            || sourceEnd
                                != check.sourceEnd
                            || destinationRow
                                != check.destinationRow) {
                            check.errors.append(
                                QStringLiteral(
                                    "rowsMoved arguments changed"));
                        }
                        if (modelIds(model)
                            != check.expectedAfter) {
                            check.errors.append(
                                QStringLiteral(
                                    "Qt destination semantics disagreed with storage"));
                        }
                        for (int row = 0;
                             row
                             < check.expectedAfter.size();
                             ++row) {
                            const QString& id =
                                check.expectedAfter.at(
                                    row);
                            const QPersistentModelIndex
                                persistent =
                                    check.persistentBefore
                                        .value(id);
                            if (!persistent.isValid()
                                || persistent.row() != row
                                || persistent
                                        .data(
                                            TaskListModel::
                                                IdRole)
                                        .toString()
                                    != id) {
                                check.errors.append(
                                    QStringLiteral(
                                        "Qt remapped a moved persistent index incorrectly"));
                                break;
                            }
                        }
                        check.completed = true;
                    });

                model.setSnapshot(target);

                const QString context =
                    QStringLiteral("[%1] -> [%2]")
                        .arg(
                            initialIds.join(
                                QStringLiteral(",")),
                            targetIds.join(
                                QStringLiteral(",")));
                QVERIFY2(
                    resetCount == 0,
                    qPrintable(
                        context
                        + QStringLiteral(
                            " unexpectedly reset")));
                QVERIFY2(
                    unexpectedMoveCompletion.isEmpty(),
                    qPrintable(
                        context
                        + QStringLiteral(": ")
                        + unexpectedMoveCompletion));
                for (const MoveCheck& check :
                     moveChecks) {
                    QVERIFY2(
                        check.completed,
                        qPrintable(
                            context
                            + QStringLiteral(
                                " did not complete a move")));
                    QVERIFY2(
                        check.errors.isEmpty(),
                        qPrintable(
                            context
                            + QStringLiteral(": ")
                            + check.errors.join(
                                QStringLiteral("; "))));
                }
                QVERIFY2(
                    model.rowCount()
                        == targetIds.size(),
                    qPrintable(
                        context
                        + QStringLiteral(
                            " had the wrong row count")));
                QVERIFY2(
                    model.snapshot() == target,
                    qPrintable(
                        context
                        + QStringLiteral(
                            " had the wrong snapshot")));
                QVERIFY2(
                    modelIds(model) == targetIds,
                    qPrintable(
                        context
                        + QStringLiteral(
                            " had the wrong row order")));

                for (const QString& id :
                     initialIds) {
                    const QPersistentModelIndex
                        persistent =
                            persistentIndexes.value(id);
                    const int targetRow =
                        targetIds.indexOf(id);
                    if (targetRow < 0) {
                        QVERIFY2(
                            !persistent.isValid(),
                            qPrintable(
                                context
                                + QStringLiteral(
                                    " retained removed index ")
                                + id));
                        continue;
                    }
                    QVERIFY2(
                        persistent.isValid(),
                        qPrintable(
                            context
                            + QStringLiteral(
                                " invalidated retained index ")
                            + id));
                    QVERIFY2(
                        persistent.row() == targetRow,
                        qPrintable(
                            context
                            + QStringLiteral(
                                " placed retained index ")
                            + id
                            + QStringLiteral(
                                " on the wrong row")));
                    QVERIFY2(
                        persistent
                                .data(
                                    TaskListModel::
                                        IdRole)
                                .toString()
                            == id,
                        qPrintable(
                            context
                            + QStringLiteral(
                                " changed retained index identity ")
                            + id));
                }
            }
        }
    }

    void removalInsertionAndMoveShareOneIncrementalTransition()
    {
        TaskListModel model;
        model.setSnapshot({
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
            task(QStringLiteral("c")),
            task(QStringLiteral("d")),
        });

        int resetCount = 0;
        int insertedRows = 0;
        int removedRows = 0;
        int movedRows = 0;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&insertedRows](
                const QModelIndex&,
                int first,
                int last) {
                insertedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsRemoved,
            &model,
            [&removedRows](
                const QModelIndex&,
                int first,
                int last) {
                removedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsMoved,
            &model,
            [&movedRows](
                const QModelIndex&,
                int first,
                int last,
                const QModelIndex&,
                int) {
                movedRows += last - first + 1;
            });

        BridgeTask updatedB = task(QStringLiteral("b"));
        updatedB.preview = QStringLiteral("Updated B");
        const QVector<BridgeTask> supplied{
            task(QStringLiteral("d")),
            updatedB,
            task(QStringLiteral("e")),
        };
        model.setSnapshot(supplied);

        QCOMPARE(resetCount, 0);
        QCOMPARE(removedRows, 2);
        QCOMPARE(insertedRows, 1);
        QVERIFY(movedRows >= 1);
        QCOMPARE(model.snapshot(), supplied);
        const QStringList expectedIds{
            QStringLiteral("d"),
            QStringLiteral("b"),
            QStringLiteral("e"),
        };
        QCOMPARE(modelIds(model), expectedIds);
    }

    void emptySnapshotRemovesRowsIncrementally()
    {
        TaskListModel model;
        model.setSnapshot({
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
            task(QStringLiteral("c")),
        });

        int resetCount = 0;
        int removedRows = 0;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsRemoved,
            &model,
            [&removedRows](
                const QModelIndex&,
                int first,
                int last) {
                removedRows += last - first + 1;
            });

        model.setSnapshot({});

        QCOMPARE(resetCount, 0);
        QCOMPARE(removedRows, 3);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.snapshot().isEmpty());
    }

    void reorderOnlyTransitionMatchesSuppliedOrder()
    {
        TaskListModel model;
        model.setSnapshot({
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
            task(QStringLiteral("c")),
            task(QStringLiteral("d")),
        });

        int resetCount = 0;
        int insertedRows = 0;
        int removedRows = 0;
        int movedRows = 0;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&insertedRows](
                const QModelIndex&,
                int first,
                int last) {
                insertedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsRemoved,
            &model,
            [&removedRows](
                const QModelIndex&,
                int first,
                int last) {
                removedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsMoved,
            &model,
            [&movedRows](
                const QModelIndex&,
                int first,
                int last,
                const QModelIndex&,
                int) {
                movedRows += last - first + 1;
            });

        const QVector<BridgeTask> supplied{
            task(QStringLiteral("c")),
            task(QStringLiteral("a")),
            task(QStringLiteral("d")),
            task(QStringLiteral("b")),
        };
        model.setSnapshot(supplied);

        QCOMPARE(resetCount, 0);
        QCOMPARE(insertedRows, 0);
        QCOMPARE(removedRows, 0);
        QVERIFY(movedRows >= 1);
        QCOMPARE(model.snapshot(), supplied);
        const QStringList expectedIds{
            QStringLiteral("c"),
            QStringLiteral("a"),
            QStringLiteral("d"),
            QStringLiteral("b"),
        };
        QCOMPARE(modelIds(model), expectedIds);
    }

    void duplicateIdsUseResetPath()
    {
        TaskListModel model;
        model.setSnapshot({
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
        });

        int resetCount = 0;
        int insertedRows = 0;
        int removedRows = 0;
        int movedRows = 0;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&insertedRows](
                const QModelIndex&,
                int first,
                int last) {
                insertedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsRemoved,
            &model,
            [&removedRows](
                const QModelIndex&,
                int first,
                int last) {
                removedRows += last - first + 1;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsMoved,
            &model,
            [&movedRows](
                const QModelIndex&,
                int first,
                int last,
                const QModelIndex&,
                int) {
                movedRows += last - first + 1;
            });

        const QVector<BridgeTask> duplicated{
            task(
                QStringLiteral("duplicate"),
                QStringLiteral("First duplicate")),
            task(
                QStringLiteral("duplicate"),
                QStringLiteral("Second duplicate")),
        };
        model.setSnapshot(duplicated);

        QCOMPARE(resetCount, 1);
        QCOMPARE(insertedRows, 0);
        QCOMPARE(removedRows, 0);
        QCOMPARE(movedRows, 0);
        QCOMPARE(model.snapshot(), duplicated);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     TaskListModel::TitleRole)
                .toString(),
            QStringLiteral("First duplicate"));
        QCOMPARE(
            model.data(
                     model.index(1, 0),
                     TaskListModel::TitleRole)
                .toString(),
            QStringLiteral("Second duplicate"));
    }

    void uniqueSnapshotRecoversFromDuplicatedCurrentStateWithReset()
    {
        TaskListModel model;
        int resetCount = 0;
        QVector<DataChange> changes;
        QObject::connect(
            &model,
            &QAbstractItemModel::modelReset,
            &model,
            [&resetCount] {
                ++resetCount;
            });
        QObject::connect(
            &model,
            &QAbstractItemModel::dataChanged,
            &model,
            [&changes](
                const QModelIndex& first,
                const QModelIndex& last,
                const QList<int>& roles) {
                changes.append(
                    {first.row(), last.row(), roles});
            });

        model.setSnapshot({
            task(QStringLiteral("duplicate")),
            task(QStringLiteral("duplicate")),
        });
        QCOMPARE(resetCount, 1);

        const QVector<BridgeTask> recovered{
            task(QStringLiteral("a")),
            task(QStringLiteral("b")),
        };
        model.setSnapshot(recovered);
        QCOMPARE(resetCount, 2);
        QCOMPARE(model.snapshot(), recovered);

        QVector<BridgeTask> incrementallyUpdated = recovered;
        incrementallyUpdated[0].title =
            QStringLiteral("Recovered A");
        model.setSnapshot(incrementallyUpdated);

        QCOMPARE(resetCount, 2);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(
            changes.first().roles,
            QList<int>{TaskListModel::TitleRole});
    }

    void absentOptionalsRemainInvalidVariants()
    {
        TaskListModel model;
        model.setSnapshot({task(QStringLiteral("optional"))});
        const QModelIndex index = model.index(0, 0);

        const QList<int> optionalRoles{
            TaskListModel::CwdRole,
            TaskListModel::ActiveTurnIdRole,
            TaskListModel::ModelRole,
            TaskListModel::ReasoningEffortRole,
            TaskListModel::GroupKindRole,
            TaskListModel::GroupTitleRole,
            TaskListModel::GoalRole,
        };
        for (const int role : optionalRoles) {
            QVERIFY(!model.data(index, role).isValid());
        }
        QCOMPARE(
            model.data(index, TaskListModel::UpdatedAtRole)
                .toDouble(),
            10.5);
    }

    void goalRolePreservesCompleteBridgeShape()
    {
        TaskListModel model;
        BridgeTask item = task(QStringLiteral("goal-thread"));
        item.goal = BridgeGoal{
            QStringLiteral("goal-thread"),
            QStringLiteral("Publish the task model"),
            GoalStatus::UsageLimited,
            90000,
            12345,
            678,
            1111111111,
            2222222222,
        };
        model.setSnapshot({item});

        const QVariantMap goal =
            model.data(
                     model.index(0, 0),
                     TaskListModel::GoalRole)
                .toMap();

        QCOMPARE(goal.size(), 8);
        QCOMPARE(
            goal.value(QStringLiteral("threadId")).toString(),
            QStringLiteral("goal-thread"));
        QCOMPARE(
            goal.value(QStringLiteral("objective")).toString(),
            QStringLiteral("Publish the task model"));
        QCOMPARE(
            goal.value(QStringLiteral("status")).toString(),
            QStringLiteral("usageLimited"));
        QCOMPARE(
            goal.value(QStringLiteral("tokenBudget"))
                .toLongLong(),
            90000);
        QCOMPARE(
            goal.value(QStringLiteral("tokensUsed"))
                .toLongLong(),
            12345);
        QCOMPARE(
            goal.value(QStringLiteral("elapsedSeconds"))
                .toLongLong(),
            678);
        QCOMPARE(
            goal.value(QStringLiteral("createdAt"))
                .toLongLong(),
            1111111111);
        QCOMPARE(
            goal.value(QStringLiteral("updatedAt"))
                .toLongLong(),
            2222222222);
        QVERIFY(
            !goal.contains(
                QStringLiteral("timeUsedSeconds")));

        item.goal->tokenBudget = std::nullopt;
        model.setSnapshot({item});
        const QVariantMap withoutBudget =
            model.data(
                     model.index(0, 0),
                     TaskListModel::GoalRole)
                .toMap();
        QCOMPARE(withoutBudget.size(), 7);
        QVERIFY(
            !withoutBudget.contains(
                QStringLiteral("tokenBudget")));
        QVERIFY(
            !withoutBudget
                 .value(QStringLiteral("tokenBudget"))
                 .isValid());
    }

    void enumBackedRolesUseWireSpellings()
    {
        const QVector<TaskStatus> statuses{
            TaskStatus::Running,
            TaskStatus::Waiting,
            TaskStatus::Completed,
            TaskStatus::Failed,
        };
        const QStringList statusNames{
            QStringLiteral("running"),
            QStringLiteral("waiting"),
            QStringLiteral("completed"),
            QStringLiteral("failed"),
        };
        const QVector<GoalStatus> goalStatuses{
            GoalStatus::Active,
            GoalStatus::Paused,
            GoalStatus::Blocked,
            GoalStatus::UsageLimited,
            GoalStatus::BudgetLimited,
            GoalStatus::Complete,
        };
        const QStringList goalStatusNames{
            QStringLiteral("active"),
            QStringLiteral("paused"),
            QStringLiteral("blocked"),
            QStringLiteral("usageLimited"),
            QStringLiteral("budgetLimited"),
            QStringLiteral("complete"),
        };

        QVector<BridgeTask> tasks;
        for (qsizetype index = 0;
             index < goalStatuses.size();
             ++index) {
            BridgeTask item =
                task(QStringLiteral("enum-%1").arg(index));
            item.status =
                statuses.at(index % statuses.size());
            item.taskGroup = BridgeTaskGroup{
                index % 2 == 0
                    ? TaskGroupKind::Chats
                    : TaskGroupKind::Project,
                index % 2 == 0
                    ? QStringLiteral("Chats")
                    : QStringLiteral("Project"),
                std::nullopt,
            };
            item.goal = BridgeGoal{
                item.id,
                QStringLiteral("Objective"),
                goalStatuses.at(index),
                std::nullopt,
                0,
                0,
                0,
                0,
            };
            tasks.append(std::move(item));
        }

        TaskListModel model;
        model.setSnapshot(tasks);

        for (qsizetype index = 0;
             index < tasks.size();
             ++index) {
            const QModelIndex modelIndex =
                model.index(static_cast<int>(index), 0);
            QCOMPARE(
                model.data(
                         modelIndex,
                         TaskListModel::StatusRole)
                    .toString(),
                statusNames.at(index % statusNames.size()));
            QCOMPARE(
                model.data(
                         modelIndex,
                         TaskListModel::GroupKindRole)
                    .toString(),
                index % 2 == 0
                    ? QStringLiteral("chats")
                    : QStringLiteral("project"));
            QCOMPARE(
                model.data(
                         modelIndex,
                         TaskListModel::GoalRole)
                    .toMap()
                    .value(QStringLiteral("status"))
                    .toString(),
                goalStatusNames.at(index));
        }
    }

    void companionStateOwnsExactlyOneTaskModel()
    {
        CompanionState state;

        TaskListModel* const first = state.tasks();
        TaskListModel* const second = state.tasks();
        QVERIFY(first != nullptr);
        QCOMPARE(first, second);
        QCOMPARE(first->parent(), &state);

        const QList<TaskListModel*> directChildren =
            state.findChildren<TaskListModel*>(
                QString(),
                Qt::FindDirectChildrenOnly);
        QCOMPARE(directChildren.size(), 1);
        QCOMPARE(directChildren.first(), first);

        const int propertyIndex =
            state.metaObject()->indexOfProperty("tasks");
        QVERIFY(propertyIndex >= 0);
        const QMetaProperty property =
            state.metaObject()->property(propertyIndex);
        QVERIFY(property.isConstant());
        QCOMPARE(
            property.read(&state).value<QObject*>(),
            static_cast<QObject*>(first));
    }

    void registrationRejectsInvalidAndDuplicateHandlers()
    {
        CompanionCommandBus bus;

        const Result<void> blank =
            bus.registerHandler(
                QStringLiteral(" \t\r\n "),
                successHandler());
        QVERIFY(!blank.hasValue());
        QCOMPARE(
            blank.error().code,
            QStringLiteral("ui.invalid_command"));
        QVERIFY(blank.error().context.isEmpty());

        const Result<void> empty =
            bus.registerHandler(
                QStringLiteral("empty"),
                CompanionCommandBus::Handler{});
        QVERIFY(!empty.hasValue());
        QCOMPARE(
            empty.error().code,
            QStringLiteral("ui.invalid_command"));
        QVERIFY(empty.error().context.isEmpty());

        int firstCalls = 0;
        int secondCalls = 0;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("owned"),
                   [&firstCalls](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       ++firstCalls;
                       completion(Result<void>::success());
                   })
                .hasValue());
        const Result<void> duplicate =
            bus.registerHandler(
                QStringLiteral("owned"),
                [&secondCalls](
                    const QVariantMap&,
                    CompanionCommandBus::Completion completion) {
                    ++secondCalls;
                    completion(Result<void>::success());
                });
        QVERIFY(!duplicate.hasValue());
        QCOMPARE(
            duplicate.error().code,
            QStringLiteral(
                "ui.command_already_registered"));
        QVERIFY(duplicate.error().context.isEmpty());

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("owned"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCOMPARE(firstCalls, 1);
        QCOMPARE(secondCalls, 0);
    }

    void registrationRollbackRequiresMatchingIdentity()
    {
        CompanionCommandBus bus;
        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        int firstCalls = 0;
        quint64 firstRegistrationId = 0;
        QVERIFY(
            detail::CompanionCommandBusTestAccess::
                registerHandlerTransaction(
                    bus,
                    QStringLiteral("owned"),
                    [&firstCalls](
                        const QVariantMap&,
                        CompanionCommandBus::Completion
                            completion) {
                        ++firstCalls;
                        completion(
                            Result<void>::success());
                    },
                    firstRegistrationId)
                .hasValue());
        QVERIFY(firstRegistrationId != 0);

        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("owned"),
                firstRegistrationId + 1);
        bus.execute(QStringLiteral("owned"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCOMPARE(startedSpy.size(), 0);
        QCOMPARE(firstCalls, 0);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral("ui.unknown_command"));

        QVERIFY(
            detail::CompanionCommandBusTestAccess::
                commitHandlerRegistration(
                    bus,
                    QStringLiteral("owned"),
                    firstRegistrationId));

        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("owned"),
                firstRegistrationId);
        bus.execute(QStringLiteral("owned"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(startedSpy.size(), 1);
        QCOMPARE(firstCalls, 1);
        QVERIFY(finishedSpy.at(1).at(1).toBool());

        quint64 rolledBackRegistrationId = 0;
        QVERIFY(
            detail::CompanionCommandBusTestAccess::
                registerHandlerTransaction(
                    bus,
                    QStringLiteral("rolled-back"),
                    successHandler(),
                    rolledBackRegistrationId)
                .hasValue());
        QVERIFY(rolledBackRegistrationId != 0);
        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("rolled-back"),
                rolledBackRegistrationId);
        int replacementCalls = 0;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("rolled-back"),
                   [&replacementCalls](
                       const QVariantMap&,
                       CompanionCommandBus::Completion
                           completion) {
                       ++replacementCalls;
                       completion(
                           Result<void>::success());
                   })
                .hasValue());

        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("rolled-back"),
                rolledBackRegistrationId);
        bus.execute(
            QStringLiteral("rolled-back"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            1000);

        QCOMPARE(firstCalls, 1);
        QCOMPARE(replacementCalls, 1);
    }

    void ownerThreadExecuteInvokesHandlerInlineOnce()
    {
        CompanionCommandBus bus;
        const std::thread::id ownerThreadId =
            std::this_thread::get_id();
        int startedCount = 0;
        int handlerCount = 0;
        std::thread::id startedThreadId;
        std::thread::id handlerThreadId;
        QVariantMap observedArguments;

        QObject::connect(
            &bus,
            &CompanionCommandBus::commandStarted,
            &bus,
            [&startedCount, &startedThreadId](
                const QString&) {
                ++startedCount;
                startedThreadId =
                    std::this_thread::get_id();
            },
            Qt::DirectConnection);
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.owner-thread"),
                   [&handlerCount,
                    &handlerThreadId,
                    &observedArguments](
                       const QVariantMap& arguments,
                       CompanionCommandBus::Completion) {
                       ++handlerCount;
                       handlerThreadId =
                           std::this_thread::get_id();
                       observedArguments = arguments;
                   })
                .hasValue());

        const QVariantMap arguments{
            {QStringLiteral("threadId"),
             QStringLiteral("owner-thread")},
        };
        bus.execute(
            QStringLiteral("task.owner-thread"),
            arguments);

        QCOMPARE(startedCount, 1);
        QCOMPARE(handlerCount, 1);
        QVERIFY(startedThreadId == ownerThreadId);
        QVERIFY(handlerThreadId == ownerThreadId);
        QCOMPARE(observedArguments, arguments);
    }

    void commandStartedAffinityMoveRequeuesHandlerToCurrentOwner()
    {
        auto* bus = new CompanionCommandBus();
        QThread ownerThread;
        QThread* const originalThread =
            QThread::currentThread();
        ownerThread.start();

        std::atomic_int startedCount = 0;
        std::atomic_int handlerCount = 0;
        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> handlerThread = nullptr;
        std::atomic<QThread*> finishedThread = nullptr;
        bool moved = false;
        bool moveBackInvoked = false;
        bool movedBack = false;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [bus,
             &ownerThread,
             &startedCount,
             &moved](
                const QString&) {
                startedCount.fetch_add(1);
                if (!moved) {
                    bus->moveToThread(
                        &ownerThread);
                    moved =
                        bus->thread()
                        == &ownerThread;
                }
            },
            Qt::DirectConnection);
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount,
             &finishedThread](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                finishedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        const Result<void> registration =
            bus->registerHandler(
                QStringLiteral(
                    "task.affinity-move"),
                [&handlerCount,
                 &handlerThread](
                    const QVariantMap&,
                    CompanionCommandBus::Completion
                        completion) {
                    handlerThread.store(
                        QThread::currentThread());
                    handlerCount.fetch_add(1);
                    completion(
                        Result<void>::success());
                });

        bus->execute(
            QStringLiteral(
                "task.affinity-move"));
        const bool completed =
            waitUntil(
                [&handlerCount,
                 &finishedCount] {
                    return handlerCount.load() == 1
                        && finishedCount.load() == 1;
                },
                1000);

        if (bus->thread() == &ownerThread) {
            moveBackInvoked =
                QMetaObject::invokeMethod(
                    bus,
                    [bus,
                     originalThread,
                     &movedBack] {
                        bus->moveToThread(
                            originalThread);
                        movedBack = true;
                    },
                    Qt::BlockingQueuedConnection);
        }
        ownerThread.quit();
        const bool threadStopped =
            ownerThread.wait(1000);
        delete bus;

        QVERIFY(registration.hasValue());
        QVERIFY(moved);
        QVERIFY(completed);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
        QCOMPARE(startedCount.load(), 1);
        QCOMPARE(handlerCount.load(), 1);
        QCOMPARE(finishedCount.load(), 1);
        QCOMPARE(
            handlerThread.load(),
            &ownerThread);
        QCOMPARE(
            finishedThread.load(),
            &ownerThread);
    }

    void foreignThreadExecuteMarshalsLookupAndHandlerToOwner()
    {
        CompanionCommandBus bus;
        const std::thread::id ownerThreadId =
            std::this_thread::get_id();
        std::atomic_int startedCount = 0;
        std::atomic_int handlerCount = 0;
        std::atomic_int finishedCount = 0;
        std::mutex observationsMutex;
        std::thread::id startedThreadId;
        std::thread::id handlerThreadId;
        QVariantMap observedArguments;
        QObject receiver;

        QObject::connect(
            &bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [&startedCount,
             &observationsMutex,
             &startedThreadId](
                const QString&) {
                {
                    const std::scoped_lock lock(
                        observationsMutex);
                    startedThreadId =
                        std::this_thread::get_id();
                }
                startedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        QObject::connect(
            &bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.foreign-thread"),
                   [&handlerCount,
                    &observationsMutex,
                    &handlerThreadId,
                    &observedArguments](
                       const QVariantMap& arguments,
                       CompanionCommandBus::Completion completion) {
                       {
                           const std::scoped_lock lock(
                               observationsMutex);
                           handlerThreadId =
                               std::this_thread::get_id();
                           observedArguments = arguments;
                       }
                       handlerCount.fetch_add(1);
                       completion(
                           Result<void>::success());
                   })
                .hasValue());

        const QVariantMap arguments{
            {QStringLiteral("threadId"),
             QStringLiteral("foreign-thread")},
        };
        std::barrier<> start(2);
        std::thread caller(
            [&bus, &arguments, &start] {
                start.arrive_and_wait();
                bus.execute(
                    QStringLiteral(
                        "task.foreign-thread"),
                    arguments);
            });
        start.arrive_and_wait();
        caller.join();

        QCOMPARE(startedCount.load(), 0);
        QCOMPARE(handlerCount.load(), 0);
        QCOMPARE(finishedCount.load(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedCount.load(),
            1,
            1000);
        QCOMPARE(startedCount.load(), 1);
        QCOMPARE(handlerCount.load(), 1);

        std::thread::id observedStartedThreadId;
        std::thread::id observedHandlerThreadId;
        QVariantMap deliveredArguments;
        {
            const std::scoped_lock lock(
                observationsMutex);
            observedStartedThreadId =
                startedThreadId;
            observedHandlerThreadId =
                handlerThreadId;
            deliveredArguments = observedArguments;
        }
        QVERIFY(
            observedStartedThreadId
            == ownerThreadId);
        QVERIFY(
            observedHandlerThreadId
            == ownerThreadId);
        QCOMPARE(deliveredArguments, arguments);

        QCoreApplication::processEvents();
        QCOMPARE(startedCount.load(), 1);
        QCOMPARE(handlerCount.load(), 1);
        QCOMPARE(finishedCount.load(), 1);
    }

    void destructionBeforeQueuedForeignLookupIsHarmless()
    {
        auto* bus = new CompanionCommandBus();
        std::atomic_int startedCount = 0;
        std::atomic_int handlerCount = 0;
        std::atomic_int finishedCount = 0;
        QObject receiver;

        QObject::connect(
            bus,
            &CompanionCommandBus::commandStarted,
            &receiver,
            [&startedCount](const QString&) {
                startedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        QVERIFY(
            bus->registerHandler(
                   QStringLiteral(
                       "task.destroy-before-lookup"),
                   [&handlerCount](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       handlerCount.fetch_add(1);
                       completion(
                           Result<void>::success());
                   })
                .hasValue());

        std::barrier<> start(2);
        std::thread caller(
            [bus, &start] {
                start.arrive_and_wait();
                bus->execute(
                    QStringLiteral(
                        "task.destroy-before-lookup"));
            });
        start.arrive_and_wait();
        caller.join();

        QCOMPARE(startedCount.load(), 0);
        QCOMPARE(handlerCount.load(), 0);
        QCOMPARE(finishedCount.load(), 0);

        delete bus;
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();

        QCOMPARE(startedCount.load(), 0);
        QCOMPARE(handlerCount.load(), 0);
        QCOMPARE(finishedCount.load(), 0);
    }

    void registeredCommandStartsBeforeInvocationAndFinishesSuccess()
    {
        CompanionCommandBus bus;
        QStringList events;
        QVariantMap observedArguments;
        bool startedBeforeHandler = false;
        QObject::connect(
            &bus,
            &CompanionCommandBus::commandStarted,
            &bus,
            [&events](const QString&) {
                events.append(QStringLiteral("started"));
            });
        QObject::connect(
            &bus,
            &CompanionCommandBus::commandFinished,
            &bus,
            [&events](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                events.append(QStringLiteral("finished"));
            });
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.open"),
                   [&events,
                    &observedArguments,
                    &startedBeforeHandler](
                       const QVariantMap& arguments,
                       CompanionCommandBus::Completion completion) {
                       const QStringList expected{
                           QStringLiteral("started"),
                       };
                       startedBeforeHandler =
                           events == expected;
                       observedArguments = arguments;
                       events.append(QStringLiteral("handler"));
                       completion(Result<void>::success());
                   })
                .hasValue());

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        const QVariantMap arguments{
            {QStringLiteral("threadId"),
             QStringLiteral("thread-a")},
            {QStringLiteral("setting"), 42},
        };
        bus.execute(
            QStringLiteral("task.open"),
            arguments);

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(startedBeforeHandler);
        QCOMPARE(observedArguments, arguments);
        QCOMPARE(startedSpy.size(), 1);
        const QStringList expectedEvents{
            QStringLiteral("started"),
            QStringLiteral("handler"),
            QStringLiteral("finished"),
        };
        QCOMPARE(events, expectedEvents);
        const QList<QVariant> finished =
            finishedSpy.takeFirst();
        QCOMPARE(
            finished.at(0).toString(),
            QStringLiteral("task.open"));
        QVERIFY(finished.at(1).toBool());
        QVERIFY(finished.at(2).toString().isEmpty());
        QVERIFY(finished.at(3).toString().isEmpty());
    }

    void handlerFailureAndUnknownCommandUseStableResults()
    {
        CompanionCommandBus bus;
        int calls = 0;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.fail"),
                   [&calls](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       ++calls;
                       completion(
                           Result<void>::failure({
                               QStringLiteral("domain.denied"),
                               QStringLiteral("Denied."),
                               false,
                               {},
                           }));
                   })
                .hasValue());

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);

        bus.execute(QStringLiteral("task.fail"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCOMPARE(startedSpy.size(), 1);
        QCOMPARE(calls, 1);
        QList<QVariant> failure = finishedSpy.at(0);
        QVERIFY(!failure.at(1).toBool());
        QCOMPARE(
            failure.at(2).toString(),
            QStringLiteral("domain.denied"));
        QCOMPARE(
            failure.at(3).toString(),
            QStringLiteral("Denied."));

        bus.execute(QStringLiteral("task.unknown"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(startedSpy.size(), 1);
        QCOMPARE(calls, 1);
        const QList<QVariant> unknown = finishedSpy.at(1);
        QCOMPARE(
            unknown.at(0).toString(),
            QStringLiteral("task.unknown"));
        QVERIFY(!unknown.at(1).toBool());
        QCOMPARE(
            unknown.at(2).toString(),
            QStringLiteral("ui.unknown_command"));
    }

    void asynchronousCompletionDoesNotBlockOwnerThread()
    {
        CompanionCommandBus bus;
        std::optional<CompanionCommandBus::Completion> pending;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.async"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("task.async"));

        QCOMPARE(startedSpy.size(), 1);
        QCOMPARE(finishedSpy.size(), 0);
        QVERIFY(pending.has_value());

        bool ownerEventRan = false;
        QTimer::singleShot(
            0,
            &bus,
            [&ownerEventRan, &pending] {
                ownerEventRan = true;
                (*pending)(Result<void>::success());
            });
        QTRY_VERIFY_WITH_TIMEOUT(
            ownerEventRan,
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
    }

    void crossThreadCompletionFinishesOnceOnBusThread()
    {
        CompanionCommandBus bus;
        std::optional<CompanionCommandBus::Completion> pending;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.cross-thread"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());

        QThread* observedThread = nullptr;
        int finishedCount = 0;
        QObject receiver;
        QObject::connect(
            &bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&observedThread, &finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                observedThread = QThread::currentThread();
                ++finishedCount;
            },
            Qt::DirectConnection);

        bus.execute(QStringLiteral("task.cross-thread"));
        QVERIFY(pending.has_value());
        std::thread worker(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        worker.join();

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedCount,
            1,
            1000);
        QCOMPARE(observedThread, bus.thread());
        QCoreApplication::processEvents();
        QCOMPARE(finishedCount, 1);
    }

    void concurrentCompletionFinishesExactlyOnce()
    {
        CompanionCommandBus bus;
        std::optional<CompanionCommandBus::Completion>
            pending;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.concurrent"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("task.concurrent"));
        QVERIFY(pending.has_value());

        std::barrier<> start(3);
        const CompanionCommandBus::Completion first =
            *pending;
        const CompanionCommandBus::Completion second =
            *pending;
        std::thread firstThread(
            [&start, first] {
                start.arrive_and_wait();
                first(Result<void>::success());
            });
        std::thread secondThread(
            [&start, second] {
                start.arrive_and_wait();
                second(Result<void>::success());
            });
        start.arrive_and_wait();
        firstThread.join();
        secondThread.join();

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(finishedSpy.first().at(1).toBool());
    }

    void deliveryHookIsNotPublicConstructionApi()
    {
        QVERIFY(
            !(std::is_constructible_v<
                CompanionCommandBus,
                detail::CommandBusDeliveryHook,
                QObject*>));
    }

    void reentrantDeliveryHookCompletesWithoutDeadlock()
    {
        constexpr auto childEnvironmentName =
            "COMPANION_COMMAND_BUS_REENTRANT_CHILD";
        if (qEnvironmentVariableIsSet(
                childEnvironmentName)) {
            bool reentered = false;
            CompanionCommandBus* busPointer = nullptr;
            auto bus =
                commandBusWithDeliveryHook(
                    [&reentered,
                     &busPointer](
                        detail::
                            CommandBusDeliveryPhase phase) {
                        if (phase
                                != detail::
                                    CommandBusDeliveryPhase::
                                    BeforePost
                            || std::exchange(
                                reentered,
                                true)) {
                            return;
                        }
                        busPointer->execute(
                            QStringLiteral(
                                "task.reentrant-inner"));
                    });
            busPointer = bus.get();
            QVERIFY(
                bus->registerHandler(
                       QStringLiteral(
                           "task.reentrant-outer"),
                       successHandler())
                    .hasValue());
            QVERIFY(
                bus->registerHandler(
                       QStringLiteral(
                           "task.reentrant-inner"),
                       successHandler())
                    .hasValue());

            QStringList finishedCommands;
            QObject::connect(
                bus.get(),
                &CompanionCommandBus::commandFinished,
                bus.get(),
                [&finishedCommands](
                    const QString& command,
                    bool,
                    const QString&,
                    const QString&) {
                    finishedCommands.append(command);
                });

            bus->execute(
                QStringLiteral(
                    "task.reentrant-outer"));
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedCommands.size(),
                2,
                1000);
            const QStringList expected{
                QStringLiteral(
                    "task.reentrant-inner"),
                QStringLiteral(
                    "task.reentrant-outer"),
            };
            QCOMPARE(finishedCommands, expected);
            return;
        }

        QProcess child;
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QString::fromLatin1(
                childEnvironmentName),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "reentrantDeliveryHookCompletesWithoutDeadlock"),
        });
        child.start();
        QVERIFY2(
            child.waitForStarted(1000),
            qPrintable(child.errorString()));

        const bool finished =
            child.waitForFinished(3000);
        if (!finished) {
            child.kill();
            child.waitForFinished(1000);
        }
        const QString output =
            QString::fromLocal8Bit(
                child.readAll());
        QVERIFY2(
            finished,
            qPrintable(
                QStringLiteral(
                    "reentrant child timed out:\n")
                + output));
        QCOMPARE(
            child.exitStatus(),
            QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    void destroyWinsSuppressesPostAndSignal()
    {
        ManualGate beforePostEntered;
        ManualGate releaseBeforePost;
        ManualGate destructionEntered;
        ManualGate destructionFinished;
        std::atomic_int finishedCount = 0;
        std::mutex eventsMutex;
        QVector<detail::CommandBusDeliveryPhase>
            events;

        auto busOwner = commandBusWithDeliveryHook(
            [&beforePostEntered,
             &releaseBeforePost,
             &destructionEntered,
             &eventsMutex,
             &events](
                detail::CommandBusDeliveryPhase phase) {
                {
                    const std::scoped_lock lock(
                        eventsMutex);
                    events.append(phase);
                }
                if (phase
                    == detail::
                        CommandBusDeliveryPhase::
                        BeforePost) {
                    beforePostEntered.release();
                    releaseBeforePost.wait();
                } else if (
                    phase
                    == detail::
                        CommandBusDeliveryPhase::
                        DestructionStarted) {
                    destructionEntered.release();
                }
            });
        auto* bus = busOwner.release();
        std::optional<CompanionCommandBus::Completion>
            pending;
        QVERIFY(
            bus->registerHandler(
                   QStringLiteral(
                       "task.destroy-contention"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        bus->execute(
            QStringLiteral(
                "task.destroy-contention"));
        QVERIFY(pending.has_value());

        QThread ownerThread;
        auto* ownerAnchor = new QObject;
        bus->moveToThread(&ownerThread);
        ownerAnchor->moveToThread(&ownerThread);
        ownerThread.start();

        std::thread completionThread(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        const bool reachedBeforePost =
            beforePostEntered.waitFor(1000);
        bool deletionQueued = false;
        if (reachedBeforePost) {
            deletionQueued =
                QMetaObject::invokeMethod(
                    ownerAnchor,
                    [bus, &destructionFinished] {
                        delete bus;
                        destructionFinished.release();
                    },
                    Qt::QueuedConnection);
        }
        const bool reachedDestruction =
            deletionQueued
            && destructionEntered.waitFor(1000);
        releaseBeforePost.release();
        completionThread.join();
        const bool destroyed =
            deletionQueued
            && destructionFinished.waitFor(1000);

        const bool anchorDeleted =
            QMetaObject::invokeMethod(
                ownerAnchor,
                [ownerAnchor] {
                    delete ownerAnchor;
                },
                Qt::BlockingQueuedConnection);
        ownerThread.quit();
        const bool threadStopped =
            ownerThread.wait(1000);

        QVector<detail::CommandBusDeliveryPhase>
            recordedEvents;
        {
            const std::scoped_lock lock(
                eventsMutex);
            recordedEvents = events;
        }
        const QVector<
            detail::CommandBusDeliveryPhase>
            expectedEvents{
                detail::CommandBusDeliveryPhase::
                    HandlerOwnerAccessAcquired,
                detail::CommandBusDeliveryPhase::
                    BeforePost,
                detail::CommandBusDeliveryPhase::
                    DestructionStarted,
            };

        QVERIFY(reachedBeforePost);
        QVERIFY(deletionQueued);
        QVERIFY(reachedDestruction);
        QVERIFY(destroyed);
        QVERIFY(anchorDeleted);
        QVERIFY(threadStopped);
        QCOMPARE(finishedCount.load(), 0);
        QCOMPARE(recordedEvents, expectedEvents);
    }

    void postWinsBeforeDestructionAndDispatchIsHarmless()
    {
        ManualGate afterPost;
        std::mutex eventsMutex;
        QVector<detail::CommandBusDeliveryPhase>
            events;
        auto busOwner = commandBusWithDeliveryHook(
            [&afterPost,
             &eventsMutex,
             &events](
                detail::CommandBusDeliveryPhase phase) {
                {
                    const std::scoped_lock lock(
                        eventsMutex);
                    events.append(phase);
                }
                if (phase
                    == detail::
                        CommandBusDeliveryPhase::
                        AfterPost) {
                    afterPost.release();
                }
            });
        auto* bus = busOwner.release();
        std::optional<CompanionCommandBus::Completion>
            pending;
        QVERIFY(
            bus->registerHandler(
                   QStringLiteral(
                       "task.post-before-destroy"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());

        std::atomic_int finishedCount = 0;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        bus->execute(
            QStringLiteral(
                "task.post-before-destroy"));
        QVERIFY(pending.has_value());

        std::thread completionThread(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        completionThread.join();
        const bool reachedAfterPost =
            afterPost.waitFor(1000);

        delete bus;
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QVector<detail::CommandBusDeliveryPhase>
            recordedEvents;
        {
            const std::scoped_lock lock(
                eventsMutex);
            recordedEvents = events;
        }
        const QVector<
            detail::CommandBusDeliveryPhase>
            expectedEvents{
                detail::CommandBusDeliveryPhase::
                    HandlerOwnerAccessAcquired,
                detail::CommandBusDeliveryPhase::
                    BeforePost,
                detail::CommandBusDeliveryPhase::
                    AfterPost,
                detail::CommandBusDeliveryPhase::
                    DestructionStarted,
            };

        QVERIFY(reachedAfterPost);
        QCOMPARE(finishedCount.load(), 0);
        QCOMPARE(recordedEvents, expectedEvents);
    }

    void postedCompletionFollowsMoveBeforeDispatch()
    {
        ManualGate posted;
        auto busOwner = commandBusWithDeliveryHook(
            [&posted](
                detail::CommandBusDeliveryPhase phase) {
                if (phase
                    == detail::
                        CommandBusDeliveryPhase::
                        AfterPost) {
                    posted.release();
                }
            });
        auto* bus = busOwner.release();
        std::optional<CompanionCommandBus::Completion>
            pending;
        QVERIFY(
            bus->registerHandler(
                   QStringLiteral(
                       "task.posted-before-move"),
                   [&pending](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       pending = std::move(completion);
                   })
                .hasValue());

        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> observedThread = nullptr;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount, &observedThread](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                observedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);
        bus->execute(
            QStringLiteral(
                "task.posted-before-move"));
        QVERIFY(pending.has_value());

        std::thread completionThread(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        completionThread.join();
        const bool wasPosted = posted.waitFor(1000);

        QThread targetThread;
        QThread* const originalThread = bus->thread();
        bus->moveToThread(&targetThread);
        const bool moved =
            bus->thread() == &targetThread;
        targetThread.start();

        const bool finished = waitUntil(
            [&finishedCount] {
                return finishedCount.load() == 1;
            },
            1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);
        const int finalFinishedCount =
            finishedCount.load();
        QThread* const deliveredThread =
            observedThread.load();

        bool movedBack = false;
        const bool moveBackInvoked =
            QMetaObject::invokeMethod(
                bus,
                [bus, originalThread, &movedBack] {
                    bus->moveToThread(originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        targetThread.quit();
        const bool threadStopped =
            targetThread.wait(1000);
        if (movedBack) {
            delete bus;
        }

        QVERIFY(wasPosted);
        QVERIFY(moved);
        QVERIFY(finished);
        QCOMPARE(finalFinishedCount, 1);
        QCOMPARE(deliveredThread, &targetThread);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
    }

    void movedHandlerDispatchSerializesDestruction()
    {
        constexpr auto childEnvironmentName =
            "COMPANION_COMMAND_BUS_HANDLER_DESTROY_CHILD";
        if (qEnvironmentVariableIsSet(
                childEnvironmentName)) {
            ManualGate deletionMayStart;
            ManualGate destructionReached;
            ManualGate deletionFinished;
            std::atomic_int handlerCalls = 0;
            std::atomic_int finishedCount = 0;
            CompanionCommandBus* busPointer = nullptr;
            auto busOwner =
                commandBusWithDeliveryHook(
                    [&deletionMayStart,
                     &destructionReached](
                        detail::
                            CommandBusDeliveryPhase phase) {
                        if (phase
                            == detail::
                                CommandBusDeliveryPhase::
                                HandlerOwnerAccessAcquired) {
                            deletionMayStart.release();
                            destructionReached.wait();
                        } else if (
                            phase
                                == detail::
                                    CommandBusDeliveryPhase::
                                    DestructionWaitingForAccess
                            || phase
                                == detail::
                                    CommandBusDeliveryPhase::
                                    DestructionStarted) {
                            destructionReached.release();
                        }
                    });
            busPointer = busOwner.release();
            QVERIFY(
                busPointer
                    ->registerHandler(
                        QStringLiteral(
                            "task.move-destroy-handler"),
                        [&handlerCalls](
                            const QVariantMap&,
                            CompanionCommandBus::Completion
                                completion) {
                            handlerCalls.fetch_add(1);
                            completion(
                                Result<void>::success());
                        })
                    .hasValue());

            QObject receiver;
            QObject::connect(
                busPointer,
                &CompanionCommandBus::commandFinished,
                &receiver,
                [&finishedCount](
                    const QString&,
                    bool,
                    const QString&,
                    const QString&) {
                    finishedCount.fetch_add(1);
                },
                Qt::DirectConnection);

            QThread ownerThread;
            auto* ownerAnchor = new QObject;
            ownerAnchor->moveToThread(&ownerThread);
            ownerThread.start();
            bool moved = false;
            bool deletionQueued = false;
            QObject::connect(
                busPointer,
                &CompanionCommandBus::commandStarted,
                &receiver,
                [busPointer,
                 &ownerThread,
                 ownerAnchor,
                 &deletionMayStart,
                 &deletionFinished,
                 &moved,
                 &deletionQueued](
                    const QString&) {
                    busPointer->moveToThread(
                        &ownerThread);
                    moved =
                        busPointer->thread()
                        == &ownerThread;
                    deletionQueued =
                        QMetaObject::invokeMethod(
                            ownerAnchor,
                            [busPointer,
                             &deletionMayStart,
                             &deletionFinished] {
                                deletionMayStart.wait();
                                delete busPointer;
                                deletionFinished.release();
                            },
                            Qt::QueuedConnection);
                },
                Qt::DirectConnection);

            busPointer->execute(
                QStringLiteral(
                    "task.move-destroy-handler"));
            const bool destroyed =
                deletionFinished.waitFor(1000);
            const bool anchorDeleted =
                QMetaObject::invokeMethod(
                    ownerAnchor,
                    [ownerAnchor] {
                        delete ownerAnchor;
                    },
                    Qt::BlockingQueuedConnection);
            ownerThread.quit();
            const bool threadStopped =
                ownerThread.wait(1000);

            QVERIFY(moved);
            QVERIFY(deletionQueued);
            QVERIFY(destroyed);
            QVERIFY(anchorDeleted);
            QVERIFY(threadStopped);
            QCOMPARE(handlerCalls.load(), 0);
            QCOMPARE(finishedCount.load(), 0);
            return;
        }

        QProcess child;
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QString::fromLatin1(
                childEnvironmentName),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "movedHandlerDispatchSerializesDestruction"),
        });
        child.start();
        QVERIFY2(
            child.waitForStarted(1000),
            qPrintable(child.errorString()));

        const bool finished =
            child.waitForFinished(3000);
        if (!finished) {
            child.kill();
            child.waitForFinished(1000);
        }
        const QString output =
            QString::fromLocal8Bit(
                child.readAll());
        QVERIFY2(
            finished,
            qPrintable(
                QStringLiteral(
                    "handler destruction child timed out:\n")
                + output));
        QCOMPARE(
            child.exitStatus(),
            QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    void movedCompletionDispatchSerializesDestruction()
    {
        constexpr auto childEnvironmentName =
            "COMPANION_COMMAND_BUS_DELIVERY_DESTROY_CHILD";
        if (qEnvironmentVariableIsSet(
                childEnvironmentName)) {
            ManualGate posted;
            ManualGate deletionMayStart;
            ManualGate destructionReached;
            ManualGate deletionFinished;
            std::atomic_int finishedCount = 0;
            CompanionCommandBus* busPointer = nullptr;
            QThread ownerThread;
            auto* ownerAnchor = new QObject;
            ownerAnchor->moveToThread(&ownerThread);
            ownerThread.start();
            bool moved = false;
            bool deletionQueued = false;

            auto busOwner =
                commandBusWithDeliveryHook(
                    [&posted,
                     &deletionMayStart,
                     &destructionReached,
                     &deletionFinished,
                     &busPointer,
                     &ownerThread,
                     ownerAnchor,
                     &moved,
                     &deletionQueued](
                        detail::
                            CommandBusDeliveryPhase phase) {
                        if (phase
                            == detail::
                                CommandBusDeliveryPhase::
                                AfterPost) {
                            posted.release();
                        } else if (
                            phase
                            == detail::
                                CommandBusDeliveryPhase::
                                BeforeDispatch) {
                            busPointer->moveToThread(
                                &ownerThread);
                            moved =
                                busPointer->thread()
                                == &ownerThread;
                            deletionQueued =
                                QMetaObject::invokeMethod(
                                    ownerAnchor,
                                    [busPointer,
                                     &deletionMayStart,
                                     &deletionFinished] {
                                        deletionMayStart.wait();
                                        delete busPointer;
                                        deletionFinished.release();
                                    },
                                    Qt::QueuedConnection);
                        } else if (
                            phase
                            == detail::
                                CommandBusDeliveryPhase::
                                DeliveryOwnerAccessAcquired) {
                            deletionMayStart.release();
                            destructionReached.wait();
                        } else if (
                            phase
                                == detail::
                                    CommandBusDeliveryPhase::
                                    DestructionWaitingForAccess
                            || phase
                                == detail::
                                    CommandBusDeliveryPhase::
                                    DestructionStarted) {
                            destructionReached.release();
                        }
                    });
            busPointer = busOwner.release();
            std::optional<
                CompanionCommandBus::Completion>
                pending;
            QVERIFY(
                busPointer
                    ->registerHandler(
                        QStringLiteral(
                            "task.move-destroy-delivery"),
                        [&pending](
                            const QVariantMap&,
                            CompanionCommandBus::Completion
                                completion) {
                            pending =
                                std::move(completion);
                        })
                    .hasValue());

            QObject receiver;
            QObject::connect(
                busPointer,
                &CompanionCommandBus::commandFinished,
                &receiver,
                [&finishedCount](
                    const QString&,
                    bool,
                    const QString&,
                    const QString&) {
                    finishedCount.fetch_add(1);
                },
                Qt::DirectConnection);
            busPointer->execute(
                QStringLiteral(
                    "task.move-destroy-delivery"));
            QVERIFY(pending.has_value());

            std::thread completionThread(
                [completion = *pending] {
                    completion(
                        Result<void>::success());
                });
            completionThread.join();
            QVERIFY(posted.waitFor(1000));
            const bool destroyed = waitUntil(
                [&deletionFinished] {
                    return deletionFinished.waitFor(0);
                },
                1000);
            const bool anchorDeleted =
                QMetaObject::invokeMethod(
                    ownerAnchor,
                    [ownerAnchor] {
                        delete ownerAnchor;
                    },
                    Qt::BlockingQueuedConnection);
            ownerThread.quit();
            const bool threadStopped =
                ownerThread.wait(1000);

            QVERIFY(moved);
            QVERIFY(deletionQueued);
            QVERIFY(destroyed);
            QVERIFY(anchorDeleted);
            QVERIFY(threadStopped);
            QCOMPARE(finishedCount.load(), 0);
            return;
        }

        QProcess child;
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QString::fromLatin1(
                childEnvironmentName),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "movedCompletionDispatchSerializesDestruction"),
        });
        child.start();
        QVERIFY2(
            child.waitForStarted(1000),
            qPrintable(child.errorString()));

        const bool finished =
            child.waitForFinished(3000);
        if (!finished) {
            child.kill();
            child.waitForFinished(1000);
        }
        const QString output =
            QString::fromLocal8Bit(
                child.readAll());
        QVERIFY2(
            finished,
            qPrintable(
                QStringLiteral(
                    "delivery destruction child timed out:\n")
                + output));
        QCOMPARE(
            child.exitStatus(),
            QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    void deliveryHookExceptionsAreContained()
    {
        std::atomic_int hookCalls = 0;
        auto bus = commandBusWithDeliveryHook(
            [&hookCalls](
                detail::CommandBusDeliveryPhase) {
                hookCalls.fetch_add(1);
                throw std::runtime_error(
                    "test hook failure");
            });
        QVERIFY(
            bus->registerHandler(
                   QStringLiteral("task.hook-throw"),
                   successHandler())
                .hasValue());

        QSignalSpy finishedSpy(
            bus.get(),
            &CompanionCommandBus::commandFinished);
        bus->execute(
            QStringLiteral("task.hook-throw"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);

        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(finishedSpy.first().at(1).toBool());
        QVERIFY(hookCalls.load() >= 3);
    }

    void completionAfterBusDestructionIsIgnored()
    {
        std::optional<CompanionCommandBus::Completion> pending;
        int finishedCount = 0;
        QObject receiver;
        auto bus = std::make_unique<CompanionCommandBus>();
        QObject::connect(
            bus.get(),
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                ++finishedCount;
            });
        QVERIFY(
            bus->registerHandler(
                    QStringLiteral("task.destroyed"),
                    [&pending](
                        const QVariantMap&,
                        CompanionCommandBus::Completion completion) {
                        pending = std::move(completion);
                    })
                .hasValue());

        bus->execute(QStringLiteral("task.destroyed"));
        QVERIFY(pending.has_value());
        bus.reset();

        std::thread worker(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        worker.join();
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedCount, 0);
    }

    void completionAfterAffinityMoveUsesCurrentOwnerThread()
    {
        auto* bus = new CompanionCommandBus();
        std::optional<CompanionCommandBus::Completion> pending;
        const Result<void> registration =
            bus->registerHandler(
                QStringLiteral("task.moved"),
                [&pending](
                    const QVariantMap&,
                    CompanionCommandBus::Completion completion) {
                    pending = std::move(completion);
                });
        QVERIFY(registration.hasValue());

        std::atomic_int finishedCount = 0;
        std::atomic<QThread*> observedThread = nullptr;
        QObject receiver;
        QObject::connect(
            bus,
            &CompanionCommandBus::commandFinished,
            &receiver,
            [&finishedCount, &observedThread](
                const QString&,
                bool,
                const QString&,
                const QString&) {
                observedThread.store(
                    QThread::currentThread());
                finishedCount.fetch_add(1);
            },
            Qt::DirectConnection);

        bus->execute(QStringLiteral("task.moved"));
        QVERIFY(pending.has_value());

        QThread targetThread;
        targetThread.start();
        QThread* const originalThread = bus->thread();
        bus->moveToThread(&targetThread);
        const bool moved =
            bus->thread() == &targetThread;

        std::thread worker(
            [completion = *pending] {
                completion(Result<void>::success());
            });
        worker.join();
        const bool finished = waitUntil(
            [&finishedCount] {
                return finishedCount.load() == 1;
            },
            1000);
        QThread* const deliveredThread =
            observedThread.load();

        bool movedBack = false;
        const bool moveBackInvoked =
            QMetaObject::invokeMethod(
                bus,
                [bus, originalThread, &movedBack] {
                    bus->moveToThread(originalThread);
                    movedBack = true;
                },
                Qt::BlockingQueuedConnection);
        if (movedBack) {
            delete bus;
        } else {
            QMetaObject::invokeMethod(
                bus,
                [bus] {
                    delete bus;
                },
                Qt::BlockingQueuedConnection);
        }
        targetThread.quit();
        const bool threadStopped =
            targetThread.wait(1000);

        QVERIFY(moved);
        QVERIFY(finished);
        QCOMPARE(finishedCount.load(), 1);
        QCOMPARE(deliveredThread, &targetThread);
        QVERIFY(moveBackInvoked);
        QVERIFY(movedBack);
        QVERIFY(threadStopped);
    }

    void duplicateCompletionUsesFirstResult()
    {
        CompanionCommandBus bus;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.duplicate-completion"),
                   [](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       completion(Result<void>::success());
                       completion(
                           Result<void>::failure({
                               QStringLiteral("late.failure"),
                               QStringLiteral("Late failure."),
                               false,
                               {},
                           }));
                   })
                .hasValue());

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("task.duplicate-completion"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        const QList<QVariant> finished =
            finishedSpy.first();
        QVERIFY(finished.at(1).toBool());
        QVERIFY(finished.at(2).toString().isEmpty());
        QVERIFY(finished.at(3).toString().isEmpty());
    }

    void completionThenThrowUsesFirstResult()
    {
        CompanionCommandBus bus;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.complete-then-throw"),
                   [](
                       const QVariantMap&,
                       CompanionCommandBus::Completion completion) {
                       completion(Result<void>::success());
                       throw std::runtime_error(
                           "throw after completion");
                   })
                .hasValue());

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("task.complete-then-throw"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QCoreApplication::processEvents();
        QTest::qWait(25);

        QCOMPARE(finishedSpy.size(), 1);
        const QList<QVariant> finished =
            finishedSpy.first();
        QVERIFY(finished.at(1).toBool());
        QVERIFY(finished.at(2).toString().isEmpty());
        QVERIFY(finished.at(3).toString().isEmpty());
    }

    void throwingHandlerMapsToCommandFailed()
    {
        CompanionCommandBus bus;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.throw"),
                   [](
                       const QVariantMap&,
                       CompanionCommandBus::Completion) {
                       throw std::runtime_error(
                           "private exception detail");
                   })
                .hasValue());

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("task.throw"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);

        QCOMPARE(startedSpy.size(), 1);
        const QList<QVariant> finished =
            finishedSpy.first();
        QVERIFY(!finished.at(1).toBool());
        QCOMPARE(
            finished.at(2).toString(),
            QStringLiteral("ui.command_failed"));
        QVERIFY(
            !finished.at(3)
                 .toString()
                 .contains(
                     QStringLiteral(
                         "private exception detail")));
    }

    void failureSignalsDoNotLeakArguments()
    {
        CompanionCommandBus bus;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("task.private-throw"),
                   [](
                       const QVariantMap&,
                       CompanionCommandBus::Completion) {
                       throw std::runtime_error(
                           "throwing handler failed");
                   })
                .hasValue());

        const QStringList secrets{
            QStringLiteral("prompt-private-7fa4"),
            QStringLiteral("attachment-private.bin"),
            QStringLiteral("credential-private-118"),
            QStringLiteral("response-private-body"),
            QStringLiteral("goal-private-objective"),
        };
        const QVariantMap arguments{
            {QStringLiteral("prompt"), secrets.at(0)},
            {QStringLiteral("attachmentFilename"),
             secrets.at(1)},
            {QStringLiteral("credential"), secrets.at(2)},
            {QStringLiteral("responseBody"), secrets.at(3)},
            {QStringLiteral("goalObjective"), secrets.at(4)},
        };

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("task.private-unknown"),
            arguments);
        bus.execute(
            QStringLiteral("task.private-throw"),
            arguments);
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);

        QCOMPARE(startedSpy.size(), 1);
        for (const QList<QVariant>& signal :
             finishedSpy) {
            const QString publicFailure =
                signal.at(2).toString()
                + QStringLiteral("\n")
                + signal.at(3).toString();
            for (const QString& secret : secrets) {
                QVERIFY(
                    !publicFailure.contains(secret));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TaskListModelTests)

#include "TaskListModelTests.moc"
