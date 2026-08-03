#include "codex/attachments/AttachmentStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <system_error>

using namespace companion;
using namespace std::chrono_literals;

namespace {

inline constexpr qsizetype kMiB = 1024 * 1024;

QString uuidText(const QUuid& value)
{
    return value.toString(QUuid::WithoutBraces).toUpper();
}

QString cleanAbsolutePath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

BridgeAttachment attachment(
    qsizetype byteCount,
    QString filename = QStringLiteral("attachment.bin"),
    AttachmentKind kind = AttachmentKind::File,
    std::optional<QString> mimeType = std::nullopt,
    QUuid id = QUuid::createUuid())
{
    return {
        id,
        kind,
        std::move(filename),
        std::move(mimeType),
        QByteArray(byteCount, '\x5a'),
    };
}

QVector<BridgeAttachment> attachments(
    qsizetype count,
    qsizetype byteCount)
{
    QVector<BridgeAttachment> result;
    result.reserve(count);
    const QByteArray sharedData(byteCount, '\x5a');
    for (qsizetype index = 0; index < count; ++index) {
        result.append({
            QUuid::createUuid(),
            AttachmentKind::File,
            QStringLiteral("attachment-%1.bin").arg(index),
            std::nullopt,
            sharedData,
        });
    }
    return result;
}

QVector<BridgeAttachment> attachments(
    std::initializer_list<qsizetype> byteCounts)
{
    QVector<BridgeAttachment> result;
    result.reserve(static_cast<qsizetype>(byteCounts.size()));
    qsizetype index = 0;
    for (const qsizetype byteCount : byteCounts) {
        result.append(attachment(
            byteCount,
            QStringLiteral("attachment-%1.bin").arg(index)));
        ++index;
    }
    return result;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

Result<void> atomicWrite(
    const QString& path,
    const QByteArray& data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure({
            QStringLiteral("test.open_failed"),
            file.errorString(),
            false,
            {},
        });
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return Result<void>::failure({
            QStringLiteral("test.write_failed"),
            file.errorString(),
            false,
            {},
        });
    }
    if (!file.commit()) {
        return Result<void>::failure({
            QStringLiteral("test.commit_failed"),
            file.errorString(),
            false,
            {},
        });
    }
    return Result<void>::success();
}

bool setDirectoryAge(
    const QString& path,
    std::chrono::hours age,
    QString* errorMessage)
{
    namespace filesystem = std::filesystem;

    const auto now = filesystem::file_time_type::clock::now();
    const auto ageInFileTime =
        std::chrono::duration_cast<
            filesystem::file_time_type::duration>(age);
    std::error_code error;
    filesystem::last_write_time(
        filesystem::path(
            QDir::toNativeSeparators(path).toStdWString()),
        now - ageInFileTime,
        error);
    if (!error) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = QString::fromStdString(error.message());
    }
    return false;
}

bool createDirectoryJunction(
    const QString& junctionPath,
    const QString& targetPath,
    QString* errorMessage)
{
    QProcess process;
    process.setProgram(
        qEnvironmentVariable(
            "COMSPEC",
            QStringLiteral("C:\\Windows\\System32\\cmd.exe")));
    process.setNativeArguments(
        QStringLiteral(
            "/d /c mklink /J \"%1\" \"%2\"")
            .arg(
                QDir::toNativeSeparators(junctionPath),
                QDir::toNativeSeparators(targetPath)));
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000)
        || !process.waitForFinished(5000)
        || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        if (errorMessage != nullptr) {
            *errorMessage =
                QString::fromLocal8Bit(process.readAll()).trimmed();
        }
        return false;
    }
    return true;
}

} // namespace

class AttachmentStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void enforcesCountItemAndTotalBoundaries()
    {
        AttachmentStore store;

        QVERIFY(store.validate(attachments(10, 1)).hasValue());
        const auto tooMany = store.validate(attachments(11, 1));
        QVERIFY(!tooMany.hasValue());
        QCOMPARE(
            tooMany.error().code,
            QStringLiteral("attachment.too_many"));

        QVERIFY(
            store.validate(attachments(1, 20 * kMiB))
                .hasValue());
        const auto itemTooLarge =
            store.validate(attachments(1, 20 * kMiB + 1));
        QVERIFY(!itemTooLarge.hasValue());
        QCOMPARE(
            itemTooLarge.error().code,
            QStringLiteral("attachment.item_too_large"));

        QVERIFY(
            store.validate(
                     attachments(
                         {20 * kMiB, 20 * kMiB, 10 * kMiB}))
                .hasValue());
        const auto totalTooLarge =
            store.validate(
                attachments(
                    {20 * kMiB, 20 * kMiB, 10 * kMiB, 1}));
        QVERIFY(!totalTooLarge.hasValue());
        QCOMPARE(
            totalTooLarge.error().code,
            QStringLiteral("attachment.total_too_large"));
    }

    void validationFailureDoesNotCreateStorage()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);

        const auto result =
            store.stage(attachments(11, 1), QUuid::createUuid());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("attachment.too_many"));
        QVERIFY(!QFileInfo::exists(root));
    }

    void stagesSafeNamesAndEveryProtocolRepresentation()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);
        const QUuid requestId(
            QStringLiteral(
                "10000000-0000-0000-0000-000000000001"));
        const BridgeAttachment file = attachment(
            5,
            QStringLiteral("..\\secret.txt"),
            AttachmentKind::File,
            QStringLiteral("text/plain"),
            QUuid(
                QStringLiteral(
                    "20000000-0000-0000-0000-000000000002")));
        const BridgeAttachment image = attachment(
            4,
            QStringLiteral("../shadow image.png"),
            AttachmentKind::Image,
            QStringLiteral("image/png"),
            QUuid(
                QStringLiteral(
                    "30000000-0000-0000-0000-000000000003")));

        const auto result =
            store.stage({file, image}, requestId);

        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message));
        const auto& staged = result.value();
        QCOMPARE(staged.size(), 2);
        QCOMPARE(staged.at(0).label, QStringLiteral("secret.txt"));
        QCOMPARE(
            staged.at(1).label,
            QStringLiteral("shadow image.png"));

        const QString requestDirectory =
            QDir(root).filePath(uuidText(requestId));
        QCOMPARE(
            cleanAbsolutePath(
                QFileInfo(staged.at(0).path).absolutePath()),
            cleanAbsolutePath(requestDirectory));
        QCOMPARE(staged.at(0).path, staged.at(0).fsPath);
        QCOMPARE(
            QFileInfo(staged.at(0).path).fileName(),
            uuidText(file.id) + QStringLiteral("-secret.txt"));
        QCOMPARE(readFile(staged.at(0).path), file.data);
        QCOMPARE(readFile(staged.at(1).path), image.data);
        QVERIFY(!QFileInfo::exists(
            requestDirectory + QStringLiteral(".partial")));

        const QJsonObject expectedNative{
            {QStringLiteral("label"), QStringLiteral("secret.txt")},
            {QStringLiteral("path"), staged.at(0).path},
            {QStringLiteral("fsPath"), staged.at(0).fsPath},
        };
        QCOMPARE(staged.at(0).followerNative(), expectedNative);
        QVERIFY(!staged.at(0).followerInput().has_value());
        QCOMPARE(
            staged.at(0).appServerInput(),
            QJsonObject({
                {QStringLiteral("type"), QStringLiteral("mention")},
                {QStringLiteral("name"), QStringLiteral("secret.txt")},
                {QStringLiteral("path"), staged.at(0).path},
            }));
        QVERIFY(!staged.at(0).queuedImage().has_value());
        QVERIFY(staged.at(0).queuedFile().has_value());
        QCOMPARE(
            *staged.at(0).queuedFile(),
            staged.at(0).followerNative());

        const QJsonObject expectedImageInput{
            {QStringLiteral("type"), QStringLiteral("localImage")},
            {QStringLiteral("path"), staged.at(1).path},
        };
        QVERIFY(staged.at(1).followerInput().has_value());
        QCOMPARE(
            *staged.at(1).followerInput(),
            expectedImageInput);
        QCOMPARE(
            staged.at(1).appServerInput(),
            expectedImageInput);
        QVERIFY(!staged.at(1).queuedFile().has_value());
        QVERIFY(staged.at(1).queuedImage().has_value());
        QCOMPARE(
            *staged.at(1).queuedImage(),
            QJsonObject({
                {QStringLiteral("id"), uuidText(image.id)},
                {
                    QStringLiteral("src"),
                    QUrl::fromLocalFile(staged.at(1).path)
                        .toString(QUrl::FullyEncoded),
                },
                {
                    QStringLiteral("filename"),
                    QStringLiteral("shadow image.png"),
                },
                {
                    QStringLiteral("localPath"),
                    staged.at(1).path,
                },
                {
                    QStringLiteral("uploadStatus"),
                    QStringLiteral("uploaded"),
                },
                {
                    QStringLiteral("mimeType"),
                    QStringLiteral("image/png"),
                },
            }));
    }

    void queuedImageOmitsMissingOrBlankMimeType()
    {
        const StagedAttachment missing{
            QUuid::createUuid(),
            AttachmentKind::Image,
            QStringLiteral("image.png"),
            QStringLiteral("C:\\images\\image.png"),
            QStringLiteral("C:\\images\\image.png"),
            std::nullopt,
        };
        const StagedAttachment blank{
            QUuid::createUuid(),
            AttachmentKind::Image,
            QStringLiteral("image.png"),
            QStringLiteral("C:\\images\\image.png"),
            QStringLiteral("C:\\images\\image.png"),
            QString(),
        };

        QVERIFY(missing.queuedImage().has_value());
        QVERIFY(!missing.queuedImage()->contains(
            QStringLiteral("mimeType")));
        QVERIFY(blank.queuedImage().has_value());
        QVERIFY(!blank.queuedImage()->contains(
            QStringLiteral("mimeType")));
    }

    void rejectsInvalidFilenamesWithoutPublishing()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);
        QString embeddedNul = QStringLiteral("bad");
        embeddedNul.insert(2, QChar(0));
        const QVector<QString> invalidNames{
            QStringLiteral("."),
            QStringLiteral(".."),
            QStringLiteral(" \r\n\t "),
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

        for (const QString& filename : invalidNames) {
            const QUuid requestId = QUuid::createUuid();
            const auto result =
                store.stage(
                    {attachment(1, filename)},
                    requestId);
            QVERIFY(!result.hasValue());
            QCOMPARE(
                result.error().code,
                QStringLiteral("attachment.invalid_filename"));
            QVERIFY(!QFileInfo::exists(
                QDir(root).filePath(uuidText(requestId))));
            QVERIFY(!QFileInfo::exists(
                QDir(root).filePath(
                    uuidText(requestId)
                    + QStringLiteral(".partial"))));
        }
    }

    void safelyEncodesWindowsSpecificCharacters()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);
        const QVector<QString> filenames{
            QStringLiteral("name."),
            QStringLiteral("question?.txt"),
            QStringLiteral("quote\".txt"),
            QStringLiteral("angle<name>.txt"),
            QStringLiteral("pipe|name.txt"),
        };
        QVector<BridgeAttachment> input;
        input.reserve(filenames.size());
        for (const QString& filename : filenames) {
            input.append(attachment(1, filename));
        }

        const auto result =
            store.stage(input, QUuid::createUuid());

        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message));
        QCOMPARE(result.value().size(), filenames.size());
        const QString forbidden =
            QStringLiteral("<>:\"/\\|?*");
        for (qsizetype index = 0;
             index < result.value().size();
             ++index) {
            const StagedAttachment& staged =
                result.value().at(index);
            QCOMPARE(staged.label, filenames.at(index));
            QVERIFY(QFileInfo::exists(staged.path));
            QCOMPARE(readFile(staged.path), QByteArray(1, '\x5a'));
            const QString storedName =
                QFileInfo(staged.path).fileName();
            for (const QChar character : forbidden) {
                QVERIFY2(
                    !storedName.contains(character),
                    qPrintable(storedName));
            }
            QVERIFY(!storedName.endsWith(QLatin1Char('.')));
            QVERIFY(!storedName.endsWith(QLatin1Char(' ')));
        }
    }

    void failedSecondWriteRemovesOnlyThePartialRequest()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        QVERIFY(QDir().mkpath(root));
        const QString unrelated =
            QDir(root).filePath(QStringLiteral("unrelated"));
        QVERIFY(QDir().mkpath(unrelated));
        const QUuid requestId(
            QStringLiteral(
                "40000000-0000-0000-0000-000000000004"));
        qsizetype writes = 0;
        AttachmentStore store(
            root,
            {},
            [&writes](
                const QString& path,
                const QByteArray& data) -> Result<void> {
                ++writes;
                if (writes == 2) {
                    return Result<void>::failure({
                        QStringLiteral("test.second_write_failed"),
                        QStringLiteral("Injected second-write failure."),
                        false,
                        {},
                    });
                }
                return atomicWrite(path, data);
            });

        const auto result = store.stage(
            {
                attachment(1, QStringLiteral("first.txt")),
                attachment(1, QStringLiteral("second.txt")),
            },
            requestId);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("test.second_write_failed"));
        QCOMPARE(writes, 2);
        QVERIFY(QFileInfo::exists(unrelated));
        QVERIFY(!QFileInfo::exists(
            QDir(root).filePath(uuidText(requestId))));
        QVERIFY(!QFileInfo::exists(
            QDir(root).filePath(
                uuidText(requestId)
                + QStringLiteral(".partial"))));
    }

    void failureCleanupNeverTraversesNestedNtfsJunction()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QString external =
            QDir(parent.path()).filePath(
                QStringLiteral("external"));
        QVERIFY(QDir().mkpath(external));
        QFile marker(
            QDir(external).filePath(
                QStringLiteral("must-survive.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("keep"), qint64(4));
        marker.close();
        const QUuid requestId(
            QStringLiteral(
                "41000000-0000-0000-0000-000000000004"));
        const QString partialDirectory =
            QDir(root).filePath(
                uuidText(requestId)
                + QStringLiteral(".partial"));
        const QString nestedJunction =
            QDir(partialDirectory).filePath(
                QStringLiteral("nested-junction"));
        bool junctionCreated = false;
        AttachmentStore store(
            root,
            {},
            [&](
                const QString&,
                const QByteArray&) -> Result<void> {
                QString junctionError;
                junctionCreated = createDirectoryJunction(
                    nestedJunction,
                    external,
                    &junctionError);
                if (!junctionCreated) {
                    return Result<void>::failure({
                        QStringLiteral(
                            "test.junction_setup_failed"),
                        junctionError,
                        false,
                        {},
                    });
                }
                return Result<void>::failure({
                    QStringLiteral("test.injected_write_failed"),
                    QStringLiteral(
                        "Injected failure after planting a junction."),
                    false,
                    {},
                });
            });

        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            requestId);

        QVERIFY(junctionCreated);
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("test.injected_write_failed"));
        QCOMPARE(readFile(marker.fileName()), QByteArray("keep"));
        QVERIFY(QFileInfo::exists(partialDirectory));
        QVERIFY(QFileInfo(nestedJunction).isJunction());
        QVERIFY(
            QDir(partialDirectory).rmdir(
                QStringLiteral("nested-junction")));
        QVERIFY(
            QDir(root).rmdir(
                uuidText(requestId)
                + QStringLiteral(".partial")));
    }

    void requestDirectoryIsPublishedOnlyAfterAllAtomicWrites()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QUuid requestId(
            QStringLiteral(
                "50000000-0000-0000-0000-000000000005"));
        const QString finalDirectory =
            QDir(root).filePath(uuidText(requestId));
        const QString partialDirectory =
            finalDirectory + QStringLiteral(".partial");
        bool sawPartial = false;
        bool sawFinalBeforeCompletion = false;
        AttachmentStore store(
            root,
            {},
            [&](
                const QString& path,
                const QByteArray& data) -> Result<void> {
                sawPartial =
                    sawPartial
                    || cleanAbsolutePath(
                           QFileInfo(path).absolutePath())
                        == cleanAbsolutePath(partialDirectory);
                sawFinalBeforeCompletion =
                    sawFinalBeforeCompletion
                    || QFileInfo::exists(finalDirectory);
                return atomicWrite(path, data);
            });

        const auto result = store.stage(
            {
                attachment(3, QStringLiteral("first.txt")),
                attachment(4, QStringLiteral("second.txt")),
            },
            requestId);

        QVERIFY(result.hasValue());
        QVERIFY(sawPartial);
        QVERIFY(!sawFinalBeforeCompletion);
        QVERIFY(QFileInfo::exists(finalDirectory));
        QVERIFY(!QFileInfo::exists(partialDirectory));
        const QFileInfoList entries =
            QDir(finalDirectory).entryInfoList(
                QDir::Files | QDir::NoDotAndDotDot);
        QCOMPARE(entries.size(), 2);
        for (const QFileInfo& entry : entries) {
            QVERIFY(!entry.fileName().contains(
                QStringLiteral(".partial")));
        }
    }

    void existingRequestDirectoryIsNeverOverwritten()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QUuid requestId(
            QStringLiteral(
                "60000000-0000-0000-0000-000000000006"));
        const QString finalDirectory =
            QDir(root).filePath(uuidText(requestId));
        QVERIFY(QDir().mkpath(finalDirectory));
        QFile marker(
            QDir(finalDirectory).filePath(
                QStringLiteral("existing.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("keep"), qint64(4));
        marker.close();
        AttachmentStore store(root);

        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            requestId);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("attachment.request_exists"));
        QCOMPARE(readFile(marker.fileName()), QByteArray("keep"));
        QVERIFY(!QFileInfo::exists(
            finalDirectory + QStringLiteral(".partial")));
    }

    void publishRaceCleansPartialWithoutTouchingWinner()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QUuid requestId(
            QStringLiteral(
                "70000000-0000-0000-0000-000000000007"));
        const QString finalDirectory =
            QDir(root).filePath(uuidText(requestId));
        const QString partialDirectory =
            finalDirectory + QStringLiteral(".partial");
        AttachmentStore store(
            root,
            {},
            [&](
                const QString& path,
                const QByteArray& data) -> Result<void> {
                const Result<void> written =
                    atomicWrite(path, data);
                if (!written.hasValue()) {
                    return written;
                }
                if (!QDir().mkpath(finalDirectory)) {
                    return Result<void>::failure({
                        QStringLiteral("test.race_setup_failed"),
                        QStringLiteral(
                            "Could not create the winning directory."),
                        false,
                        {},
                    });
                }
                QFile marker(
                    QDir(finalDirectory).filePath(
                        QStringLiteral("winner.txt")));
                if (!marker.open(QIODevice::WriteOnly)
                    || marker.write("keep") != 4) {
                    return Result<void>::failure({
                        QStringLiteral("test.race_setup_failed"),
                        marker.errorString(),
                        false,
                        {},
                    });
                }
                return Result<void>::success();
            });

        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            requestId);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("attachment.publish_failed"));
        QVERIFY(!QFileInfo::exists(partialDirectory));
        QCOMPARE(
            readFile(
                QDir(finalDirectory).filePath(
                    QStringLiteral("winner.txt"))),
            QByteArray("keep"));
    }

    void prunesOnlyDirectoriesOlderThanSevenDays()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        QVERIFY(QDir().mkpath(root));
        const QString expired =
            QDir(root).filePath(QStringLiteral("expired"));
        const QString boundary =
            QDir(root).filePath(QStringLiteral("boundary"));
        const QString fresh =
            QDir(root).filePath(QStringLiteral("fresh"));
        QVERIFY(QDir().mkpath(expired));
        QVERIFY(QDir().mkpath(boundary));
        QVERIFY(QDir().mkpath(fresh));
        QFile ordinaryFile(
            QDir(root).filePath(QStringLiteral("old-file.txt")));
        QVERIFY(ordinaryFile.open(QIODevice::WriteOnly));
        QCOMPARE(ordinaryFile.write("keep"), qint64(4));
        ordinaryFile.close();

        QString timeError;
        QVERIFY2(
            setDirectoryAge(expired, 8 * 24h, &timeError),
            qPrintable(timeError));
        QVERIFY2(
            setDirectoryAge(
                boundary,
                7 * 24h - 1h,
                &timeError),
            qPrintable(timeError));
        QVERIFY2(
            setDirectoryAge(fresh, 6 * 24h, &timeError),
            qPrintable(timeError));

        AttachmentStore store(root);
        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            QUuid::createUuid());

        QVERIFY(result.hasValue());
        QVERIFY(!QFileInfo::exists(expired));
        QVERIFY(QFileInfo::exists(boundary));
        QVERIFY(QFileInfo::exists(fresh));
        QVERIFY(QFileInfo::exists(ordinaryFile.fileName()));
    }

    void pruningNeverTraversesAnNtfsJunction()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QString external =
            QDir(parent.path()).filePath(
                QStringLiteral("external"));
        const QString junction =
            QDir(root).filePath(
                QStringLiteral("expired-junction"));
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(external));
        QFile marker(
            QDir(external).filePath(
                QStringLiteral("must-survive.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("keep"), qint64(4));
        marker.close();
        QString junctionError;
        QVERIFY2(
            createDirectoryJunction(
                junction,
                external,
                &junctionError),
            qPrintable(junctionError));
        QVERIFY(QFileInfo(junction).isJunction());
        AttachmentStore store(
            root,
            [] {
                return QDateTime::currentDateTimeUtc()
                    .addDays(8);
            });

        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            QUuid::createUuid());

        QVERIFY(result.hasValue());
        QCOMPARE(readFile(marker.fileName()), QByteArray("keep"));
        QVERIFY(QFileInfo::exists(junction));
        QVERIFY(
            QDir(root).rmdir(
                QStringLiteral("expired-junction")));
    }

    void pruningNeverTraversesNestedNtfsJunction()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QString expired =
            QDir(root).filePath(QStringLiteral("expired"));
        const QString external =
            QDir(parent.path()).filePath(
                QStringLiteral("external"));
        const QString nestedJunction =
            QDir(expired).filePath(
                QStringLiteral("nested-junction"));
        QVERIFY(QDir().mkpath(expired));
        QVERIFY(QDir().mkpath(external));
        QFile marker(
            QDir(external).filePath(
                QStringLiteral("must-survive.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("keep"), qint64(4));
        marker.close();
        QString junctionError;
        QVERIFY2(
            createDirectoryJunction(
                nestedJunction,
                external,
                &junctionError),
            qPrintable(junctionError));
        QVERIFY(QFileInfo(nestedJunction).isJunction());
        AttachmentStore store(
            root,
            [] {
                return QDateTime::currentDateTimeUtc()
                    .addDays(8);
            });

        const auto result = store.stage(
            {attachment(1, QStringLiteral("new.txt"))},
            QUuid::createUuid());

        QVERIFY(result.hasValue());
        QCOMPARE(readFile(marker.fileName()), QByteArray("keep"));
        QVERIFY(QFileInfo::exists(expired));
        QVERIFY(QFileInfo(nestedJunction).isJunction());
        QVERIFY(
            QDir(expired).rmdir(
                QStringLiteral("nested-junction")));
        QVERIFY(
            QDir(root).rmdir(
                QStringLiteral("expired")));
    }

    void emptyStageDoesNotCreateStorage()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);

        const auto result =
            store.stage({}, QUuid::createUuid());

        QVERIFY(result.hasValue());
        QVERIFY(result.value().isEmpty());
        QVERIFY(!QFileInfo::exists(root));
    }

    void ownedStageRemovesPublishedDirectoryUnlessRetained()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QUuid requestId(
            QStringLiteral(
                "80000000-0000-0000-0000-000000000008"));
        const QString finalDirectory =
            QDir(root).filePath(uuidText(requestId));
        AttachmentStore store(root);

        auto result = store.stageOwned(
            {attachment(3, QStringLiteral("owned.txt"))},
            requestId);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().attachments.size(), 1);
        QVERIFY(
            result.value().cleanupLease != nullptr);
        QVERIFY(QFileInfo::exists(finalDirectory));

        result.value().cleanupLease.reset();

        QVERIFY(!QFileInfo::exists(finalDirectory));
        QVERIFY(QFileInfo::exists(root));
    }

    void retainedOwnedStageSurvivesLeaseDestruction()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        const QUuid requestId(
            QStringLiteral(
                "90000000-0000-0000-0000-000000000009"));
        const QString finalDirectory =
            QDir(root).filePath(uuidText(requestId));
        AttachmentStore store(root);

        auto result = store.stageOwned(
            {attachment(4, QStringLiteral("retained.txt"))},
            requestId);

        QVERIFY(result.hasValue());
        QVERIFY(
            result.value().cleanupLease != nullptr);
        result.value().cleanupLease
            ->retainForCommittedUse();
        result.value().cleanupLease.reset();

        QVERIFY(QFileInfo::exists(finalDirectory));
        QCOMPARE(
            readFile(
                result.value()
                    .attachments.front()
                    .path),
            QByteArray(4, '\x5a'));
    }

    void emptyOwnedStageReturnsInertLeaseWithoutStorage()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString root =
            QDir(parent.path()).filePath(
                QStringLiteral("incoming"));
        AttachmentStore store(root);

        auto result =
            store.stageOwned({}, QUuid::createUuid());

        QVERIFY(result.hasValue());
        QVERIFY(result.value().attachments.isEmpty());
        QVERIFY(
            result.value().cleanupLease != nullptr);
        result.value().cleanupLease
            ->retainForCommittedUse();
        result.value().cleanupLease.reset();
        QVERIFY(!QFileInfo::exists(root));
    }
};

QTEST_GUILESS_MAIN(AttachmentStoreTests)

#include "AttachmentStoreTests.moc"
