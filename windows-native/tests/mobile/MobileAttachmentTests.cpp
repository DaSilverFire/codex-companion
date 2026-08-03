#include "mobile/attachments/MobileIncomingAttachmentStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace companion;

namespace {

constexpr qsizetype kMiB = 1024 * 1024;

QString uuidText(const QUuid& value)
{
    return value.toString(QUuid::WithoutBraces).toUpper();
}

BridgeAttachment attachment(
    qsizetype byteCount,
    QString filename,
    AttachmentKind kind = AttachmentKind::File,
    QUuid id = QUuid::createUuid())
{
    return {
        id,
        kind,
        std::move(filename),
        kind == AttachmentKind::Image
            ? std::optional<QString>(
                  QStringLiteral("image/png"))
            : std::optional<QString>(
                  QStringLiteral("text/plain")),
        QByteArray(byteCount, '\x5a'),
    };
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

class EnvironmentVariableGuard final {
public:
    explicit EnvironmentVariableGuard(QByteArray name)
        : name_(std::move(name)),
          existed_(qEnvironmentVariableIsSet(name_.constData())),
          value_(qgetenv(name_.constData()))
    {
    }

    ~EnvironmentVariableGuard()
    {
        if (existed_) {
            qputenv(name_.constData(), value_);
        } else {
            qunsetenv(name_.constData());
        }
    }

private:
    QByteArray name_;
    bool existed_ = false;
    QByteArray value_;
};

} // namespace

class MobileAttachmentTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsBoundViolationsBeforeCreatingStorage()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        MobileIncomingAttachmentStore store(root);

        QVector<BridgeAttachment> tooMany;
        for (int index = 0; index < 11; ++index) {
            tooMany.append(
                attachment(
                    1,
                    QStringLiteral("item-%1.txt")
                        .arg(index)));
        }
        const auto countResult =
            store.stage(tooMany, QUuid::createUuid());
        QVERIFY(!countResult.hasValue());
        QCOMPARE(
            countResult.error().code,
            QStringLiteral("attachment.too_many"));
        QVERIFY(!QFileInfo::exists(root));

        const auto itemResult = store.stage(
            {
                attachment(
                    20 * kMiB + 1,
                    QStringLiteral("large.bin")),
            },
            QUuid::createUuid());
        QVERIFY(!itemResult.hasValue());
        QCOMPARE(
            itemResult.error().code,
            QStringLiteral("attachment.item_too_large"));
        QVERIFY(!QFileInfo::exists(root));

        const auto totalResult = store.stage(
            {
                attachment(
                    20 * kMiB,
                    QStringLiteral("one.bin")),
                attachment(
                    20 * kMiB,
                    QStringLiteral("two.bin")),
                attachment(
                    10 * kMiB,
                    QStringLiteral("three.bin")),
                attachment(
                    1,
                    QStringLiteral("four.bin")),
            },
            QUuid::createUuid());
        QVERIFY(!totalResult.hasValue());
        QCOMPARE(
            totalResult.error().code,
            QStringLiteral("attachment.total_too_large"));
        QVERIFY(!QFileInfo::exists(root));
    }

    void stagesBasenamesInOrderWithCodexInputs()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        MobileIncomingAttachmentStore store(root);
        const QUuid requestId(
            QStringLiteral(
                "10000000-0000-0000-0000-000000000001"));
        const QVector<BridgeAttachment> input{
            attachment(
                3,
                QStringLiteral("../notes.txt"),
                AttachmentKind::File,
                QUuid(
                    QStringLiteral(
                        "20000000-0000-0000-0000-000000000002"))),
            attachment(
                4,
                QStringLiteral("C:\\temp\\photo.png"),
                AttachmentKind::Image,
                QUuid(
                    QStringLiteral(
                        "30000000-0000-0000-0000-000000000003"))),
            attachment(
                5,
                QStringLiteral("/tmp/final.txt"),
                AttachmentKind::File,
                QUuid(
                    QStringLiteral(
                        "40000000-0000-0000-0000-000000000004"))),
        };

        const auto result = store.stage(input, requestId);

        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message));
        QCOMPARE(result.value().size(), input.size());
        const QVector<QString> expectedLabels{
            QStringLiteral("notes.txt"),
            QStringLiteral("photo.png"),
            QStringLiteral("final.txt"),
        };
        const QString requestDirectory =
            QDir(root).filePath(uuidText(requestId));
        for (qsizetype index = 0;
             index < input.size();
             ++index) {
            const StagedAttachment& staged =
                result.value().at(index);
            QCOMPARE(staged.id, input.at(index).id);
            QCOMPARE(
                staged.label,
                expectedLabels.at(index));
            QCOMPARE(
                QFileInfo(staged.path).absolutePath(),
                QFileInfo(requestDirectory)
                    .absoluteFilePath());
            QCOMPARE(
                QFileInfo(staged.path).fileName(),
                uuidText(input.at(index).id)
                    + QLatin1Char('-')
                    + expectedLabels.at(index));
            QCOMPARE(
                readFile(staged.path),
                input.at(index).data);
        }
        QCOMPARE(
            result.value().at(0).appServerInput(),
            QJsonObject({
                {
                    QStringLiteral("type"),
                    QStringLiteral("mention"),
                },
                {
                    QStringLiteral("name"),
                    QStringLiteral("notes.txt"),
                },
                {
                    QStringLiteral("path"),
                    result.value().at(0).path,
                },
            }));
        QCOMPARE(
            result.value().at(1).appServerInput(),
            QJsonObject({
                {
                    QStringLiteral("type"),
                    QStringLiteral("localImage"),
                },
                {
                    QStringLiteral("path"),
                    result.value().at(1).path,
                },
            }));
    }

    void rejectsUnsafeWindowsComponentsBeforeCreatingStorage()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        MobileIncomingAttachmentStore store(root);
        QString embeddedNul = QStringLiteral("bad.txt");
        embeddedNul.insert(3, QChar(0));
        const QVector<QString> invalid{
            QString(),
            QStringLiteral("."),
            QStringLiteral(".."),
            embeddedNul,
            QStringLiteral("CON"),
            QStringLiteral("con.txt"),
            QStringLiteral("AUX.log"),
            QStringLiteral("NUL.bin"),
            QStringLiteral("COM1.txt"),
            QStringLiteral("LPT9"),
            QStringLiteral("report.txt:stream"),
            QString(256, QLatin1Char('x')),
        };

        for (const QString& filename : invalid) {
            const auto result = store.stage(
                {attachment(1, filename)},
                QUuid::createUuid());
            QVERIFY2(
                !result.hasValue(),
                qPrintable(filename));
            QCOMPARE(
                result.error().code,
                QStringLiteral(
                    "attachment.invalid_filename"));
            QVERIFY(!QFileInfo::exists(root));
        }
    }

    void usesLocalAppDataRootAndPrunesAfterValidation()
    {
        QTemporaryDir localData;
        QVERIFY(localData.isValid());
        EnvironmentVariableGuard guard("LOCALAPPDATA");
        QVERIFY(
            qputenv(
                "LOCALAPPDATA",
                QFile::encodeName(localData.path())));
        const QString root =
            QDir(localData.path()).filePath(
                QStringLiteral(
                    "Codex Companion/IncomingAttachments"));
        const QString expired =
            QDir(root).filePath(
                QStringLiteral("expired"));
        QVERIFY(QDir().mkpath(expired));
        QFile marker(
            QDir(expired).filePath(
                QStringLiteral("old.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("old"), qint64(3));
        marker.close();
        const QDateTime expiredModified =
            QFileInfo(expired)
                .lastModified()
                .toUTC();
        QVERIFY(expiredModified.isValid());
        const QDateTime now =
            expiredModified.addDays(8);

        MobileIncomingAttachmentStore store(
            {},
            [now] {
                return now;
            });
        const QUuid requestId = QUuid::createUuid();
        const auto result = store.stage(
            {
                attachment(
                    1,
                    QStringLiteral("fresh.txt")),
            },
            requestId);

        QVERIFY(result.hasValue());
        QVERIFY(!QFileInfo::exists(expired));
        QVERIFY(
            QFileInfo(
                result.value().front().path)
                .absoluteFilePath()
                .startsWith(
                    QFileInfo(
                        QDir(root).filePath(
                            uuidText(requestId)))
                        .absoluteFilePath(),
                    Qt::CaseInsensitive));
    }

    void failedWriteRemovesOnlyIncompleteRequest()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QString unrelated =
            QDir(root).filePath(
                QStringLiteral("unrelated"));
        QVERIFY(QDir().mkpath(unrelated));
        qsizetype writes = 0;
        MobileIncomingAttachmentStore store(
            root,
            {},
            [&writes](
                const QString&,
                const QByteArray&) -> Result<void> {
                ++writes;
                if (writes == 2) {
                    return Result<void>::failure({
                        QStringLiteral(
                            "test.write_failed"),
                        QStringLiteral(
                            "Injected write failure."),
                        false,
                        {},
                    });
                }
                return Result<void>::success();
            });
        const QUuid requestId = QUuid::createUuid();

        const auto result = store.stage(
            {
                attachment(
                    1,
                    QStringLiteral("one.txt")),
                attachment(
                    1,
                    QStringLiteral("two.txt")),
            },
            requestId);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("test.write_failed"));
        QVERIFY(QFileInfo::exists(unrelated));
        QVERIFY(!QFileInfo::exists(
            QDir(root).filePath(
                uuidText(requestId))));
        QVERIFY(!QFileInfo::exists(
            QDir(root).filePath(
                uuidText(requestId)
                    + QStringLiteral(".partial"))));
    }
};

QTEST_GUILESS_MAIN(MobileAttachmentTests)

#include "MobileAttachmentTests.moc"
