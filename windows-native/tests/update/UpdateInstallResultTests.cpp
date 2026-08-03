#include "updater-helper/UpdateInstallResult.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class UpdateInstallResultTests final
    : public QObject {
    Q_OBJECT

private slots:
    void writesStableAtomicResult()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "result.json"));

        companion::
            UpdateInstallResultRecord record;
        record.requestId =
            QStringLiteral(
                "11111111-2222-4333-8444-"
                "555555555555");
        record.success = false;
        record.errorCode =
            QStringLiteral(
                "update.rollback_failed");
        record.message =
            QStringLiteral(
                "The update was restored.");
        record.completedAtUtc =
            QStringLiteral(
                "2026-07-25T12:34:56.789Z");
        record.installerLogPath =
            directory.filePath(
                QStringLiteral(
                    "installer.log"));
        record.context.insert(
            QStringLiteral("causeCode"),
            QStringLiteral(
                "update.installer_exit_failed"));

        const auto written =
            record.write(path);
        QVERIFY2(
            written.hasValue(),
            qPrintable(
                written.hasValue()
                    ? QString()
                    : written.error()
                          .message));

        QFile file(path);
        QVERIFY(file.open(
            QIODevice::ReadOnly));
        const QJsonDocument document =
            QJsonDocument::fromJson(
                file.readAll());
        QVERIFY(document.isObject());
        const QJsonObject object =
            document.object();
        QCOMPARE(object.size(), 8);
        QCOMPARE(
            object.value(
                QStringLiteral("schema"))
                .toInt(),
            1);
        QCOMPARE(
            object.value(
                QStringLiteral(
                    "requestId"))
                .toString(),
            record.requestId);
        QCOMPARE(
            object.value(
                QStringLiteral("success"))
                .toBool(),
            false);
        QCOMPARE(
            object.value(
                QStringLiteral(
                    "errorCode"))
                .toString(),
            record.errorCode);
        QCOMPARE(
            object.value(
                QStringLiteral("message"))
                .toString(),
            record.message);
        QCOMPARE(
            object.value(
                QStringLiteral(
                    "completedAtUtc"))
                .toString(),
            record.completedAtUtc);
        QCOMPARE(
            object.value(
                QStringLiteral(
                    "installerLogPath"))
                .toString(),
            record.installerLogPath);
        QCOMPARE(
            object.value(
                QStringLiteral("context"))
                .toObject()
                .value(
                    QStringLiteral(
                        "causeCode"))
                .toString(),
            QStringLiteral(
                "update.installer_exit_failed"));
    }

    void rejectsRelativeResultPath()
    {
        companion::
            UpdateInstallResultRecord record;
        const auto written =
            record.write(
                QStringLiteral(
                    "result.json"));
        QVERIFY(!written.hasValue());
        QCOMPARE(
            written.error().code,
            QStringLiteral(
                "update.result_path_invalid"));
    }
};

QTEST_GUILESS_MAIN(
    UpdateInstallResultTests)

#include "UpdateInstallResultTests.moc"
