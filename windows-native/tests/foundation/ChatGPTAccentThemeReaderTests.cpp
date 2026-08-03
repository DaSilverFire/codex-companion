#include "platform/windows/ChatGPTAccentThemeReader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeStorageFile(
    const QString& path,
    const QByteArray& payload,
    const QDateTime& modifiedAt)
{
    QFile file(path);
    QVERIFY(file.open(
        QIODevice::WriteOnly
        | QIODevice::Truncate));
    QCOMPARE(file.write(payload), payload.size());
    QVERIFY(file.flush());
    QVERIFY(file.setFileTime(
        modifiedAt,
        QFileDevice::FileModificationTime));
}

QString createStorageDirectory(
    const QString& root,
    const QString& relativePath)
{
    const QString path =
        QDir(root).filePath(relativePath);
    return QDir().mkpath(path)
        ? path
        : QString();
}

} // namespace

class ChatGPTAccentThemeReaderTests final
    : public QObject {
    Q_OBJECT

private slots:
    void payloadTheme_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<int>("theme");

        const auto add = [](
                             const char* name,
                             const char* value,
                             companion::
                                 ChatGPTAccentTheme theme) {
            QTest::newRow(name)
                << QByteArray(value)
                << static_cast<int>(theme);
        };
        add("blue", "blue",
            companion::ChatGPTAccentTheme::Blue);
        add("green", "green",
            companion::ChatGPTAccentTheme::Green);
        add("orange", "orange",
            companion::ChatGPTAccentTheme::Orange);
        add("pink", "pink",
            companion::ChatGPTAccentTheme::Pink);
        add("purple", "purple",
            companion::ChatGPTAccentTheme::Purple);
        add("red", "red",
            companion::ChatGPTAccentTheme::Red);
        add("teal", "teal",
            companion::ChatGPTAccentTheme::Teal);
        add("yellow", "yellow",
            companion::ChatGPTAccentTheme::Yellow);
        add("default", "default",
            companion::ChatGPTAccentTheme::Blue);
    }

    void payloadTheme()
    {
        QFETCH(QByteArray, value);
        QFETCH(int, theme);
        const QByteArray payload =
            QByteArrayLiteral(
                "unrelated\0chatTheme/user-test\1")
            + QByteArrayLiteral("\"")
            + value
            + QByteArrayLiteral("\"");

        const auto parsed =
            companion::ChatGPTAccentThemeReader::
                themeInPayload(payload);

        QVERIFY(parsed.has_value());
        QCOMPARE(
            static_cast<int>(*parsed),
            theme);
    }

    void ignoresUnrelatedColorText()
    {
        const auto parsed =
            companion::ChatGPTAccentThemeReader::
                themeInPayload(
                    QByteArrayLiteral(
                        "\"orange\" without a theme key"));
        QVERIFY(!parsed.has_value());
    }

    void readsPrefixCompressedChromiumThemeEntry()
    {
        const QByteArray payload = QByteArray::fromHex(
            "1935092f617070732f018198"
            "5468656d652f757365722d394b4a5446"
            "71776230387377656138635266666b35"
            "557341016c1e0001607801"
            "226f72616e676522");

        const auto parsed =
            companion::ChatGPTAccentThemeReader::
                themeInPayload(payload);

        QVERIFY(parsed.has_value());
        QCOMPARE(
            *parsed,
            companion::ChatGPTAccentTheme::Orange);
    }

    void ignoresCompressedThemeMarkerWithoutStorageContext()
    {
        const auto parsed =
            companion::ChatGPTAccentThemeReader::
                themeInPayload(
                    QByteArrayLiteral(
                        "Theme/user-test\1\"orange\""));
        QVERIFY(!parsed.has_value());
    }

    void compressedThemeUsesNearestValue()
    {
        const QByteArray payload =
            QByteArrayLiteral(
                "/apps/\1Theme/user-test\1\"orange\"")
            + QByteArray(16, '\0')
            + QByteArrayLiteral("\"default\"");

        const auto parsed =
            companion::ChatGPTAccentThemeReader::
                themeInPayload(payload);

        QVERIFY(parsed.has_value());
        QCOMPARE(
            *parsed,
            companion::ChatGPTAccentTheme::Orange);
    }

    void newestStorageFileWins()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDateTime now =
            QDateTime::currentDateTimeUtc();
        writeStorageFile(
            directory.filePath(
                QStringLiteral("000001.ldb")),
            QByteArrayLiteral(
                "chatTheme/user-test\1\"purple\""),
            now.addSecs(-60));
        writeStorageFile(
            directory.filePath(
                QStringLiteral("000002.log")),
            QByteArrayLiteral(
                "chatTheme/user-test\1\"orange\""),
            now);

        QCOMPARE(
            companion::ChatGPTAccentThemeReader::
                themeInStorageDirectory(
                    directory.path()),
            companion::ChatGPTAccentTheme::Orange);
    }

    void codexStorageWithoutThemeFallsBackToChatGPT()
    {
        QTemporaryDir localAppData;
        QVERIFY(localAppData.isValid());
        const QDateTime now =
            QDateTime::currentDateTimeUtc();

        const QString chatStorage =
            createStorageDirectory(
                localAppData.path(),
                QStringLiteral(
                    "Packages/"
                    "OpenAI.ChatGPT-Desktop_test/"
                    "LocalCache/Roaming/ChatGPT/"
                    "Local Storage/leveldb"));
        QVERIFY(!chatStorage.isEmpty());
        writeStorageFile(
            QDir(chatStorage).filePath(
                QStringLiteral("000001.ldb")),
            QByteArrayLiteral(
                "/apps/\1Theme/user-test\1"
                "\"orange\""),
            now.addSecs(-60));

        const QString codexStorage =
            createStorageDirectory(
                localAppData.path(),
                QStringLiteral(
                    "Packages/OpenAI.Codex_test/"
                    "LocalCache/Roaming/Codex/web/"
                    "Codex/Default/Local Storage/"
                    "leveldb"));
        QVERIFY(!codexStorage.isEmpty());
        writeStorageFile(
            QDir(codexStorage).filePath(
                QStringLiteral("000002.ldb")),
            QByteArrayLiteral(
                "newer Codex storage without a "
                "chat theme"),
            now);

        QCOMPARE(
            companion::ChatGPTAccentThemeReader::
                currentThemeInRoots(
                    localAppData.path(),
                    QString()),
            companion::ChatGPTAccentTheme::Orange);
    }

    void newestThemeWinsAcrossCodexAndChatGPT()
    {
        QTemporaryDir localAppData;
        QVERIFY(localAppData.isValid());
        const QDateTime now =
            QDateTime::currentDateTimeUtc();

        const QString chatStorage =
            createStorageDirectory(
                localAppData.path(),
                QStringLiteral(
                    "Packages/"
                    "OpenAI.ChatGPT-Desktop_test/"
                    "LocalCache/Roaming/ChatGPT/"
                    "Local Storage/leveldb"));
        QVERIFY(!chatStorage.isEmpty());
        writeStorageFile(
            QDir(chatStorage).filePath(
                QStringLiteral("000001.ldb")),
            QByteArrayLiteral(
                "/apps/\1Theme/user-test\1"
                "\"orange\""),
            now.addSecs(-60));

        const QString codexStorage =
            createStorageDirectory(
                localAppData.path(),
                QStringLiteral(
                    "Packages/OpenAI.Codex_test/"
                    "LocalCache/Roaming/Codex/"
                    "Local Storage/leveldb"));
        QVERIFY(!codexStorage.isEmpty());
        writeStorageFile(
            QDir(codexStorage).filePath(
                QStringLiteral("000002.log")),
            QByteArrayLiteral(
                "/apps/\1Theme/user-test\1"
                "\"green\""),
            now);

        QCOMPARE(
            companion::ChatGPTAccentThemeReader::
                currentThemeInRoots(
                    localAppData.path(),
                    QString()),
            companion::ChatGPTAccentTheme::Green);
    }

    void colorNamesMatchMacPalette_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QString>("color");

        const auto add = [](
                             const char* name,
                             companion::
                                 ChatGPTAccentTheme theme,
                             const char* color) {
            QTest::newRow(name)
                << static_cast<int>(theme)
                << QString::fromLatin1(color);
        };
        add("blue",
            companion::ChatGPTAccentTheme::Blue,
            "#297af5");
        add("green",
            companion::ChatGPTAccentTheme::Green,
            "#21ad66");
        add("orange",
            companion::ChatGPTAccentTheme::Orange,
            "#ff6e14");
        add("pink",
            companion::ChatGPTAccentTheme::Pink,
            "#f04f94");
        add("purple",
            companion::ChatGPTAccentTheme::Purple,
            "#965ceb");
        add("red",
            companion::ChatGPTAccentTheme::Red,
            "#eb3d40");
        add("teal",
            companion::ChatGPTAccentTheme::Teal,
            "#1aabab");
        add("yellow",
            companion::ChatGPTAccentTheme::Yellow,
            "#edb324");
    }

    void colorNamesMatchMacPalette()
    {
        QFETCH(int, theme);
        QFETCH(QString, color);
        QCOMPARE(
            companion::ChatGPTAccentThemeReader::
                colorName(
                    static_cast<
                        companion::ChatGPTAccentTheme>(
                        theme)),
            color);
    }
};

QTEST_GUILESS_MAIN(
    ChatGPTAccentThemeReaderTests)
#include "ChatGPTAccentThemeReaderTests.moc"
