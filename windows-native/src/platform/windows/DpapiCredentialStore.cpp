#include "platform/windows/DpapiCredentialStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Aclapi.h>
#include <Windows.h>
#include <dpapi.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace companion {

namespace {

constexpr qsizetype kMaximumCredentialBlobBytes =
    1024 * 1024;
constexpr qsizetype kMaximumCredentialSecretBytes =
    64 * 1024;

class NativeHandle final {
public:
    explicit NativeHandle(
        HANDLE handle = nullptr)
        : handle_(handle)
    {
    }

    ~NativeHandle()
    {
        if (handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(
        const NativeHandle&) = delete;

    HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

class LocalAllocation final {
public:
    explicit LocalAllocation(
        void* allocation = nullptr)
        : allocation_(allocation)
    {
    }

    ~LocalAllocation()
    {
        if (allocation_ != nullptr) {
            LocalFree(allocation_);
        }
    }

    LocalAllocation(const LocalAllocation&) = delete;
    LocalAllocation& operator=(
        const LocalAllocation&) = delete;

private:
    void* allocation_ = nullptr;
};

class ProtectedData final {
public:
    explicit ProtectedData(
        DATA_BLOB blob = {})
        : blob_(blob)
    {
    }

    ~ProtectedData()
    {
        if (blob_.pbData != nullptr) {
            SecureZeroMemory(
                blob_.pbData, blob_.cbData);
            LocalFree(blob_.pbData);
        }
    }

    ProtectedData(const ProtectedData&) = delete;
    ProtectedData& operator=(
        const ProtectedData&) = delete;

    const DATA_BLOB& get() const noexcept
    {
        return blob_;
    }

private:
    DATA_BLOB blob_{};
};

class PlaintextCopy final {
public:
    explicit PlaintextCopy(
        QByteArrayView value)
        : bytes_(
              value.data(),
              value.size())
    {
    }

    ~PlaintextCopy()
    {
        clear();
    }

    QByteArray& bytes() noexcept
    {
        return bytes_;
    }

    void clear() noexcept
    {
        if (!bytes_.isEmpty()) {
            SecureZeroMemory(
                bytes_.data(),
                static_cast<SIZE_T>(
                    bytes_.size()));
            bytes_.clear();
        }
    }

private:
    QByteArray bytes_;
};

CompanionError credentialError(
    QString code,
    QString message,
    const QString& service,
    DWORD windowsError = ERROR_SUCCESS,
    const QString& path = {})
{
    QVariantMap context{
        {QStringLiteral("service"), service},
    };
    if (windowsError != ERROR_SUCCESS) {
        context.insert(
            QStringLiteral("windowsError"),
            static_cast<qulonglong>(
                windowsError));
    }
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"), path);
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

Result<QByteArray> credentialReadFailure(
    QString code,
    QString message,
    const QString& service,
    DWORD windowsError = ERROR_SUCCESS,
    const QString& path = {})
{
    return Result<QByteArray>::failure(
        credentialError(
            std::move(code),
            std::move(message),
            service,
            windowsError,
            path));
}

Result<void> credentialWriteFailure(
    QString code,
    QString message,
    const QString& service,
    DWORD windowsError = ERROR_SUCCESS,
    const QString& path = {})
{
    return Result<void>::failure(
        credentialError(
            std::move(code),
            std::move(message),
            service,
            windowsError,
            path));
}

bool isSafeServiceName(
    const QString& service)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(service).hasMatch();
}

Result<void> restrictToCurrentUser(
    const QString& path,
    bool directory)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &rawToken)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.acl_failed"),
            QStringLiteral(
                "Could not inspect the current Windows user."),
            QString(),
            GetLastError(),
            path);
    }
    NativeHandle token(rawToken);

    DWORD tokenBytes = 0;
    GetTokenInformation(
        token.get(),
        TokenUser,
        nullptr,
        0,
        &tokenBytes);
    if (tokenBytes == 0) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.acl_failed"),
            QStringLiteral(
                "Could not inspect the current Windows user."),
            QString(),
            GetLastError(),
            path);
    }
    QByteArray tokenBuffer(
        static_cast<qsizetype>(tokenBytes),
        Qt::Uninitialized);
    if (!GetTokenInformation(
            token.get(),
            TokenUser,
            tokenBuffer.data(),
            tokenBytes,
            &tokenBytes)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.acl_failed"),
            QStringLiteral(
                "Could not inspect the current Windows user."),
            QString(),
            GetLastError(),
            path);
    }
    auto* tokenUser =
        reinterpret_cast<TOKEN_USER*>(
            tokenBuffer.data());

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = directory
        ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
        : NO_INHERITANCE;
    BuildTrusteeWithSidW(
        &access.Trustee,
        tokenUser->User.Sid);

    PACL rawAcl = nullptr;
    const DWORD aclStatus =
        SetEntriesInAclW(
            1,
            &access,
            nullptr,
            &rawAcl);
    if (aclStatus != ERROR_SUCCESS) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.acl_failed"),
            QStringLiteral(
                "Could not protect the credential permissions."),
            QString(),
            aclStatus,
            path);
    }
    LocalAllocation acl(rawAcl);

    std::wstring nativePath =
        QDir::toNativeSeparators(path)
            .toStdWString();
    const DWORD securityStatus =
        SetNamedSecurityInfoW(
            nativePath.data(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION
                | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            rawAcl,
            nullptr);
    if (securityStatus != ERROR_SUCCESS) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.acl_failed"),
            QStringLiteral(
                "Could not protect the credential permissions."),
            QString(),
            securityStatus,
            path);
    }
    return Result<void>::success();
}

DATA_BLOB dataBlob(QByteArray& bytes)
{
    return {
        static_cast<DWORD>(bytes.size()),
        reinterpret_cast<BYTE*>(
            bytes.data()),
    };
}

} // namespace

DpapiCredentialStore::DpapiCredentialStore()
    : DpapiCredentialStore(
          defaultRootDirectory())
{
}

DpapiCredentialStore::DpapiCredentialStore(
    QString rootDirectory)
    : DpapiCredentialStore(
          std::move(rootDirectory),
          {})
{
}

DpapiCredentialStore::DpapiCredentialStore(
    QString rootDirectory,
    CredentialAclApplier aclApplier)
    : rootDirectory_(
          QDir::cleanPath(
              std::move(rootDirectory))),
      aclApplier_(
          aclApplier
              ? std::move(aclApplier)
              : CredentialAclApplier(
                    &restrictToCurrentUser))
{
}

QString
DpapiCredentialStore::defaultRootDirectory()
{
    QString localAppData =
        qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.trimmed().isEmpty()) {
        localAppData =
            QStandardPaths::writableLocation(
                QStandardPaths::
                    GenericDataLocation);
    }
    return QDir(localAppData).filePath(
        QStringLiteral(
            "Codex Companion/Credentials"));
}

unsigned long
DpapiCredentialStore::protectionFlags() noexcept
{
    return CRYPTPROTECT_UI_FORBIDDEN;
}

Result<QString>
DpapiCredentialStore::pathForService(
    const QString& service) const
{
    if (!isSafeServiceName(service)) {
        return Result<QString>::failure(
            credentialError(
                QStringLiteral(
                    "credential.invalid_service"),
                QStringLiteral(
                    "The credential service name is invalid."),
                service));
    }
    return Result<QString>::success(
        QDir(rootDirectory_).filePath(
            service + QStringLiteral(".bin")));
}

Result<void>
DpapiCredentialStore::ensureRootDirectory() const
{
    if (rootDirectory_.trimmed().isEmpty()) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.directory_failed"),
            QStringLiteral(
                "The credential directory is unavailable."),
            QString());
    }
    QDir root;
    if (!root.mkpath(rootDirectory_)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.directory_failed"),
            QStringLiteral(
                "Could not create the credential directory."),
            QString(),
            ERROR_SUCCESS,
            rootDirectory_);
    }
    return aclApplier_(
        rootDirectory_, true);
}

Result<QByteArray> DpapiCredentialStore::read(
    const QString& service) const
{
    const Result<QString> resolved =
        pathForService(service);
    if (!resolved.hasValue()) {
        return Result<QByteArray>::failure(
            resolved.error());
    }
    const QString path = resolved.value();
    QFile file(path);
    if (!file.exists()) {
        return credentialReadFailure(
            QStringLiteral(
                "credential.not_found"),
            QStringLiteral(
                "The requested credential was not found."),
            service);
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return credentialReadFailure(
            QStringLiteral(
                "credential.read_failed"),
            QStringLiteral(
                "Could not read the credential."),
            service,
            ERROR_SUCCESS,
            path);
    }
    if (file.size() <= 0
        || file.size()
            > kMaximumCredentialBlobBytes) {
        return credentialReadFailure(
            QStringLiteral(
                "credential.invalid_blob"),
            QStringLiteral(
                "The stored credential is invalid."),
            service,
            ERROR_SUCCESS,
            path);
    }
    QByteArray ciphertext = file.readAll();
    if (ciphertext.size() != file.size()) {
        return credentialReadFailure(
            QStringLiteral(
                "credential.read_failed"),
            QStringLiteral(
                "Could not read the credential."),
            service,
            ERROR_SUCCESS,
            path);
    }

    QByteArray entropy = service.toUtf8();
    DATA_BLOB input = dataBlob(ciphertext);
    DATA_BLOB entropyBlob = dataBlob(entropy);
    DATA_BLOB output{};
    if (!CryptUnprotectData(
            &input,
            nullptr,
            &entropyBlob,
            nullptr,
            nullptr,
            protectionFlags(),
            &output)) {
        return credentialReadFailure(
            QStringLiteral(
                "credential.unprotect_failed"),
            QStringLiteral(
                "Windows could not decrypt the credential for this user."),
            service,
            GetLastError(),
            path);
    }
    ProtectedData plaintext(output);
    return Result<QByteArray>::success(
        QByteArray(
            reinterpret_cast<const char*>(
                plaintext.get().pbData),
            static_cast<qsizetype>(
                plaintext.get().cbData)));
}

Result<void> DpapiCredentialStore::write(
    const QString& service,
    QByteArrayView secret)
{
    const Result<QString> resolved =
        pathForService(service);
    if (!resolved.hasValue()) {
        return Result<void>::failure(
            resolved.error());
    }
    if (secret.isEmpty()) {
        return remove(service);
    }
    if (secret.size()
        > kMaximumCredentialSecretBytes) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.secret_too_large"),
            QStringLiteral(
                "The credential is too large to protect."),
            service);
    }

    const Result<void> directory =
        ensureRootDirectory();
    if (!directory.hasValue()) {
        return directory;
    }

    PlaintextCopy plaintext(secret);
    QByteArray entropy = service.toUtf8();
    DATA_BLOB input =
        dataBlob(plaintext.bytes());
    DATA_BLOB entropyBlob =
        dataBlob(entropy);
    DATA_BLOB output{};
    if (!CryptProtectData(
            &input,
            L"Codex Companion credential",
            &entropyBlob,
            nullptr,
            nullptr,
            protectionFlags(),
            &output)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.protect_failed"),
            QStringLiteral(
                "Windows could not protect the credential."),
            service,
            GetLastError());
    }
    plaintext.clear();
    ProtectedData ciphertext(output);

    const QString path = resolved.value();
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.write_failed"),
            QStringLiteral(
                "Could not write the protected credential."),
            service,
            ERROR_SUCCESS,
            path);
    }
    const qint64 written = file.write(
        reinterpret_cast<const char*>(
            ciphertext.get().pbData),
        static_cast<qint64>(
            ciphertext.get().cbData));
    if (written
        != static_cast<qint64>(
            ciphertext.get().cbData)
        || !file.commit()) {
        file.cancelWriting();
        return credentialWriteFailure(
            QStringLiteral(
                "credential.write_failed"),
            QStringLiteral(
                "Could not write the protected credential."),
            service,
            ERROR_SUCCESS,
            path);
    }
    return Result<void>::success();
}

Result<void> DpapiCredentialStore::remove(
    const QString& service)
{
    const Result<QString> resolved =
        pathForService(service);
    if (!resolved.hasValue()) {
        return Result<void>::failure(
            resolved.error());
    }
    const QString path = resolved.value();
    if (!QFileInfo::exists(path)) {
        return Result<void>::success();
    }
    if (!QFile::remove(path)) {
        return credentialWriteFailure(
            QStringLiteral(
                "credential.remove_failed"),
            QStringLiteral(
                "Could not remove the credential."),
            service,
            ERROR_SUCCESS,
            path);
    }
    return Result<void>::success();
}

bool DpapiCredentialStore::contains(
    const QString& service) const
{
    const Result<QString> resolved =
        pathForService(service);
    return resolved.hasValue()
        && QFileInfo(resolved.value()).isFile();
}

} // namespace companion
