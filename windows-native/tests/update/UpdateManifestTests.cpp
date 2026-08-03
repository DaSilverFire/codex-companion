#include "update/UpdateManifest.h"

#include <array>
#include <limits>

#include <QFile>
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

QJsonObject validObject()
{
    const QJsonDocument document =
        QJsonDocument::fromJson(fixture(QStringLiteral("manifest-valid.json")));
    Q_ASSERT(document.isObject());
    return document.object();
}

QByteArray compact(QJsonObject object)
{
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
}

QString decodeErrorCode(QByteArrayView payload)
{
    const auto decoded = companion::UpdateManifest::decode(payload);
    return decoded.hasValue()
        ? QStringLiteral("<success>")
        : decoded.error().code;
}

void replaceField(
    QJsonObject* object,
    const QString& field,
    const QJsonValue& value)
{
    object->insert(field, value);
}

void verifyWrongFieldTypes()
{
    const QList<QPair<QString, QJsonValue>> cases{
        {
            QStringLiteral("schemaVersion"),
            QJsonValue(QStringLiteral("1")),
        },
        {
            QStringLiteral("version"),
            QJsonValue(304),
        },
        {
            QStringLiteral("build"),
            QJsonValue(QStringLiteral("34")),
        },
        {
            QStringLiteral("minimumSystemVersion"),
            QJsonValue(10),
        },
        {
            QStringLiteral("publishedAt"),
            QJsonValue(true),
        },
        {
            QStringLiteral("downloadURL"),
            QJsonValue(QJsonObject{}),
        },
        {
            QStringLiteral("sha256"),
            QJsonValue(QJsonValue::Null),
        },
        {
            QStringLiteral("size"),
            QJsonValue(QStringLiteral("42000")),
        },
        {
            QStringLiteral("signature"),
            QJsonValue(QJsonObject{}),
        },
    };

    for (const auto& [field, replacement] : cases) {
        QJsonObject object = validObject();
        replaceField(&object, field, replacement);
        const QString code =
            decodeErrorCode(compact(std::move(object)));
        QVERIFY2(
            code == QStringLiteral("update.invalid_manifest"),
            qPrintable(field));
    }
}

void verifyInsecureOrIncompleteDownloadUrls()
{
    const std::array urls{
        QStringLiteral(
            "http://updates.example.test/update.exe"),
        QStringLiteral("/update.exe"),
        QStringLiteral("https:///update.exe"),
        QStringLiteral("not a URL"),
    };

    for (const QString& url : urls) {
        QJsonObject object = validObject();
        object.insert(QStringLiteral("downloadURL"), url);
        const QString code =
            decodeErrorCode(compact(std::move(object)));
        QVERIFY2(
            code
                == QStringLiteral(
                    "update.insecure_download_url"),
            qPrintable(url));
    }
}

} // namespace

class UpdateManifestTests final : public QObject {
    Q_OBJECT

private slots:
    void decodesValidSchemaOneManifest()
    {
        const auto decoded = companion::UpdateManifest::decode(
            fixture(QStringLiteral("manifest-valid.json")));

        QVERIFY2(
            decoded.hasValue(),
            qPrintable(decoded.hasValue()
                           ? QString()
                           : decoded.error().message));
        QCOMPARE(decoded.value().schemaVersion, 1);
        QCOMPARE(decoded.value().version, QStringLiteral("0.3.4"));
        QCOMPARE(decoded.value().build, 34);
        QCOMPARE(
            decoded.value().minimumSystemVersion,
            QStringLiteral("10.0.22000"));
        QCOMPARE(
            decoded.value().publishedAt,
            QStringLiteral("2026-07-19T12:34:56Z"));
        QCOMPARE(
            decoded.value().downloadUrl,
            QStringLiteral(
                "https://updates.example.test/"
                "Codex-Companion-0.3.4-34-windows-x64.exe"));
        QCOMPARE(
            decoded.value().sha256,
            QStringLiteral(
                "ABCDEF0123456789ABCDEF0123456789"
                "ABCDEF0123456789ABCDEF0123456789"));
        QCOMPARE(decoded.value().size, 42000);
        QVERIFY(!decoded.value().signature.isEmpty());
    }

    void canonicalPayloadMatchesGoldenBytes()
    {
        const auto decoded = companion::UpdateManifest::decode(
            fixture(QStringLiteral("manifest-valid.json")));
        QVERIFY(decoded.hasValue());

        const QByteArray expected =
            fixture(QStringLiteral("manifest-valid.canonical"));
        QVERIFY(!expected.endsWith('\n'));
        QCOMPARE(decoded.value().canonicalPayload(), expected);
    }

    void canonicalPayloadLowercasesDigestWithoutTrailingNewline()
    {
        const auto decoded = companion::UpdateManifest::decode(
            fixture(QStringLiteral("manifest-valid.json")));
        QVERIFY(decoded.hasValue());

        const QByteArray payload = decoded.value().canonicalPayload();
        QVERIFY(payload.contains(
            "abcdef0123456789abcdef0123456789"
            "abcdef0123456789abcdef0123456789"));
        QVERIFY(!payload.contains(
            "ABCDEF0123456789ABCDEF0123456789"));
        QVERIFY(!payload.endsWith('\n'));
    }

    void rejectsMalformedJson()
    {
        QCOMPARE(
            decodeErrorCode(
                fixture(QStringLiteral("manifest-malformed.json"))),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsRootOtherThanObject_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("array") << QByteArrayLiteral("[]");
        QTest::newRow("string") << QByteArrayLiteral("\"manifest\"");
        QTest::newRow("number") << QByteArrayLiteral("1");
        QTest::newRow("null") << QByteArrayLiteral("null");
    }

    void rejectsRootOtherThanObject()
    {
        QFETCH(QByteArray, payload);
        QCOMPARE(
            decodeErrorCode(payload),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsEveryMissingField_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {
                 "schemaVersion",
                 "version",
                 "build",
                 "minimumSystemVersion",
                 "publishedAt",
                 "downloadURL",
                 "sha256",
                 "size",
                 "signature",
             }) {
            QTest::newRow(field) << QString::fromLatin1(field);
        }
    }

    void rejectsEveryMissingField()
    {
        QFETCH(QString, field);
        QJsonObject object = validObject();
        object.remove(field);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsDuplicateFields()
    {
        QCOMPARE(
            decodeErrorCode(
                fixture(
                    QStringLiteral(
                        "manifest-duplicate-fields.json"))),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsEveryWrongFieldType()
    {
        verifyWrongFieldTypes();
    }

    void rejectsWrongTypesFixture()
    {
        QCOMPARE(
            decodeErrorCode(
                fixture(QStringLiteral("manifest-wrong-types.json"))),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsUnknownFields()
    {
        QJsonObject object = validObject();
        object.insert(QStringLiteral("futureField"), true);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            QStringLiteral("update.invalid_manifest"));
    }

    void rejectsUnsupportedSchema()
    {
        QJsonObject object = validObject();
        object.insert(QStringLiteral("schemaVersion"), 2);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            QStringLiteral("update.unsupported_schema"));
    }

    void rejectsInsecureOrIncompleteDownloadUrl()
    {
        verifyInsecureOrIncompleteDownloadUrls();
    }

    void rejectsInvalidDigest_data()
    {
        QTest::addColumn<QString>("digest");
        QTest::newRow("short")
            << QString(63, QLatin1Char('a'));
        QTest::newRow("long")
            << QString(65, QLatin1Char('a'));
        QTest::newRow("non-hex")
            << (QString(63, QLatin1Char('a')) + QLatin1Char('g'));
    }

    void rejectsInvalidDigest()
    {
        QFETCH(QString, digest);
        QJsonObject object = validObject();
        object.insert(QStringLiteral("sha256"), digest);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            QStringLiteral("update.invalid_digest"));
    }

    void rejectsNonpositiveBuild_data()
    {
        QTest::addColumn<qint64>("build");
        QTest::newRow("zero") << qint64(0);
        QTest::newRow("negative") << qint64(-1);
        QTest::newRow("minimum")
            << std::numeric_limits<qint64>::min();
    }

    void rejectsNonpositiveBuild()
    {
        QFETCH(qint64, build);
        QJsonObject object = validObject();
        object.insert(QStringLiteral("build"), build);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            QStringLiteral("update.invalid_build"));
    }

    void enforcesSignedSizeBounds_data()
    {
        constexpr qint64 maximum = 512LL * 1024LL * 1024LL;
        QTest::addColumn<qint64>("size");
        QTest::addColumn<QString>("expectedCode");
        QTest::newRow("zero")
            << qint64(0)
            << QStringLiteral("update.invalid_size");
        QTest::newRow("negative")
            << qint64(-1)
            << QStringLiteral("update.invalid_size");
        QTest::newRow("minimum")
            << std::numeric_limits<qint64>::min()
            << QStringLiteral("update.invalid_size");
        QTest::newRow("maximum")
            << maximum
            << QStringLiteral("<success>");
        QTest::newRow("over-maximum")
            << maximum + 1
            << QStringLiteral("update.invalid_size");
    }

    void enforcesSignedSizeBounds()
    {
        QFETCH(qint64, size);
        QFETCH(QString, expectedCode);
        QJsonObject object = validObject();
        object.insert(QStringLiteral("size"), size);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            expectedCode);
    }

    void validatesPublishedAtAsRfc3339Utc_data()
    {
        QTest::addColumn<QString>("publishedAt");
        QTest::addColumn<QString>("expectedCode");
        QTest::newRow("seconds")
            << QStringLiteral("2026-07-19T12:34:56Z")
            << QStringLiteral("<success>");
        QTest::newRow("fractional")
            << QStringLiteral("2026-07-19T12:34:56.123456Z")
            << QStringLiteral("<success>");
        QTest::newRow("offset")
            << QStringLiteral("2026-07-19T12:34:56+00:00")
            << QStringLiteral("update.invalid_published_at");
        QTest::newRow("lowercase-z")
            << QStringLiteral("2026-07-19T12:34:56z")
            << QStringLiteral("update.invalid_published_at");
        QTest::newRow("invalid-calendar-date")
            << QStringLiteral("2026-02-30T12:34:56Z")
            << QStringLiteral("update.invalid_published_at");
        QTest::newRow("invalid-time")
            << QStringLiteral("2026-07-19T24:00:00Z")
            << QStringLiteral("update.invalid_published_at");
    }

    void validatesPublishedAtAsRfc3339Utc()
    {
        QFETCH(QString, publishedAt);
        QFETCH(QString, expectedCode);
        QJsonObject object = validObject();
        object.insert(QStringLiteral("publishedAt"), publishedAt);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            expectedCode);
    }

    void validatesWindowsDottedMinimumVersion_data()
    {
        QTest::addColumn<QString>("minimumVersion");
        QTest::addColumn<QString>("expectedCode");
        QTest::newRow("two-components")
            << QStringLiteral("11.0")
            << QStringLiteral("<success>");
        QTest::newRow("three-components")
            << QStringLiteral("10.0.22000")
            << QStringLiteral("<success>");
        QTest::newRow("four-components")
            << QStringLiteral("10.0.22000.1")
            << QStringLiteral("<success>");
        QTest::newRow("one-component")
            << QStringLiteral("10")
            << QStringLiteral("update.invalid_minimum_system_version");
        QTest::newRow("five-components")
            << QStringLiteral("10.0.22000.1.2")
            << QStringLiteral("update.invalid_minimum_system_version");
        QTest::newRow("nonnumeric")
            << QStringLiteral("10.x.22000")
            << QStringLiteral("update.invalid_minimum_system_version");
        QTest::newRow("empty-component")
            << QStringLiteral("10..22000")
            << QStringLiteral("update.invalid_minimum_system_version");
        QTest::newRow("whitespace")
            << QStringLiteral(" 10.0.22000")
            << QStringLiteral("update.invalid_minimum_system_version");
    }

    void validatesWindowsDottedMinimumVersion()
    {
        QFETCH(QString, minimumVersion);
        QFETCH(QString, expectedCode);
        QJsonObject object = validObject();
        object.insert(
            QStringLiteral("minimumSystemVersion"),
            minimumVersion);

        QCOMPARE(
            decodeErrorCode(compact(std::move(object))),
            expectedCode);
    }
};

QTEST_GUILESS_MAIN(UpdateManifestTests)
#include "UpdateManifestTests.moc"
