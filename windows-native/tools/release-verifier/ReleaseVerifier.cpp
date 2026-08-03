#include "ReleaseVerifier.h"

#include "update/ReleaseVersion.h"
#include "update/UpdateCompatibility.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

namespace companion {
namespace {

constexpr qint64 kMaximumManifestBytes =
    64LL * 1024LL;
constexpr qint64 kMaximumMetadataBytes =
    64LL * 1024LL;
constexpr qsizetype kReadBufferBytes =
    1024 * 1024;
constexpr auto kApplicationRelativePath =
    "bin/CodexCompanion.exe";

CompanionError releaseError(
    QString code,
    QString message,
    QStringView path = {})
{
    QVariantMap context;
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"),
            path.toString());
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

class UniqueHandle final {
public:
    explicit UniqueHandle(
        HANDLE handle =
            INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) =
        delete;
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

struct PlainFile final {
    UniqueHandle handle;
    qint64 size = 0;
};

struct FileInspection final {
    qint64 size = 0;
    QByteArray sha256;
    QByteArray bytes;
};

QString cleanAbsolutePath(
    QStringView path)
{
    return QDir::cleanPath(
        QFileInfo(path.toString())
            .absoluteFilePath());
}

QString apiPath(QStringView path)
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
    QStringView path)
{
    const QString native =
        QDir::toNativeSeparators(
            path.toString());
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

Result<void> validatePathChain(
    QStringView path)
{
    QString current =
        cleanAbsolutePath(path);
    while (!current.isEmpty()) {
        const std::wstring native =
            apiPath(current).toStdWString();
        const DWORD attributes =
            GetFileAttributesW(
                native.c_str());
        if (attributes
                != INVALID_FILE_ATTRIBUTES
            && (attributes
                & (FILE_ATTRIBUTE_REPARSE_POINT
                   | FILE_ATTRIBUTE_DEVICE))
                != 0) {
            return Result<void>::failure(
                releaseError(
                    QStringLiteral(
                        "release.unsafe_path_chain"),
                    QStringLiteral(
                        "A release path contains a link or device."),
                    current));
        }

        const QString parent =
            cleanAbsolutePath(
                QFileInfo(current)
                    .absolutePath());
        if (parent.isEmpty()
            || parent.compare(
                   current,
                   Qt::CaseInsensitive)
                   == 0) {
            break;
        }
        current = parent;
    }
    return Result<void>::success();
}

Result<PlainFile> openPlainFile(
    QStringView path,
    qint64 maximumBytes)
{
    const QString absolute =
        cleanAbsolutePath(path);
    if (absolute.isEmpty()
        || hasAlternateStreamSyntax(
            absolute)) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.invalid_file_path"),
                QStringLiteral(
                    "A release input path is invalid."),
                path));
    }
    const Result<void> safePath =
        validatePathChain(absolute);
    if (!safePath.hasValue()) {
        return Result<PlainFile>::failure(
            safePath.error());
    }

    const std::wstring native =
        apiPath(absolute).toStdWString();
    UniqueHandle handle(
        CreateFileW(
            native.c_str(),
            GENERIC_READ
                | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
    if (!handle.valid()) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_open_failed"),
                QStringLiteral(
                    "A release input could not be opened."),
                absolute));
    }
    if (GetFileType(handle.get())
        != FILE_TYPE_DISK) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_not_disk"),
                QStringLiteral(
                    "A release input is not a disk file."),
                absolute));
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            handle.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes))) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_inspection_failed"),
                QStringLiteral(
                    "A release input could not be inspected."),
                absolute));
    }
    if ((attributes.FileAttributes
         & FILE_ATTRIBUTE_DIRECTORY)
            != 0) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_not_regular"),
                QStringLiteral(
                    "A release input is not a regular file."),
                absolute));
    }
    if ((attributes.FileAttributes
         & (FILE_ATTRIBUTE_REPARSE_POINT
            | FILE_ATTRIBUTE_DEVICE))
            != 0) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_unsafe"),
                QStringLiteral(
                    "A release input cannot be a link or device."),
                absolute));
    }

    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle.get(),
            FileStandardInfo,
            &standard,
            sizeof(standard))
        || standard.Directory
        || standard.EndOfFile.QuadPart < 0) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_inspection_failed"),
                QStringLiteral(
                    "A release input could not be inspected."),
                absolute));
    }

    const qint64 size =
        standard.EndOfFile.QuadPart;
    if (maximumBytes < 0
        || size > maximumBytes) {
        return Result<PlainFile>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_too_large"),
                QStringLiteral(
                    "A release input exceeds its size limit."),
                absolute));
    }
    return Result<PlainFile>::success(
        {
            std::move(handle),
            size,
        });
}

Result<FileInspection> inspectPlainFile(
    QStringView path,
    qint64 maximumBytes,
    bool captureBytes)
{
    auto opened =
        openPlainFile(
            path,
            maximumBytes);
    if (!opened.hasValue()) {
        return Result<FileInspection>::
            failure(opened.error());
    }

    PlainFile file =
        std::move(opened.value());
    if (captureBytes
        && file.size
            > std::numeric_limits<
                int>::max()) {
        return Result<FileInspection>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_too_large"),
                QStringLiteral(
                    "A release input exceeds its memory limit."),
                path));
    }

    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    QByteArray captured;
    if (captureBytes) {
        captured.reserve(
            static_cast<int>(file.size));
    }
    QByteArray buffer(
        kReadBufferBytes,
        Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        DWORD read = 0;
        if (!ReadFile(
                file.handle.get(),
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()),
                &read,
                nullptr)) {
            return Result<FileInspection>::
                failure(
                    releaseError(
                        QStringLiteral(
                            "release.file_read_failed"),
                        QStringLiteral(
                            "A release input could not be read."),
                        path));
        }
        if (read == 0) {
            break;
        }
        const qint64 count =
            static_cast<qint64>(read);
        if (total
            > file.size - count) {
            return Result<FileInspection>::
                failure(
                    releaseError(
                        QStringLiteral(
                            "release.file_changed"),
                        QStringLiteral(
                            "A release input changed while it was inspected."),
                        path));
        }
        const QByteArrayView chunk(
            buffer.constData(),
            static_cast<qsizetype>(read));
        hash.addData(chunk);
        if (captureBytes) {
            captured.append(
                chunk.data(),
                chunk.size());
        }
        total += count;
    }
    if (total != file.size) {
        return Result<FileInspection>::failure(
            releaseError(
                QStringLiteral(
                    "release.file_changed"),
                QStringLiteral(
                    "A release input changed while it was inspected."),
                path));
    }

    return Result<FileInspection>::success(
        {
            total,
            hash.result(),
            std::move(captured),
        });
}

Result<void> requirePlainDirectory(
    QStringView path)
{
    const QString absolute =
        cleanAbsolutePath(path);
    if (absolute.isEmpty()
        || hasAlternateStreamSyntax(
            absolute)) {
        return Result<void>::failure(
            releaseError(
                QStringLiteral(
                    "release.invalid_stage_path"),
                QStringLiteral(
                    "The portable release path is invalid."),
                path));
    }
    const Result<void> safePath =
        validatePathChain(absolute);
    if (!safePath.hasValue()) {
        return safePath;
    }
    const std::wstring native =
        apiPath(absolute).toStdWString();
    const DWORD attributes =
        GetFileAttributesW(
            native.c_str());
    if (attributes
            == INVALID_FILE_ATTRIBUTES
        || (attributes
            & FILE_ATTRIBUTE_DIRECTORY)
            == 0
        || (attributes
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))
            != 0) {
        return Result<void>::failure(
            releaseError(
                QStringLiteral(
                    "release.stage_unavailable"),
                QStringLiteral(
                    "The portable release tree is missing or unsafe."),
                absolute));
    }
    return Result<void>::success();
}

QString normalizedRelativePath(
    QStringView value)
{
    QString normalized =
        QDir::fromNativeSeparators(
            QDir::cleanPath(
                value.toString()));
    while (normalized.startsWith(
        QStringLiteral("./"))) {
        normalized.remove(0, 2);
    }
    return normalized.toLower();
}

QByteArray utf16Le(
    QByteArrayView ascii)
{
    QByteArray encoded;
    encoded.reserve(ascii.size() * 2);
    for (const char character : ascii) {
        encoded.append(character);
        encoded.append('\0');
    }
    return encoded;
}

QList<QByteArray> withUtf16Patterns(
    QList<QByteArray> ascii)
{
    const QList<QByteArray> source =
        ascii;
    for (const QByteArray& value :
         source) {
        ascii.append(
            utf16Le(value));
    }
    return ascii;
}

const QList<QByteArray>&
forbiddenContentPatterns(
    bool includePrivateKeyMarkers)
{
    static const QList<QByteArray>
        worktreePatterns =
            withUtf16Patterns(
                {
                    QByteArrayLiteral(
                        "codex companion windows "
                        "worktrees"),
                    QByteArrayLiteral(
                        "documents\\codex companion "
                        "windows"),
                    QByteArrayLiteral(
                        "documents/codex companion "
                        "windows"),
                });
    static const QList<QByteArray>
        allPatterns = [] {
            QList<QByteArray> patterns =
                worktreePatterns;
            patterns.append(
                withUtf16Patterns(
                    {
                        QByteArrayLiteral(
                            "-----begin private "
                            "key-----"),
                        QByteArrayLiteral(
                            "-----begin encrypted "
                            "private key-----"),
                        QByteArrayLiteral(
                            "-----begin openssh "
                            "private key-----"),
                        QByteArrayLiteral(
                            "-----begin rsa private "
                            "key-----"),
                        QByteArrayLiteral(
                            "-----begin ec private "
                            "key-----"),
                    }));
            return patterns;
        }();
    return includePrivateKeyMarkers
        ? allPatterns
        : worktreePatterns;
}

Result<bool> fileContainsForbiddenContent(
    QStringView path,
    bool includePrivateKeyMarkers)
{
    auto opened =
        openPlainFile(
            path,
            std::numeric_limits<
                qint64>::max());
    if (!opened.hasValue()) {
        return Result<bool>::failure(
            opened.error());
    }

    PlainFile file =
        std::move(opened.value());
    const QList<QByteArray>& patterns =
        forbiddenContentPatterns(
            includePrivateKeyMarkers);
    qsizetype longest = 0;
    for (const QByteArray& pattern :
         patterns) {
        longest = std::max(
            longest,
            pattern.size());
    }

    QByteArray carry;
    QByteArray buffer(
        kReadBufferBytes,
        Qt::Uninitialized);
    while (true) {
        DWORD read = 0;
        if (!ReadFile(
                file.handle.get(),
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()),
                &read,
                nullptr)) {
            return Result<bool>::failure(
                releaseError(
                    QStringLiteral(
                        "release.stage_scan_failed"),
                    QStringLiteral(
                        "The portable release tree could not be scanned."),
                    path));
        }
        if (read == 0) {
            break;
        }

        QByteArray window = carry;
        window.append(
            buffer.constData(),
            static_cast<qsizetype>(read));
        window = window.toLower();
        for (const QByteArray& pattern :
             patterns) {
            if (window.contains(pattern)) {
                return Result<bool>::success(
                    true);
            }
        }
        const qsizetype retained =
            std::min(
                window.size(),
                std::max<qsizetype>(
                    0,
                    longest - 1));
        carry = window.right(retained);
    }
    return Result<bool>::success(false);
}

bool isForbiddenFileName(
    const QFileInfo& file,
    const QSet<QString>&
        approvedSupportScripts)
{
    const QString name =
        file.fileName().toLower();
    const QString suffix =
        file.suffix().toLower();
    static const QSet<QString>
        forbiddenSuffixes{
            QStringLiteral("pdb"),
            QStringLiteral("lib"),
            QStringLiteral("exp"),
            QStringLiteral("ilk"),
            QStringLiteral("obj"),
            QStringLiteral("cmake"),
            QStringLiteral("py"),
            QStringLiteral("pfx"),
            QStringLiteral("p12"),
            QStringLiteral("pem"),
            QStringLiteral("key"),
            QStringLiteral("cer"),
            QStringLiteral("crt"),
        };
    if (forbiddenSuffixes.contains(
            suffix)) {
        return true;
    }
    if (suffix
            == QStringLiteral("ps1")
        && !approvedSupportScripts
                .contains(name)) {
        return true;
    }
    return name
               == QStringLiteral("id_rsa")
        || name
               == QStringLiteral(
                   "id_ed25519")
        || name.contains(
            QStringLiteral("private-key"))
        || name.contains(
            QStringLiteral("private_key"))
        || name
               == QStringLiteral(
                   "cmakecache.txt");
}

bool shouldScanPrivateKeyMarkers(
    const QFileInfo& file)
{
    static const QSet<QString>
        textSuffixes{
            QStringLiteral("cfg"),
            QStringLiteral("conf"),
            QStringLiteral("css"),
            QStringLiteral("html"),
            QStringLiteral("ini"),
            QStringLiteral("js"),
            QStringLiteral("json"),
            QStringLiteral("log"),
            QStringLiteral("md"),
            QStringLiteral("mjs"),
            QStringLiteral("properties"),
            QStringLiteral("qml"),
            QStringLiteral("toml"),
            QStringLiteral("txt"),
            QStringLiteral("xml"),
            QStringLiteral("yaml"),
            QStringLiteral("yml"),
        };
    return file.suffix().isEmpty()
        || textSuffixes.contains(
            file.suffix().toLower());
}

Result<int> countForbiddenStageEntries(
    QStringView stagePath,
    const QStringList&
        approvedSupportScripts)
{
    const QString stage =
        cleanAbsolutePath(stagePath);
    QSet<QString> approved;
    for (const QString& relative :
         approvedSupportScripts) {
        const QString normalized =
            normalizedRelativePath(relative);
        if (!normalized.isEmpty()
            && !normalized.startsWith(
                QStringLiteral("../"))
            && !QDir::isAbsolutePath(
                normalized)) {
            approved.insert(normalized);
            approved.insert(
                QFileInfo(normalized)
                    .fileName()
                    .toLower());
        }
    }
    QSet<QString> requiredReleaseFiles;
    for (const QString& relative :
         ReleaseVerifier::requiredStageFiles()) {
        requiredReleaseFiles.insert(
            normalizedRelativePath(relative));
    }

    int violations = 0;
    QDirIterator iterator(
        stage,
        QDir::AllEntries
            | QDir::NoDotAndDotDot
            | QDir::Hidden
            | QDir::System,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo entry =
            iterator.fileInfo();
        const QString absolute =
            entry.absoluteFilePath();
        const QString relative =
            normalizedRelativePath(
                QDir(stage).relativeFilePath(
                    absolute));
        if (relative.compare(
                QStringLiteral(
                    "resources/pets"),
                Qt::CaseInsensitive)
                == 0
            || relative.startsWith(
                QStringLiteral(
                    "resources/pets/"),
                Qt::CaseInsensitive)) {
            ++violations;
            continue;
        }
        const std::wstring native =
            apiPath(absolute).toStdWString();
        const DWORD attributes =
            GetFileAttributesW(
                native.c_str());
        if (attributes
            == INVALID_FILE_ATTRIBUTES) {
            return Result<int>::failure(
                releaseError(
                    QStringLiteral(
                        "release.stage_scan_failed"),
                    QStringLiteral(
                        "The portable release tree could not be scanned."),
                    absolute));
        }
        if ((attributes
             & (FILE_ATTRIBUTE_REPARSE_POINT
                | FILE_ATTRIBUTE_DEVICE))
            != 0) {
            ++violations;
            continue;
        }
        if (entry.isDir()) {
            if (entry.fileName().compare(
                    QStringLiteral(".git"),
                    Qt::CaseInsensitive)
                == 0) {
                ++violations;
            }
            continue;
        }
        if (!entry.isFile()) {
            ++violations;
            continue;
        }
        const bool requiredReleaseFile =
            requiredReleaseFiles.contains(
                relative);
        if ((!requiredReleaseFile
             && isForbiddenFileName(
                 entry,
                 approved))
            || (entry.suffix().compare(
                    QStringLiteral("ps1"),
                    Qt::CaseInsensitive)
                    == 0
                && !approved.contains(
                    relative))) {
            ++violations;
            continue;
        }
        const auto content =
            fileContainsForbiddenContent(
                absolute,
                shouldScanPrivateKeyMarkers(
                    entry));
        if (!content.hasValue()) {
            return Result<int>::failure(
                content.error());
        }
        if (content.value()) {
            ++violations;
        }
    }
    return Result<int>::success(
        violations);
}

QString stableCheckCode(
    QStringView code,
    QStringView fallback)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[a-z0-9][a-z0-9_.-]{0,95}$"));
    const QString normalized =
        code.toString().trimmed().toLower();
    return pattern.match(normalized)
            .hasMatch()
        ? normalized
        : fallback.toString();
}

void addCheck(
    ReleaseVerificationEvidence* evidence,
    QString id,
    ReleaseCheckStatus status,
    QString code)
{
    evidence->checks.append(
        {
            std::move(id),
            status,
            stableCheckCode(
                code,
                QStringLiteral(
                    "release.verification_failed")),
        });
}

QString statusName(
    ReleaseCheckStatus status)
{
    switch (status) {
    case ReleaseCheckStatus::Passed:
        return QStringLiteral("passed");
    case ReleaseCheckStatus::Failed:
        return QStringLiteral("failed");
    case ReleaseCheckStatus::Skipped:
        return QStringLiteral("skipped");
    }
    return QStringLiteral("skipped");
}

bool isHex(
    QStringView value,
    qsizetype minimumLength,
    qsizetype maximumLength)
{
    const qsizetype size =
        value.size();
    if (size < minimumLength
        || size > maximumLength) {
        return false;
    }
    for (const QChar character : value) {
        if (!((character >= QLatin1Char('0')
               && character
                   <= QLatin1Char('9'))
              || (character
                      >= QLatin1Char('a')
                  && character
                      <= QLatin1Char('f'))
              || (character
                      >= QLatin1Char('A')
                  && character
                      <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

bool copyHexMetadata(
    const QString& input,
    QString* output,
    qsizetype minimumLength,
    qsizetype maximumLength)
{
    if (input.isEmpty()) {
        output->clear();
        return true;
    }
    if (!isHex(
            input,
            minimumLength,
            maximumLength)) {
        output->clear();
        return false;
    }
    *output = input.toLower();
    return true;
}

bool copyVersionMetadata(
    const QString& input,
    QString* output)
{
    if (input.isEmpty()) {
        output->clear();
        return true;
    }
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9]+(?:\\.[0-9]+){1,3}$"));
    if (input.size() > 48
        || !pattern.match(input).hasMatch()) {
        output->clear();
        return false;
    }
    *output = input;
    return true;
}

bool copyCompilerMetadata(
    const QString& input,
    QString* output)
{
    if (input.isEmpty()) {
        output->clear();
        return true;
    }
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9A-Za-z ._()+-]{1,128}$"));
    if (!pattern.match(input).hasMatch()) {
        output->clear();
        return false;
    }
    *output = input;
    return true;
}

bool copyEvidenceMetadata(
    const ReleaseEvidenceMetadata& source,
    ReleaseVerificationEvidence* evidence)
{
    bool valid = true;
    valid =
        copyHexMetadata(
            source.sourceCommit,
            &evidence->sourceCommit,
            40,
            64)
        && valid;
    valid =
        copyHexMetadata(
            source.sourceTree,
            &evidence->sourceTree,
            40,
            64)
        && valid;
    valid =
        copyHexMetadata(
            source.cmakeCacheSha256,
            &evidence->cmakeCacheSha256,
            64,
            64)
        && valid;
    valid =
        copyCompilerMetadata(
            source.compilerVersion,
            &evidence->compilerVersion)
        && valid;
    valid =
        copyVersionMetadata(
            source.qtVersion,
            &evidence->qtVersion)
        && valid;
    valid =
        copyVersionMetadata(
            source.innoVersion,
            &evidence->innoVersion)
        && valid;
    valid =
        copyVersionMetadata(
            source.windowsSdkVersion,
            &evidence->windowsSdkVersion)
        && valid;
    valid =
        copyHexMetadata(
            source.webSocketsSourceCommit,
            &evidence
                 ->webSocketsSourceCommit,
            40,
            64)
        && valid;
    valid =
        copyHexMetadata(
            source.monocypherCommit,
            &evidence->monocypherCommit,
            40,
            64)
        && valid;
    valid =
        copyHexMetadata(
            source.recordingSha256,
            &evidence->recordingSha256,
            64,
            64)
        && valid;
    return valid;
}

QString expectedInstallerFilename(
    const UpdateManifest& manifest)
{
    return QStringLiteral(
               "Codex-Companion-%1-%2-"
               "windows-x64.exe")
        .arg(manifest.version)
        .arg(manifest.build);
}

QString expectedInstallerMarker(
    const UpdateManifest& manifest)
{
    return QStringLiteral(
               "cc-update/1|%1|%2|w|x64|%3")
        .arg(manifest.version)
        .arg(manifest.build)
        .arg(manifest.minimumSystemVersion);
}

bool releaseManifestIdentityIsValid(
    const UpdateManifest& manifest)
{
    static const QRegularExpression
        safeVersion(
            QStringLiteral(
                "^[0-9A-Za-z]"
                "[0-9A-Za-z.+-]{0,63}$"));
    const QUrl url(manifest.downloadUrl);
    return manifest.schemaVersion == 1
        && manifest.build > 0
        && manifest.build <= 65535
        && ReleaseVersion::parse(
               manifest.version)
               .has_value()
        && safeVersion.match(
               manifest.version)
               .hasMatch()
        && WindowsVersion::parse(
               manifest
                   .minimumSystemVersion)
               .has_value()
        && url.isValid()
        && url.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive)
               == 0
        && url.userName().isEmpty()
        && url.password().isEmpty()
        && url.fileName()
               == expectedInstallerFilename(
                   manifest);
}

bool installerMetadataMatches(
    const UpdateManifest& manifest,
    const InstallerMetadata& metadata,
    QStringView installerPath)
{
    return metadata.productName
               == QStringLiteral(
                   "Codex Companion")
        && metadata.productVersionMarker
               == expectedInstallerMarker(
                   manifest)
        && metadata.originalFilename
               == expectedInstallerFilename(
                   manifest)
        && QFileInfo(
               installerPath.toString())
               .fileName()
               == expectedInstallerFilename(
                   manifest);
}

bool stageHasNonemptyDirectory(
    QStringView stagePath,
    QStringView relativePath)
{
    const QString directory =
        QDir(stagePath.toString())
            .filePath(
                relativePath.toString());
    if (!requirePlainDirectory(directory)
             .hasValue()) {
        return false;
    }
    QDirIterator iterator(
        directory,
        QDir::Files
            | QDir::NoDotAndDotDot
            | QDir::Hidden
            | QDir::System,
        QDirIterator::Subdirectories);
    return iterator.hasNext();
}

QString checkCode(
    const Result<void>& result)
{
    return result.hasValue()
        ? QStringLiteral("release.ok")
        : result.error().code;
}

template <typename T>
QString resultCode(
    const Result<T>& result)
{
    return result.hasValue()
        ? QStringLiteral("release.ok")
        : result.error().code;
}

bool readOptionalString(
    const QJsonObject& object,
    QStringView key,
    QString* value)
{
    const QString name = key.toString();
    if (!object.contains(name)) {
        return true;
    }
    const QJsonValue json =
        object.value(name);
    if (!json.isString()) {
        return false;
    }
    *value = json.toString();
    return true;
}

} // namespace

ReleaseVerifier::ReleaseVerifier(
    ReleaseVerifierDependencies
        dependencies)
    : dependencies_(
          std::move(dependencies))
{
}

QStringList ReleaseVerifier::
    requiredStageFiles()
{
    return {
        QStringLiteral(
            "bin/CodexCompanion.exe"),
        QStringLiteral(
            "bin/CodexCompanionUpdater.exe"),
        QStringLiteral("bin/Qt6Core.dll"),
        QStringLiteral("bin/Qt6Gui.dll"),
        QStringLiteral("bin/Qt6Network.dll"),
        QStringLiteral("bin/Qt6Qml.dll"),
        QStringLiteral("bin/Qt6Quick.dll"),
        QStringLiteral(
            "bin/Qt6QuickControls2.dll"),
        QStringLiteral("bin/Qt6Sql.dll"),
        QStringLiteral(
            "bin/Qt6WebSockets.dll"),
        QStringLiteral("bin/msvcp140.dll"),
        QStringLiteral("bin/msvcp140_1.dll"),
        QStringLiteral("bin/vcruntime140.dll"),
        QStringLiteral(
            "bin/vcruntime140_1.dll"),
        QStringLiteral(
            "plugins/platforms/qwindows.dll"),
        QStringLiteral(
            "plugins/tls/qschannelbackend.dll"),
        QStringLiteral(
            "plugins/sqldrivers/qsqlite.dll"),
        QStringLiteral(
            "plugins/imageformats/qwebp.dll"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "SKILL.md"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "agents/openai.yaml"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "references/"
            "codex-pet-schema-2026-07-13.json"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "references/companion-contract.md"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "scripts/companion_pet_assets.py"),
    };
}

QStringList ReleaseVerifier::
    requiredStageDirectories()
{
    return {
        QStringLiteral("qml"),
    };
}

ReleaseVerificationEvidence
ReleaseVerifier::verify(
    const ReleaseVerifierOptions&
        options) const
{
    ReleaseVerificationEvidence evidence;

    const bool metadataValid =
        options.metadataErrorCode.isEmpty()
        && copyEvidenceMetadata(
            options.metadata,
            &evidence);
    addCheck(
        &evidence,
        QStringLiteral("evidence.metadata"),
        metadataValid
            ? ReleaseCheckStatus::Passed
            : ReleaseCheckStatus::Failed,
        metadataValid
            ? QStringLiteral("release.ok")
            : (options.metadataErrorCode
                       .isEmpty()
                   ? QStringLiteral(
                         "release.invalid_evidence_metadata")
                   : options
                         .metadataErrorCode));

    const auto manifestFile =
        inspectPlainFile(
            options.manifestPath,
            kMaximumManifestBytes,
            true);
    Result<UpdateManifest>
        manifestResult =
            Result<UpdateManifest>::failure(
                releaseError(
                    QStringLiteral(
                        "release.manifest_unavailable"),
                    QStringLiteral(
                        "The release manifest is unavailable.")));
    if (manifestFile.hasValue()) {
        evidence.manifestSha256 =
            QString::fromLatin1(
                manifestFile.value()
                    .sha256.toHex());
        if (dependencies_
                .manifestValidator) {
            manifestResult =
                dependencies_
                    .manifestValidator(
                        manifestFile
                            .value()
                            .bytes,
                        options
                            .publicKeyBase64);
        } else {
            manifestResult =
                Result<UpdateManifest>::
                    failure(
                        releaseError(
                            QStringLiteral(
                                "release.manifest_validator_unavailable"),
                            QStringLiteral(
                                "Manifest authentication is unavailable.")));
        }
    }

    const bool manifestAuthenticated =
        manifestFile.hasValue()
        && manifestResult.hasValue();
    addCheck(
        &evidence,
        QStringLiteral(
            "manifest.authentication"),
        manifestAuthenticated
            ? ReleaseCheckStatus::Passed
            : ReleaseCheckStatus::Failed,
        manifestFile.hasValue()
            ? resultCode(manifestResult)
            : manifestFile.error().code);

    bool manifestReleaseValid = false;
    UpdateManifest manifest;
    if (manifestAuthenticated) {
        manifest = manifestResult.value();
        manifestReleaseValid =
            releaseManifestIdentityIsValid(
                manifest);
        if (manifestReleaseValid) {
            evidence.version =
                manifest.version;
            evidence.build =
                manifest.build;
            evidence.minimumSystemVersion =
                manifest
                    .minimumSystemVersion;
        }
    }
    addCheck(
        &evidence,
        QStringLiteral("manifest.release"),
        !manifestAuthenticated
            ? ReleaseCheckStatus::Skipped
            : (manifestReleaseValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        !manifestAuthenticated
            ? QStringLiteral(
                  "release.upstream_failed")
            : (manifestReleaseValid
                   ? QStringLiteral("release.ok")
                   : QStringLiteral(
                         "release.invalid_manifest_release")));

    const auto installerFile =
        inspectPlainFile(
            options.installerPath,
            UpdateManifest::
                maximumSignedSize,
            false);
    std::optional<PlainFile>
        pinnedInstaller;
    bool installerFileValid =
        installerFile.hasValue()
        && installerFile.value().size > 0;
    QString installerFileCode =
        installerFile.hasValue()
        ? (installerFileValid
               ? QStringLiteral("release.ok")
               : QStringLiteral(
                     "release.installer_empty"))
        : installerFile.error().code;
    if (installerFileValid) {
        auto pin =
            openPlainFile(
                options.installerPath,
                UpdateManifest::
                    maximumSignedSize);
        if (!pin.hasValue()) {
            installerFileValid = false;
            installerFileCode =
                pin.error().code;
        } else {
            const auto stable =
                inspectPlainFile(
                    options.installerPath,
                    UpdateManifest::
                        maximumSignedSize,
                    false);
            if (!stable.hasValue()
                || stable.value().size
                    != installerFile.value()
                           .size
                || stable.value().sha256
                    != installerFile.value()
                           .sha256) {
                installerFileValid = false;
                installerFileCode =
                    stable.hasValue()
                    ? QStringLiteral(
                          "release.file_changed")
                    : stable.error().code;
            } else {
                pinnedInstaller.emplace(
                    std::move(pin.value()));
            }
        }
    }
    if (installerFile.hasValue()) {
        evidence.installerSize =
            installerFile.value().size;
        evidence.installerSha256 =
            QString::fromLatin1(
                installerFile.value()
                    .sha256.toHex());
    }
    addCheck(
        &evidence,
        QStringLiteral("installer.file"),
        installerFileValid
            ? ReleaseCheckStatus::Passed
            : ReleaseCheckStatus::Failed,
        installerFileCode);

    bool installerDigestValid = false;
    QString installerDigestCode =
        QStringLiteral(
            "release.upstream_failed");
    if (installerFileValid
        && manifestReleaseValid) {
        if (installerFile.value().size
            != manifest.size) {
            installerDigestCode =
                QStringLiteral(
                    "update.artifact_size_mismatch");
        } else if (
            installerFile.value().sha256
            != QByteArray::fromHex(
                manifest.sha256
                    .toLatin1())) {
            installerDigestCode =
                QStringLiteral(
                    "update.artifact_digest_mismatch");
        } else {
            installerDigestValid = true;
            installerDigestCode =
                QStringLiteral("release.ok");
        }
    }
    addCheck(
        &evidence,
        QStringLiteral("installer.digest"),
        !installerFileValid
                || !manifestReleaseValid
            ? ReleaseCheckStatus::Skipped
            : (installerDigestValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        installerDigestCode);

    Result<PeMachine> machineResult =
        Result<PeMachine>::failure(
            releaseError(
                QStringLiteral(
                    "release.pe_inspector_unavailable"),
                QStringLiteral(
                    "PE inspection is unavailable.")));
    if (installerFileValid
        && dependencies_.peInspector) {
        machineResult =
            dependencies_.peInspector(
                options.installerPath);
    }
    const bool peValid =
        machineResult.hasValue()
        // Inno uses an I386 bootstrap for an
        // x64-compatible target installation.
        && (machineResult.value()
                == PeMachine::X86
            || machineResult.value()
                == PeMachine::X64);
    addCheck(
        &evidence,
        QStringLiteral("installer.pe"),
        !installerFileValid
            ? ReleaseCheckStatus::Skipped
            : (peValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        !installerFileValid
            ? QStringLiteral(
                  "release.upstream_failed")
            : (machineResult.hasValue()
                   ? (peValid
                          ? QStringLiteral(
                                "release.ok")
                          : QStringLiteral(
                                "update.artifact_architecture_mismatch"))
                   : machineResult
                         .error()
                         .code));

    Result<InstallerMetadata>
        installerMetadataResult =
            Result<InstallerMetadata>::
                failure(
                    releaseError(
                        QStringLiteral(
                            "release.metadata_inspector_unavailable"),
                        QStringLiteral(
                            "Installer metadata inspection is unavailable.")));
    if (installerFileValid
        && manifestReleaseValid
        && dependencies_
               .metadataInspector) {
        installerMetadataResult =
            dependencies_
                .metadataInspector(
                    options.installerPath);
    }
    const bool installerMetadataValid =
        installerMetadataResult.hasValue()
        && installerMetadataMatches(
            manifest,
            installerMetadataResult.value(),
            options.installerPath);
    addCheck(
        &evidence,
        QStringLiteral(
            "installer.metadata"),
        !installerFileValid
                || !manifestReleaseValid
            ? ReleaseCheckStatus::Skipped
            : (installerMetadataValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        !installerFileValid
                || !manifestReleaseValid
            ? QStringLiteral(
                  "release.upstream_failed")
            : (installerMetadataResult
                       .hasValue()
                   ? (installerMetadataValid
                          ? QStringLiteral(
                                "release.ok")
                          : QStringLiteral(
                                "update.installer_identity_mismatch"))
                   : installerMetadataResult
                         .error()
                         .code));

    Result<AuthenticodeIdentity>
        signerResult =
            Result<AuthenticodeIdentity>::
                failure(
                    releaseError(
                        QStringLiteral(
                            "release.signer_inspector_unavailable"),
                        QStringLiteral(
                            "Authenticode inspection is unavailable.")));
    if (installerFileValid
        && dependencies_
               .signerInspector) {
        signerResult =
            dependencies_.signerInspector(
                options.installerPath);
    }

    QSet<QString> allowedSigners;
    bool signerPolicyValid =
        !dependencies_
             .allowedSignerSha256
             .isEmpty();
    for (const QString& raw :
         dependencies_
             .allowedSignerSha256) {
        const QString normalized =
            AuthenticodeVerifier::
                normalizeThumbprint(raw);
        if (normalized.isEmpty()) {
            signerPolicyValid = false;
        } else {
            allowedSigners.insert(
                normalized);
        }
    }
    QString normalizedSigner;
    if (signerResult.hasValue()) {
        normalizedSigner =
            AuthenticodeVerifier::
                normalizeThumbprint(
                    signerResult.value()
                        .sha256Thumbprint);
    }
    const bool signerValid =
        signerResult.hasValue()
        && signerPolicyValid
        && !normalizedSigner.isEmpty()
        && allowedSigners.contains(
            normalizedSigner);
    if (signerValid) {
        evidence.signerSha256 =
            normalizedSigner.toLower();
    }
    addCheck(
        &evidence,
        QStringLiteral(
            "installer.authenticode"),
        !installerFileValid
            ? ReleaseCheckStatus::Skipped
            : (signerValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        !installerFileValid
            ? QStringLiteral(
                  "release.upstream_failed")
            : (!signerResult.hasValue()
                   ? signerResult
                         .error()
                         .code
                   : (!signerPolicyValid
                          ? QStringLiteral(
                                "update.invalid_signer_policy")
                          : (!signerValid
                                 ? QStringLiteral(
                                       "update.untrusted_signer")
                                 : QStringLiteral(
                                       "release.ok")))));

    bool compatibilityValid = false;
    QString compatibilityCode =
        QStringLiteral(
            "release.upstream_failed");
    const bool compatibilityReady =
        manifestReleaseValid
        && installerFileValid
        && installerDigestValid
        && peValid
        && installerMetadataValid
        && signerValid;
    if (compatibilityReady) {
        if (!dependencies_
                 .windowsVersionProvider) {
            compatibilityCode =
                QStringLiteral(
                    "release.windows_version_unavailable");
        } else {
            const auto currentWindows =
                dependencies_
                    .windowsVersionProvider();
            if (!currentWindows.hasValue()) {
                compatibilityCode =
                    currentWindows
                        .error()
                        .code;
            } else {
                ArtifactFacts facts;
                facts.path =
                    options.installerPath;
                facts.exists = true;
                facts.regularFile = true;
                facts.reparsePoint = false;
                facts.size =
                    installerFile.value()
                        .size;
                facts.sha256 =
                    installerFile.value()
                        .sha256;
                facts.machine =
                    machineResult.value();
                facts.metadata =
                    installerMetadataResult
                        .value();
                facts.signer =
                    signerResult.value();
                const UpdateCompatibility
                    compatibility(
                        currentWindows.value(),
                        dependencies_
                            .allowedSignerSha256);
                const Result<void> result =
                    compatibility.validate(
                        manifest,
                        facts);
                compatibilityValid =
                    result.hasValue();
                compatibilityCode =
                    checkCode(result);
            }
        }
    }
    addCheck(
        &evidence,
        QStringLiteral(
            "installer.compatibility"),
        !compatibilityReady
            ? ReleaseCheckStatus::Skipped
            : (compatibilityValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        compatibilityCode);

    const Result<void> stageRoot =
        requirePlainDirectory(
            options.stagePath);
    bool stageComplete =
        stageRoot.hasValue();
    if (stageComplete) {
        const QDir root(
            options.stagePath);
        for (const QString& relative :
             requiredStageFiles()) {
            const auto required =
                openPlainFile(
                    root.filePath(relative),
                    std::numeric_limits<
                        qint64>::max());
            if (!required.hasValue()
                || required.value().size
                    <= 0) {
                stageComplete = false;
            }
        }
        for (const QString& relative :
             requiredStageDirectories()) {
            if (!stageHasNonemptyDirectory(
                    options.stagePath,
                    relative)) {
                stageComplete = false;
            }
        }
    }
    addCheck(
        &evidence,
        QStringLiteral(
            "stage.completeness"),
        stageComplete
            ? ReleaseCheckStatus::Passed
            : ReleaseCheckStatus::Failed,
        !stageRoot.hasValue()
            ? stageRoot.error().code
            : (stageComplete
                   ? QStringLiteral("release.ok")
                   : QStringLiteral(
                         "release.stage_incomplete")));

    bool iconsValid = false;
    QString iconsCode =
        QStringLiteral(
            "release.upstream_failed");
    if (stageRoot.hasValue()
        && installerFileValid) {
        if (!dependencies_.iconInspector) {
            iconsCode =
                QStringLiteral(
                    "release.icon_inspector_unavailable");
        } else {
            const auto appIcon =
                dependencies_.iconInspector(
                    QDir(options.stagePath)
                        .filePath(
                            QString::fromLatin1(
                                kApplicationRelativePath)),
                    ExecutableIconExpectation::
                        CompanionApplication);
            const auto installerIcon =
                dependencies_.iconInspector(
                    options.installerPath,
                    ExecutableIconExpectation::
                        Installer);
            iconsValid =
                appIcon.hasValue()
                && appIcon.value()
                && installerIcon.hasValue()
                && installerIcon.value();
            if (iconsValid) {
                iconsCode =
                    QStringLiteral("release.ok");
            } else if (!appIcon.hasValue()) {
                iconsCode =
                    appIcon.error().code;
            } else if (!installerIcon
                            .hasValue()) {
                iconsCode =
                    installerIcon
                        .error()
                        .code;
            } else {
                iconsCode =
                    QStringLiteral(
                        "release.executable_icon_missing");
            }
        }
    }
    addCheck(
        &evidence,
        QStringLiteral("stage.icons"),
        !stageRoot.hasValue()
                || !installerFileValid
            ? ReleaseCheckStatus::Skipped
            : (iconsValid
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        iconsCode);

    Result<int> forbidden =
        Result<int>::failure(
            releaseError(
                QStringLiteral(
                    "release.stage_unavailable"),
                QStringLiteral(
                    "The portable release tree is unavailable.")));
    if (stageRoot.hasValue()) {
        forbidden =
            countForbiddenStageEntries(
                options.stagePath,
                options
                    .approvedSupportScripts);
    }
    const bool forbiddenFree =
        forbidden.hasValue()
        && forbidden.value() == 0;
    addCheck(
        &evidence,
        QStringLiteral(
            "stage.forbidden_files"),
        !stageRoot.hasValue()
            ? ReleaseCheckStatus::Skipped
            : (forbiddenFree
                   ? ReleaseCheckStatus::Passed
                   : ReleaseCheckStatus::Failed),
        !stageRoot.hasValue()
            ? QStringLiteral(
                  "release.upstream_failed")
            : (!forbidden.hasValue()
                   ? forbidden.error().code
                   : (forbiddenFree
                          ? QStringLiteral(
                                "release.ok")
                          : QStringLiteral(
                                "release.forbidden_files_present"))));

    evidence.passed =
        std::all_of(
            evidence.checks.cbegin(),
            evidence.checks.cend(),
            [](const ReleaseVerificationCheck&
                   check) {
                return check.status
                    == ReleaseCheckStatus::
                        Passed;
            });
    return evidence;
}

QJsonObject releaseVerificationEvidenceJson(
    const ReleaseVerificationEvidence&
        evidence)
{
    QJsonArray checks;
    for (const ReleaseVerificationCheck&
         check : evidence.checks) {
        checks.append(
            QJsonObject{
                {
                    QStringLiteral("code"),
                    check.code,
                },
                {
                    QStringLiteral("id"),
                    check.id,
                },
                {
                    QStringLiteral("passed"),
                    check.status
                        == ReleaseCheckStatus::
                            Passed,
                },
                {
                    QStringLiteral("status"),
                    statusName(check.status),
                },
            });
    }

    return {
        {
            QStringLiteral("build"),
            evidence.build,
        },
        {
            QStringLiteral("checks"),
            checks,
        },
        {
            QStringLiteral(
                "cmakeCacheSha256"),
            evidence.cmakeCacheSha256,
        },
        {
            QStringLiteral("compilerVersion"),
            evidence.compilerVersion,
        },
        {
            QStringLiteral("innoVersion"),
            evidence.innoVersion,
        },
        {
            QStringLiteral("installerSha256"),
            evidence.installerSha256,
        },
        {
            QStringLiteral("installerSize"),
            evidence.installerSize,
        },
        {
            QStringLiteral("manifestSha256"),
            evidence.manifestSha256,
        },
        {
            QStringLiteral(
                "minimumSystemVersion"),
            evidence.minimumSystemVersion,
        },
        {
            QStringLiteral("monocypherCommit"),
            evidence.monocypherCommit,
        },
        {
            QStringLiteral("passed"),
            evidence.passed,
        },
        {
            QStringLiteral("qtVersion"),
            evidence.qtVersion,
        },
        {
            QStringLiteral("recordingSha256"),
            evidence.recordingSha256,
        },
        {
            QStringLiteral("schemaVersion"),
            1,
        },
        {
            QStringLiteral("signerSha256"),
            evidence.signerSha256,
        },
        {
            QStringLiteral("sourceCommit"),
            evidence.sourceCommit,
        },
        {
            QStringLiteral("sourceTree"),
            evidence.sourceTree,
        },
        {
            QStringLiteral("version"),
            evidence.version,
        },
        {
            QStringLiteral(
                "webSocketsSourceCommit"),
            evidence.webSocketsSourceCommit,
        },
        {
            QStringLiteral(
                "windowsSdkVersion"),
            evidence.windowsSdkVersion,
        },
    };
}

Result<void> writeReleaseVerificationEvidence(
    QStringView path,
    const ReleaseVerificationEvidence&
        evidence)
{
    const QString output =
        path.toString().trimmed();
    if (output.isEmpty()) {
        return Result<void>::failure(
            releaseError(
                QStringLiteral(
                    "release.output_path_invalid"),
                QStringLiteral(
                    "The release evidence output path is invalid.")));
    }

    const QString parent =
        QFileInfo(output).absolutePath();
    if (!QDir().mkpath(parent)) {
        return Result<void>::failure(
            releaseError(
                QStringLiteral(
                    "release.output_directory_failed"),
                QStringLiteral(
                    "The release evidence directory could not be created."),
                parent));
    }

    QByteArray json =
        QJsonDocument(
            releaseVerificationEvidenceJson(
                evidence))
            .toJson(
                QJsonDocument::Indented);
    if (!json.endsWith('\n')) {
        json.append('\n');
    }

    QSaveFile file(output);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)
        || file.write(json)
            != json.size()
        || !file.commit()) {
        return Result<void>::failure(
            releaseError(
                QStringLiteral(
                    "release.output_write_failed"),
                QStringLiteral(
                    "The release evidence could not be written."),
                output));
    }
    return Result<void>::success();
}

Result<ReleaseEvidenceMetadata>
loadReleaseEvidenceMetadata(
    QStringView path)
{
    const auto file =
        inspectPlainFile(
            path,
            kMaximumMetadataBytes,
            true);
    if (!file.hasValue()) {
        return Result<
            ReleaseEvidenceMetadata>::failure(
                file.error());
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            file.value().bytes,
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<
            ReleaseEvidenceMetadata>::failure(
                releaseError(
                    QStringLiteral(
                        "release.invalid_metadata_json"),
                    QStringLiteral(
                        "Release metadata is not valid JSON."),
                    path));
    }

    const QJsonObject object =
        document.object();
    if (object.contains(
            QStringLiteral(
                "schemaVersion"))) {
        const QJsonValue schema =
            object.value(
                QStringLiteral(
                    "schemaVersion"));
        if (!schema.isDouble()
            || schema.toDouble() != 1.0) {
            return Result<
                ReleaseEvidenceMetadata>::
                failure(
                    releaseError(
                        QStringLiteral(
                            "release.unsupported_metadata_schema"),
                        QStringLiteral(
                            "Release metadata uses an unsupported schema."),
                        path));
        }
    }

    ReleaseEvidenceMetadata metadata;
    if (!readOptionalString(
            object,
            QStringLiteral("sourceCommit"),
            &metadata.sourceCommit)
        || !readOptionalString(
            object,
            QStringLiteral("sourceTree"),
            &metadata.sourceTree)
        || !readOptionalString(
            object,
            QStringLiteral(
                "cmakeCacheSha256"),
            &metadata.cmakeCacheSha256)
        || !readOptionalString(
            object,
            QStringLiteral(
                "compilerVersion"),
            &metadata.compilerVersion)
        || !readOptionalString(
            object,
            QStringLiteral("qtVersion"),
            &metadata.qtVersion)
        || !readOptionalString(
            object,
            QStringLiteral("innoVersion"),
            &metadata.innoVersion)
        || !readOptionalString(
            object,
            QStringLiteral(
                "windowsSdkVersion"),
            &metadata.windowsSdkVersion)
        || !readOptionalString(
            object,
            QStringLiteral(
                "webSocketsSourceCommit"),
            &metadata
                 .webSocketsSourceCommit)
        || !readOptionalString(
            object,
            QStringLiteral(
                "monocypherCommit"),
            &metadata.monocypherCommit)
        || !readOptionalString(
            object,
            QStringLiteral(
                "recordingSha256"),
            &metadata.recordingSha256)) {
        return Result<
            ReleaseEvidenceMetadata>::failure(
                releaseError(
                    QStringLiteral(
                        "release.invalid_metadata_fields"),
                    QStringLiteral(
                        "Release metadata contains invalid field types."),
                    path));
    }

    return Result<
        ReleaseEvidenceMetadata>::success(
            std::move(metadata));
}

ReleaseEvidenceMetadata
mergeReleaseEvidenceMetadata(
    ReleaseEvidenceMetadata defaults,
    const ReleaseEvidenceMetadata&
        overrides)
{
    const auto replace =
        [](QString* destination,
           const QString& source) {
            if (!source.isEmpty()) {
                *destination = source;
            }
        };
    replace(
        &defaults.sourceCommit,
        overrides.sourceCommit);
    replace(
        &defaults.sourceTree,
        overrides.sourceTree);
    replace(
        &defaults.cmakeCacheSha256,
        overrides.cmakeCacheSha256);
    replace(
        &defaults.compilerVersion,
        overrides.compilerVersion);
    replace(
        &defaults.qtVersion,
        overrides.qtVersion);
    replace(
        &defaults.innoVersion,
        overrides.innoVersion);
    replace(
        &defaults.windowsSdkVersion,
        overrides.windowsSdkVersion);
    replace(
        &defaults.webSocketsSourceCommit,
        overrides.webSocketsSourceCommit);
    replace(
        &defaults.monocypherCommit,
        overrides.monocypherCommit);
    replace(
        &defaults.recordingSha256,
        overrides.recordingSha256);
    return defaults;
}

int releaseVerifierExitCode(
    const ReleaseVerificationEvidence&
        evidence) noexcept
{
    return evidence.passed ? 0 : 2;
}

} // namespace companion
