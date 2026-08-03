#include "update/UpdateArtifactStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace {

constexpr auto kTrustedSigner =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";
constexpr auto kSessionId =
    "{11111111-2222-3333-4444-555555555555}";
constexpr auto kSecondSessionId =
    "{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";

struct InspectionTrace final {
    QStringList paths;
    QString readyPath;
    bool readyExistedDuringInspection =
        false;
};

companion::UpdateManifest manifestFor(
    const QByteArray& payload)
{
    companion::UpdateManifest manifest;
    manifest.schemaVersion = 1;
    manifest.version =
        QStringLiteral("0.3.4");
    manifest.build = 34;
    manifest.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    manifest.publishedAt =
        QStringLiteral(
            "2026-07-24T12:00:00Z");
    manifest.downloadUrl =
        QStringLiteral(
            "https://updates.example.test/"
            "Codex-Companion-0.3.4-34-windows-x64.exe");
    manifest.sha256 =
        QString::fromLatin1(
            QCryptographicHash::hash(
                payload,
                QCryptographicHash::Sha256)
                .toHex());
    manifest.size = payload.size();
    manifest.signature =
        QStringLiteral("test-signature");
    return manifest;
}

companion::InstallerMetadata
matchingMetadata()
{
    return {
        QStringLiteral(
            "Codex Companion"),
        QStringLiteral(
            "cc-update/1|0.3.4|34|w|x64|10.0.22000"),
        QStringLiteral(
            "Codex-Companion-0.3.4-34-windows-x64.exe"),
    };
}

companion::UpdateArtifactStoreOptions
optionsFor(
    const QString& root,
    InspectionTrace* trace = nullptr)
{
    companion::UpdateArtifactStoreOptions
        options;
    options.rootPath = root;
    options.currentWindowsVersion = {
        10,
        0,
        26100,
        0,
    };
    options.allowedSignerSha256 = {
        QString::fromLatin1(
            kTrustedSigner),
    };
    options.idFactory = [] {
        return QUuid(
            QString::fromLatin1(
                kSessionId));
    };
    options.peInspector =
        [trace](QStringView path) {
            if (trace != nullptr) {
                trace->paths.append(
                    path.toString());
                trace
                    ->readyExistedDuringInspection =
                    trace
                        ->readyExistedDuringInspection
                    || QFileInfo::exists(
                        trace->readyPath);
            }
            return companion::
                Result<companion::PeMachine>::
                    success(
                        companion::
                            PeMachine::X64);
        };
    options.metadataInspector =
        [trace](QStringView path) {
            if (trace != nullptr) {
                trace->paths.append(
                    path.toString());
                trace
                    ->readyExistedDuringInspection =
                    trace
                        ->readyExistedDuringInspection
                    || QFileInfo::exists(
                        trace->readyPath);
            }
            return companion::Result<
                companion::
                    InstallerMetadata>::
                success(
                    matchingMetadata());
        };
    options.signerInspector =
        [trace](QStringView path) {
            if (trace != nullptr) {
                trace->paths.append(
                    path.toString());
                trace
                    ->readyExistedDuringInspection =
                    trace
                        ->readyExistedDuringInspection
                    || QFileInfo::exists(
                        trace->readyPath);
            }
            return companion::Result<
                companion::
                    AuthenticodeIdentity>::
                success({
                    QString::fromLatin1(
                        kTrustedSigner),
                    QStringLiteral(
                        "DaSilverFire"),
                });
        };
    return options;
}

QString resultCode(
    const companion::Result<void>& result)
{
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

QString resultCode(
    const companion::Result<
        companion::VerifiedArtifact>& result)
{
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("could not read test artifact");
    }
    return file.readAll();
}

std::wstring nativePath(
    QStringView path)
{
    return QDir::toNativeSeparators(
               QFileInfo(path.toString())
                   .absoluteFilePath())
        .toStdWString();
}

bool createHardLink(
    const QString& linkPath,
    QStringView existingPath)
{
    const std::wstring nativeLink =
        nativePath(linkPath);
    const std::wstring nativeExisting =
        nativePath(existingPath);
    return CreateHardLinkW(
               nativeLink.c_str(),
               nativeExisting.c_str(),
               nullptr)
        != FALSE;
}

bool replaceAtSamePath(
    QStringView path,
    const QString& backupPath,
    QByteArrayView replacement)
{
    const std::wstring nativeOriginal =
        nativePath(path);
    const std::wstring nativeBackup =
        nativePath(backupPath);
    if (!MoveFileW(
            nativeOriginal.c_str(),
            nativeBackup.c_str())) {
        return false;
    }

    QFile file(path.toString());
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    return file.write(
               replacement.data(),
               replacement.size())
            == replacement.size()
        && file.flush();
}

bool overwriteAtSameIdentity(
    QStringView path,
    QByteArrayView replacement)
{
    QFile file(path.toString());
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    return file.write(
               replacement.data(),
               replacement.size())
            == replacement.size()
        && file.flush();
}

bool setLastWriteTime(
    const QString& path,
    const QDateTime& time)
{
    const std::wstring native =
        QDir::toNativeSeparators(
            QFileInfo(path)
                .absoluteFilePath())
            .toStdWString();
    const HANDLE handle =
        CreateFileW(
            native.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ
                | FILE_SHARE_WRITE
                | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
    if (handle
            == INVALID_HANDLE_VALUE
        || handle == nullptr) {
        return false;
    }

    constexpr quint64 epochDeltaMilliseconds =
        11'644'473'600'000ULL;
    const quint64 ticks =
        (static_cast<quint64>(
             time.toUTC()
                 .toMSecsSinceEpoch())
         + epochDeltaMilliseconds)
        * 10'000ULL;
    ULARGE_INTEGER value{};
    value.QuadPart = ticks;
    const FILETIME fileTime{
        value.LowPart,
        value.HighPart,
    };
    const bool succeeded =
        SetFileTime(
            handle,
            nullptr,
            nullptr,
            &fileTime)
        != FALSE;
    CloseHandle(handle);
    return succeeded;
}

bool createDirectoryLink(
    const QString& link,
    const QString& target)
{
    const std::wstring nativeLink =
        QDir::toNativeSeparators(
            QFileInfo(link)
                .absoluteFilePath())
            .toStdWString();
    const std::wstring nativeTarget =
        QDir::toNativeSeparators(
            QFileInfo(target)
                .absoluteFilePath())
            .toStdWString();
    if (CreateSymbolicLinkW(
            nativeLink.c_str(),
            nativeTarget.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY
                | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        != FALSE) {
        return true;
    }

    if (!CreateDirectoryW(
            nativeLink.c_str(),
            nullptr)) {
        return false;
    }

    const HANDLE handle =
        CreateFileW(
            nativeLink.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
    if (handle
            == INVALID_HANDLE_VALUE
        || handle == nullptr) {
        RemoveDirectoryW(
            nativeLink.c_str());
        return false;
    }

    struct MountPointBuffer final {
        DWORD reparseTag;
        WORD reparseDataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };
    static_assert(
        offsetof(
            MountPointBuffer,
            pathBuffer)
        == 16);

    const std::wstring substitute =
        L"\\??\\" + nativeTarget;
    const std::wstring print =
        nativeTarget;
    const size_t substituteBytes =
        substitute.size()
        * sizeof(wchar_t);
    const size_t printBytes =
        print.size()
        * sizeof(wchar_t);
    const size_t pathBytes =
        substituteBytes
        + sizeof(wchar_t)
        + printBytes
        + sizeof(wchar_t);
    const size_t totalBytes =
        offsetof(
            MountPointBuffer,
            pathBuffer)
        + pathBytes;
    if (totalBytes
        > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        CloseHandle(handle);
        RemoveDirectoryW(
            nativeLink.c_str());
        return false;
    }

    std::vector<BYTE> storage(
        totalBytes);
    auto* buffer =
        reinterpret_cast<
            MountPointBuffer*>(
            storage.data());
    buffer->reparseTag =
        IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength =
        static_cast<WORD>(
            totalBytes - 8);
    buffer->substituteNameOffset = 0;
    buffer->substituteNameLength =
        static_cast<WORD>(
            substituteBytes);
    buffer->printNameOffset =
        static_cast<WORD>(
            substituteBytes
            + sizeof(wchar_t));
    buffer->printNameLength =
        static_cast<WORD>(
            printBytes);
    std::memcpy(
        buffer->pathBuffer,
        substitute.data(),
        substituteBytes);
    std::memcpy(
        reinterpret_cast<BYTE*>(
            buffer->pathBuffer)
            + buffer->printNameOffset,
        print.data(),
        printBytes);

    DWORD returned = 0;
    const bool succeeded =
        DeviceIoControl(
            handle,
            FSCTL_SET_REPARSE_POINT,
            buffer,
            static_cast<DWORD>(
                totalBytes),
            nullptr,
            0,
            &returned,
            nullptr)
        != FALSE;
    CloseHandle(handle);
    if (!succeeded) {
        RemoveDirectoryW(
            nativeLink.c_str());
    }
    return succeeded;
}

} // namespace

class UpdateArtifactStoreTests final
    : public QObject {
    Q_OBJECT

private slots:
    void streamsThenPublishesOnlyAfterInspection()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral(
                    "Updates"));
        InspectionTrace trace;
        trace.readyPath =
            QDir(root).filePath(
                QStringLiteral(
                    "ready/0.3.4-34/"
                    "installer.exe"));
        companion::UpdateArtifactStore store(
            optionsFor(root, &trace));
        const QByteArray payload =
            QByteArrayLiteral(
                "streamed signed installer bytes");
        const companion::UpdateManifest
            manifest =
                manifestFor(payload);

        auto started =
            store.begin(manifest);
        QVERIFY2(
            started.hasValue(),
            qPrintable(
                started.hasValue()
                    ? QString()
                    : started.error().message));
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            partial.endsWith(
                QStringLiteral(
                    ".partial")));
        QVERIFY(QFileInfo::exists(partial));
        QVERIFY(
            partial.contains(
                QStringLiteral(
                    "/staging/"),
                Qt::CaseInsensitive)
            || partial.contains(
                QStringLiteral(
                    "\\staging\\"),
                Qt::CaseInsensitive));

        const qsizetype split =
            payload.size() / 2;
        QVERIFY(
            session
                ->append(
                    QByteArrayView(
                        payload)
                        .first(split))
                .hasValue());
        QCOMPARE(
            session->receivedBytes(),
            qint64(split));
        QVERIFY(
            session
                ->append(
                    QByteArrayView(
                        payload)
                        .sliced(split))
                .hasValue());

        const auto verified =
            session->finish();

        QVERIFY2(
            verified.hasValue(),
            qPrintable(
                verified.hasValue()
                    ? QString()
                    : QStringLiteral(
                          "%1 (%2, win32=%3, path=%4, resolved=%5)")
                          .arg(
                              verified.error()
                                  .message,
                              verified.error()
                                  .code,
                              verified.error()
                                  .context
                                  .value(
                                      QStringLiteral(
                                          "win32Error"))
                                  .toString(),
                              verified.error()
                                  .context
                                  .value(
                                      QStringLiteral(
                                          "path"))
                                  .toString(),
                              verified.error()
                                  .context
                                  .value(
                                      QStringLiteral(
                                          "resolvedPath"))
                                  .toString())));
        QCOMPARE(
            verified.value().path,
            trace.readyPath);
        QCOMPARE(
            verified.value().size,
            payload.size());
        QCOMPARE(
            verified.value().sha256,
            QCryptographicHash::hash(
                payload,
                QCryptographicHash::Sha256));
        QCOMPARE(
            readFile(
                verified.value().path),
            payload);
        QVERIFY(!QFileInfo::exists(partial));
        QCOMPARE(trace.paths.size(), 3);
        for (const QString& path :
             trace.paths) {
            QCOMPARE(path, partial);
        }
        QVERIFY(
            !trace
                 .readyExistedDuringInspection);
    }

    void abortsBeforeWritingPastSignedSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::UpdateArtifactStore store(
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates"))));
        const QByteArray payload =
            QByteArrayLiteral("four");
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();

        const auto appended =
            session->append(
                QByteArrayLiteral(
                    "five!"));

        QCOMPARE(
            resultCode(appended),
            QStringLiteral(
                "update.artifact_size_exceeded"));
        QVERIFY(!QFileInfo::exists(partial));
        QCOMPARE(
            session->receivedBytes(),
            qint64(0));
    }

    void requiresExactFinalSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::UpdateArtifactStore store(
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates"))));
        const QByteArray payload =
            QByteArrayLiteral(
                "complete payload");
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            session
                ->append(
                    QByteArrayView(payload)
                        .first(4))
                .hasValue());

        const auto verified =
            session->finish();

        QCOMPARE(
            resultCode(verified),
            QStringLiteral(
                "update.artifact_size_mismatch"));
        QVERIFY(!QFileInfo::exists(partial));
    }

    void requiresExactStreamingDigest()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        InspectionTrace trace;
        companion::UpdateArtifactStore store(
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates")),
                &trace));
        const QByteArray payload =
            QByteArrayLiteral(
                "digest checked payload");
        companion::UpdateManifest manifest =
            manifestFor(payload);
        manifest.sha256 =
            QString(64, QLatin1Char('0'));
        auto started =
            store.begin(manifest);
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            session->append(payload)
                .hasValue());

        const auto verified =
            session->finish();

        QCOMPARE(
            resultCode(verified),
            QStringLiteral(
                "update.artifact_digest_mismatch"));
        QVERIFY(!QFileInfo::exists(partial));
        QVERIFY(trace.paths.isEmpty());
    }

    void rejectsHardLinkCreatedAfterWriterCloses()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload =
            QByteArrayLiteral(
                "hard-link checked payload");
        QString hardLinkPath;
        bool hardLinkCreated = false;
        auto options =
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates")));
        options.afterWriterClosed =
            [&](QStringView path) {
                hardLinkPath =
                    path.toString()
                    + QStringLiteral(
                        ".linked");
                hardLinkCreated =
                    createHardLink(
                        hardLinkPath,
                        path);
            };
        companion::UpdateArtifactStore store(
            std::move(options));
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            session->append(payload)
                .hasValue());

        const auto verified =
            session->finish();

        QVERIFY(hardLinkCreated);
        QCOMPARE(
            resultCode(verified),
            QStringLiteral(
                "update.artifact_link_count_invalid"));
        QVERIFY(!QFileInfo::exists(partial));
        QVERIFY(QFileInfo::exists(hardLinkPath));
        QVERIFY(QFile::remove(hardLinkPath));
    }

    void rejectsReplacementAfterWriterCloses()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload =
            QByteArrayLiteral(
                "identity checked payload");
        QString backupPath;
        bool replaced = false;
        auto options =
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates")));
        options.afterWriterClosed =
            [&](QStringView path) {
                backupPath =
                    path.toString()
                    + QStringLiteral(
                        ".original");
                replaced =
                    replaceAtSamePath(
                        path,
                        backupPath,
                        payload);
            };
        companion::UpdateArtifactStore store(
            std::move(options));
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            session->append(payload)
                .hasValue());

        const auto verified =
            session->finish();

        QVERIFY(replaced);
        QCOMPARE(
            resultCode(verified),
            QStringLiteral(
                "update.artifact_identity_changed"));
        QVERIFY(!QFileInfo::exists(partial));
        QVERIFY(QFileInfo::exists(backupPath));
        QVERIFY(QFile::remove(backupPath));
    }

    void rejectsMutationBeforePublishReopen()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral(
                    "Updates"));
        const QByteArray payload =
            QByteArrayLiteral(
                "publish digest payload");
        const QByteArray replacement(
            payload.size(),
            'X');
        InspectionTrace trace;
        trace.readyPath =
            QDir(root).filePath(
                QStringLiteral(
                    "ready/0.3.4-34/"
                    "installer.exe"));
        bool overwritten = false;
        auto options =
            optionsFor(root, &trace);
        options.beforePublishReopen =
            [&](QStringView path) {
                overwritten =
                    overwriteAtSameIdentity(
                        path,
                        replacement);
            };
        companion::UpdateArtifactStore store(
            std::move(options));
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        QVERIFY(
            session->append(payload)
                .hasValue());

        const auto verified =
            session->finish();

        QVERIFY(overwritten);
        QCOMPARE(
            resultCode(verified),
            QStringLiteral(
                "update.artifact_digest_mismatch"));
        QCOMPARE(trace.paths.size(), 3);
        QVERIFY(!QFileInfo::exists(partial));
        QVERIFY(!QFileInfo::exists(trace.readyPath));
    }

    void cancelRemovesPartialDirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::UpdateArtifactStore store(
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates"))));
        const QByteArray payload =
            QByteArrayLiteral(
                "cancelled payload");
        auto started =
            store.begin(
                manifestFor(payload));
        QVERIFY(started.hasValue());
        auto session =
            std::move(started.value());
        const QString partial =
            session->partialPath();
        const QString sessionDirectory =
            QFileInfo(partial)
                .absolutePath();
        QVERIFY(
            session
                ->append(
                    QByteArrayView(payload)
                        .first(5))
                .hasValue());

        session->cancel();

        QVERIFY(!QFileInfo::exists(partial));
        QVERIFY(
            !QFileInfo::exists(
                sessionDirectory));
        QCOMPARE(
            resultCode(
                session->append(
                    QByteArrayLiteral(
                        "x"))),
            QStringLiteral(
                "update.artifact_session_closed"));
    }

    void neverOverwritesReadyArtifact()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral(
                    "Updates"));
        const QByteArray payload =
            QByteArrayLiteral(
                "first verified installer");
        const companion::UpdateManifest
            manifest =
                manifestFor(payload);
        companion::UpdateArtifactStore firstStore(
            optionsFor(root));
        auto firstStarted =
            firstStore.begin(manifest);
        QVERIFY(firstStarted.hasValue());
        auto first =
            std::move(
                firstStarted.value());
        QVERIFY(
            first->append(payload)
                .hasValue());
        const auto firstResult =
            first->finish();
        QVERIFY(firstResult.hasValue());

        auto secondOptions =
            optionsFor(root);
        secondOptions.idFactory = [] {
            return QUuid(
                QString::fromLatin1(
                    kSecondSessionId));
        };
        companion::UpdateArtifactStore
            secondStore(
                std::move(
                    secondOptions));
        auto secondStarted =
            secondStore.begin(manifest);
        QVERIFY(secondStarted.hasValue());
        auto second =
            std::move(
                secondStarted.value());
        QVERIFY(
            second->append(payload)
                .hasValue());

        const auto secondResult =
            second->finish();

        QCOMPARE(
            resultCode(secondResult),
            QStringLiteral(
                "update.ready_artifact_exists"));
        QCOMPARE(
            readFile(
                firstResult.value().path),
            payload);
    }

    void rejectsAlternateStreamStoragePath()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::UpdateArtifactStore store(
            optionsFor(
                directory.filePath(
                    QStringLiteral(
                        "Updates:stream"))));

        const auto started =
            store.begin(
                manifestFor(
                    QByteArrayLiteral(
                        "payload")));

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "update.invalid_storage_path"));
    }

    void rejectsReparsePointInStorageChain()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target =
            directory.filePath(
                QStringLiteral("target"));
        QVERIFY(QDir().mkpath(target));
        const QString link =
            directory.filePath(
                QStringLiteral("linked"));
        if (!createDirectoryLink(
                link,
                target)) {
            QSKIP(
                "Unprivileged directory links are unavailable.");
        }
        companion::UpdateArtifactStore store(
            optionsFor(
                QDir(link).filePath(
                    QStringLiteral(
                        "Updates"))));

        const auto started =
            store.begin(
                manifestFor(
                    QByteArrayLiteral(
                        "payload")));

        QVERIFY(!started.hasValue());
        QCOMPARE(
            started.error().code,
            QStringLiteral(
                "update.unsafe_reparse_path"));
        QVERIFY(
            !QFileInfo::exists(
                QDir(target).filePath(
                    QStringLiteral(
                        "Updates"))));
    }

    void prunesExpiredDirectoriesButKeepsActive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral(
                    "Updates"));
        const QString staging =
            QDir(root).filePath(
                QStringLiteral("staging"));
        const QString ready =
            QDir(root).filePath(
                QStringLiteral("ready"));
        QVERIFY(QDir().mkpath(staging));
        QVERIFY(QDir().mkpath(ready));

        const QString oldPartial =
            QDir(staging).filePath(
                QStringLiteral(
                    "old-partial"));
        const QString freshPartial =
            QDir(staging).filePath(
                QStringLiteral(
                    "fresh-partial"));
        const QString oldReady =
            QDir(ready).filePath(
                QStringLiteral(
                    "0.3.1-31"));
        const QString activeReady =
            QDir(ready).filePath(
                QStringLiteral(
                    "0.3.2-32"));
        for (const QString& path : {
                 oldPartial,
                 freshPartial,
                 oldReady,
                 activeReady,
             }) {
            QVERIFY(QDir().mkpath(path));
            QFile marker(
                QDir(path).filePath(
                    QStringLiteral(
                        "installer.exe")));
            QVERIFY(
                marker.open(
                    QIODevice::WriteOnly));
            marker.write("x");
            marker.close();
        }

        const QDateTime now =
            QDateTime::fromString(
                QStringLiteral(
                    "2026-07-24T20:00:00Z"),
                Qt::ISODate);
        QVERIFY(
            setLastWriteTime(
                oldPartial,
                now.addSecs(
                    -25 * 60 * 60)));
        QVERIFY(
            setLastWriteTime(
                freshPartial,
                now.addSecs(
                    -23 * 60 * 60)));
        QVERIFY(
            setLastWriteTime(
                oldReady,
                now.addDays(-15)));
        QVERIFY(
            setLastWriteTime(
                activeReady,
                now.addDays(-20)));

        auto options =
            optionsFor(root);
        options.clock = [now] {
            return now;
        };
        companion::UpdateArtifactStore store(
            std::move(options));
        const QString activeArtifact =
            QDir(activeReady).filePath(
                QStringLiteral(
                    "installer.exe"));

        const auto pruned =
            store.prune(activeArtifact);

        QVERIFY(pruned.hasValue());
        QVERIFY(
            !QFileInfo::exists(
                oldPartial));
        QVERIFY(
            QFileInfo::exists(
                freshPartial));
        QVERIFY(
            !QFileInfo::exists(
                oldReady));
        QVERIFY(
            QFileInfo::exists(
                activeReady));
    }
};

QTEST_GUILESS_MAIN(
    UpdateArtifactStoreTests)
#include "UpdateArtifactStoreTests.moc"
