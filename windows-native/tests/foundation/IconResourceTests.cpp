#include <algorithm>
#include <vector>

#include <QFile>
#include <QtEndian>
#include <QtTest>

#include <windows.h>

class IconResourceTests final : public QObject {
    Q_OBJECT

private slots:
    void generatedIconContainsRequiredBinaryContract()
    {
        QFile file(QString::fromLocal8Bit(qgetenv("COMPANION_TEST_ICON")));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.size() > 6);
        QCOMPARE(qFromLittleEndian<quint16>(
                     reinterpret_cast<const uchar*>(bytes.constData())),
                 quint16(0));
        QCOMPARE(qFromLittleEndian<quint16>(
                     reinterpret_cast<const uchar*>(bytes.constData() + 2)),
                 quint16(1));

        const auto count = qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar*>(bytes.constData() + 4));
        QCOMPARE(count, quint16(8));

        const QByteArray pngSignature =
            QByteArray::fromHex("89504E470D0A1A0A");
        QSet<int> sizes;
        QVector<QPair<quint32, quint32>> payloadRanges;
        for (quint16 index = 0; index < count; ++index) {
            const int offset = 6 + index * 16;
            QVERIFY(offset + 16 <= bytes.size());
            const uchar encodedWidth = static_cast<uchar>(bytes.at(offset));
            const uchar encodedHeight = static_cast<uchar>(bytes.at(offset + 1));
            const int width = encodedWidth == 0 ? 256 : encodedWidth;
            const int height = encodedHeight == 0 ? 256 : encodedHeight;
            QCOMPARE(width, height);
            QCOMPARE(static_cast<uchar>(bytes.at(offset + 2)), uchar(0));
            QCOMPARE(static_cast<uchar>(bytes.at(offset + 3)), uchar(0));
            QCOMPARE(qFromLittleEndian<quint16>(
                         reinterpret_cast<const uchar*>(bytes.constData() + offset + 4)),
                     quint16(1));
            QCOMPARE(qFromLittleEndian<quint16>(
                         reinterpret_cast<const uchar*>(bytes.constData() + offset + 6)),
                     quint16(32));

            const quint32 bytesInResource = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(bytes.constData() + offset + 8));
            QVERIFY(bytesInResource > 0);

            const quint32 imageOffset = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(bytes.constData() + offset + 12));
            QVERIFY(imageOffset >= quint32(6 + (count * 16)));
            QVERIFY(imageOffset < quint32(bytes.size()));
            QVERIFY(imageOffset + bytesInResource <= quint32(bytes.size()));

            const QByteArray payload =
                bytes.mid(static_cast<int>(imageOffset), static_cast<int>(bytesInResource));
            QCOMPARE(payload.left(pngSignature.size()), pngSignature);

            payloadRanges.append(qMakePair(imageOffset, imageOffset + bytesInResource));
            sizes.insert(width);
        }

        QCOMPARE(payloadRanges.size(), 8);
        std::sort(payloadRanges.begin(), payloadRanges.end());
        for (int index = 1; index < payloadRanges.size(); ++index) {
            QVERIFY(payloadRanges.at(index - 1).second <= payloadRanges.at(index).first);
        }

        QCOMPARE(sizes, QSet<int>({16, 20, 24, 32, 40, 48, 64, 256}));
    }

    void executableContainsIconGroupResource101()
    {
        const HMODULE module = GetModuleHandleW(nullptr);
        QVERIFY(module != nullptr);

        const HRSRC resource =
            FindResourceW(module, MAKEINTRESOURCEW(101), RT_GROUP_ICON);
        QVERIFY(resource != nullptr);
    }

    void executableContainsRelayConfigurationVersionKey()
    {
        std::vector<wchar_t> executablePath(
            32'768);
        const DWORD pathLength =
            GetModuleFileNameW(
                nullptr,
                executablePath.data(),
                DWORD(executablePath.size()));
        QVERIFY(pathLength > 0);
        QVERIFY(
            pathLength
            < executablePath.size());

        DWORD ignoredHandle = 0;
        const DWORD versionBytes =
            GetFileVersionInfoSizeW(
                executablePath.data(),
                &ignoredHandle);
        QVERIFY(versionBytes > 0);

        std::vector<BYTE> versionInfo(
            versionBytes);
        QVERIFY(
            GetFileVersionInfoW(
                executablePath.data(),
                0,
                versionBytes,
                versionInfo.data()));

        void* rawValue = nullptr;
        UINT valueCharacters = 0;
        QVERIFY(
            VerQueryValueW(
                versionInfo.data(),
                L"\\StringFileInfo\\040904B0\\CompanionRelayURL",
                &rawValue,
                &valueCharacters));
        QVERIFY(rawValue != nullptr);

        const auto* text =
            static_cast<const wchar_t*>(
                rawValue);
        const QString configured =
            valueCharacters == 0
            ? QString()
            : QString::fromWCharArray(
                  text,
                  int(valueCharacters - 1));
        QCOMPARE(
            configured,
            QString::fromLocal8Bit(
                qgetenv(
                    "COMPANION_TEST_RELAY_URL")));
        QVERIFY(!configured.isEmpty());
        QVERIFY(
            configured.startsWith(
                QStringLiteral("wss://")));
    }

    void packagedExecutableContainsIconAndRelayConfiguration()
    {
        const QString executablePath =
            QString::fromLocal8Bit(
                qgetenv(
                    "COMPANION_TEST_APP_EXECUTABLE"));
        QVERIFY(!executablePath.isEmpty());
        const std::wstring nativePath =
            executablePath.toStdWString();

        const HMODULE module =
            LoadLibraryExW(
                nativePath.c_str(),
                nullptr,
                LOAD_LIBRARY_AS_DATAFILE
                    | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        QVERIFY(module != nullptr);
        const HRSRC icon =
            FindResourceW(
                module,
                MAKEINTRESOURCEW(101),
                RT_GROUP_ICON);
        QVERIFY(icon != nullptr);
        QVERIFY(FreeLibrary(module));

        DWORD ignoredHandle = 0;
        const DWORD versionBytes =
            GetFileVersionInfoSizeW(
                nativePath.c_str(),
                &ignoredHandle);
        QVERIFY(versionBytes > 0);
        std::vector<BYTE> versionInfo(
            versionBytes);
        QVERIFY(
            GetFileVersionInfoW(
                nativePath.c_str(),
                0,
                versionBytes,
                versionInfo.data()));

        void* rawValue = nullptr;
        UINT valueCharacters = 0;
        QVERIFY(
            VerQueryValueW(
                versionInfo.data(),
                L"\\StringFileInfo\\040904B0\\CompanionRelayURL",
                &rawValue,
                &valueCharacters));
        QVERIFY(rawValue != nullptr);
        QVERIFY(valueCharacters > 1);
        QCOMPARE(
            QString::fromWCharArray(
                static_cast<
                    const wchar_t*>(
                    rawValue),
                int(valueCharacters - 1)),
            QString::fromLocal8Bit(
                qgetenv(
                    "COMPANION_TEST_RELAY_URL")));
    }
};

QTEST_GUILESS_MAIN(IconResourceTests)
#include "IconResourceTests.moc"
