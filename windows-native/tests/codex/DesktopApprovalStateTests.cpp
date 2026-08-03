#include "codex/discovery/CodexEnvironment.h"
#include "codex/ipc/FollowerRequestFactory.h"
#include "codex/state/DesktopApprovalState.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <chrono>
#include <future>
#include <utility>
#include <vector>

using namespace companion;

namespace {

constexpr auto kMaximumFileAge = std::chrono::hours(48);
constexpr qsizetype kFullHistoryPaddingBytes =
    5 * 1024 * 1024 + 64 * 1024;

QDateTime utcDate(
    int year,
    int month,
    int day,
    int hour = 0,
    int minute = 0,
    int second = 0)
{
    return QDateTime(
        QDate(year, month, day),
        QTime(hour, minute, second),
        QTimeZone::UTC);
}

QByteArray showLine(
    QString timestamp,
    QString threadId,
    qint64 requestId,
    QString kind)
{
    return QStringLiteral(
               "%1 [desktop-notifications] show approval "
               "conversationId=%2 requestId=%3 kind=%4")
        .arg(
            std::move(timestamp),
            std::move(threadId),
            QString::number(requestId),
            std::move(kind))
        .toUtf8();
}

QByteArray responseLine(
    QString timestamp,
    qint64 requestId,
    QString method)
{
    return QStringLiteral(
               "%1 Sending server response id=%2 method=%3")
        .arg(
            std::move(timestamp),
            QString::number(requestId),
            std::move(method))
        .toUtf8();
}

QByteArray joinedLines(
    const QVector<QByteArray>& lines,
    bool trailingNewline = true)
{
    QByteArray bytes;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            bytes.append('\n');
        }
        bytes.append(lines.at(index));
    }
    if (trailingNewline) {
        bytes.append('\n');
    }
    return bytes;
}

void writeFile(
    const QString& path,
    QByteArrayView bytes,
    const QDateTime& modifiedAt)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        qFatal("could not create log fixture directory");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("could not open log fixture");
    }
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
        qFatal("could not write log fixture");
    }
    if (!file.flush()) {
        qFatal("could not flush log fixture");
    }
    if (!file.setFileTime(
            modifiedAt,
            QFileDevice::FileModificationTime)) {
        qFatal("could not set log fixture modification time");
    }
}

void appendFile(
    const QString& path,
    QByteArrayView bytes,
    const QDateTime& modifiedAt)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qFatal("could not append log fixture");
    }
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
        qFatal("could not finish appended log fixture");
    }
    if (!file.flush()) {
        qFatal("could not flush appended log fixture");
    }
    if (!file.setFileTime(
            modifiedAt,
            QFileDevice::FileModificationTime)) {
        qFatal("could not update log fixture modification time");
    }
}

QString stableLogsRoot(const QString& localAppData)
{
    return QDir(localAppData).filePath(QStringLiteral(
        "Packages/OpenAI.Codex_2p2nqsd0c76g0/"
        "LocalCache/Local/Codex/Logs"));
}

QString betaLogsRoot(const QString& localAppData)
{
    return QDir(localAppData).filePath(QStringLiteral(
        "Packages/OpenAI.CodexBeta_2p2nqsd0c76g0/"
        "LocalCache/Local/Codex/Logs"));
}

const PendingApproval& approvalFor(
    const TaskProjectionState& state,
    QStringView threadId)
{
    const auto iterator =
        state.pendingApprovals.constFind(threadId.toString());
    if (iterator == state.pendingApprovals.constEnd()) {
        qFatal("missing expected pending approval");
    }
    return iterator.value();
}

} // namespace

class DesktopApprovalStateTests final : public QObject {
    Q_OBJECT

private slots:
    void parsesCrossMethodResponsesByRequestId()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        const QString logPath = directory.filePath(
            QStringLiteral("session-a-t0-main.log"));
        writeFile(
            logPath,
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:00.000Z"),
                    QStringLiteral("thread-command"),
                    41,
                    QStringLiteral("commandExecution")),
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("thread-file"),
                    42,
                    QStringLiteral("fileChange")),
                responseLine(
                    QStringLiteral("2026-07-22T11:59:02.000Z"),
                    41,
                    QStringLiteral(
                        "item/fileChange/requestApproval")),
            }),
            now.addSecs(-1));

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        const TaskProjectionState first = store.snapshot(now);

        QVERIFY(!first.pendingApprovals.contains(
            QStringLiteral("thread-command")));
        QVERIFY(first.pendingApprovals.contains(
            QStringLiteral("thread-file")));
        QCOMPARE(
            static_cast<int>(
                approvalFor(first, u"thread-file").method),
            static_cast<int>(PendingApprovalMethod::FileChange));
        QCOMPARE(
            first.pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("thread-file")});
        QCOMPARE(
            first.attentionPromotedThreadIds,
            first.pendingApprovalThreadIds);

        appendFile(
            logPath,
            joinedLines({
                responseLine(
                    QStringLiteral("2026-07-22T11:59:03.000Z"),
                    42,
                    QStringLiteral(
                        "item/commandExecution/requestApproval")),
            }),
            now);
        const TaskProjectionState second =
            store.snapshot(now.addSecs(1));

        QVERIFY(second.pendingApprovals.isEmpty());
        QVERIFY(second.pendingApprovalThreadIds.isEmpty());
        QCOMPARE(
            second.attentionPromotedThreadIds,
            QSet<QString>{QStringLiteral("thread-file")});
    }

    void newerRequestReplacesPreviousRequestForThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        writeFile(
            directory.filePath(
                QStringLiteral("session-b-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:00.000Z"),
                    QStringLiteral("thread-one"),
                    10,
                    QStringLiteral("commandExecution")),
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("thread-one"),
                    11,
                    QStringLiteral("fileChange")),
                responseLine(
                    QStringLiteral("2026-07-22T11:59:02.000Z"),
                    10,
                    QStringLiteral(
                        "item/commandExecution/requestApproval")),
            }),
            now);

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        const TaskProjectionState state = store.snapshot(now);

        QCOMPARE(state.pendingApprovals.size(), 1);
        const PendingApproval& pending =
            approvalFor(state, u"thread-one");
        QCOMPARE(pending.requestId, qint64(11));
        QCOMPARE(
            static_cast<int>(pending.method),
            static_cast<int>(PendingApprovalMethod::FileChange));
    }

    void ignoresMalformedUnsupportedAndUnrelatedLines()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        writeFile(
            directory.filePath(
                QStringLiteral("session-c-t0-main.log")),
            joinedLines({
                QByteArrayLiteral("unrelated private log content"),
                QByteArrayLiteral(
                    "2026-07-22T11:59:00.000Z "
                    "[desktop-notifications] show approval "
                    "conversationId=missing-request kind=fileChange"),
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("unsupported"),
                    90,
                    QStringLiteral("networkAccess")),
                showLine(
                    QStringLiteral("2026-07-22T11:59:02.000Z"),
                    QStringLiteral("valid"),
                    91,
                    QStringLiteral("commandExecution")),
                responseLine(
                    QStringLiteral("2026-07-22T11:59:03.000Z"),
                    91,
                    QStringLiteral("other/method")),
            }),
            now);

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        const TaskProjectionState state = store.snapshot(now);

        QCOMPARE(state.pendingApprovals.size(), 1);
        QVERIFY(state.pendingApprovals.contains(
            QStringLiteral("valid")));
        QCOMPARE(
            approvalFor(state, u"valid").requestId,
            qint64(91));
    }

    void environmentConstructorUsesExactStableAndBetaRoots()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);

        writeFile(
            QDir(stableLogsRoot(directory.path())).filePath(
                QStringLiteral("stable-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:58:00.000Z"),
                    QStringLiteral("stable-thread"),
                    1,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(-120));
        writeFile(
            QDir(betaLogsRoot(directory.path())).filePath(
                QStringLiteral("beta-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:00.000Z"),
                    QStringLiteral("beta-thread"),
                    2,
                    QStringLiteral("fileChange")),
            }),
            now.addSecs(-60));

        CodexEnvironment environment;
        environment.localAppData = directory.path();
        DesktopApprovalStateStore store(environment);
        const TaskProjectionState state = store.snapshot(now);

        QCOMPARE(
            state.pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("beta-thread")});
    }

    void selectsOnlyEligibleFilesFromNewestRootSession()
    {
        QTemporaryDir stable;
        QTemporaryDir beta;
        QVERIFY(stable.isValid());
        QVERIFY(beta.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);

        writeFile(
            stable.filePath(QStringLiteral("stable-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:50:00.000Z"),
                    QStringLiteral("stable-thread"),
                    1,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(-120));
        writeFile(
            beta.filePath(
                QStringLiteral("nested/selected-t0-first.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:58:00.000Z"),
                    QStringLiteral("selected-first"),
                    2,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(-20));
        writeFile(
            beta.filePath(
                QStringLiteral("selected-t0-second.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:00.000Z"),
                    QStringLiteral("selected-second"),
                    3,
                    QStringLiteral("fileChange")),
            }),
            now.addSecs(-10));
        writeFile(
            beta.filePath(
                QStringLiteral("other-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:57:00.000Z"),
                    QStringLiteral("other-session"),
                    4,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(-30));
        writeFile(
            beta.filePath(
                QStringLiteral("selected-t0-uppercase.LOG")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:10.000Z"),
                    QStringLiteral("uppercase"),
                    5,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(10));
        writeFile(
            beta.filePath(
                QStringLiteral("selected-t1-secondary.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:11.000Z"),
                    QStringLiteral("secondary"),
                    6,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(11));
        writeFile(
            beta.filePath(
                QStringLiteral(".hidden/selected-t0-hidden.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:12.000Z"),
                    QStringLiteral("hidden"),
                    7,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(12));

        DesktopApprovalStateStore store(
            {stable.path(), beta.path()},
            kMaximumFileAge);
        const TaskProjectionState state = store.snapshot(now);

        QCOMPARE(
            state.pendingApprovalThreadIds,
            QSet<QString>({
                QStringLiteral("selected-first"),
                QStringLiteral("selected-second"),
            }));
    }

    void sortsRotatedLogEventsByTimestampAcrossModificationOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);

        writeFile(
            directory.filePath(
                QStringLiteral("rotated-t0-response.log")),
            joinedLines({
                responseLine(
                    QStringLiteral("2026-07-22T11:59:02.000Z"),
                    92,
                    QStringLiteral(
                        "item/commandExecution/requestApproval")),
            }),
            now.addSecs(-20));
        writeFile(
            directory.filePath(
                QStringLiteral("rotated-t0-show.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("rotated-thread"),
                    92,
                    QStringLiteral("commandExecution")),
            }),
            now.addSecs(-10));

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        const TaskProjectionState state = store.snapshot(now);

        QVERIFY(state.pendingApprovals.isEmpty());
        QVERIFY(state.pendingApprovalThreadIds.isEmpty());
    }

    void includesExactAgeBoundaryAndRejectsOlderFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        const qint64 maximumAgeMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kMaximumFileAge)
                .count();

        writeFile(
            directory.filePath(
                QStringLiteral("boundary-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-20T12:00:00.000Z"),
                    QStringLiteral("boundary"),
                    8,
                    QStringLiteral("commandExecution")),
            }),
            now.addMSecs(-maximumAgeMilliseconds));
        writeFile(
            directory.filePath(
                QStringLiteral("older-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-20T11:59:59.000Z"),
                    QStringLiteral("older"),
                    9,
                    QStringLiteral("commandExecution")),
            }),
            now.addMSecs(-maximumAgeMilliseconds - 1));

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        const TaskProjectionState state = store.snapshot(now);

        QCOMPARE(
            state.pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("boundary")});
    }

    void retainsUnfinishedLinesAndResetsAfterTruncation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        const QString logPath = directory.filePath(
            QStringLiteral("incremental-t0-main.log"));
        const QByteArray unfinished = showLine(
            QStringLiteral("2026-07-22T11:59:00.000Z"),
            QStringLiteral("unfinished-thread-with-a-long-name"),
            100,
            QStringLiteral("commandExecution"));
        writeFile(logPath, unfinished, now.addSecs(-2));

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        QVERIFY(store.snapshot(now).pendingApprovals.isEmpty());

        appendFile(
            logPath,
            QByteArrayView("\n", 1),
            now.addSecs(-1));
        const TaskProjectionState completed = store.snapshot(now);
        QVERIFY(completed.pendingApprovals.contains(
            QStringLiteral(
                "unfinished-thread-with-a-long-name")));

        writeFile(
            logPath,
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("new"),
                    101,
                    QStringLiteral("fileChange")),
            }),
            now);
        const TaskProjectionState truncated =
            store.snapshot(now.addSecs(1));

        QCOMPARE(
            truncated.pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("new")});
    }

    void zeroLengthTruncationResetsSameSizeRewrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        const QString logPath = directory.filePath(
            QStringLiteral("same-size-t0-main.log"));
        const QByteArray oldLine = joinedLines({
            showLine(
                QStringLiteral("2026-07-22T11:59:00.000Z"),
                QStringLiteral("thread-old"),
                201,
                QStringLiteral("commandExecution")),
        });
        const QByteArray newLine = joinedLines({
            showLine(
                QStringLiteral("2026-07-22T11:59:00.000Z"),
                QStringLiteral("thread-new"),
                202,
                QStringLiteral("commandExecution")),
        });
        QCOMPARE(oldLine.size(), newLine.size());
        writeFile(logPath, oldLine, now.addSecs(-2));

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        QCOMPARE(
            store.snapshot(now).pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("thread-old")});

        writeFile(logPath, QByteArrayView(), now.addSecs(-1));
        QVERIFY(store.snapshot(now).pendingApprovals.isEmpty());

        writeFile(logPath, newLine, now);
        QCOMPARE(
            store.snapshot(now).pendingApprovalThreadIds,
            QSet<QString>{QStringLiteral("thread-new")});
    }

    void scansCompleteHistoryBeyondFormerTailWindowAndCapsEventsPerFile()
    {
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            QByteArray bytes = joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:00:00.000Z"),
                    QStringLiteral("early"),
                    1,
                    QStringLiteral("commandExecution")),
            });
            bytes.append(
                QByteArray(kFullHistoryPaddingBytes, 'x'));
            bytes.append('\n');
            bytes.append(showLine(
                QStringLiteral("2026-07-22T11:59:00.000Z"),
                QStringLiteral("late"),
                2,
                QStringLiteral("fileChange")));
            bytes.append('\n');
            QVERIFY(
                bytes.size()
                > 5 * 1024 * 1024);
            writeFile(
                directory.filePath(
                    QStringLiteral("history-t0-main.log")),
                bytes,
                now);

            DesktopApprovalStateStore store(
                {directory.path()},
                kMaximumFileAge);
            const TaskProjectionState state = store.snapshot(now);
            QCOMPARE(
                state.pendingApprovalThreadIds,
                QSet<QString>({
                    QStringLiteral("early"),
                    QStringLiteral("late"),
                }));
        }

        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            QVector<QByteArray> lines;
            lines.reserve(4097);
            for (int index = 0; index < 4097; ++index) {
                lines.append(showLine(
                    QStringLiteral(
                        "2026-07-22T11:%1:%2.000Z")
                        .arg(
                            index / 60,
                            2,
                            10,
                            QLatin1Char('0'))
                        .arg(
                            index % 60,
                            2,
                            10,
                            QLatin1Char('0')),
                    QStringLiteral("thread-%1")
                        .arg(index, 4, 10, QLatin1Char('0')),
                    index,
                    QStringLiteral("commandExecution")));
            }
            writeFile(
                directory.filePath(
                    QStringLiteral("cap-t0-main.log")),
                joinedLines(lines),
                now);

            DesktopApprovalStateStore store(
                {directory.path()},
                kMaximumFileAge);
            const TaskProjectionState state = store.snapshot(now);
            QCOMPARE(state.pendingApprovals.size(), 4096);
            QVERIFY(!state.pendingApprovals.contains(
                QStringLiteral("thread-0000")));
            QVERIFY(state.pendingApprovals.contains(
                QStringLiteral("thread-4096")));
        }
    }

    void promotionTrackerExpiresExactlyAtHoldBoundary()
    {
        ApprovalPromotionTracker tracker(std::chrono::seconds(10));
        const QDateTime firstSeen = utcDate(2026, 7, 22, 12, 0, 0);
        const QSet<QString> pending{
            QStringLiteral("thread-promoted"),
        };

        QCOMPARE(
            tracker.promotedThreadIds(pending, firstSeen),
            pending);
        QCOMPARE(
            tracker.promotedThreadIds(
                {},
                firstSeen.addSecs(1)),
            pending);
        QCOMPARE(
            tracker.promotedThreadIds(
                {},
                firstSeen.addMSecs(10'999)),
            pending);
        QVERIFY(
            tracker.promotedThreadIds(
                {},
                firstSeen.addMSecs(11'000))
                .isEmpty());
    }

    void concurrentSnapshotsAreDeterministic()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        writeFile(
            directory.filePath(
                QStringLiteral("concurrent-t0-main.log")),
            joinedLines({
                showLine(
                    QStringLiteral("2026-07-22T11:59:00.000Z"),
                    QStringLiteral("thread-a"),
                    1,
                    QStringLiteral("commandExecution")),
                showLine(
                    QStringLiteral("2026-07-22T11:59:01.000Z"),
                    QStringLiteral("thread-b"),
                    2,
                    QStringLiteral("fileChange")),
            }),
            now);

        DesktopApprovalStateStore store(
            {directory.path()},
            kMaximumFileAge);
        std::vector<std::future<TaskProjectionState>> futures;
        futures.reserve(24);
        for (int index = 0; index < 24; ++index) {
            futures.emplace_back(std::async(
                std::launch::async,
                [&store, now] {
                    return store.snapshot(now);
                }));
        }

        const QSet<QString> expected{
            QStringLiteral("thread-a"),
            QStringLiteral("thread-b"),
        };
        for (auto& future : futures) {
            const TaskProjectionState state = future.get();
            QCOMPARE(state.pendingApprovalThreadIds, expected);
            QCOMPARE(state.attentionPromotedThreadIds, expected);
            QCOMPARE(
                QSet<QString>(
                    state.pendingApprovals.keyBegin(),
                    state.pendingApprovals.keyEnd()),
                expected);
        }
    }

    void absentRootsReturnEmptyState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DesktopApprovalStateStore store(
            {
                directory.filePath(QStringLiteral("missing")),
                directory.filePath(QStringLiteral("not-a-directory")),
            },
            kMaximumFileAge);
        writeFile(
            directory.filePath(QStringLiteral("not-a-directory")),
            QByteArrayLiteral("ordinary file"),
            utcDate(2026, 7, 22, 12, 0, 0));

        const TaskProjectionState state =
            store.snapshot(utcDate(2026, 7, 22, 12, 0, 0));

        QVERIFY(state.pendingApprovals.isEmpty());
        QVERIFY(state.pendingApprovalThreadIds.isEmpty());
        QVERIFY(state.attentionPromotedThreadIds.isEmpty());
    }
};

QTEST_GUILESS_MAIN(DesktopApprovalStateTests)
#include "DesktopApprovalStateTests.moc"
