#include "codex/state/HistoryModels.h"
#include "codex/state/TimelineProjector.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace companion;

namespace {

QString fixturePath()
{
    return QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral(
            "/codex-v034/rollout-semantic-timeline.jsonl");
}

QByteArray compactLine(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray messageLine(
    const QString& id,
    const QString& role,
    const QString& text,
    const QString& turnId)
{
    const QString fragmentType =
        role == QStringLiteral("assistant")
            ? QStringLiteral("output_text")
            : QStringLiteral("input_text");
    return compactLine({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-22T01:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("message")},
             {QStringLiteral("id"), id},
             {QStringLiteral("role"), role},
             {QStringLiteral("turn_id"), turnId},
             {QStringLiteral("content"),
              QJsonArray{
                  QJsonObject{
                      {QStringLiteral("type"), fragmentType},
                      {QStringLiteral("text"), text},
                  },
              }},
         }},
    });
}

QByteArray lifecycleLine(
    const QString& type,
    const QString& turnId)
{
    return compactLine({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-22T01:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("type"), type},
             {QStringLiteral("turn_id"), turnId},
         }},
    });
}

QByteArray reasoningLine(
    const QString& id,
    const QString& text,
    const QString& turnId)
{
    return compactLine({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-22T01:00:01.000Z")},
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("type"),
              QStringLiteral("agent_reasoning")},
             {QStringLiteral("id"), id},
             {QStringLiteral("text"), text},
             {QStringLiteral("turn_id"), turnId},
         }},
    });
}

QByteArray toolCallLine(
    const QString& id,
    const QString& callId,
    const QString& name,
    const QString& input,
    const QString& turnId)
{
    return compactLine({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-22T01:00:01.000Z")},
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("type"),
              QStringLiteral("custom_tool_call")},
             {QStringLiteral("id"), id},
             {QStringLiteral("call_id"), callId},
             {QStringLiteral("name"), name},
             {QStringLiteral("status"), QStringLiteral("completed")},
             {QStringLiteral("input"), input},
             {QStringLiteral("turn_id"), turnId},
         }},
    });
}

void writeLines(
    const QString& path,
    const QVector<QByteArray>& lines)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("could not create rollout fixture");
    }
    for (const QByteArray& line : lines) {
        if (file.write(line) != line.size() ||
            !file.putChar('\n')) {
            qFatal("could not finish rollout fixture");
        }
    }
}

const BridgeTimelineItem* itemById(
    const QVector<BridgeTimelineItem>& items,
    const QString& id)
{
    for (const BridgeTimelineItem& item : items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

QString visibleTimelineText(
    const QVector<BridgeTimelineItem>& items)
{
    QStringList values;
    for (const BridgeTimelineItem& item : items) {
        if (item.title.has_value()) {
            values.append(*item.title);
        }
        if (item.text.has_value()) {
            values.append(*item.text);
        }
        if (item.detail.has_value()) {
            values.append(*item.detail);
        }
    }
    return values.join(QLatin1Char('\n'));
}

} // namespace

class TimelineProjectionTests final : public QObject {
    Q_OBJECT

private slots:
    void loadsPrivacyFilteredSemanticTimeline()
    {
        const QString path = fixturePath();
        const Result<TimelinePage> result =
            TimelineProjector::loadTimeline(
                path, std::nullopt, 20);

        if (!result.hasValue()) {
            QFAIL(qPrintable(result.error().message));
        }
        const TimelinePage& page = result.value();
        QCOMPARE(page.items.size(), 8);
        QCOMPARE(
            QVector<QString>({
                page.items.at(0).id,
                page.items.at(1).id,
                page.items.at(2).id,
                page.items.at(3).id,
                page.items.at(4).id,
                page.items.at(5).id,
                page.items.at(6).id,
                page.items.at(7).id,
            }),
            QVector<QString>({
                QStringLiteral("user-1"),
                QStringLiteral("assistant-1"),
                QStringLiteral("reason-1"),
                QStringLiteral("search-1"),
                QStringLiteral("edit-1"),
                QStringLiteral("mcp-1"),
                QStringLiteral("delegate-1"),
                QStringLiteral("unknown-1"),
            }));

        const BridgeTimelineItem* user =
            itemById(page.items, QStringLiteral("user-1"));
        QVERIFY(user != nullptr);
        QCOMPARE(
            user->text.value(),
            QStringLiteral("Keep this request visible."));
        QCOMPARE(user->media.size(), 1);
        QCOMPARE(
            user->media.first().data,
            QByteArray::fromHex("89504e470d0a"));

        const BridgeTimelineItem* assistant =
            itemById(page.items, QStringLiteral("assistant-1"));
        QVERIFY(assistant != nullptr);
        QCOMPARE(
            assistant->phase.value(),
            TimelinePhase::Commentary);

        const BridgeTimelineItem* reasoning =
            itemById(page.items, QStringLiteral("reason-1"));
        QVERIFY(reasoning != nullptr);
        QCOMPARE(
            reasoning->title.value(),
            QStringLiteral("Inspecting timeline projection"));
        QCOMPARE(reasoning->status, TimelineStatus::Completed);

        const BridgeTimelineItem* search =
            itemById(page.items, QStringLiteral("search-1"));
        QVERIFY(search != nullptr);
        QCOMPARE(
            search->title.value(),
            QStringLiteral("Searched files"));
        QCOMPARE(search->status, TimelineStatus::Completed);
        QVERIFY(search->detail->contains(
            QStringLiteral("TaskProjector")));
        QVERIFY(search->detail->contains(
            QStringLiteral("2 matches")));

        const BridgeTimelineItem* edit =
            itemById(page.items, QStringLiteral("edit-1"));
        QVERIFY(edit != nullptr);
        QCOMPARE(
            edit->detail.value(),
            QStringLiteral(
                "Sources/App.cpp\nSources/Timeline View.cpp"));

        QVERIFY(itemById(
            page.items, QStringLiteral("wrapper-1")) == nullptr);
        const BridgeTimelineItem* mcp =
            itemById(page.items, QStringLiteral("mcp-1"));
        QVERIFY(mcp != nullptr);
        QCOMPARE(
            mcp->title.value(),
            QStringLiteral("Inspected an app"));
        QCOMPARE(
            mcp->detail.value(),
            QStringLiteral("Inspect Companion"));

        const BridgeTimelineItem* delegation =
            itemById(page.items, QStringLiteral("delegate-1"));
        QVERIFY(delegation != nullptr);
        QCOMPARE(
            delegation->title.value(),
            QStringLiteral("Messaged an agent"));
        QVERIFY(delegation->detail->contains(
            QStringLiteral("Target: Turing")));
        QVERIFY(delegation->detail->contains(
            QStringLiteral("Audit the timeline")));
        QVERIFY(delegation->detail->contains(
            QStringLiteral("Also verify deterministic pagination.")));
        QVERIFY(!delegation->detail->contains(
            QStringLiteral("timed_out")));

        const BridgeTimelineItem* unknown =
            itemById(page.items, QStringLiteral("unknown-1"));
        QVERIFY(unknown != nullptr);
        QCOMPARE(
            unknown->title.value(),
            QStringLiteral("Used a tool"));
        QCOMPARE(unknown->status, TimelineStatus::Completed);
        QVERIFY(unknown->detail->contains(
            QStringLiteral("C:\\Temp\\result.json")));
        QVERIFY(unknown->detail->contains(QStringLiteral("done")));

        const QString visible = visibleTimelineText(page.items);
        QVERIFY(!visible.contains(QStringLiteral("private context")));
        QVERIFY(!visible.contains(QStringLiteral("private source")));
        QVERIFY(!visible.contains(QStringLiteral("secretImplementation")));
        QVERIFY(!visible.contains(QStringLiteral("x:10")));

        QVERIFY(page.contextUsage.has_value());
        QCOMPARE(page.contextUsage->usedTokens, qint64(96000));
        QCOMPARE(page.contextUsage->contextWindow, qint64(128000));
        QVERIFY(!page.nextCursor.has_value());

        const QFileInfo information(path);
        QCOMPARE(
            page.revision,
            QString::number(information.size())
                + QLatin1Char(':')
                + QString::number(
                    information.lastModified()
                        .toMSecsSinceEpoch()));
    }

    void semanticProjectionPrecedesPagination()
    {
        const Result<TimelinePage> newest =
            TimelineProjector::loadTimeline(
                fixturePath(), std::nullopt, 1);
        QVERIFY(newest.hasValue());
        QCOMPARE(newest.value().items.size(), 1);
        QCOMPARE(
            newest.value().items.first().id,
            QStringLiteral("unknown-1"));
        QCOMPARE(
            newest.value().items.first().status,
            TimelineStatus::Completed);
        QVERIFY(newest.value().items.first().detail->contains(
            QStringLiteral("done")));
        QVERIFY(newest.value().nextCursor.has_value());

        const Result<TimelinePage> older =
            TimelineProjector::loadTimeline(
                fixturePath(),
                newest.value().nextCursor,
                1);
        QVERIFY(older.hasValue());
        QCOMPARE(older.value().items.size(), 1);
        QCOMPARE(
            older.value().items.first().id,
            QStringLiteral("delegate-1"));
        QVERIFY(!older.value().contextUsage.has_value());
        QVERIFY(older.value().items.first().title !=
            std::optional<QString>(QStringLiteral("Tool result")));
    }

    void pagesVisibleMessagesBackwardWithoutDuplicates()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("history.jsonl"));
        const QString environment =
            QStringLiteral(
                "<environment_context>hidden</environment_context>\n\n");
        const QByteArray oversized = compactLine({
            {QStringLiteral("type"), QStringLiteral("event_msg")},
            {QStringLiteral("payload"),
             QJsonObject{
                 {QStringLiteral("type"),
                  QStringLiteral("token_count")},
                 {QStringLiteral("padding"),
                  QString(2 * 1024 * 1024 + 1024,
                          QLatin1Char('x'))},
             }},
        });
        writeLines(
            path,
            {
                messageLine(
                    QStringLiteral("message-1"),
                    QStringLiteral("user"),
                    environment + QStringLiteral("Visible message 1"),
                    QStringLiteral("turn-1")),
                oversized,
                messageLine(
                    QStringLiteral("internal"),
                    QStringLiteral("user"),
                    QStringLiteral(
                        "<codex_internal_context>hidden"
                        "</codex_internal_context>"),
                    QStringLiteral("turn-hidden")),
                messageLine(
                    QStringLiteral("message-2"),
                    QStringLiteral("assistant"),
                    QStringLiteral("Visible message 2"),
                    QStringLiteral("turn-2")),
                messageLine(
                    QStringLiteral("message-3"),
                    QStringLiteral("user"),
                    QStringLiteral("Visible message 3"),
                    QStringLiteral("turn-3")),
                messageLine(
                    QStringLiteral("message-4"),
                    QStringLiteral("assistant"),
                    QStringLiteral("Visible message 4"),
                    QStringLiteral("turn-4")),
                messageLine(
                    QStringLiteral("message-5"),
                    QStringLiteral("user"),
                    QStringLiteral("Visible message 5"),
                    QStringLiteral("turn-5")),
            });

        const Result<MessagePage> newest =
            TimelineProjector::loadMessages(
                path, std::nullopt, 2);
        QVERIFY(newest.hasValue());
        QCOMPARE(
            QVector<QString>({
                newest.value().messages.at(0).text,
                newest.value().messages.at(1).text,
            }),
            QVector<QString>({
                QStringLiteral("Visible message 4"),
                QStringLiteral("Visible message 5"),
            }));

        const Result<MessagePage> middle =
            TimelineProjector::loadMessages(
                path, newest.value().nextCursor, 2);
        QVERIFY(middle.hasValue());
        QCOMPARE(
            QVector<QString>({
                middle.value().messages.at(0).text,
                middle.value().messages.at(1).text,
            }),
            QVector<QString>({
                QStringLiteral("Visible message 2"),
                QStringLiteral("Visible message 3"),
            }));

        const Result<MessagePage> oldest =
            TimelineProjector::loadMessages(
                path, middle.value().nextCursor, 2);
        QVERIFY(oldest.hasValue());
        QCOMPARE(oldest.value().messages.size(), 1);
        QCOMPARE(
            oldest.value().messages.first().text,
            QStringLiteral("Visible message 1"));
        QVERIFY(!oldest.value().nextCursor.has_value());
    }

    void lifecycleCompletesToolsAndMarksActiveReasoning()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString completedPath =
            directory.filePath(QStringLiteral("completed.jsonl"));
        writeLines(
            completedPath,
            {
                lifecycleLine(
                    QStringLiteral("task_started"),
                    QStringLiteral("turn-completed")),
                toolCallLine(
                    QStringLiteral("tool-live"),
                    QStringLiteral("call-live"),
                    QStringLiteral("exec_command"),
                    QStringLiteral(R"({"cmd":"long-running-command"})"),
                    QStringLiteral("turn-completed")),
                lifecycleLine(
                    QStringLiteral("task_complete"),
                    QStringLiteral("turn-completed")),
            });

        const Result<TimelinePage> completed =
            TimelineProjector::loadTimeline(
                completedPath, std::nullopt, 20);
        QVERIFY(completed.hasValue());
        QCOMPARE(completed.value().items.size(), 1);
        QCOMPARE(
            completed.value().items.first().status,
            TimelineStatus::Completed);

        const QString activePath =
            directory.filePath(QStringLiteral("active.jsonl"));
        writeLines(
            activePath,
            {
                lifecycleLine(
                    QStringLiteral("task_started"),
                    QStringLiteral("turn-active")),
                reasoningLine(
                    QStringLiteral("reason-active"),
                    QStringLiteral("**Still working**"),
                    QStringLiteral("turn-active")),
            });

        const Result<TimelinePage> active =
            TimelineProjector::loadTimeline(
                activePath, std::nullopt, 20);
        QVERIFY(active.hasValue());
        QCOMPARE(active.value().items.size(), 1);
        QCOMPARE(
            active.value().items.first().status,
            TimelineStatus::InProgress);
    }

    void invalidCursorAndMissingHistoryUseStableErrors()
    {
        const Result<MessagePage> missing =
            TimelineProjector::loadMessages(
                QStringLiteral("C:\\missing\\rollout.jsonl"),
                std::nullopt,
                20);
        QVERIFY(!missing.hasValue());
        QCOMPARE(
            missing.error().code,
            QStringLiteral("codex.history_missing"));

        const Result<TimelinePage> invalid =
            TimelineProjector::loadTimeline(
                fixturePath(),
                QStringLiteral("not-a-number"),
                20);
        QVERIFY(!invalid.hasValue());
        QCOMPARE(
            invalid.error().code,
            QStringLiteral("codex.invalid_cursor"));

        const qint64 size = QFileInfo(fixturePath()).size();
        const Result<TimelinePage> outOfRange =
            TimelineProjector::loadTimeline(
                fixturePath(),
                QString::number(size + 1),
                20);
        QVERIFY(!outOfRange.hasValue());
        QCOMPARE(
            outOfRange.error().code,
            QStringLiteral("codex.invalid_cursor"));
    }
};

QTEST_GUILESS_MAIN(TimelineProjectionTests)
#include "TimelineProjectionTests.moc"
