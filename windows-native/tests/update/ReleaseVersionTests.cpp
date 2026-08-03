#include "update/ReleaseVersion.h"
#include "update/UpdateManifest.h"

#include <compare>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

QByteArray fixture(const QString& name)
{
    QFile file(QStringLiteral(COMPANION_UPDATE_FIXTURE_ROOT)
               + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("update fixture missing");
    }
    return file.readAll();
}

int comparisonSign(
    const companion::ReleaseVersion& left,
    const companion::ReleaseVersion& right)
{
    if (left < right) {
        return -1;
    }
    if (right < left) {
        return 1;
    }
    return 0;
}

} // namespace

class ReleaseVersionTests final : public QObject {
    Q_OBJECT

private slots:
    void matchesV034OrderingFixture()
    {
        const QJsonDocument document = QJsonDocument::fromJson(
            fixture(QStringLiteral("version-order.json")));
        QVERIFY(document.isArray());

        for (const QJsonValue& value : document.array()) {
            const QJsonObject row = value.toObject();
            const QString leftText =
                row.value(QStringLiteral("left")).toString();
            const QString rightText =
                row.value(QStringLiteral("right")).toString();
            const int expected =
                row.value(QStringLiteral("order")).toInt();
            const auto left =
                companion::ReleaseVersion::parse(leftText);
            const auto right =
                companion::ReleaseVersion::parse(rightText);

            QVERIFY2(left.has_value(), qPrintable(leftText));
            QVERIFY2(right.has_value(), qPrintable(rightText));
            QCOMPARE(comparisonSign(*left, *right), expected);
        }
    }

    void rejectsUnparseableReleaseVersions_data()
    {
        QTest::addColumn<QString>("version");
        QTest::newRow("single-component")
            << QStringLiteral("1");
        QTest::newRow("empty-component")
            << QStringLiteral("1..2");
        QTest::newRow("nonnumeric-core")
            << QStringLiteral("1.x.2");
        QTest::newRow("empty-prerelease")
            << QStringLiteral("1.2-");
        QTest::newRow("empty-prerelease-identifier")
            << QStringLiteral("1.2-alpha..1");
        QTest::newRow("overflowing-core")
            << QStringLiteral("9223372036854775808.0");
    }

    void rejectsUnparseableReleaseVersions()
    {
        QFETCH(QString, version);
        QVERIFY(!companion::ReleaseVersion::parse(version).has_value());
    }

    void acceptsV034CompatibleReleaseVersionForms_data()
    {
        QTest::addColumn<QString>("version");
        QTest::newRow("two-components")
            << QStringLiteral("0.3");
        QTest::newRow("four-components")
            << QStringLiteral("0.3.4.0");
        QTest::newRow("prerelease")
            << QStringLiteral("0.3.4-beta.2");
        QTest::newRow("metadata")
            << QStringLiteral("0.3.4+build.7");
        QTest::newRow("prerelease-and-metadata")
            << QStringLiteral("0.3.4-beta.2+build.7");
    }

    void acceptsV034CompatibleReleaseVersionForms()
    {
        QFETCH(QString, version);
        QVERIFY(companion::ReleaseVersion::parse(version).has_value());
    }

    void manifestComparisonUsesVersionBeforeBuild()
    {
        companion::UpdateManifest manifest;
        manifest.version = QStringLiteral("0.3.4");
        manifest.build = 1;

        QVERIFY(manifest.isNewerThan(QStringLiteral("0.3.3"), 99));
        QVERIFY(!manifest.isNewerThan(QStringLiteral("0.3.5"), 0));
        QVERIFY(manifest.isNewerThan(QStringLiteral("0.3.4"), 0));
        QVERIFY(!manifest.isNewerThan(QStringLiteral("0.3.4"), 1));
        QVERIFY(!manifest.isNewerThan(QStringLiteral("0.3.4"), 2));
    }

    void invalidVersionsUseNumericCaseInsensitiveFallback()
    {
        companion::UpdateManifest manifest;
        manifest.version = QStringLiteral("release10");
        manifest.build = 1;
        QVERIFY(
            manifest.isNewerThan(QStringLiteral("release9"), 99));

        manifest.version = QStringLiteral("ALPHA");
        manifest.build = 2;
        QVERIFY(manifest.isNewerThan(QStringLiteral("alpha"), 1));
        QVERIFY(!manifest.isNewerThan(QStringLiteral("alpha"), 2));

        manifest.version = QStringLiteral("release0009");
        manifest.build = 2;
        QVERIFY(
            manifest.isNewerThan(QStringLiteral("release9"), 1));
        QVERIFY(
            !manifest.isNewerThan(QStringLiteral("release9"), 2));
    }

    void fallbackHandlesNumericRunsWithoutIntegerOverflow()
    {
        companion::UpdateManifest manifest;
        manifest.version =
            QStringLiteral("release100000000000000000000000000000000000000");
        manifest.build = 1;

        QVERIFY(manifest.isNewerThan(
            QStringLiteral(
                "release99999999999999999999999999999999999999"),
            99));
    }
};

QTEST_GUILESS_MAIN(ReleaseVersionTests)
#include "ReleaseVersionTests.moc"
