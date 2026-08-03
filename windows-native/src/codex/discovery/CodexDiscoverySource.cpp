#include "codex/discovery/CodexDiscoverySource.h"

#include <QDir>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <tlhelp32.h>

namespace companion {

namespace {

constexpr wchar_t kOfficialCodexPackageFamily[] =
    L"OpenAI.Codex_2p2nqsd0c76g0";

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}

    ~UniqueHandle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

class UniqueFindHandle final {
public:
    explicit UniqueFindHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~UniqueFindHandle()
    {
        if (valid()) {
            FindClose(handle_);
        }
    }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

    bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

QString stripExtendedPrefix(QString path)
{
    if (path.startsWith(
            QStringLiteral("\\\\?\\UNC\\"),
            Qt::CaseInsensitive)) {
        return QStringLiteral("\\\\") + path.sliced(8);
    }
    if (path.startsWith(
            QStringLiteral("\\\\?\\"),
            Qt::CaseInsensitive)) {
        return path.sliced(4);
    }
    if (path.startsWith(
            QStringLiteral("\\??\\"),
            Qt::CaseInsensitive)) {
        return path.sliced(4);
    }
    return path;
}

QString cleanedNativePath(QString path)
{
    path = stripExtendedPrefix(std::move(path));
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

QString extendedApiPath(const QString& displayPath)
{
    const QString native = QDir::toNativeSeparators(displayPath);
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.sliced(2);
    }
    if (native.size() >= 2 && native.at(1) == QLatin1Char(':')) {
        return QStringLiteral("\\\\?\\") + native;
    }
    return native;
}

QString fullPath(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QString native =
        QDir::toNativeSeparators(stripExtendedPrefix(path));
    const DWORD required = GetFullPathNameW(
        reinterpret_cast<LPCWSTR>(native.utf16()), 0, nullptr, nullptr);
    if (required == 0) {
        return cleanedNativePath(
            QDir::current().absoluteFilePath(
                QDir::fromNativeSeparators(native)));
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        required,
        buffer.data(),
        nullptr);
    if (written == 0 || written >= required) {
        return cleanedNativePath(
            QDir::current().absoluteFilePath(
                QDir::fromNativeSeparators(native)));
    }

    return cleanedNativePath(
        QString::fromWCharArray(
            buffer.data(), static_cast<qsizetype>(written)));
}

QString knownFolder(REFKNOWNFOLDERID folderId)
{
    PWSTR rawPath = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        if (rawPath != nullptr) {
            CoTaskMemFree(rawPath);
        }
        return {};
    }

    const QString path = QString::fromWCharArray(rawPath);
    CoTaskMemFree(rawPath);
    return fullPath(path);
}

void appendUniquePath(
    QVector<QString>& paths,
    const QString& path)
{
    const QString candidate = fullPath(path);
    if (candidate.isEmpty()) {
        return;
    }
    const auto duplicate = std::find_if(
        paths.cbegin(),
        paths.cend(),
        [&candidate](const QString& existing) {
            return existing.compare(
                       candidate,
                       Qt::CaseInsensitive)
                == 0;
        });
    if (duplicate == paths.cend()) {
        paths.append(candidate);
    }
}

QString stagedPackagePath(
    const wchar_t* packageFullName)
{
    UINT32 pathLength = 0;
    const LONG measured =
        GetStagedPackagePathByFullName(
            packageFullName,
            &pathLength,
            nullptr);
    if (measured != ERROR_INSUFFICIENT_BUFFER
        || pathLength == 0) {
        return {};
    }

    std::vector<wchar_t> path(
        static_cast<std::size_t>(pathLength));
    if (GetStagedPackagePathByFullName(
            packageFullName,
            &pathLength,
            path.data())
        != ERROR_SUCCESS) {
        return {};
    }
    return fullPath(
        QString::fromWCharArray(path.data()));
}

QVector<QString>
discoverInstalledCodexPackageExecutables()
{
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    const UINT32 filters =
        PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;
    const LONG measured = FindPackagesByPackageFamily(
        kOfficialCodexPackageFamily,
        filters,
        &count,
        nullptr,
        &bufferLength,
        nullptr,
        nullptr);
    if (measured == ERROR_SUCCESS && count == 0) {
        return {};
    }
    if (measured != ERROR_INSUFFICIENT_BUFFER
        || count == 0
        || bufferLength == 0) {
        return {};
    }

    std::vector<PWSTR> packageNames(
        static_cast<std::size_t>(count));
    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(bufferLength));
    std::vector<UINT32> properties(
        static_cast<std::size_t>(count));
    if (FindPackagesByPackageFamily(
            kOfficialCodexPackageFamily,
            filters,
            &count,
            packageNames.data(),
            &bufferLength,
            buffer.data(),
            properties.data())
        != ERROR_SUCCESS) {
        return {};
    }

    QVector<QString> executables;
    for (UINT32 index = 0; index < count; ++index) {
        const QString packageRoot =
            stagedPackagePath(
                packageNames.at(index));
        if (packageRoot.isEmpty()) {
            continue;
        }
        appendUniquePath(
            executables,
            QDir(packageRoot).filePath(
                QStringLiteral(
                    "app/resources/codex.exe")));
    }
    return executables;
}

QString processImage(HANDLE process)
{
    std::wstring buffer(32768, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(
            process, 0, buffer.data(), &length) == FALSE) {
        return {};
    }
    return fullPath(QString::fromWCharArray(
        buffer.data(), static_cast<qsizetype>(length)));
}

QString finalPath(HANDLE handle)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return cleanedNativePath(QString::fromWCharArray(
        buffer.data(), static_cast<qsizetype>(length)));
}

class SystemCodexDiscoverySource final : public ICodexDiscoverySource {
public:
    QString environmentValue(QStringView name) const override
    {
        const QByteArray variableName = name.toUtf8();
        return qEnvironmentVariable(variableName.constData());
    }

    QString profileDirectory() const override
    {
        return knownFolder(FOLDERID_Profile);
    }

    QString localAppDataDirectory() const override
    {
        return knownFolder(FOLDERID_LocalAppData);
    }

    QVector<QString>
    protectedProgramFilesDirectories() const override
    {
        return systemProtectedProgramFilesDirectories();
    }

    QVector<QString>
    installedCodexPackageExecutables() const override
    {
        return
            discoverInstalledCodexPackageExecutables();
    }

    QVector<QString> runningProcessImages(
        QStringView executableName) const override
    {
        QVector<QString> images;
        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.valid()) {
            return images;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot.get(), &entry) == FALSE) {
            return images;
        }

        do {
            if (QString::fromWCharArray(entry.szExeFile).compare(
                    executableName, Qt::CaseInsensitive) != 0) {
                continue;
            }

            UniqueHandle process(OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.th32ProcessID));
            if (!process.valid()) {
                continue;
            }

            const QString image = processImage(process.get());
            if (!image.isEmpty()) {
                images.append(image);
            }
        } while (Process32NextW(snapshot.get(), &entry) != FALSE);

        return images;
    }

    QVector<QString> childDirectories(
        const QString& directory) const override
    {
        QVector<QString> directories;
        if (directory.isEmpty()) {
            return directories;
        }

        const QString pattern = extendedApiPath(
            QDir(directory).filePath(QStringLiteral("*")));
        WIN32_FIND_DATAW data = {};
        UniqueFindHandle search(FindFirstFileW(
            reinterpret_cast<LPCWSTR>(pattern.utf16()), &data));
        if (!search.valid()) {
            return directories;
        }

        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            const QString name = QString::fromWCharArray(data.cFileName);
            if (name == QStringLiteral(".") ||
                name == QStringLiteral("..")) {
                continue;
            }
            directories.append(fullPath(QDir(directory).filePath(name)));
        } while (FindNextFileW(search.get(), &data) != FALSE);

        return directories;
    }

    QString searchPath(QStringView executableName) const override
    {
        const std::wstring executable = executableName.toString().toStdWString();
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD length = SearchPathW(
            nullptr,
            executable.c_str(),
            nullptr,
            static_cast<DWORD>(buffer.size()),
            buffer.data(),
            nullptr);
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
        return fullPath(QString::fromWCharArray(
            buffer.data(), static_cast<qsizetype>(length)));
    }

    CodexFileAttributes fileAttributes(
        const QString& path) const override
    {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        const QString apiPath = extendedApiPath(fullPath(path));
        if (apiPath.isEmpty() ||
            GetFileAttributesExW(
                reinterpret_cast<LPCWSTR>(apiPath.utf16()),
                GetFileExInfoStandard,
                &data) == FALSE) {
            return {};
        }

        const quint64 high =
            static_cast<quint64>(data.ftLastWriteTime.dwHighDateTime);
        const quint64 low =
            static_cast<quint64>(data.ftLastWriteTime.dwLowDateTime);
        return {
            true,
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
            (high << 32U) | low,
        };
    }

    QString absoluteDisplayPath(const QString& path) const override
    {
        return fullPath(path);
    }

    QString canonicalComparisonKey(
        const QString& path) const override
    {
        const QString display = fullPath(path);
        if (display.isEmpty()) {
            return {};
        }

        QString identity = display;
        const QString apiPath = extendedApiPath(display);
        UniqueHandle handle(CreateFileW(
            reinterpret_cast<LPCWSTR>(apiPath.utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (handle.valid()) {
            const QString resolved = finalPath(handle.get());
            if (!resolved.isEmpty()) {
                identity = resolved;
            }
        }

        return identity.toCaseFolded();
    }
};

} // namespace

QVector<QString>
systemProtectedProgramFilesDirectories()
{
    QVector<QString> roots;
    appendUniquePath(
        roots,
        knownFolder(FOLDERID_ProgramFiles));
    appendUniquePath(
        roots,
        knownFolder(FOLDERID_ProgramFilesX64));
    return roots;
}

const ICodexDiscoverySource& systemCodexDiscoverySource()
{
    static const SystemCodexDiscoverySource source;
    return source;
}

} // namespace companion
