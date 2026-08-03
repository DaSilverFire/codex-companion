#include "codex/runtime/HistorySnapshotLoader.h"
#include "codex/runtime/CodexRuntime.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>

using namespace companion;

namespace {

class TestDatabase final {
public:
    explicit TestDatabase(const QString& path)
        : connectionName_(
              QStringLiteral("history-loader-")
              + QUuid::createUuid().toString(
                  QUuid::WithoutBraces)),
          database_(
              QSqlDatabase::addDatabase(
                  QStringLiteral("QSQLITE"),
                  connectionName_))
    {
        database_.setDatabaseName(path);
        if (!database_.open()) {
            qFatal(
                "could not open fixture database: %s",
                qPrintable(
                    database_.lastError().text()));
        }
    }

    ~TestDatabase()
    {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(
            connectionName_);
    }

    void execute(const QString& sql)
    {
        QSqlQuery query(database_);
        if (!query.exec(sql)) {
            qFatal(
                "fixture SQL failed: %s",
                qPrintable(
                    query.lastError().text()));
        }
    }

private:
    QString connectionName_;
    QSqlDatabase database_;
};

void createHistorySchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table threads (
            id text primary key,
            rollout_path text not null,
            updated_at integer not null,
            updated_at_ms integer,
            recency_at_ms integer,
            source text not null,
            cwd text not null,
            title text not null,
            first_user_message text not null,
            archived integer not null,
            preview text not null,
            model text,
            reasoning_effort text
        )
    )SQL"));
}

QByteArray messageLine(
    const QString& role,
    const QString& text)
{
    const QString fragmentType =
        role == QStringLiteral("assistant")
        ? QStringLiteral("output_text")
        : QStringLiteral("input_text");
    return QJsonDocument(
        QJsonObject{
            {
                QStringLiteral("timestamp"),
                QStringLiteral(
                    "2026-07-22T12:00:00.000Z"),
            },
            {
                QStringLiteral("type"),
                QStringLiteral("response_item"),
            },
            {
                QStringLiteral("payload"),
                QJsonObject{
                    {
                        QStringLiteral("type"),
                        QStringLiteral("message"),
                    },
                    {
                        QStringLiteral("role"),
                        role,
                    },
                    {
                        QStringLiteral("content"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral(
                                        "type"),
                                    fragmentType,
                                },
                                {
                                    QStringLiteral(
                                        "text"),
                                    text,
                                },
                            },
                        },
                    },
                },
            },
        })
        .toJson(QJsonDocument::Compact);
}

void writeRollout(const QString& path)
{
    QFile file(path);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        qFatal("could not create rollout fixture");
    }
    file.write(
        messageLine(
            QStringLiteral("user"),
            QStringLiteral("History request")));
    file.write("\n");
    file.write(
        messageLine(
            QStringLiteral("assistant"),
            QStringLiteral("History response")));
    file.write("\n");
}

CodexEnvironment environment(
    const QString& directory,
    const QString& databasePath)
{
    CodexEnvironment value;
    value.codexHome = directory;
    value.stateDatabase = databasePath;
    value.rolloutRoot = directory;
    return value;
}

HistoryKey key(const QString& threadId)
{
    return {
        threadId,
        std::nullopt,
        30,
    };
}

QDateTime fixedNow()
{
    return QDateTime(
        QDate(2026, 7, 22),
        QTime(12, 0),
        QTimeZone::UTC);
}

HistorySnapshot snapshot(const QString& marker)
{
    return {
        {
            BridgeMessage{
                marker,
                MessageRole::Assistant,
                marker,
                std::nullopt,
                std::nullopt,
            },
        },
        QStringLiteral("messages-") + marker,
        {},
        QStringLiteral("revision-") + marker,
        QStringLiteral("timeline-") + marker,
        {},
        std::nullopt,
    };
}

class ManualExecutor final {
public:
    RuntimeExecutor executor()
    {
        return [this](std::function<void()> worker) {
            const std::scoped_lock lock(mutex_);
            workers_.push_back(
                std::move(worker));
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
            const std::scoped_lock lock(mutex_);
            if (workers_.empty()) {
                qFatal("no queued runtime worker");
            }
            worker =
                std::move(workers_.front());
            workers_.pop_front();
        }
        std::thread thread(
            [worker = std::move(worker)]() mutable {
                worker();
            });
        thread.join();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> workers_;
};

RuntimeGoalLoader emptyGoalLoader()
{
    return [](
               const QVector<QString>&,
               std::stop_token) {
        return Result<
            QHash<
                QString,
                std::optional<BridgeGoal>>>::
            success({});
    };
}

QFuture<Result<BridgeUsageSnapshot>>
readyUsageFuture()
{
    auto promise = std::make_shared<
        QPromise<Result<BridgeUsageSnapshot>>>();
    promise->start();
    QFuture<Result<BridgeUsageSnapshot>> future =
        promise->future();
    promise->addResult(
        Result<BridgeUsageSnapshot>::success(
            {}));
    promise->finish();
    return future;
}

CodexRuntimeDependencies runtimeDependencies(
    RuntimeExecutor executor,
    RuntimeHistoryLoader historyLoader,
    std::shared_ptr<HistoryCoordinator>
        coordinator,
    RuntimeTaskLoader taskLoader = {})
{
    if (!taskLoader) {
        taskLoader =
            [](
                const QHash<
                    QString,
                    BridgeGoal>&,
                std::stop_token) {
                return Result<
                    CodexProcessSnapshot>::
                    success({});
            };
    }
    return {
        std::move(taskLoader),
        emptyGoalLoader(),
        std::move(executor),
        [] {
            return fixedNow();
        },
        CodexRuntimeHistoryDependencies{
            std::move(historyLoader),
            std::move(coordinator),
        },
        CodexRuntimeReadDependencies{
            [](
                const QString&,
                std::stop_token) {
                return Result<
                    BridgeCapabilities>::success(
                    {});
            },
            [] {
                return readyUsageFuture();
            },
        },
    };
}

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

} // namespace

class CodexRuntimeHistoryTests final
    : public QObject {
    Q_OBJECT

private slots:
    void productionLoaderReadsExactArchivedThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(
                QStringLiteral("state.sqlite"));
        const QString rolloutPath =
            directory.filePath(
                QStringLiteral("archived.jsonl"));
        writeRollout(rolloutPath);
        {
            TestDatabase database(databasePath);
            createHistorySchema(database);
            database.execute(
                QStringLiteral(
                    "insert into threads values "
                    "('archived', '%1', 1700000000, "
                    "null, null, 'desktop', 'C:\\work', "
                    "'Archived', 'Request', 1, '', "
                    "null, null)")
                    .arg(
                        QString(rolloutPath)
                            .replace(
                                QLatin1Char('\''),
                                QStringLiteral("''"))));
        }
        HistorySnapshotLoader loader(
            environment(
                directory.path(),
                databasePath));

        const auto result = loader.load(
            key(QStringLiteral("archived")),
            {},
            fixedNow(),
            {});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().messages.size(), 2);
        QCOMPARE(
            result.value().messages.first().text,
            QStringLiteral("History request"));
        QCOMPARE(
            result.value().messages.last().text,
            QStringLiteral("History response"));
        QVERIFY(!result.value().timelineItems.isEmpty());
        QVERIFY(result.value().subagents.isEmpty());
    }

    void productionLoaderSanitizesMissingThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(
                QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath);
            createHistorySchema(database);
        }
        HistorySnapshotLoader loader(
            environment(
                directory.path(),
                databasePath));

        const auto result = loader.load(
            key(QStringLiteral("missing")),
            {},
            fixedNow(),
            {});

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.history_load_failed"));
        QCOMPARE(
            result.error().message,
            QStringLiteral(
                "Could not load Codex task history."));
        QVERIFY(result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
    }

    void productionLoaderHonorsPreCanceledToken()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        HistorySnapshotLoader loader(
            environment(
                directory.path(),
                directory.filePath(
                    QStringLiteral(
                        "missing.sqlite"))));
        std::stop_source stopSource;
        stopSource.request_stop();

        const auto result = loader.load(
            key(QStringLiteral("thread")),
            {},
            fixedNow(),
            stopSource.get_token());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QVERIFY(result.error().context.isEmpty());
    }

    void d2SurfaceBindsExactlyFourReadCommands()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                [](const HistoryKey&,
                   const QSet<QString>&,
                   const QDateTime&,
                   std::stop_token) {
                    return Result<
                        HistorySnapshot>::success(
                        snapshot(
                            QStringLiteral(
                                "history")));
                },
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("codex.refresh"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QVERIFY(finishedSpy.at(1).at(1).toBool());

        bus.execute(
            QStringLiteral(
                "codex.capabilities.load"));
        QCOMPARE(executor.pendingCount(), 1);
        executor.runNext();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            5000);
        QVERIFY(finishedSpy.at(2).at(1).toBool());

        bus.execute(
            QStringLiteral("codex.usage.load"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            4,
            5000);
        QVERIFY(finishedSpy.at(3).at(1).toBool());

        bus.execute(
            QStringLiteral("codex.reply"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            5,
            5000);
        QVERIFY(!finishedSpy.at(4).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(4).at(2).toString(),
            QStringLiteral("ui.unknown_command"));
    }

    void newerSelectedThreadSuppressesOlderPublication()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore oldEntered;
        QSemaphore newEntered;
        QSemaphore oldRelease;
        QSemaphore newRelease;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey& historyKey,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token) {
                if (historyKey.threadId
                    == QStringLiteral("old")) {
                    oldEntered.release();
                    oldRelease.acquire();
                    return Result<HistorySnapshot>::
                        success(
                            snapshot(
                                QStringLiteral("old")));
                }
                newEntered.release();
                newRelease.acquire();
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("new")));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy historySpy(
            &runtime,
            &CodexRuntime::historyChanged);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("old"),
                },
            });
        QVERIFY(oldEntered.tryAcquire(1, 5000));
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("new"),
                },
            });
        QVERIFY(newEntered.tryAcquire(1, 5000));

        QCOMPARE(historySpy.size(), 2);
        QVERIFY(runtime.historyLoading());
        QCOMPARE(
            runtime.selectedHistoryThreadId(),
            QStringLiteral("new"));
        QVERIFY(
            !runtime.historyPublication()
                 .has_value());

        oldRelease.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QCOMPARE(historySpy.size(), 2);
        QVERIFY(runtime.historyLoading());
        QVERIFY(
            !runtime.historyPublication()
                 .has_value());

        newRelease.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            historySpy.size(),
            3,
            5000);
        QVERIFY(!runtime.historyLoading());
        QVERIFY(
            runtime.historyPublication()
                .has_value());
        QCOMPARE(
            runtime.historyPublication()
                ->threadId,
            QStringLiteral("new"));
        QCOMPARE(
            runtime.historyPublication()
                ->snapshot.messages.first().text,
            QStringLiteral("new"));
    }

    void identicalRequestsCoalesceAndCacheCompletesAsynchronously()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        std::atomic_int loaderCalls = 0;
        QSemaphore entered;
        QSemaphore release;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey&,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                entered.release();
                release.acquire();
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("shared")));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy historySpy(
            &runtime,
            &CodexRuntime::historyChanged);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        const QVariantMap arguments{
            {
                QStringLiteral("threadId"),
                QStringLiteral("thread"),
            },
        };
        bus.execute(
            QStringLiteral("codex.history.load"),
            arguments);
        QVERIFY(entered.tryAcquire(1, 5000));
        bus.execute(
            QStringLiteral("codex.history.load"),
            arguments);
        QCOMPARE(loaderCalls.load(), 1);
        QCOMPARE(historySpy.size(), 2);

        release.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            historySpy.size(),
            3,
            5000);
        QCOMPARE(loaderCalls.load(), 1);
        QVERIFY(
            runtime.historyPublication()
                .has_value());

        bus.execute(
            QStringLiteral("codex.history.load"),
            arguments);
        QCOMPARE(finishedSpy.size(), 2);
        QCOMPARE(historySpy.size(), 4);
        QVERIFY(runtime.historyLoading());
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            historySpy.size(),
            5,
            5000);
        QCOMPARE(loaderCalls.load(), 1);
        QVERIFY(!runtime.historyLoading());
    }

    void sameThreadNewerCursorSuppressesOlderPublication()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore olderEntered;
        QSemaphore newerEntered;
        QSemaphore olderRelease;
        QSemaphore newerRelease;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey& historyKey,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token) {
                if (historyKey.cursor
                        == std::optional<QString>(
                            QStringLiteral("older"))) {
                    olderEntered.release();
                    olderRelease.acquire();
                    return Result<HistorySnapshot>::
                        success(
                            snapshot(
                                QStringLiteral("older")));
                }
                newerEntered.release();
                newerRelease.acquire();
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("newer")));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy historySpy(
            &runtime,
            &CodexRuntime::historyChanged);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("cursor"),
                    QStringLiteral("older"),
                },
            });
        QVERIFY(
            olderEntered.tryAcquire(1, 5000));
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("cursor"),
                    QStringLiteral("newer"),
                },
            });
        QVERIFY(
            newerEntered.tryAcquire(1, 5000));
        QCOMPARE(historySpy.size(), 2);

        olderRelease.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QCOMPARE(historySpy.size(), 2);
        QVERIFY(runtime.historyLoading());
        QVERIFY(
            !runtime.historyPublication()
                 .has_value());

        newerRelease.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            historySpy.size(),
            3,
            5000);
        QVERIFY(
            runtime.historyPublication()
                .has_value());
        QCOMPARE(
            runtime.historyPublication()
                ->threadId,
            QStringLiteral("thread"));
        QCOMPARE(
            runtime.historyPublication()
                ->requestCursor,
            std::optional<QString>(
                QStringLiteral("newer")));
        QCOMPARE(
            runtime.historyPublication()
                ->snapshot.messages.first().text,
            QStringLiteral("newer"));
    }

    void argumentsAreStrictAndLimitIsClamped()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        std::atomic_int loaderCalls = 0;
        std::mutex observedMutex;
        std::optional<HistoryKey> observedKey;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey& historyKey,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token) {
                loaderCalls.fetch_add(1);
                {
                    const std::scoped_lock lock(
                        observedMutex);
                    observedKey = historyKey;
                }
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("valid")));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy historySpy(
            &runtime,
            &CodexRuntime::historyChanged);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        const QVector<QVariantMap> invalidArguments{
            {},
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("   "),
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    42,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("unknown"),
                    QStringLiteral("private"),
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("cursor"),
                    12,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    QStringLiteral("30"),
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    true,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    0,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    -1,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    1.5,
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    std::numeric_limits<
                        double>::quiet_NaN(),
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    std::numeric_limits<
                        double>::infinity(),
                },
            },
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("limit"),
                    static_cast<qulonglong>(
                        std::numeric_limits<
                            int>::max())
                        + 1ULL,
                },
            },
        };
        for (qsizetype index = 0;
             index < invalidArguments.size();
             ++index) {
            bus.execute(
                QStringLiteral(
                    "codex.history.load"),
                invalidArguments.at(index));
            QTRY_COMPARE_WITH_TIMEOUT(
                finishedSpy.size(),
                index + 1,
                5000);
            const QList<QVariant> result =
                finishedSpy.at(index);
            QVERIFY(!result.at(1).toBool());
            QCOMPARE(
                result.at(2).toString(),
                QStringLiteral(
                    "codex.command_invalid_arguments"));
            QCOMPARE(
                result.at(3).toString(),
                QStringLiteral(
                    "Invalid Codex command arguments."));
        }
        QCOMPARE(loaderCalls.load(), 0);
        QCOMPARE(historySpy.size(), 0);

        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("  thread  "),
                },
                {
                    QStringLiteral("cursor"),
                    QString(),
                },
                {
                    QStringLiteral("limit"),
                    500.0,
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            invalidArguments.size() + 1,
            5000);
        QCOMPARE(loaderCalls.load(), 1);
        HistoryKey captured;
        {
            const std::scoped_lock lock(
                observedMutex);
            QVERIFY(observedKey.has_value());
            captured = *observedKey;
        }
        QCOMPARE(
            captured.threadId,
            QStringLiteral("thread"));
        QVERIFY(captured.cursor.has_value());
        QVERIFY(captured.cursor->isEmpty());
        QCOMPARE(
            captured.limit,
            static_cast<int>(
                kMaximumPageSize));
        QVERIFY(
            runtime.historyPublication()
                .has_value());
        QCOMPARE(
            runtime.historyPublication()
                ->requestCursor,
            std::optional<QString>(
                QString()));
    }

    void failureRetainsPublicationAndSanitizesState()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        const RuntimeHistoryLoader historyLoader =
            [](const HistoryKey& historyKey,
               const QSet<QString>&,
               const QDateTime&,
               std::stop_token) {
                if (!historyKey.cursor.has_value()) {
                    return Result<HistorySnapshot>::
                        success(
                            snapshot(
                                QStringLiteral("kept")));
                }
                return Result<HistorySnapshot>::
                    failure({
                        QStringLiteral(
                            "private.failure"),
                        QStringLiteral(
                            "C:/private/rollout.jsonl secret"),
                        false,
                        {
                            {
                                QStringLiteral("path"),
                                QStringLiteral(
                                    "C:/private/rollout.jsonl"),
                            },
                        },
                    });
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(
            runtime.historyPublication()
                .has_value());
        const CodexHistoryPublication kept =
            *runtime.historyPublication();

        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
                {
                    QStringLiteral("cursor"),
                    QStringLiteral("older"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            5000);
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(1).at(2).toString(),
            QStringLiteral(
                "codex.history_load_failed"));
        QCOMPARE(
            finishedSpy.at(1).at(3).toString(),
            QStringLiteral(
                "Could not load Codex task history."));
        QCOMPARE(
            runtime.historyPublication(),
            std::optional<
                CodexHistoryPublication>(kept));
        QCOMPARE(
            runtime.historyErrorCode(),
            QStringLiteral(
                "codex.history_load_failed"));
        QCOMPARE(
            runtime.historyErrorMessage(),
            QStringLiteral(
                "Could not load Codex task history."));
        QVERIFY(
            !runtime.historyErrorMessage()
                 .contains(
                     QStringLiteral("private")));
    }

    void pendingApprovalsAreCopiedOffOwnerThread()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        const QThread* const ownerThread =
            QThread::currentThread();
        std::atomic_bool ranOffOwner = false;
        std::mutex observedMutex;
        QSet<QString> observedApprovals;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey&,
                const QSet<QString>& approvals,
                const QDateTime&,
                std::stop_token) {
                ranOffOwner.store(
                    QThread::currentThread()
                    != ownerThread);
                {
                    const std::scoped_lock lock(
                        observedMutex);
                    observedApprovals = approvals;
                }
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("copied")));
            };
        const RuntimeTaskLoader taskLoader =
            [](
                const QHash<QString, BridgeGoal>&,
                std::stop_token) {
                CodexProcessSnapshot result;
                result.pendingApprovals.insert(
                    QStringLiteral(
                        "approval-thread"),
                    PendingApproval{
                        QStringLiteral(
                            "approval-thread"),
                        42,
                        PendingApprovalMethod::
                            CommandExecution,
                        std::nullopt,
                    });
                return Result<
                    CodexProcessSnapshot>::
                    success(
                        std::move(result));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator,
                taskLoader),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        bool signalStayedOnOwner = true;
        QObject::connect(
            &runtime,
            &CodexRuntime::historyChanged,
            &runtime,
            [&] {
                signalStayedOnOwner =
                    signalStayedOnOwner
                    && QThread::currentThread()
                        == ownerThread;
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);

        QVERIFY(ranOffOwner.load());
        QVERIFY(signalStayedOnOwner);
        {
            const std::scoped_lock lock(
                observedMutex);
            const QSet<QString> expected{
                QStringLiteral(
                    "approval-thread"),
            };
            QCOMPARE(
                observedApprovals,
                expected);
        }
    }

    void stoppingOneRuntimeKeepsSharedLoadAlive()
    {
        CompanionState firstState;
        CompanionState secondState;
        CompanionCommandBus firstBus;
        CompanionCommandBus secondBus;
        ManualExecutor firstExecutor;
        ManualExecutor secondExecutor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        std::atomic_int loaderCalls = 0;
        std::atomic_int stopCallbacks = 0;
        QSemaphore entered;
        QSemaphore release;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey&,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token stopToken) {
                loaderCalls.fetch_add(1);
                std::stop_callback callback(
                    stopToken,
                    [&] {
                        stopCallbacks.fetch_add(1);
                    });
                entered.release();
                release.acquire();
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("shared")));
            };
        const auto firstHost =
            std::make_shared<
                RuntimeContinuationHost>();
        const auto secondHost =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime firstRuntime(
            firstState,
            firstBus,
            runtimeDependencies(
                firstExecutor.executor(),
                historyLoader,
                coordinator),
            firstHost);
        CodexRuntime secondRuntime(
            secondState,
            secondBus,
            runtimeDependencies(
                secondExecutor.executor(),
                historyLoader,
                coordinator),
            secondHost);
        QVERIFY(firstRuntime.start().hasValue());
        QVERIFY(secondRuntime.start().hasValue());
        firstExecutor.runNext();
        secondExecutor.runNext();
        QVERIFY(waitUntil([&] {
            return !firstRuntime.loading()
                && !secondRuntime.loading();
        }));

        QSignalSpy firstFinished(
            &firstBus,
            &CompanionCommandBus::commandFinished);
        QSignalSpy secondFinished(
            &secondBus,
            &CompanionCommandBus::commandFinished);
        const QVariantMap arguments{
            {
                QStringLiteral("threadId"),
                QStringLiteral("thread"),
            },
        };
        firstBus.execute(
            QStringLiteral("codex.history.load"),
            arguments);
        QVERIFY(entered.tryAcquire(1, 5000));
        secondBus.execute(
            QStringLiteral("codex.history.load"),
            arguments);
        QCOMPARE(loaderCalls.load(), 1);

        firstRuntime.stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            firstFinished.size(),
            1,
            5000);
        QCOMPARE(stopCallbacks.load(), 0);
        QVERIFY(secondRuntime.historyLoading());

        release.release();
        QTRY_COMPARE_WITH_TIMEOUT(
            secondFinished.size(),
            1,
            5000);
        QVERIFY(
            secondFinished.at(0).at(1).toBool());
        QCOMPARE(stopCallbacks.load(), 0);
        QVERIFY(
            secondRuntime.historyPublication()
                .has_value());
        QCOMPARE(
            secondRuntime.historyPublication()
                ->snapshot.messages.first().text,
            QStringLiteral("shared"));
    }

    void reentrantStopFromHistoryStartPreventsWorkerLaunch()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        std::atomic_int loaderCalls = 0;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                [&](const HistoryKey&,
                    const QSet<QString>&,
                    const QDateTime&,
                    std::stop_token) {
                    loaderCalls.fetch_add(1);
                    return Result<
                        HistorySnapshot>::success(
                        snapshot(
                            QStringLiteral(
                                "unexpected")));
                },
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QObject::connect(
            &runtime,
            &CodexRuntime::historyChanged,
            &runtime,
            [&] {
                if (runtime.historyLoading()) {
                    runtime.stop();
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QCOMPARE(loaderCalls.load(), 0);
        QVERIFY(!runtime.running());
        QVERIFY(!runtime.historyLoading());
    }

    void reentrantStopAfterPublicationKeepsCompletedResult()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                [](const HistoryKey&,
                   const QSet<QString>&,
                   const QDateTime&,
                   std::stop_token) {
                    return Result<
                        HistorySnapshot>::success(
                        snapshot(
                            QStringLiteral(
                                "published")));
                },
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QObject::connect(
            &runtime,
            &CodexRuntime::historyChanged,
            &runtime,
            [&] {
                if (!runtime.historyLoading()
                    && runtime
                           .historyPublication()
                           .has_value()) {
                    runtime.stop();
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QVERIFY(!runtime.running());
        QVERIFY(
            runtime.historyPublication()
                .has_value());
        QCOMPARE(
            runtime.historyPublication()
                ->snapshot.messages.first().text,
            QStringLiteral("published"));
    }

    void runtimeDestructionCompletesWaiterAndDropsLateResult()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore entered;
        QSemaphore stopped;
        QSemaphore release;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        auto runtime =
            std::make_unique<CodexRuntime>(
                state,
                bus,
                runtimeDependencies(
                    executor.executor(),
                    [&](const HistoryKey&,
                        const QSet<QString>&,
                        const QDateTime&,
                        std::stop_token stopToken) {
                        std::stop_callback callback(
                            stopToken,
                            [&] {
                                stopped.release();
                            });
                        entered.release();
                        release.acquire();
                        return Result<
                            HistorySnapshot>::success(
                            snapshot(
                                QStringLiteral(
                                    "late")));
                    },
                    coordinator),
                host);
        QVERIFY(runtime->start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime->loading();
        }));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QVERIFY(entered.tryAcquire(1, 5000));

        runtime.reset();

        QVERIFY(stopped.tryAcquire(1, 5000));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));

        release.release();
        host->stopAcceptingAndDrain();
        QCoreApplication::processEvents();
        QCOMPARE(finishedSpy.size(), 1);
    }

    void destroyedStateSuppressesHistoryPublication()
    {
        auto state =
            std::make_unique<CompanionState>();
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore entered;
        QSemaphore release;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        auto runtime =
            std::make_unique<CodexRuntime>(
                *state,
                bus,
                runtimeDependencies(
                    executor.executor(),
                    [&](const HistoryKey&,
                        const QSet<QString>&,
                        const QDateTime&,
                        std::stop_token) {
                        entered.release();
                        release.acquire();
                        return Result<
                            HistorySnapshot>::success(
                            snapshot(
                                QStringLiteral(
                                    "late")));
                    },
                    coordinator),
                host);
        QVERIFY(runtime->start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime->loading();
        }));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QVERIFY(entered.tryAcquire(1, 5000));

        state.reset();
        release.release();

        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!runtime->running());
        QVERIFY(
            !runtime->historyPublication()
                 .has_value());
    }

    void destroyedCommandBusSuppressesHistoryPublication()
    {
        CompanionState state;
        auto bus =
            std::make_unique<
                CompanionCommandBus>();
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore entered;
        QSemaphore release;
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        auto runtime =
            std::make_unique<CodexRuntime>(
                state,
                *bus,
                runtimeDependencies(
                    executor.executor(),
                    [&](const HistoryKey&,
                        const QSet<QString>&,
                        const QDateTime&,
                        std::stop_token) {
                        entered.release();
                        release.acquire();
                        return Result<
                            HistorySnapshot>::success(
                            snapshot(
                                QStringLiteral(
                                    "late")));
                    },
                    coordinator),
                host);
        QVERIFY(runtime->start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime->loading();
        }));

        bus->execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QVERIFY(entered.tryAcquire(1, 5000));

        bus.reset();
        release.release();

        QVERIFY(waitUntil([&runtime] {
            return !runtime->running();
        }));
        QVERIFY(
            !runtime->historyPublication()
                 .has_value());
        QCOMPARE(
            runtime->errorCode(),
            QStringLiteral(
                "codex.runtime_unavailable"));
    }

    void runtimeStopCompletesHistoryUnavailableAndIgnoresLateSuccess()
    {
        CompanionState state;
        CompanionCommandBus bus;
        ManualExecutor executor;
        const auto coordinator =
            std::make_shared<HistoryCoordinator>();
        QSemaphore entered;
        QSemaphore stopped;
        QSemaphore release;
        const RuntimeHistoryLoader historyLoader =
            [&](const HistoryKey&,
                const QSet<QString>&,
                const QDateTime&,
                std::stop_token stopToken) {
                std::stop_callback callback(
                    stopToken,
                    [&] {
                        stopped.release();
                    });
                entered.release();
                release.acquire();
                return Result<HistorySnapshot>::
                    success(
                        snapshot(
                            QStringLiteral("late")));
            };
        const auto host =
            std::make_shared<
                RuntimeContinuationHost>();
        CodexRuntime runtime(
            state,
            bus,
            runtimeDependencies(
                executor.executor(),
                historyLoader,
                coordinator),
            host);
        QVERIFY(runtime.start().hasValue());
        executor.runNext();
        QVERIFY(waitUntil([&runtime] {
            return !runtime.loading();
        }));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(
            QStringLiteral("codex.history.load"),
            {
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread"),
                },
            });
        QVERIFY(entered.tryAcquire(1, 5000));
        QVERIFY(runtime.historyLoading());

        runtime.stop();

        QVERIFY(stopped.tryAcquire(1, 5000));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            5000);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
        QCOMPARE(
            finishedSpy.at(0).at(2).toString(),
            QStringLiteral(
                "codex.runtime_unavailable"));
        QVERIFY(!runtime.historyLoading());
        QVERIFY(
            !runtime.historyPublication()
                 .has_value());

        release.release();
        QTest::qWait(50);
        QCoreApplication::processEvents();
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(
            !runtime.historyPublication()
                 .has_value());
    }
};

QTEST_GUILESS_MAIN(CodexRuntimeHistoryTests)

#include "CodexRuntimeHistoryTests.moc"
