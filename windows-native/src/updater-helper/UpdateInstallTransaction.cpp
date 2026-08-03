#include "updater-helper/UpdateInstallTransaction.h"

#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>
#include <QVariantMap>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace companion {
namespace {

constexpr auto kInstallerTimeout =
    std::chrono::minutes(15);
constexpr qint64 kMaximumShortcutBytes =
    4LL * 1024LL * 1024LL;
constexpr wchar_t
    kUninstallRegistrySubkey[] =
        L"Software\\Microsoft\\Windows\\"
        L"CurrentVersion\\Uninstall\\"
        L"{9B3C42CB-4B7F-4A08-B675-"
        L"071708948C88}_is1";

class UniqueHandle final {
public:
    explicit UniqueHandle(
        HANDLE handle = nullptr)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(
        const UniqueHandle&) = delete;
    UniqueHandle& operator=(
        const UniqueHandle&) = delete;

    UniqueHandle(
        UniqueHandle&& other) noexcept
        : handle_(
              std::exchange(
                  other.handle_,
                  nullptr))
    {
    }

    UniqueHandle& operator=(
        UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(
                std::exchange(
                    other.handle_,
                    nullptr));
        }
        return *this;
    }

    bool valid() const noexcept
    {
        return handle_ != nullptr
            && handle_
                != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

    HANDLE release() noexcept
    {
        return std::exchange(
            handle_,
            nullptr);
    }

    void reset(HANDLE replacement = nullptr)
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

class UniqueRegistryKey final {
public:
    explicit UniqueRegistryKey(
        HKEY key = nullptr)
        : key_(key)
    {
    }

    ~UniqueRegistryKey()
    {
        reset();
    }

    UniqueRegistryKey(
        const UniqueRegistryKey&) =
        delete;
    UniqueRegistryKey& operator=(
        const UniqueRegistryKey&) =
        delete;

    UniqueRegistryKey(
        UniqueRegistryKey&& other)
        noexcept
        : key_(
              std::exchange(
                  other.key_,
                  nullptr))
    {
    }

    UniqueRegistryKey& operator=(
        UniqueRegistryKey&& other)
        noexcept
    {
        if (this != &other) {
            reset(
                std::exchange(
                    other.key_,
                    nullptr));
        }
        return *this;
    }

    bool valid() const noexcept
    {
        return key_ != nullptr;
    }

    HKEY get() const noexcept
    {
        return key_;
    }

    void reset(HKEY replacement = nullptr)
    {
        if (valid()) {
            RegCloseKey(key_);
        }
        key_ = replacement;
    }

private:
    HKEY key_ = nullptr;
};

CompanionError transactionError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

CompanionError win32Error(
    QString code,
    QString message,
    DWORD error,
    QStringView path = {})
{
    QVariantMap context{
        {
            QStringLiteral(
                "win32Error"),
            QVariant::fromValue<
                qulonglong>(error),
        },
    };
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"),
            path.toString());
    }
    return transactionError(
        std::move(code),
        std::move(message),
        std::move(context));
}

QString normalizedPath(QStringView path)
{
    return QDir::cleanPath(
        QDir::fromNativeSeparators(
            path.toString()));
}

bool sameWindowsPath(
    QStringView left,
    QStringView right)
{
    return normalizedPath(left).compare(
               normalizedPath(right),
               Qt::CaseInsensitive)
        == 0;
}

QString apiPath(QStringView path)
{
    QString native =
        QDir::toNativeSeparators(
            normalizedPath(path));
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

quint64 fileTimeValue(FILETIME value)
{
    ULARGE_INTEGER converted{};
    converted.LowPart =
        value.dwLowDateTime;
    converted.HighPart =
        value.dwHighDateTime;
    return converted.QuadPart;
}

FILETIME fileTime(quint64 value)
{
    ULARGE_INTEGER converted{};
    converted.QuadPart = value;
    return {
        converted.LowPart,
        converted.HighPart,
    };
}

Result<void> requirePlainFile(
    QStringView path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    const DWORD attributes =
        GetFileAttributesW(
            native.c_str());
    if (attributes
            == INVALID_FILE_ATTRIBUTES
        || (attributes
            & FILE_ATTRIBUTE_DIRECTORY)
        || (attributes
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.helper_file_invalid"),
                QStringLiteral(
                    "A required update file is missing or unsafe."),
                attributes
                        == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_INVALID_DATA,
                path));
    }
    return Result<void>::success();
}

Result<void> requirePlainDirectory(
    QStringView path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    const DWORD attributes =
        GetFileAttributesW(
            native.c_str());
    if (attributes
            == INVALID_FILE_ATTRIBUTES
        || !(attributes
             & FILE_ATTRIBUTE_DIRECTORY)
        || (attributes
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.helper_directory_invalid"),
                QStringLiteral(
                    "A required update directory is missing or unsafe."),
                attributes
                        == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_INVALID_DATA,
                path));
    }
    return Result<void>::success();
}

Result<QByteArray> sha256File(
    QStringView path,
    qint64 expectedSize)
{
    const auto plain =
        requirePlainFile(path);
    if (!plain.hasValue()) {
        return Result<QByteArray>::
            failure(plain.error());
    }

    QFile file(path.toString());
    if (!file.open(
            QIODevice::ReadOnly)) {
        return Result<QByteArray>::
            failure(
                transactionError(
                    QStringLiteral(
                        "update.helper_installer_open_failed"),
                    QStringLiteral(
                        "The update installer could not be reopened for validation."),
                    {
                        {
                            QStringLiteral(
                                "path"),
                            path.toString(),
                        },
                    }));
    }
    if (file.size() != expectedSize) {
        return Result<QByteArray>::
            failure(
                transactionError(
                    QStringLiteral(
                        "update.artifact_size_mismatch"),
                    QStringLiteral(
                        "The update installer changed after verification.")));
    }

    QCryptographicHash hash(
        QCryptographicHash::
            Sha256);
    qint64 bytesRead = 0;
    while (!file.atEnd()) {
        const QByteArray chunk =
            file.read(256 * 1024);
        if (chunk.isEmpty()
            && file.error()
                != QFileDevice::NoError) {
            return Result<QByteArray>::
                failure(
                    transactionError(
                        QStringLiteral(
                            "update.helper_installer_read_failed"),
                        QStringLiteral(
                            "The update installer could not be read during validation.")));
        }
        hash.addData(chunk);
        bytesRead += chunk.size();
        if (bytesRead
            > expectedSize) {
            return Result<QByteArray>::
                failure(
                    transactionError(
                        QStringLiteral(
                            "update.artifact_size_mismatch"),
                        QStringLiteral(
                            "The update installer changed after verification.")));
        }
    }
    if (bytesRead != expectedSize
        || file.size() != expectedSize) {
        return Result<QByteArray>::
            failure(
                transactionError(
                    QStringLiteral(
                        "update.artifact_size_mismatch"),
                    QStringLiteral(
                        "The update installer changed after verification.")));
    }
    return Result<QByteArray>::success(
        hash.result());
}

QString expectedInstallerMarker(
    const UpdateInstallRequest& request)
{
    return QStringLiteral(
               "cc-update/1|%1|%2|w|x64|"
               "10.0.22000")
        .arg(request.expectedVersion)
        .arg(request.expectedBuild);
}

QString expectedInstallerFilename(
    const UpdateInstallRequest& request)
{
    return QStringLiteral(
               "Codex-Companion-%1-%2-"
               "windows-x64.exe")
        .arg(request.expectedVersion)
        .arg(request.expectedBuild);
}

Result<void> revalidateInstaller(
    const UpdateInstallRequest& request)
{
    const auto digest =
        sha256File(
            request.installerPath,
            request.expectedSize);
    if (!digest.hasValue()) {
        return Result<void>::failure(
            digest.error());
    }
    if (digest.value()
        != QByteArray::fromHex(
            request.expectedSha256
                .toLatin1())) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.artifact_digest_mismatch"),
                QStringLiteral(
                    "The update installer changed after verification.")));
    }

    const auto machine =
        PeImageInspector().machine(
            request.installerPath);
    if (!machine.hasValue()) {
        return Result<void>::failure(
            machine.error());
    }
    if (machine.value()
            != PeMachine::X86
        && machine.value()
            != PeMachine::X64) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.artifact_architecture_mismatch"),
                QStringLiteral(
                    "The update installer is not a supported Windows setup executable.")));
    }

    const auto metadata =
        InstallerMetadataReader().read(
            request.installerPath);
    if (!metadata.hasValue()) {
        return Result<void>::failure(
            metadata.error());
    }
    if (metadata.value().productName
            != QStringLiteral(
                "Codex Companion")
        || metadata.value()
                   .productVersionMarker
            != expectedInstallerMarker(
                request)
        || metadata.value()
                   .originalFilename
            != expectedInstallerFilename(
                request)) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.installer_identity_mismatch"),
                QStringLiteral(
                    "The update installer identity changed after verification.")));
    }

    const auto signer =
        AuthenticodeVerifier().verify(
            request.installerPath,
            AuthenticodePolicy::
                fromBuildConfiguration());
    if (!signer.hasValue()) {
        return Result<void>::failure(
            signer.error());
    }
    return Result<void>::success();
}

Result<UpdateAcknowledgementHandle>
prepareAcknowledgement(
    QStringView name)
{
    const std::wstring nativeName =
        name.toString().toStdWString();
    HANDLE handle =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nativeName.c_str());
    if (handle == nullptr) {
        return Result<
            UpdateAcknowledgementHandle>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.ack_event_create_failed"),
                    QStringLiteral(
                        "The update acknowledgement event could not be created."),
                    GetLastError()));
    }
    if (!ResetEvent(handle)) {
        const DWORD error =
            GetLastError();
        CloseHandle(handle);
        return Result<
            UpdateAcknowledgementHandle>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.ack_event_reset_failed"),
                    QStringLiteral(
                        "The update acknowledgement event could not be reset."),
                    error));
    }

    return Result<
        UpdateAcknowledgementHandle>::
        success(
            UpdateAcknowledgementHandle(
                handle,
                [](void* rawHandle) {
                    if (rawHandle != nullptr) {
                        CloseHandle(
                            static_cast<
                                HANDLE>(
                                rawHandle));
                    }
                }));
}

DWORD timeoutMilliseconds(
    std::chrono::milliseconds timeout)
{
    return static_cast<DWORD>(
        std::clamp<qint64>(
            timeout.count(),
            1,
            std::numeric_limits<DWORD>::
                max() - 1));
}

Result<void> waitForParentExit(
    quint32 processId,
    std::chrono::milliseconds timeout)
{
    UniqueHandle process(
        OpenProcess(
            SYNCHRONIZE,
            FALSE,
            processId));
    if (!process.valid()) {
        const DWORD error =
            GetLastError();
        if (error
            == ERROR_INVALID_PARAMETER) {
            return Result<void>::
                success();
        }
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.parent_open_failed"),
                QStringLiteral(
                    "The updater could not wait for Codex Companion to exit."),
                error));
    }

    const DWORD waited =
        WaitForSingleObject(
            process.get(),
            timeoutMilliseconds(timeout));
    if (waited == WAIT_OBJECT_0) {
        return Result<void>::success();
    }
    if (waited == WAIT_TIMEOUT) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.parent_exit_timeout"),
                QStringLiteral(
                    "Codex Companion did not exit before the update timeout.")));
    }
    return Result<void>::failure(
        win32Error(
            QStringLiteral(
                "update.parent_wait_failed"),
            QStringLiteral(
                "The updater could not confirm that Codex Companion exited."),
            GetLastError()));
}

Result<void> signalHelperReady(
    const UpdateInstallRequest& request)
{
    const std::wstring eventName =
        UpdateInstallRequest::
            helperReadyEventFor(
                request.requestId)
                .toStdWString();
    UniqueHandle event(
        OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            eventName.c_str()));
    if (!event.valid()) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.helper_ready_event_open_failed"),
                QStringLiteral(
                    "The updater could not open its startup handshake event."),
                GetLastError()));
    }
    if (!SetEvent(event.get())) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.helper_ready_event_signal_failed"),
                QStringLiteral(
                    "The updater could not confirm that it started."),
                GetLastError()));
    }
    return Result<void>::success();
}

Result<UpdateRegistrySnapshot>
snapshotUninstallRegistry()
{
    HKEY rawKey = nullptr;
    const LSTATUS opened =
        RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kUninstallRegistrySubkey,
            0,
            KEY_QUERY_VALUE,
            &rawKey);
    if (opened
        == ERROR_FILE_NOT_FOUND) {
        return Result<
            UpdateRegistrySnapshot>::
            success({});
    }
    if (opened != ERROR_SUCCESS) {
        return Result<
            UpdateRegistrySnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.registry_snapshot_failed"),
                    QStringLiteral(
                        "The current uninstall registration could not be read."),
                    static_cast<DWORD>(
                        opened)));
    }
    UniqueRegistryKey key(rawKey);

    DWORD valueCount = 0;
    DWORD maximumNameCharacters = 0;
    DWORD maximumDataBytes = 0;
    const LSTATUS queried =
        RegQueryInfoKeyW(
            key.get(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &valueCount,
            &maximumNameCharacters,
            &maximumDataBytes,
            nullptr,
            nullptr);
    if (queried != ERROR_SUCCESS) {
        return Result<
            UpdateRegistrySnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.registry_snapshot_failed"),
                    QStringLiteral(
                        "The current uninstall registration could not be inspected."),
                    static_cast<DWORD>(
                        queried)));
    }

    UpdateRegistrySnapshot snapshot;
    snapshot.existed = true;
    snapshot.values.reserve(
        static_cast<qsizetype>(
            valueCount));
    std::vector<wchar_t> name(
        static_cast<size_t>(
            maximumNameCharacters)
            + 2,
        L'\0');
    QByteArray data(
        static_cast<qsizetype>(
            maximumDataBytes),
        Qt::Uninitialized);

    for (DWORD index = 0;
         index < valueCount;
         ++index) {
        DWORD nameCharacters =
            maximumNameCharacters + 1;
        DWORD dataBytes =
            maximumDataBytes;
        DWORD type = REG_NONE;
        const LSTATUS enumerated =
            RegEnumValueW(
                key.get(),
                index,
                name.data(),
                &nameCharacters,
                nullptr,
                &type,
                data.isEmpty()
                    ? nullptr
                    : reinterpret_cast<BYTE*>(
                          data.data()),
                &dataBytes);
        if (enumerated
            != ERROR_SUCCESS) {
            return Result<
                UpdateRegistrySnapshot>::
                failure(
                    win32Error(
                        QStringLiteral(
                            "update.registry_snapshot_failed"),
                        QStringLiteral(
                            "An uninstall registration value could not be read."),
                        static_cast<DWORD>(
                            enumerated)));
        }
        snapshot.values.append({
            QString::fromWCharArray(
                name.data(),
                static_cast<int>(
                    nameCharacters)),
            type,
            data.first(
                static_cast<qsizetype>(
                    dataBytes)),
        });
    }
    return Result<
        UpdateRegistrySnapshot>::
        success(std::move(snapshot));
}

Result<UpdateShortcutSnapshot>
snapshotShortcut(QStringView path)
{
    const std::wstring native =
        apiPath(path).toStdWString();
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
        const DWORD error =
            GetLastError();
        if (error == ERROR_FILE_NOT_FOUND
            || error
                == ERROR_PATH_NOT_FOUND) {
            return Result<
                UpdateShortcutSnapshot>::
                success({});
        }
        return Result<
            UpdateShortcutSnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.shortcut_snapshot_failed"),
                    QStringLiteral(
                        "The current Start Menu shortcut could not be opened."),
                    error,
                    path));
    }

    FILE_ATTRIBUTE_TAG_INFO
        attributeInfo{};
    if (!GetFileInformationByHandleEx(
            handle.get(),
            FileAttributeTagInfo,
            &attributeInfo,
            sizeof(attributeInfo))
        || (attributeInfo.FileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY
               | FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))) {
        return Result<
            UpdateShortcutSnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.shortcut_snapshot_failed"),
                    QStringLiteral(
                        "The current Start Menu shortcut is unsafe."),
                    GetLastError(),
                    path));
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(
            handle.get(),
            &size)
        || size.QuadPart < 0
        || size.QuadPart
            > kMaximumShortcutBytes) {
        return Result<
            UpdateShortcutSnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.shortcut_snapshot_failed"),
                    QStringLiteral(
                        "The current Start Menu shortcut has an invalid size."),
                    GetLastError(),
                    path));
    }

    QByteArray bytes(
        static_cast<qsizetype>(
            size.QuadPart),
        Qt::Uninitialized);
    DWORD total = 0;
    while (total
           < static_cast<DWORD>(
               bytes.size())) {
        DWORD read = 0;
        const DWORD remaining =
            static_cast<DWORD>(
                bytes.size()) - total;
        if (!ReadFile(
                handle.get(),
                bytes.data() + total,
                remaining,
                &read,
                nullptr)
            || read == 0) {
            return Result<
                UpdateShortcutSnapshot>::
                failure(
                    win32Error(
                        QStringLiteral(
                            "update.shortcut_snapshot_failed"),
                        QStringLiteral(
                            "The current Start Menu shortcut could not be read."),
                        GetLastError(),
                        path));
        }
        total += read;
    }

    FILETIME creation{};
    FILETIME access{};
    FILETIME write{};
    if (!GetFileTime(
            handle.get(),
            &creation,
            &access,
            &write)) {
        return Result<
            UpdateShortcutSnapshot>::
            failure(
                win32Error(
                    QStringLiteral(
                        "update.shortcut_snapshot_failed"),
                    QStringLiteral(
                        "The current Start Menu shortcut metadata could not be read."),
                    GetLastError(),
                    path));
    }

    return Result<
        UpdateShortcutSnapshot>::success({
        true,
        std::move(bytes),
        attributeInfo.FileAttributes,
        fileTimeValue(creation),
        fileTimeValue(access),
        fileTimeValue(write),
    });
}

Result<UpdateInstallStateSnapshot>
snapshotState(
    const UpdateInstallRequest& request)
{
    const auto registry =
        snapshotUninstallRegistry();
    if (!registry.hasValue()) {
        return Result<
            UpdateInstallStateSnapshot>::
            failure(registry.error());
    }
    const auto shortcut =
        snapshotShortcut(
            request.startMenuShortcut);
    if (!shortcut.hasValue()) {
        return Result<
            UpdateInstallStateSnapshot>::
            failure(shortcut.error());
    }
    return Result<
        UpdateInstallStateSnapshot>::
        success({
            registry.value(),
            shortcut.value(),
        });
}

Result<void> moveInstallToRollback(
    const UpdateInstallRequest& request)
{
    const auto current =
        requirePlainDirectory(
            request.installRoot);
    if (!current.hasValue()) {
        return current;
    }
    const std::wstring rollback =
        apiPath(
            request.rollbackRoot)
            .toStdWString();
    if (GetFileAttributesW(
            rollback.c_str())
        != INVALID_FILE_ATTRIBUTES) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.rollback_already_exists"),
                QStringLiteral(
                    "The update rollback directory already exists.")));
    }

    const std::wstring installed =
        apiPath(
            request.installRoot)
            .toStdWString();
    if (!MoveFileExW(
            installed.c_str(),
            rollback.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.install_backup_failed"),
                QStringLiteral(
                    "The current Codex Companion installation could not be moved into rollback storage."),
                GetLastError(),
                request.installRoot));
    }
    return Result<void>::success();
}

void hideChildProcess(
    QProcess::CreateProcessArguments*
        arguments)
{
    arguments->flags |=
        CREATE_NO_WINDOW;
    arguments->startupInfo->dwFlags |=
        STARTF_USESHOWWINDOW;
    arguments->startupInfo->wShowWindow =
        SW_HIDE;
}

Result<int> runInstaller(
    const UpdateInstallRequest& request,
    QStringView logPath)
{
    QProcess process;
    process.setProgram(
        request.installerPath);
    process.setWorkingDirectory(
        QFileInfo(request.installerPath)
            .absolutePath());
    process.setArguments({
        QStringLiteral("/VERYSILENT"),
        QStringLiteral(
            "/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        QStringLiteral("/CURRENTUSER"),
        QStringLiteral("/LOG=%1")
            .arg(logPath),
    });
    process.setCreateProcessArgumentsModifier(
        hideChildProcess);
    process.start();
    if (!process.waitForStarted(
            30'000)) {
        return Result<int>::failure(
            transactionError(
                QStringLiteral(
                    "update.installer_start_failed"),
                QStringLiteral(
                    "The verified Windows installer could not be started."),
                {
                    {
                        QStringLiteral(
                            "detail"),
                        process.errorString(),
                    },
                }));
    }
    if (!process.waitForFinished(
            static_cast<int>(
                kInstallerTimeout
                    .count()
                * 60'000))) {
        process.kill();
        process.waitForFinished(
            10'000);
        return Result<int>::failure(
            transactionError(
                QStringLiteral(
                    "update.installer_timeout"),
                QStringLiteral(
                    "The Windows installer did not finish before the update timeout.")));
    }
    if (process.exitStatus()
        != QProcess::NormalExit) {
        return Result<int>::failure(
            transactionError(
                QStringLiteral(
                    "update.installer_crashed"),
                QStringLiteral(
                    "The Windows installer terminated unexpectedly.")));
    }
    return Result<int>::success(
        process.exitCode());
}

std::optional<std::array<quint16, 3>>
coreVersion(QStringView version)
{
    QString core =
        version.toString();
    const qsizetype suffix =
        std::min(
            core.indexOf(
                QLatin1Char('-'))
                    < 0
                ? core.size()
                : core.indexOf(
                      QLatin1Char('-')),
            core.indexOf(
                QLatin1Char('+'))
                    < 0
                ? core.size()
                : core.indexOf(
                      QLatin1Char('+')));
    core = core.first(suffix);
    const QStringList parts =
        core.split(
            QLatin1Char('.'),
            Qt::KeepEmptyParts);
    if (parts.size() != 3) {
        return std::nullopt;
    }

    std::array<quint16, 3> result{};
    for (qsizetype index = 0;
         index < parts.size();
         ++index) {
        bool valid = false;
        const uint value =
            parts.at(index)
                .toUInt(
                    &valid,
                    10);
        if (!valid
            || value
                > std::numeric_limits<
                    quint16>::max()) {
            return std::nullopt;
        }
        result.at(
            static_cast<size_t>(
                index)) =
            static_cast<quint16>(
                value);
    }
    return result;
}

Result<void> verifyExecutableVersion(
    QStringView path,
    QStringView expectedVersion,
    qint64 expectedBuild)
{
    const auto expectedCore =
        coreVersion(expectedVersion);
    if (!expectedCore.has_value()
        || expectedBuild <= 0
        || expectedBuild
            > std::numeric_limits<
                quint16>::max()) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.expected_version_invalid"),
                QStringLiteral(
                    "The expected installed application version is invalid.")));
    }

    const std::wstring native =
        path.toString().toStdWString();
    DWORD ignored = 0;
    const DWORD bytes =
        GetFileVersionInfoSizeW(
            native.c_str(),
            &ignored);
    if (bytes == 0) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.installed_version_unavailable"),
                QStringLiteral(
                    "The installed Codex Companion version could not be read."),
                GetLastError(),
                path));
    }
    std::vector<BYTE> buffer(bytes);
    if (!GetFileVersionInfoW(
            native.c_str(),
            0,
            bytes,
            buffer.data())) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.installed_version_unavailable"),
                QStringLiteral(
                    "The installed Codex Companion version could not be read."),
                GetLastError(),
                path));
    }

    void* rawInfo = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(
            buffer.data(),
            L"\\",
            &rawInfo,
            &infoBytes)
        || rawInfo == nullptr
        || infoBytes
            < sizeof(VS_FIXEDFILEINFO)) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.installed_version_unavailable"),
                QStringLiteral(
                    "The installed Codex Companion version metadata is incomplete.")));
    }
    const auto* fixed =
        static_cast<
            const VS_FIXEDFILEINFO*>(
            rawInfo);
    const std::array<quint16, 4>
        actual{
            static_cast<quint16>(
                HIWORD(
                    fixed
                        ->dwFileVersionMS)),
            static_cast<quint16>(
                LOWORD(
                    fixed
                        ->dwFileVersionMS)),
            static_cast<quint16>(
                HIWORD(
                    fixed
                        ->dwFileVersionLS)),
            static_cast<quint16>(
                LOWORD(
                    fixed
                        ->dwFileVersionLS)),
        };
    const std::array<quint16, 4>
        expected{
            expectedCore->at(0),
            expectedCore->at(1),
            expectedCore->at(2),
            static_cast<quint16>(
                expectedBuild),
        };
    if (actual != expected) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.installed_version_mismatch"),
                QStringLiteral(
                    "The installed Codex Companion version does not match the update.")));
    }
    return Result<void>::success();
}

Result<void> verifyEmbeddedIcon(
    QStringView executable)
{
    const std::wstring native =
        executable.toString()
            .toStdWString();
    HMODULE module =
        LoadLibraryExW(
            native.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.installed_icon_unavailable"),
                QStringLiteral(
                    "The installed Codex Companion icon could not be read."),
                GetLastError(),
                executable));
    }
    const HRSRC icon =
        FindResourceW(
            module,
            MAKEINTRESOURCEW(101),
            RT_GROUP_ICON);
    const DWORD error =
        icon == nullptr
        ? GetLastError()
        : ERROR_SUCCESS;
    FreeLibrary(module);
    if (icon == nullptr) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.installed_icon_missing"),
                QStringLiteral(
                    "The installed Codex Companion icon is missing."),
                error,
                executable));
    }
    return Result<void>::success();
}

Result<void> verifyInstalledTree(
    const UpdateInstallRequest& request)
{
    const auto root =
        requirePlainDirectory(
            request.installRoot);
    if (!root.hasValue()) {
        return root;
    }
    const QDir install(
        request.installRoot);
    const QString executable =
        install.filePath(
            QStringLiteral(
                "bin/CodexCompanion.exe"));
    const QString helper =
        install.filePath(
            QStringLiteral(
                "bin/"
                "CodexCompanionUpdater.exe"));
    const QString qtCore =
        install.filePath(
            QStringLiteral(
                "bin/Qt6Core.dll"));
    for (const QString& path : {
             executable,
             helper,
             qtCore,
             request.startMenuShortcut,
         }) {
        const auto plain =
            requirePlainFile(path);
        if (!plain.hasValue()) {
            return plain;
        }
    }

    const auto version =
        verifyExecutableVersion(
            executable,
            request.expectedVersion,
            request.expectedBuild);
    if (!version.hasValue()) {
        return version;
    }
    return verifyEmbeddedIcon(
        executable);
}

Result<quint32> launchApplication(
    QStringView executable,
    const QStringList& arguments,
    QStringView workingDirectory)
{
    qint64 processId = 0;
    const bool launched =
        QProcess::startDetached(
            executable.toString(),
            arguments,
            workingDirectory.toString(),
            &processId);
    if (!launched
        || processId <= 0
        || processId
            > std::numeric_limits<
                quint32>::max()) {
        return Result<quint32>::
            failure(
                transactionError(
                    QStringLiteral(
                        "update.application_launch_failed"),
                    QStringLiteral(
                        "Codex Companion could not be launched after the update.")));
    }
    return Result<quint32>::success(
        static_cast<quint32>(
            processId));
}

Result<void> waitForAcknowledgement(
    const UpdateAcknowledgementHandle&
        handle,
    std::chrono::milliseconds timeout)
{
    if (!handle) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.ack_event_unavailable"),
                QStringLiteral(
                    "The update acknowledgement event is unavailable.")));
    }
    const DWORD waited =
        WaitForSingleObject(
            static_cast<HANDLE>(
                handle.get()),
            timeoutMilliseconds(timeout));
    if (waited == WAIT_OBJECT_0) {
        return Result<void>::success();
    }
    if (waited == WAIT_TIMEOUT) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.acknowledgement_timeout"),
                QStringLiteral(
                    "The updated Codex Companion did not confirm a successful startup.")));
    }
    return Result<void>::failure(
        win32Error(
            QStringLiteral(
                "update.acknowledgement_wait_failed"),
            QStringLiteral(
                "The updater could not wait for the updated application."),
            GetLastError()));
}

Result<void> terminateProcess(
    quint32 processId)
{
    UniqueHandle process(
        OpenProcess(
            PROCESS_TERMINATE
                | SYNCHRONIZE,
            FALSE,
            processId));
    if (!process.valid()) {
        const DWORD error =
            GetLastError();
        if (error
            == ERROR_INVALID_PARAMETER) {
            return Result<void>::
                success();
        }
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.new_process_open_failed"),
                QStringLiteral(
                    "The failed updated application could not be opened for shutdown."),
                error));
    }
    if (!TerminateProcess(
            process.get(),
            1)) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.new_process_terminate_failed"),
                QStringLiteral(
                    "The failed updated application could not be stopped."),
                GetLastError()));
    }
    (void)WaitForSingleObject(
        process.get(),
        10'000);
    return Result<void>::success();
}

bool isAllowedInstallTree(
    const UpdateInstallRequest& request,
    QStringView path)
{
    const QString candidate =
        normalizedPath(path);
    const auto within =
        [&candidate](
            QStringView root) {
            const QString normalizedRoot =
                normalizedPath(root);
            return candidate.compare(
                       normalizedRoot,
                       Qt::CaseInsensitive)
                    == 0
                || candidate.startsWith(
                    normalizedRoot
                        + QLatin1Char('/'),
                    Qt::CaseInsensitive);
        };
    return within(request.installRoot)
        || within(request.rollbackRoot);
}

Result<void> removeTreeContents(
    const UpdateInstallRequest& request,
    QStringView directory)
{
    if (!isAllowedInstallTree(
            request,
            directory)) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.unsafe_remove_path"),
                QStringLiteral(
                    "The updater refused to remove an unexpected directory.")));
    }

    const std::wstring nativeDirectory =
        apiPath(directory)
            .toStdWString();
    const DWORD rootAttributes =
        GetFileAttributesW(
            nativeDirectory.c_str());
    if (rootAttributes
            == INVALID_FILE_ATTRIBUTES) {
        const DWORD error =
            GetLastError();
        if (error == ERROR_FILE_NOT_FOUND
            || error
                == ERROR_PATH_NOT_FOUND) {
            return Result<void>::
                success();
        }
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.remove_tree_failed"),
                QStringLiteral(
                    "The update installation directory could not be inspected."),
                error,
                directory));
    }
    if (!(rootAttributes
          & FILE_ATTRIBUTE_DIRECTORY)
        || (rootAttributes
            & (FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.unsafe_remove_path"),
                QStringLiteral(
                    "The updater refused to traverse an unsafe installation directory.")));
    }

    const QString searchPath =
        apiPath(
            normalizedPath(directory)
            + QStringLiteral("/*"));
    WIN32_FIND_DATAW found{};
    HANDLE rawFind =
        FindFirstFileW(
            searchPath
                .toStdWString()
                .c_str(),
            &found);
    if (rawFind
        == INVALID_HANDLE_VALUE) {
        const DWORD error =
            GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            return Result<void>::failure(
                win32Error(
                    QStringLiteral(
                        "update.remove_tree_failed"),
                    QStringLiteral(
                        "The update installation directory could not be enumerated."),
                    error,
                    directory));
        }
    } else {
        do {
            const QString name =
                QString::fromWCharArray(
                    found.cFileName);
            if (name == QStringLiteral(".")
                || name
                    == QStringLiteral("..")) {
                continue;
            }
            const QString child =
                QDir(
                    directory.toString())
                    .filePath(name);
            const std::wstring nativeChild =
                apiPath(child)
                    .toStdWString();
            const bool childDirectory =
                (found.dwFileAttributes
                 & FILE_ATTRIBUTE_DIRECTORY)
                != 0;
            const bool childReparse =
                (found.dwFileAttributes
                 & FILE_ATTRIBUTE_REPARSE_POINT)
                != 0;
            if (childDirectory
                && !childReparse) {
                const auto removed =
                    removeTreeContents(
                        request,
                        child);
                if (!removed.hasValue()) {
                    FindClose(rawFind);
                    return removed;
                }
                continue;
            }

            if ((found.dwFileAttributes
                 & FILE_ATTRIBUTE_READONLY)
                != 0) {
                SetFileAttributesW(
                    nativeChild.c_str(),
                    found.dwFileAttributes
                        & ~FILE_ATTRIBUTE_READONLY);
            }
            const BOOL removed =
                childDirectory
                ? RemoveDirectoryW(
                      nativeChild.c_str())
                : DeleteFileW(
                      nativeChild.c_str());
            if (!removed) {
                const DWORD error =
                    GetLastError();
                FindClose(rawFind);
                return Result<void>::failure(
                    win32Error(
                        QStringLiteral(
                            "update.remove_tree_failed"),
                        QStringLiteral(
                            "A file in the update installation directory could not be removed."),
                        error,
                        child));
            }
        } while (FindNextFileW(
                     rawFind,
                     &found));
        const DWORD findError =
            GetLastError();
        FindClose(rawFind);
        if (findError
            != ERROR_NO_MORE_FILES) {
            return Result<void>::failure(
                win32Error(
                    QStringLiteral(
                        "update.remove_tree_failed"),
                    QStringLiteral(
                        "The update installation directory enumeration failed."),
                    findError,
                    directory));
        }
    }

    if (!RemoveDirectoryW(
            nativeDirectory.c_str())) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.remove_tree_failed"),
                QStringLiteral(
                    "The update installation directory could not be removed."),
                GetLastError(),
                directory));
    }
    return Result<void>::success();
}

Result<void> restoreRegistry(
    const UpdateRegistrySnapshot&
        snapshot)
{
    const LSTATUS deleted =
        RegDeleteTreeW(
            HKEY_CURRENT_USER,
            kUninstallRegistrySubkey);
    if (deleted != ERROR_SUCCESS
        && deleted
            != ERROR_FILE_NOT_FOUND) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.registry_restore_failed"),
                QStringLiteral(
                    "The failed update uninstall registration could not be cleared."),
                static_cast<DWORD>(
                    deleted)));
    }
    if (!snapshot.existed) {
        return Result<void>::success();
    }

    HKEY rawKey = nullptr;
    const LSTATUS created =
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kUninstallRegistrySubkey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &rawKey,
            nullptr);
    if (created != ERROR_SUCCESS) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.registry_restore_failed"),
                QStringLiteral(
                    "The previous uninstall registration could not be recreated."),
                static_cast<DWORD>(
                    created)));
    }
    UniqueRegistryKey key(rawKey);
    for (const auto& value :
         snapshot.values) {
        const std::wstring name =
            value.name.toStdWString();
        const LSTATUS written =
            RegSetValueExW(
                key.get(),
                name.empty()
                    ? nullptr
                    : name.c_str(),
                0,
                value.type,
                value.data.isEmpty()
                    ? nullptr
                    : reinterpret_cast<
                          const BYTE*>(
                          value.data
                              .constData()),
                static_cast<DWORD>(
                    value.data.size()));
        if (written != ERROR_SUCCESS) {
            return Result<void>::failure(
                win32Error(
                    QStringLiteral(
                        "update.registry_restore_failed"),
                    QStringLiteral(
                        "A previous uninstall registration value could not be restored."),
                    static_cast<DWORD>(
                        written)));
        }
    }
    return Result<void>::success();
}

Result<void> restoreShortcut(
    QStringView path,
    const UpdateShortcutSnapshot&
        snapshot)
{
    const std::wstring native =
        apiPath(path).toStdWString();
    if (!snapshot.existed) {
        if (!DeleteFileW(
                native.c_str())) {
            const DWORD error =
                GetLastError();
            if (error != ERROR_FILE_NOT_FOUND
                && error
                    != ERROR_PATH_NOT_FOUND) {
                return Result<void>::failure(
                    win32Error(
                        QStringLiteral(
                            "update.shortcut_restore_failed"),
                        QStringLiteral(
                            "The failed update Start Menu shortcut could not be removed."),
                        error,
                        path));
            }
        }
        return Result<void>::success();
    }

    const QFileInfo information(
        path.toString());
    if (!QDir().mkpath(
            information
                .absolutePath())) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.shortcut_restore_failed"),
                QStringLiteral(
                    "The previous Start Menu shortcut directory could not be recreated.")));
    }

    QSaveFile file(
        information
            .absoluteFilePath());
    file.setDirectWriteFallback(false);
    if (!file.open(
            QIODevice::WriteOnly)
        || file.write(snapshot.bytes)
            != snapshot.bytes.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.shortcut_restore_failed"),
                QStringLiteral(
                    "The previous Start Menu shortcut could not be restored.")));
    }

    UniqueHandle handle(
        CreateFileW(
            native.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    if (!handle.valid()) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.shortcut_restore_failed"),
                QStringLiteral(
                    "The restored Start Menu shortcut metadata could not be opened."),
                GetLastError(),
                path));
    }
    const FILETIME creation =
        fileTime(
            snapshot.creationTime);
    const FILETIME access =
        fileTime(
            snapshot.accessTime);
    const FILETIME write =
        fileTime(
            snapshot.writeTime);
    if (!SetFileTime(
            handle.get(),
            &creation,
            &access,
            &write)
        || !SetFileAttributesW(
            native.c_str(),
            snapshot.attributes)) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.shortcut_restore_failed"),
                QStringLiteral(
                    "The previous Start Menu shortcut metadata could not be restored."),
                GetLastError(),
                path));
    }
    return Result<void>::success();
}

Result<void> restoreRollback(
    const UpdateInstallRequest& request,
    const UpdateInstallStateSnapshot&
        snapshot)
{
    const auto removed =
        removeTreeContents(
            request,
            request.installRoot);
    if (!removed.hasValue()) {
        return removed;
    }

    const auto rollback =
        requirePlainDirectory(
            request.rollbackRoot);
    if (!rollback.hasValue()) {
        return rollback;
    }
    const std::wstring rollbackNative =
        apiPath(
            request.rollbackRoot)
            .toStdWString();
    const std::wstring installNative =
        apiPath(
            request.installRoot)
            .toStdWString();
    if (!MoveFileExW(
            rollbackNative.c_str(),
            installNative.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        return Result<void>::failure(
            win32Error(
                QStringLiteral(
                    "update.rollback_restore_failed"),
                QStringLiteral(
                    "The previous Codex Companion installation could not be restored."),
                GetLastError(),
                request.rollbackRoot));
    }

    const auto registry =
        restoreRegistry(
            snapshot.uninstallRegistry);
    if (!registry.hasValue()) {
        return registry;
    }
    return restoreShortcut(
        request.startMenuShortcut,
        snapshot.startMenuShortcut);
}

Result<void> commitRollback(
    const UpdateInstallRequest& request)
{
    return removeTreeContents(
        request,
        request.rollbackRoot);
}

Result<void> missingDependency(
    QStringView name)
{
    return Result<void>::failure(
        transactionError(
            QStringLiteral(
                "update.helper_dependency_missing"),
            QStringLiteral(
                "The updater transaction is missing a required operation."),
            {
                {
                    QStringLiteral(
                        "operation"),
                    name.toString(),
                },
            }));
}

} // namespace

UpdateInstallTransaction::
UpdateInstallTransaction(
    UpdateInstallTransactionOptions options,
    UpdateInstallTransactionDependencies
        dependencies)
    : options_(std::move(options)),
      dependencies_(
          std::move(dependencies))
{
}

Result<void>
UpdateInstallTransaction::run(
    const UpdateInstallRequest& request)
{
    const auto requestValid =
        request.validate();
    if (!requestValid.hasValue()) {
        return requestValid;
    }
    if (options_.transactionRoot
            .isEmpty()) {
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.transaction_root_invalid"),
                QStringLiteral(
                    "The updater transaction directory is unavailable.")));
    }

    if (!dependencies_
             .prepareAcknowledgement) {
        return missingDependency(
            u"prepareAcknowledgement");
    }
    const auto acknowledgement =
        dependencies_
            .prepareAcknowledgement(
                request
                    .acknowledgementEvent);
    if (!acknowledgement.hasValue()) {
        return Result<void>::failure(
            acknowledgement.error());
    }

    if (!dependencies_.signalReady) {
        return missingDependency(
            u"signalReady");
    }
    const auto ready =
        dependencies_.signalReady(
            request);
    if (!ready.hasValue()) {
        return ready;
    }

    if (!dependencies_
             .waitForParentExit) {
        return missingDependency(
            u"waitForParentExit");
    }
    const auto parentExited =
        dependencies_.waitForParentExit(
            request.parentProcessId,
            options_.parentExitTimeout);
    if (!parentExited.hasValue()) {
        return parentExited;
    }

    if (!dependencies_
             .revalidateInstaller) {
        return missingDependency(
            u"revalidateInstaller");
    }
    const auto revalidated =
        dependencies_
            .revalidateInstaller(
                request);
    if (!revalidated.hasValue()) {
        return revalidated;
    }

    if (!dependencies_.snapshotState) {
        return missingDependency(
            u"snapshotState");
    }
    const auto snapshot =
        dependencies_.snapshotState(
            request);
    if (!snapshot.hasValue()) {
        return Result<void>::failure(
            snapshot.error());
    }

    if (!dependencies_
             .moveInstallToRollback) {
        return missingDependency(
            u"moveInstallToRollback");
    }
    const auto moved =
        dependencies_
            .moveInstallToRollback(
                request);
    if (!moved.hasValue()) {
        return moved;
    }

    if (!dependencies_.runInstaller) {
        return rollback(
            request,
            snapshot.value(),
            missingDependency(
                u"runInstaller")
                .error(),
            0);
    }
    const QString logPath =
        QDir(options_.transactionRoot)
            .filePath(
                QStringLiteral(
                    "installer.log"));
    const auto installer =
        dependencies_.runInstaller(
            request,
            logPath);
    if (!installer.hasValue()) {
        return rollback(
            request,
            snapshot.value(),
            installer.error(),
            0);
    }
    if (installer.value() != 0) {
        return rollback(
            request,
            snapshot.value(),
            transactionError(
                QStringLiteral(
                    "update.installer_exit_failed"),
                QStringLiteral(
                    "The Windows installer reported a failed update."),
                {
                    {
                        QStringLiteral(
                            "exitCode"),
                        installer.value(),
                    },
                }),
            0);
    }

    if (!dependencies_
             .verifyInstalledTree) {
        return rollback(
            request,
            snapshot.value(),
            missingDependency(
                u"verifyInstalledTree")
                .error(),
            0);
    }
    const auto installed =
        dependencies_
            .verifyInstalledTree(
                request);
    if (!installed.hasValue()) {
        return rollback(
            request,
            snapshot.value(),
            installed.error(),
            0);
    }

    if (dependencies_.afterReplacement) {
        const auto continued =
            dependencies_.afterReplacement(
                request);
        if (!continued.hasValue()) {
            return rollback(
                request,
                snapshot.value(),
                continued.error(),
                0);
        }
    }

    if (!dependencies_
             .launchApplication) {
        return rollback(
            request,
            snapshot.value(),
            missingDependency(
                u"launchApplication")
                .error(),
            0);
    }
    const QString executable =
        installedExecutablePath(
            request);
    const auto launched =
        dependencies_
            .launchApplication(
                executable,
                {
                    QStringLiteral(
                        "--post-update-ack"),
                    request.requestId,
                },
                QFileInfo(executable)
                    .absolutePath());
    if (!launched.hasValue()) {
        return rollback(
            request,
            snapshot.value(),
            launched.error(),
            0);
    }

    if (!dependencies_
             .waitForAcknowledgement) {
        return rollback(
            request,
            snapshot.value(),
            missingDependency(
                u"waitForAcknowledgement")
                .error(),
            launched.value());
    }
    const auto acknowledged =
        dependencies_
            .waitForAcknowledgement(
                acknowledgement.value(),
                options_
                    .acknowledgementTimeout);
    if (!acknowledged.hasValue()) {
        return rollback(
            request,
            snapshot.value(),
            acknowledged.error(),
            launched.value());
    }

    if (!dependencies_.commitRollback) {
        return missingDependency(
            u"commitRollback");
    }
    return dependencies_
        .commitRollback(request);
}

UpdateInstallTransaction
UpdateInstallTransaction::createProduction(
    QString transactionRoot)
{
    UpdateInstallTransactionOptions
        options;
    options.transactionRoot =
        normalizedPath(
            transactionRoot);
    return UpdateInstallTransaction(
        std::move(options),
        productionUpdateInstallDependencies());
}

QString UpdateInstallTransaction::
installedExecutablePath(
    const UpdateInstallRequest& request)
{
    return QDir(request.installRoot)
        .filePath(
            QStringLiteral(
                "bin/CodexCompanion.exe"));
}

Result<void>
UpdateInstallTransaction::rollback(
    const UpdateInstallRequest& request,
    const UpdateInstallStateSnapshot&
        snapshot,
    const CompanionError& cause,
    quint32 newProcessId)
{
    std::optional<CompanionError>
        rollbackFailure;
    if (newProcessId != 0) {
        if (!dependencies_
                 .terminateProcess) {
            rollbackFailure =
                missingDependency(
                    u"terminateProcess")
                    .error();
        } else {
            const auto terminated =
                dependencies_
                    .terminateProcess(
                        newProcessId);
            if (!terminated.hasValue()) {
                rollbackFailure =
                    terminated.error();
            }
        }
    }

    if (!dependencies_.restoreRollback) {
        rollbackFailure =
            missingDependency(
                u"restoreRollback")
                .error();
    } else {
        const auto restored =
            dependencies_.restoreRollback(
                request,
                snapshot);
        if (!restored.hasValue()) {
            rollbackFailure =
                restored.error();
        }
    }

    CompanionError launchFailure;
    bool previousLaunched = false;
    if (dependencies_.launchApplication) {
        const QString executable =
            installedExecutablePath(
                request);
        const auto launched =
            dependencies_
                .launchApplication(
                    executable,
                    {},
                    QFileInfo(executable)
                        .absolutePath());
        previousLaunched =
            launched.hasValue();
        if (!previousLaunched) {
            launchFailure =
                launched.error();
        }
    } else {
        launchFailure =
            missingDependency(
                u"launchApplication")
                .error();
    }

    if (rollbackFailure.has_value()
        || !previousLaunched) {
        QVariantMap context{
            {
                QStringLiteral(
                    "causeCode"),
                cause.code,
            },
        };
        if (rollbackFailure
                .has_value()) {
            context.insert(
                QStringLiteral(
                    "rollbackCode"),
                rollbackFailure->code);
        }
        if (!previousLaunched) {
            context.insert(
                QStringLiteral(
                    "relaunchCode"),
                launchFailure.code);
        }
        return Result<void>::failure(
            transactionError(
                QStringLiteral(
                    "update.rollback_failed"),
                QStringLiteral(
                    "The update failed and the previous Codex Companion installation could not be fully restored."),
                std::move(context)));
    }
    return Result<void>::failure(cause);
}

UpdateInstallTransactionDependencies
productionUpdateInstallDependencies()
{
    UpdateInstallTransactionDependencies
        dependencies;
    dependencies.prepareAcknowledgement =
        prepareAcknowledgement;
    dependencies.signalReady =
        signalHelperReady;
    dependencies.waitForParentExit =
        waitForParentExit;
    dependencies.revalidateInstaller =
        revalidateInstaller;
    dependencies.snapshotState =
        snapshotState;
    dependencies.moveInstallToRollback =
        moveInstallToRollback;
    dependencies.runInstaller =
        runInstaller;
    dependencies.verifyInstalledTree =
        verifyInstalledTree;
    dependencies.launchApplication =
        launchApplication;
    dependencies.waitForAcknowledgement =
        waitForAcknowledgement;
    dependencies.terminateProcess =
        terminateProcess;
    dependencies.restoreRollback =
        restoreRollback;
    dependencies.commitRollback =
        commitRollback;
    return dependencies;
}

} // namespace companion
