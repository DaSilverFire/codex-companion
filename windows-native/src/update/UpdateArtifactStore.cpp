#include "update/UpdateArtifactStore.h"

#include "update/UpdateCompatibility.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

namespace companion {
namespace {

constexpr qint64 kStagingRetentionHours =
    24;
constexpr qint64 kReadyRetentionDays =
    14;
constexpr int kMaximumSessionIdAttempts =
    8;

class UniqueHandle final {
public:
    explicit UniqueHandle(
        HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(
        const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other)
        noexcept
        : handle_(
              std::exchange(
                  other.handle_,
                  INVALID_HANDLE_VALUE))
    {
    }

    UniqueHandle& operator=(
        UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(
                std::exchange(
                    other.handle_,
                    INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    bool valid() const noexcept
    {
        return handle_
            != INVALID_HANDLE_VALUE
            && handle_ != nullptr;
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

    HANDLE release() noexcept
    {
        return std::exchange(
            handle_,
            INVALID_HANDLE_VALUE);
    }

    void reset(
        HANDLE replacement =
            INVALID_HANDLE_VALUE)
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ =
        INVALID_HANDLE_VALUE;
};

CompanionError artifactError(
    QString code,
    QString message,
    QStringView path = {},
    DWORD win32Error = ERROR_SUCCESS,
    QVariantMap context = {})
{
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"),
            path.toString());
    }
    if (win32Error != ERROR_SUCCESS) {
        context.insert(
            QStringLiteral("win32Error"),
            QVariant::fromValue<qulonglong>(
                win32Error));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

QString cleanAbsolutePath(
    const QString& path)
{
    return QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
}

QString normalizedPath(
    const QString& path)
{
    return QDir::fromNativeSeparators(
        cleanAbsolutePath(path));
}

bool sameWindowsPath(
    const QString& left,
    const QString& right)
{
    return normalizedPath(left).compare(
               normalizedPath(right),
               Qt::CaseInsensitive)
        == 0;
}

bool isWithinTree(
    const QString& root,
    const QString& candidate)
{
    const QString normalizedRoot =
        normalizedPath(root);
    const QString normalizedCandidate =
        normalizedPath(candidate);
    return normalizedCandidate.compare(
               normalizedRoot,
               Qt::CaseInsensitive)
            == 0
        || normalizedCandidate.startsWith(
            normalizedRoot
                + QLatin1Char('/'),
            Qt::CaseInsensitive);
}

QString apiPath(const QString& path)
{
    QString native =
        QDir::toNativeSeparators(
            cleanAbsolutePath(path));
    if (native.startsWith(
            QStringLiteral("\\\\?\\"))) {
        return native;
    }
    if (native.startsWith(
            QStringLiteral("\\\\"))) {
        return QStringLiteral(
                   "\\\\?\\UNC\\")
            + native.sliced(2);
    }
    return QStringLiteral("\\\\?\\")
        + native;
}

bool hasAlternateStreamSyntax(
    const QString& path)
{
    const QString native =
        QDir::toNativeSeparators(path);
    const qsizetype firstColon =
        native.indexOf(QLatin1Char(':'));
    if (firstColon < 0) {
        return false;
    }
    return firstColon != 1
        || native.indexOf(
               QLatin1Char(':'),
               firstColon + 1)
            >= 0;
}

DWORD attributes(const QString& path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    return GetFileAttributesW(
        native.c_str());
}

bool isMissingError(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND
        || error == ERROR_PATH_NOT_FOUND;
}

Result<void> validatePathChain(
    const QString& path)
{
    QString current =
        cleanAbsolutePath(path);
    while (!current.isEmpty()) {
        const DWORD value =
            attributes(current);
        if (value
            != INVALID_FILE_ATTRIBUTES) {
            if ((value
                 & FILE_ATTRIBUTE_REPARSE_POINT)
                != 0) {
                return Result<void>::failure(
                    artifactError(
                        QStringLiteral(
                            "update.unsafe_reparse_path"),
                        QStringLiteral(
                            "The update storage path cannot contain filesystem links."),
                        current));
            }
            if ((value
                 & FILE_ATTRIBUTE_DEVICE)
                != 0) {
                return Result<void>::failure(
                    artifactError(
                        QStringLiteral(
                            "update.unsafe_device_path"),
                        QStringLiteral(
                            "The update storage path cannot contain device entries."),
                        current));
            }
        } else {
            const DWORD error =
                GetLastError();
            if (!isMissingError(error)) {
                return Result<void>::failure(
                    artifactError(
                        QStringLiteral(
                            "update.storage_inspection_failed"),
                        QStringLiteral(
                            "The update storage path could not be inspected."),
                        current,
                        error));
            }
        }

        const QString parent =
            cleanAbsolutePath(
                QFileInfo(current)
                    .absolutePath());
        if (parent.isEmpty()
            || sameWindowsPath(
                current,
                parent)) {
            break;
        }
        current = parent;
    }
    return Result<void>::success();
}

Result<void> ensureSafeDirectory(
    const QString& path)
{
    const QString absolute =
        cleanAbsolutePath(path);
    if (absolute.isEmpty()
        || hasAlternateStreamSyntax(
            absolute)) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.invalid_storage_path"),
                QStringLiteral(
                    "The update storage path is invalid."),
                absolute));
    }

    const Result<void> before =
        validatePathChain(absolute);
    if (!before.hasValue()) {
        return before;
    }

    DWORD value = attributes(absolute);
    if (value
        != INVALID_FILE_ATTRIBUTES) {
        if ((value
             & FILE_ATTRIBUTE_DIRECTORY)
                == 0) {
            return Result<void>::failure(
                artifactError(
                    QStringLiteral(
                        "update.storage_not_directory"),
                    QStringLiteral(
                        "The update storage path is not a directory."),
                    absolute));
        }
        return Result<void>::success();
    }

    const DWORD inspectionError =
        GetLastError();
    if (!isMissingError(
            inspectionError)) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.storage_inspection_failed"),
                QStringLiteral(
                    "The update storage path could not be inspected."),
                absolute,
                inspectionError));
    }

    const QString parent =
        cleanAbsolutePath(
            QFileInfo(absolute)
                .absolutePath());
    if (!sameWindowsPath(
            absolute,
            parent)) {
        const Result<void> parentReady =
            ensureSafeDirectory(parent);
        if (!parentReady.hasValue()) {
            return parentReady;
        }
    }

    const std::wstring native =
        apiPath(absolute).toStdWString();
    if (!CreateDirectoryW(
            native.c_str(),
            nullptr)) {
        const DWORD error =
            GetLastError();
        if (error
            != ERROR_ALREADY_EXISTS) {
            return Result<void>::failure(
                artifactError(
                    QStringLiteral(
                        "update.storage_create_failed"),
                    QStringLiteral(
                        "The update storage directory could not be created."),
                    absolute,
                    error));
        }
    }

    value = attributes(absolute);
    if (value
            == INVALID_FILE_ATTRIBUTES
        || (value
            & FILE_ATTRIBUTE_DIRECTORY)
            == 0
        || (value
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))
            != 0) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.unsafe_storage_directory"),
                QStringLiteral(
                    "The update storage directory is unsafe."),
                absolute,
                value
                        == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_SUCCESS));
    }
    return validatePathChain(absolute);
}

struct NativeFileIdentity final {
    quint64 volumeSerialNumber = 0;
    FILE_ID_128 fileId{};
};

struct NativeFileSnapshot final {
    NativeFileIdentity identity;
    qint64 size = 0;
    DWORD linkCount = 0;
};

struct NativeDigest final {
    qint64 size = 0;
    QByteArray sha256;
};

bool sameFileIdentity(
    const NativeFileIdentity& left,
    const NativeFileIdentity& right)
{
    return left.volumeSerialNumber
            == right.volumeSerialNumber
        && std::memcmp(
               left.fileId.Identifier,
               right.fileId.Identifier,
               sizeof(left.fileId.Identifier))
            == 0;
}

Result<QString> finalHandlePath(
    HANDLE handle,
    QStringView diagnosticPath)
{
    std::vector<wchar_t> buffer(
        32768,
        L'\0');
    const DWORD length =
        GetFinalPathNameByHandleW(
            handle,
            buffer.data(),
            static_cast<DWORD>(
                buffer.size()),
            FILE_NAME_NORMALIZED
                | VOLUME_NAME_DOS);
    if (length == 0
        || length >= buffer.size()) {
        return Result<QString>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_path_unavailable"),
                QStringLiteral(
                    "The update installer path could not be resolved."),
                diagnosticPath,
                GetLastError()));
    }

    QString resolved =
        QString::fromWCharArray(
            buffer.data(),
            static_cast<qsizetype>(
                length));
    if (resolved.startsWith(
            QStringLiteral(
                "\\\\?\\UNC\\"))) {
        resolved =
            QStringLiteral("\\\\")
            + resolved.sliced(8);
    } else if (resolved.startsWith(
                   QStringLiteral(
                       "\\\\?\\"))) {
        resolved = resolved.sliced(4);
    }
    return Result<QString>::success(
        cleanAbsolutePath(resolved));
}

Result<NativeFileSnapshot>
inspectRegularHandle(
    HANDLE handle,
    QStringView path)
{
    if (GetFileType(handle)
        != FILE_TYPE_DISK) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_not_disk_file"),
                QStringLiteral(
                    "The update installer is not a disk file."),
                path));
    }

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &information,
            sizeof(information))) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_inspection_failed"),
                QStringLiteral(
                    "The update installer could not be inspected."),
                path,
                GetLastError()));
    }
    if ((information.FileAttributes
         & FILE_ATTRIBUTE_DIRECTORY)
        != 0) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_not_regular_file"),
                QStringLiteral(
                    "The update installer is not a regular file."),
                path));
    }
    if ((information.FileAttributes
         & FILE_ATTRIBUTE_REPARSE_POINT)
        != 0) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_reparse_point"),
                QStringLiteral(
                    "The update installer cannot be a filesystem link."),
                path));
    }
    if ((information.FileAttributes
         & FILE_ATTRIBUTE_DEVICE)
        != 0) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_device"),
                QStringLiteral(
                    "The update installer cannot be a device."),
                path));
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileStandardInfo,
            &standard,
            sizeof(standard))) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_inspection_failed"),
                QStringLiteral(
                    "The update installer metadata could not be inspected."),
                path,
                GetLastError()));
    }
    if (standard.Directory) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_not_regular_file"),
                QStringLiteral(
                    "The update installer is not a regular file."),
                path));
    }

    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            &identity,
            sizeof(identity))) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_identity_unavailable"),
                QStringLiteral(
                    "The update installer identity could not be read."),
                path,
                GetLastError()));
    }

    const auto resolved =
        finalHandlePath(handle, path);
    if (!resolved.hasValue()) {
        return Result<
            NativeFileSnapshot>::failure(
            resolved.error());
    }
    if (!sameWindowsPath(
            resolved.value(),
            path.toString())) {
        return Result<
            NativeFileSnapshot>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_path_changed"),
                QStringLiteral(
                    "The update installer resolved outside its staging path."),
                path,
                ERROR_SUCCESS,
                {
                    {
                        QStringLiteral(
                            "resolvedPath"),
                        resolved.value(),
                    },
                }));
    }

    return Result<
        NativeFileSnapshot>::success({
        {
            identity.VolumeSerialNumber,
            identity.FileId,
        },
        standard.EndOfFile.QuadPart,
        standard.NumberOfLinks,
    });
}

Result<UniqueHandle> openPinnedDirectory(
    const QString& path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    UniqueHandle handle(
        CreateFileW(
            native.c_str(),
            FILE_LIST_DIRECTORY
                | FILE_ADD_FILE
                | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ
                | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    if (!handle.valid()) {
        return Result<
            UniqueHandle>::failure(
            artifactError(
                QStringLiteral(
                    "update.storage_pin_failed"),
                QStringLiteral(
                    "The update storage directory could not be pinned."),
                path,
                GetLastError()));
    }

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (GetFileType(handle.get())
            != FILE_TYPE_DISK
        || !GetFileInformationByHandleEx(
            handle.get(),
            FileAttributeTagInfo,
            &information,
            sizeof(information))
        || (information.FileAttributes
            & FILE_ATTRIBUTE_DIRECTORY)
            == 0
        || (information.FileAttributes
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))
            != 0) {
        return Result<
            UniqueHandle>::failure(
            artifactError(
                QStringLiteral(
                    "update.unsafe_storage_directory"),
                QStringLiteral(
                    "The update storage directory is unsafe."),
                path,
                GetLastError()));
    }

    const auto resolved =
        finalHandlePath(
            handle.get(),
            path);
    if (!resolved.hasValue()) {
        return Result<
            UniqueHandle>::failure(
            resolved.error());
    }
    if (!sameWindowsPath(
            resolved.value(),
            path)) {
        return Result<
            UniqueHandle>::failure(
            artifactError(
                QStringLiteral(
                    "update.storage_path_changed"),
                QStringLiteral(
                    "The update storage directory resolved outside its expected path."),
                path,
                ERROR_SUCCESS,
                {
                    {
                        QStringLiteral(
                            "resolvedPath"),
                        resolved.value(),
                    },
                }));
    }
    return Result<UniqueHandle>::success(
        std::move(handle));
}

Result<NativeDigest> hashHandle(
    HANDLE handle,
    QStringView path,
    qint64 maximumBytes)
{
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(
            handle,
            zero,
            nullptr,
            FILE_BEGIN)) {
        return Result<NativeDigest>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_read_failed"),
                QStringLiteral(
                    "The staged update installer could not be read."),
                path,
                GetLastError()));
    }

    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    QByteArray buffer(
        1024 * 1024,
        Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        DWORD read = 0;
        if (!ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()),
                &read,
                nullptr)) {
            return Result<
                NativeDigest>::failure(
                artifactError(
                    QStringLiteral(
                        "update.artifact_read_failed"),
                    QStringLiteral(
                        "The staged update installer could not be read."),
                    path,
                    GetLastError()));
        }
        if (read == 0) {
            break;
        }
        if (maximumBytes < 0
            || total
                    > maximumBytes
                        - static_cast<
                            qint64>(read)) {
            return Result<
                NativeDigest>::failure(
                artifactError(
                    QStringLiteral(
                        "update.artifact_size_exceeded"),
                    QStringLiteral(
                        "The update installer exceeded the signed size."),
                    path));
        }
        hash.addData(
            QByteArrayView(
                buffer.constData(),
                static_cast<qsizetype>(
                    read)));
        total +=
            static_cast<qint64>(read);
    }
    return Result<NativeDigest>::success({
        total,
        hash.result(),
    });
}

Result<void> validateExpectedSnapshot(
    const NativeFileSnapshot& snapshot,
    const NativeFileIdentity& expectedIdentity,
    qint64 expectedSize,
    QStringView path)
{
    if (!sameFileIdentity(
            snapshot.identity,
            expectedIdentity)) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_identity_changed"),
                QStringLiteral(
                    "The staged update installer was replaced during verification."),
                path));
    }
    if (snapshot.linkCount != 1) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_link_count_invalid"),
                QStringLiteral(
                    "The update installer has an unsafe hard-link count."),
                path,
                ERROR_SUCCESS,
                {
                    {
                        QStringLiteral(
                            "linkCount"),
                        QVariant::fromValue<
                            qulonglong>(
                            snapshot.linkCount),
                    },
                }));
    }
    if (snapshot.size != expectedSize) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_size_mismatch"),
                QStringLiteral(
                    "The update installer size changed while it was staged."),
                path,
                ERROR_SUCCESS,
                {
                    {
                        QStringLiteral(
                            "expectedSize"),
                        expectedSize,
                    },
                    {
                        QStringLiteral(
                            "actualSize"),
                        snapshot.size,
                    },
                }));
    }
    return Result<void>::success();
}

Result<void> validateExpectedDigest(
    const NativeDigest& digest,
    qint64 expectedSize,
    QByteArrayView streamedDigest,
    QStringView signedDigest,
    QStringView path)
{
    if (digest.size != expectedSize) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_size_mismatch"),
                QStringLiteral(
                    "The update installer size changed while it was verified."),
                path));
    }
    const QByteArray expected =
        QByteArray::fromHex(
            signedDigest.toLatin1());
    if (expected.size() != 32
        || digest.sha256.size() != 32
        || digest.sha256
            != streamedDigest
        || digest.sha256 != expected) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_digest_mismatch"),
                QStringLiteral(
                    "The update installer digest does not match the signed manifest."),
                path));
    }
    return Result<void>::success();
}

bool removeSafeTree(
    const QString& root,
    const QString& path)
{
    if (!isWithinTree(root, path)) {
        return false;
    }

    const DWORD value = attributes(path);
    if (value
        == INVALID_FILE_ATTRIBUTES) {
        return isMissingError(
            GetLastError());
    }
    if ((value
         & (FILE_ATTRIBUTE_REPARSE_POINT
            | FILE_ATTRIBUTE_DEVICE))
        != 0) {
        return false;
    }

    if ((value
         & FILE_ATTRIBUTE_DIRECTORY)
        == 0) {
        const std::wstring native =
            apiPath(path).toStdWString();
        return DeleteFileW(
                   native.c_str())
            != FALSE;
    }

    const QFileInfoList entries =
        QDir(path).entryInfoList(
            QDir::AllEntries
                | QDir::NoDotAndDotDot
                | QDir::Hidden
                | QDir::System,
            QDir::NoSort);
    for (const QFileInfo& entry :
         entries) {
        if (!removeSafeTree(
                root,
                entry.absoluteFilePath())) {
            return false;
        }
    }

    const std::wstring native =
        apiPath(path).toStdWString();
    return RemoveDirectoryW(
               native.c_str())
        != FALSE;
}

void removeEmptyDirectory(
    const QString& path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    (void)RemoveDirectoryW(
        native.c_str());
}

bool isSafeVersionComponent(
    QStringView version)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            R"(^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$)"));
    return pattern.matchView(version)
        .hasMatch()
        && !version.endsWith(
            QLatin1Char('.'))
        && !version.endsWith(
            QLatin1Char(' '));
}

Result<void> renameHandle(
    HANDLE handle,
    QStringView destination)
{
    const std::wstring native =
        QDir::toNativeSeparators(
            cleanAbsolutePath(
                destination.toString()))
            .toStdWString();
    const DWORD filenameBytes =
        static_cast<DWORD>(
            native.size()
            * sizeof(wchar_t));
    const size_t bufferBytes =
        offsetof(
            FILE_RENAME_INFO,
            FileName)
        + filenameBytes
        + sizeof(wchar_t);
    std::vector<std::byte> buffer(
        bufferBytes);
    auto* information =
        reinterpret_cast<FILE_RENAME_INFO*>(
            buffer.data());
    information->ReplaceIfExists =
        FALSE;
    information->RootDirectory =
        nullptr;
    information->FileNameLength =
        filenameBytes;
    std::memcpy(
        information->FileName,
        native.data(),
        filenameBytes);

    if (!SetFileInformationByHandle(
            handle,
            FileRenameInfo,
            information,
            static_cast<DWORD>(
                buffer.size()))) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_publish_failed"),
                QStringLiteral(
                    "The verified update installer could not be published."),
                destination,
                GetLastError()));
    }
    return Result<void>::success();
}

Result<void> writeAll(
    HANDLE handle,
    QByteArrayView bytes,
    QStringView path)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qsizetype remaining =
            bytes.size() - offset;
        const DWORD requested =
            static_cast<DWORD>(
                std::min<quint64>(
                    static_cast<quint64>(
                        remaining),
                    std::numeric_limits<
                        DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                handle,
                bytes.data() + offset,
                requested,
                &written,
                nullptr)
            || written == 0) {
            return Result<void>::failure(
                artifactError(
                    QStringLiteral(
                        "update.artifact_write_failed"),
                    QStringLiteral(
                        "The update installer could not be written."),
                    path,
                    GetLastError()));
        }
        offset +=
            static_cast<qsizetype>(
                written);
    }
    return Result<void>::success();
}

} // namespace

struct UpdateArtifactStoreState final {
    QString rootPath;
    UpdateArtifactClock clock;
    UpdateArtifactIdFactory idFactory;
    UpdatePeInspector peInspector;
    UpdateMetadataInspector
        metadataInspector;
    UpdateSignerInspector signerInspector;
    UpdateArtifactPathHook
        afterWriterClosed;
    UpdateArtifactPathHook
        beforePublishReopen;
    UpdateCompatibility compatibility;

    explicit UpdateArtifactStoreState(
        UpdateArtifactStoreOptions options)
        : rootPath(
              cleanAbsolutePath(
                  options.rootPath
                          .trimmed()
                          .isEmpty()
                      ? UpdateArtifactStore::
                            defaultRootPath()
                      : std::move(
                            options.rootPath))),
          clock(
              options.clock
                  ? std::move(
                        options.clock)
                  : UpdateArtifactClock([] {
                        return QDateTime::
                            currentDateTimeUtc();
                    })),
          idFactory(
              options.idFactory
                  ? std::move(
                        options.idFactory)
                  : UpdateArtifactIdFactory(
                        &QUuid::createUuid)),
          peInspector(
              options.peInspector
                  ? std::move(
                        options.peInspector)
                  : UpdatePeInspector(
                        [](QStringView path) {
                            return PeImageInspector()
                                .machine(path);
                        })),
          metadataInspector(
              options.metadataInspector
                  ? std::move(
                        options
                            .metadataInspector)
                  : UpdateMetadataInspector(
                        [](QStringView path) {
                            return InstallerMetadataReader()
                                .read(path);
                        })),
          signerInspector(
              options.signerInspector
                  ? std::move(
                        options.signerInspector)
                  : UpdateSignerInspector(
                        [](QStringView path) {
                            return AuthenticodeVerifier()
                                .verify(
                                    path,
                                    AuthenticodePolicy::
                                        fromBuildConfiguration());
                        })),
          afterWriterClosed(
              std::move(
                  options
                      .afterWriterClosed)),
          beforePublishReopen(
              std::move(
                  options
                      .beforePublishReopen)),
          compatibility(
              options.currentWindowsVersion,
              std::move(
                  options
                      .allowedSignerSha256))
    {
    }
};

struct UpdateArtifactStagingSession::
    Implementation final {
    std::shared_ptr<
        UpdateArtifactStoreState>
        store;
    UpdateManifest manifest;
    QString sessionDirectory;
    QString partialFilePath;
    QString readyDirectory;
    QString readyFilePath;
    UniqueHandle handle;
    std::vector<UniqueHandle>
        pinnedDirectories;
    NativeFileIdentity originalIdentity;
    bool identityCaptured = false;
    QCryptographicHash hash{
        QCryptographicHash::Sha256,
    };
    qint64 bytesWritten = 0;
    bool closed = false;
    bool published = false;

    void cleanup() noexcept
    {
        handle.reset();
        pinnedDirectories.clear();
        if (!published
            && !partialFilePath.isEmpty()) {
            const DWORD value =
                attributes(
                    partialFilePath);
            if (value
                    != INVALID_FILE_ATTRIBUTES
                && (value
                    & FILE_ATTRIBUTE_REPARSE_POINT)
                    == 0) {
                const std::wstring native =
                    apiPath(
                        partialFilePath)
                        .toStdWString();
                (void)DeleteFileW(
                    native.c_str());
            }
        }
        if (!sessionDirectory.isEmpty()) {
            removeEmptyDirectory(
                sessionDirectory);
        }
        if (!readyDirectory.isEmpty()) {
            removeEmptyDirectory(
                readyDirectory);
        }
        closed = true;
    }

    template <typename T>
    Result<T> fail(
        CompanionError error)
    {
        cleanup();
        return Result<T>::failure(
            std::move(error));
    }
};

UpdateArtifactStagingSession::
UpdateArtifactStagingSession(
    std::unique_ptr<Implementation>
        implementation)
    : implementation_(
          std::move(implementation))
{
}

UpdateArtifactStagingSession::
~UpdateArtifactStagingSession()
{
    cancel();
}

UpdateArtifactStagingSession::
UpdateArtifactStagingSession(
    UpdateArtifactStagingSession&&)
    noexcept = default;

UpdateArtifactStagingSession&
UpdateArtifactStagingSession::operator=(
    UpdateArtifactStagingSession&&)
    noexcept = default;

Result<void>
UpdateArtifactStagingSession::append(
    QByteArrayView bytes)
{
    if (implementation_ == nullptr
        || implementation_->closed
        || !implementation_->handle.valid()) {
        return Result<void>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_session_closed"),
                QStringLiteral(
                    "The update staging session is closed.")));
    }
    if (bytes.isEmpty()) {
        return Result<void>::success();
    }

    const qint64 remaining =
        implementation_->manifest.size
        - implementation_->bytesWritten;
    if (remaining < 0
        || bytes.size() > remaining) {
        return implementation_->fail<void>(
            artifactError(
                QStringLiteral(
                    "update.artifact_size_exceeded"),
                QStringLiteral(
                    "The update installer exceeded the signed size."),
                implementation_
                    ->partialFilePath,
                ERROR_SUCCESS,
                {
                    {
                        QStringLiteral(
                            "expectedSize"),
                        implementation_
                            ->manifest.size,
                    },
                    {
                        QStringLiteral(
                            "receivedSize"),
                        implementation_
                                ->bytesWritten
                            + bytes.size(),
                    },
                }));
    }

    const Result<void> written =
        writeAll(
            implementation_->handle.get(),
            bytes,
            implementation_
                ->partialFilePath);
    if (!written.hasValue()) {
        return implementation_->fail<void>(
            written.error());
    }
    implementation_->hash.addData(bytes);
    implementation_->bytesWritten +=
        bytes.size();
    return Result<void>::success();
}

Result<VerifiedArtifact>
UpdateArtifactStagingSession::finish()
{
    if (implementation_ == nullptr
        || implementation_->closed
        || !implementation_->handle.valid()) {
        return Result<
            VerifiedArtifact>::failure(
            artifactError(
                QStringLiteral(
                    "update.artifact_session_closed"),
                QStringLiteral(
                    "The update staging session is closed.")));
    }

    if (implementation_->bytesWritten
        != implementation_->manifest.size) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.artifact_size_mismatch"),
                    QStringLiteral(
                        "The update installer size does not match the signed manifest."),
                    implementation_
                        ->partialFilePath,
                    ERROR_SUCCESS,
                    {
                        {
                            QStringLiteral(
                                "expectedSize"),
                            implementation_
                                ->manifest.size,
                        },
                        {
                            QStringLiteral(
                                "actualSize"),
                            implementation_
                                ->bytesWritten,
                        },
                    }));
    }

    if (!FlushFileBuffers(
            implementation_->handle.get())) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.artifact_flush_failed"),
                    QStringLiteral(
                        "The update installer could not be flushed to disk."),
                    implementation_
                        ->partialFilePath,
                    GetLastError()));
    }

    const auto flushedSnapshot =
        inspectRegularHandle(
            implementation_->handle.get(),
            implementation_
                ->partialFilePath);
    if (!flushedSnapshot.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                flushedSnapshot.error());
    }
    if (!implementation_->identityCaptured) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.artifact_identity_unavailable"),
                    QStringLiteral(
                        "The update installer identity was not captured."),
                    implementation_
                        ->partialFilePath));
    }
    const Result<void> flushedMatches =
        validateExpectedSnapshot(
            flushedSnapshot.value(),
            implementation_
                ->originalIdentity,
            implementation_
                ->bytesWritten,
            implementation_
                ->partialFilePath);
    if (!flushedMatches.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                flushedMatches.error());
    }

    const QByteArray streamedDigest =
        implementation_->hash.result();
    implementation_->handle.reset();

    if (implementation_->store
            ->afterWriterClosed) {
        try {
            implementation_->store
                ->afterWriterClosed(
                    implementation_
                        ->partialFilePath);
        } catch (...) {
            return implementation_->
                fail<VerifiedArtifact>(
                    artifactError(
                        QStringLiteral(
                            "update.artifact_verification_failed"),
                        QStringLiteral(
                            "The update installer verification hook failed."),
                        implementation_
                            ->partialFilePath));
        }
    }

    const std::wstring partialNative =
        apiPath(
            implementation_
                ->partialFilePath)
            .toStdWString();
    implementation_->handle.reset(
        CreateFileW(
            partialNative.c_str(),
            GENERIC_READ
                | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    if (!implementation_->handle.valid()) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.artifact_reopen_failed"),
                    QStringLiteral(
                        "The staged update installer could not be reopened for verification."),
                    implementation_
                        ->partialFilePath,
                    GetLastError()));
    }

    const auto verificationSnapshot =
        inspectRegularHandle(
            implementation_->handle.get(),
            implementation_
                ->partialFilePath);
    if (!verificationSnapshot.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                verificationSnapshot.error());
    }
    const Result<void> verificationMatches =
        validateExpectedSnapshot(
            verificationSnapshot.value(),
            implementation_
                ->originalIdentity,
            implementation_
                ->bytesWritten,
            implementation_
                ->partialFilePath);
    if (!verificationMatches.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                verificationMatches.error());
    }

    const auto diskDigest =
        hashHandle(
            implementation_->handle.get(),
            implementation_
                ->partialFilePath,
            implementation_
                ->manifest.size);
    if (!diskDigest.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                diskDigest.error());
    }
    const Result<void> digestMatches =
        validateExpectedDigest(
            diskDigest.value(),
            implementation_
                ->manifest.size,
            streamedDigest,
            implementation_
                ->manifest.sha256,
            implementation_
                ->partialFilePath);
    if (!digestMatches.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                digestMatches.error());
    }

    ArtifactFacts facts;
    facts.path =
        implementation_->partialFilePath;
    facts.exists = true;
    facts.regularFile = true;
    facts.reparsePoint = false;
    facts.size =
        implementation_->bytesWritten;
    facts.sha256 =
        diskDigest.value().sha256;

    const auto machine =
        implementation_->store
            ->peInspector(
                facts.path);
    if (!machine.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                machine.error());
    }
    facts.machine = machine.value();

    const auto metadata =
        implementation_->store
            ->metadataInspector(
                facts.path);
    if (!metadata.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                metadata.error());
    }
    facts.metadata =
        metadata.value();

    const auto signer =
        implementation_->store
            ->signerInspector(
                facts.path);
    if (!signer.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                signer.error());
    }
    facts.signer = signer.value();

    const Result<void> compatible =
        implementation_->store
            ->compatibility.validate(
                implementation_->manifest,
                facts);
    if (!compatible.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                compatible.error());
    }

    const QString readyRoot =
        QDir(
            implementation_->store
                ->rootPath)
            .filePath(
                QStringLiteral("ready"));
    const Result<void> readyRootResult =
        ensureSafeDirectory(readyRoot);
    if (!readyRootResult.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                readyRootResult.error());
    }
    auto pinnedReadyRoot =
        openPinnedDirectory(
            readyRoot);
    if (!pinnedReadyRoot.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                pinnedReadyRoot.error());
    }

    implementation_->readyDirectory =
        QDir(readyRoot).filePath(
            QStringLiteral("%1-%2")
                .arg(
                    implementation_
                        ->manifest.version)
                .arg(
                    implementation_
                        ->manifest.build));
    const Result<void> readyResult =
        ensureSafeDirectory(
            implementation_
                ->readyDirectory);
    if (!readyResult.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                readyResult.error());
    }
    auto pinnedReadyDirectory =
        openPinnedDirectory(
            implementation_
                ->readyDirectory);
    if (!pinnedReadyDirectory.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                pinnedReadyDirectory.error());
    }
    implementation_->readyFilePath =
        QDir(
            implementation_
                ->readyDirectory)
            .filePath(
                QStringLiteral(
                    "installer.exe"));
    if (attributes(
            implementation_
                ->readyFilePath)
        != INVALID_FILE_ATTRIBUTES) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.ready_artifact_exists"),
                    QStringLiteral(
                        "A verified installer for this update already exists."),
                    implementation_
                        ->readyFilePath));
    }

    implementation_->pinnedDirectories
        .push_back(
            std::move(
                pinnedReadyRoot.value()));
    implementation_->pinnedDirectories
        .push_back(
            std::move(
                pinnedReadyDirectory.value()));
    implementation_->handle.reset();
    if (implementation_->store
            ->beforePublishReopen) {
        try {
            implementation_->store
                ->beforePublishReopen(
                    implementation_
                        ->partialFilePath);
        } catch (...) {
            return implementation_->
                fail<VerifiedArtifact>(
                    artifactError(
                        QStringLiteral(
                            "update.artifact_verification_failed"),
                        QStringLiteral(
                            "The update installer publication hook failed."),
                        implementation_
                            ->partialFilePath));
        }
    }

    implementation_->handle.reset(
        CreateFileW(
            partialNative.c_str(),
            GENERIC_READ
                | FILE_READ_ATTRIBUTES
                | DELETE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    if (!implementation_->handle.valid()) {
        return implementation_->
            fail<VerifiedArtifact>(
                artifactError(
                    QStringLiteral(
                        "update.artifact_publish_reopen_failed"),
                    QStringLiteral(
                        "The verified update installer could not be reopened for publication."),
                    implementation_
                        ->partialFilePath,
                    GetLastError()));
    }

    const auto publishSnapshot =
        inspectRegularHandle(
            implementation_->handle.get(),
            implementation_
                ->partialFilePath);
    if (!publishSnapshot.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                publishSnapshot.error());
    }
    const Result<void> publishMatches =
        validateExpectedSnapshot(
            publishSnapshot.value(),
            implementation_
                ->originalIdentity,
            implementation_
                ->bytesWritten,
            implementation_
                ->partialFilePath);
    if (!publishMatches.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                publishMatches.error());
    }

    const auto publishDigest =
        hashHandle(
            implementation_->handle.get(),
            implementation_
                ->partialFilePath,
            implementation_
                ->manifest.size);
    if (!publishDigest.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                publishDigest.error());
    }
    const Result<void> publishDigestMatches =
        validateExpectedDigest(
            publishDigest.value(),
            implementation_
                ->manifest.size,
            streamedDigest,
            implementation_
                ->manifest.sha256,
            implementation_
                ->partialFilePath);
    if (!publishDigestMatches.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                publishDigestMatches.error());
    }

    const Result<void> renamed =
        renameHandle(
            implementation_->handle.get(),
            implementation_
                ->readyFilePath);
    if (!renamed.hasValue()) {
        return implementation_->
            fail<VerifiedArtifact>(
                renamed.error());
    }

    const auto publishedPath =
        finalHandlePath(
            implementation_->handle.get(),
            implementation_
                ->readyFilePath);
    if (!publishedPath.hasValue()
        || !sameWindowsPath(
            publishedPath.hasValue()
                ? publishedPath.value()
                : QString(),
            implementation_
                ->readyFilePath)) {
        return implementation_->
            fail<VerifiedArtifact>(
                publishedPath.hasValue()
                ? artifactError(
                      QStringLiteral(
                          "update.artifact_publish_path_mismatch"),
                      QStringLiteral(
                          "The verified update installer was published to an unexpected path."),
                      implementation_
                          ->readyFilePath,
                      ERROR_SUCCESS,
                      {
                          {
                              QStringLiteral(
                                  "resolvedPath"),
                              publishedPath
                                  .value(),
                          },
                      })
                : publishedPath.error());
    }

    implementation_->published = true;
    implementation_->closed = true;
    implementation_->handle.reset();
    implementation_->pinnedDirectories
        .clear();
    removeEmptyDirectory(
        implementation_
            ->sessionDirectory);

    facts.path =
        implementation_->readyFilePath;
    return Result<
        VerifiedArtifact>::success({
        facts.path,
        facts.size,
        facts.sha256,
        facts.metadata,
        facts.signer,
    });
}

void UpdateArtifactStagingSession::cancel()
    noexcept
{
    if (implementation_ != nullptr
        && !implementation_->closed) {
        implementation_->cleanup();
    }
}

QString
UpdateArtifactStagingSession::partialPath()
    const
{
    return implementation_ == nullptr
        ? QString()
        : implementation_
              ->partialFilePath;
}

qint64
UpdateArtifactStagingSession::receivedBytes()
    const noexcept
{
    return implementation_ == nullptr
        ? 0
        : implementation_
              ->bytesWritten;
}

QString UpdateArtifactStore::defaultRootPath()
{
    QString localAppData =
        qEnvironmentVariable(
            "LOCALAPPDATA");
    if (localAppData.trimmed().isEmpty()) {
        localAppData =
            QStandardPaths::
                writableLocation(
                    QStandardPaths::
                        GenericDataLocation);
    }
    return QDir(localAppData).filePath(
        QStringLiteral(
            "Codex Companion/Updates"));
}

UpdateArtifactStore::UpdateArtifactStore(
    UpdateArtifactStoreOptions options)
    : state_(
          std::make_shared<
              UpdateArtifactStoreState>(
              std::move(options)))
{
}

UpdateArtifactStore::~UpdateArtifactStore() =
    default;

Result<std::unique_ptr<
    UpdateArtifactStagingSession>>
UpdateArtifactStore::begin(
    const UpdateManifest& manifest)
{
    if (!isSafeVersionComponent(
            manifest.version)) {
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                artifactError(
                    QStringLiteral(
                        "update.invalid_version_path"),
                    QStringLiteral(
                        "The update version cannot be used as a storage path component.")));
    }
    if (state_->rootPath.isEmpty()
        || hasAlternateStreamSyntax(
            state_->rootPath)) {
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                artifactError(
                    QStringLiteral(
                        "update.invalid_storage_path"),
                    QStringLiteral(
                        "The update storage path is invalid."),
                    state_->rootPath));
    }

    const Result<void> pruned = prune();
    if (!pruned.hasValue()) {
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(pruned.error());
    }

    const QString stagingRoot =
        QDir(state_->rootPath).filePath(
            QStringLiteral("staging"));
    const Result<void> stagingReady =
        ensureSafeDirectory(
            stagingRoot);
    if (!stagingReady.hasValue()) {
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                stagingReady.error());
    }

    QString sessionDirectory;
    for (int attempt = 0;
         attempt < kMaximumSessionIdAttempts;
         ++attempt) {
        const QUuid id =
            state_->idFactory();
        if (id.isNull()) {
            continue;
        }
        const QString name =
            id.toString(
                  QUuid::WithoutBraces)
                .toLower();
        const QString candidate =
            QDir(stagingRoot).filePath(
                name);
        const std::wstring native =
            apiPath(candidate)
                .toStdWString();
        if (CreateDirectoryW(
                native.c_str(),
                nullptr)) {
            sessionDirectory =
                candidate;
            break;
        }
        if (GetLastError()
            != ERROR_ALREADY_EXISTS) {
            return Result<std::unique_ptr<
                UpdateArtifactStagingSession>>::
                failure(
                    artifactError(
                        QStringLiteral(
                            "update.staging_create_failed"),
                        QStringLiteral(
                            "The update staging directory could not be created."),
                        candidate,
                        GetLastError()));
        }
    }
    if (sessionDirectory.isEmpty()) {
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                artifactError(
                    QStringLiteral(
                        "update.staging_collision"),
                    QStringLiteral(
                        "A unique update staging directory could not be created."),
                    stagingRoot));
    }

    const Result<void> sessionSafe =
        validatePathChain(
            sessionDirectory);
    if (!sessionSafe.hasValue()) {
        removeEmptyDirectory(
            sessionDirectory);
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                sessionSafe.error());
    }

    std::vector<UniqueHandle>
        pinnedDirectories;
    for (const QString& directory : {
             state_->rootPath,
             stagingRoot,
             sessionDirectory,
         }) {
        auto pinned =
            openPinnedDirectory(
                directory);
        if (!pinned.hasValue()) {
            pinnedDirectories.clear();
            removeEmptyDirectory(
                sessionDirectory);
            return Result<std::unique_ptr<
                UpdateArtifactStagingSession>>::
                failure(
                    pinned.error());
        }
        pinnedDirectories.push_back(
            std::move(
                pinned.value()));
    }

    const QString partialPath =
        QDir(sessionDirectory)
            .filePath(
                QStringLiteral(
                    "installer.exe.partial"));
    const std::wstring nativePartial =
        apiPath(partialPath)
            .toStdWString();
    UniqueHandle handle(
        CreateFileW(
            nativePartial.c_str(),
            GENERIC_READ
                | GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    if (!handle.valid()) {
        const DWORD error =
            GetLastError();
        pinnedDirectories.clear();
        removeEmptyDirectory(
            sessionDirectory);
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                artifactError(
                    QStringLiteral(
                        "update.artifact_create_failed"),
                    QStringLiteral(
                        "The update installer staging file could not be created."),
                    partialPath,
                    error));
    }

    const auto initialSnapshot =
        inspectRegularHandle(
            handle.get(),
            partialPath);
    if (!initialSnapshot.hasValue()
        || initialSnapshot.value().size
            != 0
        || initialSnapshot.value().linkCount
            != 1) {
        const CompanionError error =
            initialSnapshot.hasValue()
            ? artifactError(
                  initialSnapshot
                              .value()
                              .linkCount
                          != 1
                      ? QStringLiteral(
                            "update.artifact_link_count_invalid")
                      : QStringLiteral(
                            "update.artifact_size_mismatch"),
                  initialSnapshot
                              .value()
                              .linkCount
                          != 1
                      ? QStringLiteral(
                            "The update installer has an unsafe hard-link count.")
                      : QStringLiteral(
                            "The new update staging file was not empty."),
                  partialPath)
            : initialSnapshot.error();
        handle.reset();
        pinnedDirectories.clear();
        const std::wstring native =
            apiPath(partialPath)
                .toStdWString();
        (void)DeleteFileW(
            native.c_str());
        removeEmptyDirectory(
            sessionDirectory);
        return Result<std::unique_ptr<
            UpdateArtifactStagingSession>>::
            failure(
                error);
    }

    auto implementation =
        std::make_unique<
            UpdateArtifactStagingSession::
                Implementation>();
    implementation->store = state_;
    implementation->manifest =
        manifest;
    implementation->sessionDirectory =
        sessionDirectory;
    implementation->partialFilePath =
        partialPath;
    implementation->handle =
        std::move(handle);
    implementation->pinnedDirectories =
        std::move(
            pinnedDirectories);
    implementation->originalIdentity =
        initialSnapshot
            .value()
            .identity;
    implementation->identityCaptured =
        true;
    return Result<std::unique_ptr<
        UpdateArtifactStagingSession>>::
        success(
            std::unique_ptr<
                UpdateArtifactStagingSession>(
                new UpdateArtifactStagingSession(
                    std::move(
                        implementation))));
}

Result<void> UpdateArtifactStore::prune(
    QStringView activeArtifactPath)
    const
{
    const DWORD rootAttributes =
        attributes(state_->rootPath);
    if (rootAttributes
        == INVALID_FILE_ATTRIBUTES) {
        return isMissingError(
                   GetLastError())
            ? Result<void>::success()
            : Result<void>::failure(
                  artifactError(
                      QStringLiteral(
                          "update.storage_inspection_failed"),
                      QStringLiteral(
                          "The update storage path could not be inspected."),
                      state_->rootPath,
                      GetLastError()));
    }

    const Result<void> rootSafe =
        validatePathChain(
            state_->rootPath);
    if (!rootSafe.hasValue()) {
        return rootSafe;
    }

    const QDateTime now =
        state_->clock().toUTC();
    const QString active =
        activeArtifactPath
                .trimmed()
                .isEmpty()
            ? QString()
            : cleanAbsolutePath(
                  activeArtifactPath
                      .toString());

    struct PruneRoot final {
        QString name;
        QDateTime cutoff;
    };
    const QList<PruneRoot> roots{
        {
            QStringLiteral("staging"),
            now.addSecs(
                -kStagingRetentionHours
                * 60 * 60),
        },
        {
            QStringLiteral("ready"),
            now.addDays(
                -kReadyRetentionDays),
        },
    };

    for (const PruneRoot& definition :
         roots) {
        const QString directory =
            QDir(state_->rootPath)
                .filePath(
                    definition.name);
        const DWORD value =
            attributes(directory);
        if (value
            == INVALID_FILE_ATTRIBUTES) {
            if (isMissingError(
                    GetLastError())) {
                continue;
            }
            return Result<void>::failure(
                artifactError(
                    QStringLiteral(
                        "update.storage_inspection_failed"),
                    QStringLiteral(
                        "The update storage path could not be inspected."),
                    directory,
                    GetLastError()));
        }
        if ((value
             & FILE_ATTRIBUTE_DIRECTORY)
                == 0
            || (value
                & (FILE_ATTRIBUTE_REPARSE_POINT
                   | FILE_ATTRIBUTE_DEVICE))
                != 0) {
            return Result<void>::failure(
                artifactError(
                    QStringLiteral(
                        "update.unsafe_storage_directory"),
                    QStringLiteral(
                        "The update storage directory is unsafe."),
                    directory));
        }

        const QFileInfoList candidates =
            QDir(directory).entryInfoList(
                QDir::Dirs
                    | QDir::NoDotAndDotDot
                    | QDir::Hidden
                    | QDir::System,
                QDir::NoSort);
        for (const QFileInfo& candidate :
             candidates) {
            const QString candidatePath =
                candidate.absoluteFilePath();
            const DWORD candidateAttributes =
                attributes(candidatePath);
            if (candidateAttributes
                    == INVALID_FILE_ATTRIBUTES
                || (candidateAttributes
                    & (FILE_ATTRIBUTE_REPARSE_POINT
                       | FILE_ATTRIBUTE_DEVICE))
                    != 0
                || (candidateAttributes
                    & FILE_ATTRIBUTE_DIRECTORY)
                    == 0
                || !candidate.lastModified()
                        .isValid()
                || candidate.lastModified()
                       .toUTC()
                    >= definition.cutoff
                || (!active.isEmpty()
                    && isWithinTree(
                        candidatePath,
                        active))) {
                continue;
            }
            (void)removeSafeTree(
                directory,
                candidatePath);
        }
    }

    return Result<void>::success();
}

} // namespace companion
