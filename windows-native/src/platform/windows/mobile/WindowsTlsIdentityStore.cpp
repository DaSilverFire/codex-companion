#include "platform/windows/mobile/WindowsTlsIdentityStore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

#include <QCryptographicHash>
#include <QSslCertificate>
#include <QSslKey>
#include <QUuid>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace companion {
namespace {

constexpr auto kFallbackSuffix =
    L" Qt TLS Fallback";

CompanionError tlsError(
    QString code,
    QString message,
    QString operation,
    qlonglong status = 0)
{
    QVariantMap context{
        {QStringLiteral("operation"),
         std::move(operation)},
    };
    if (status != 0) {
        context.insert(
            QStringLiteral("status"),
            status);
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

std::wstring wide(
    const QString& text)
{
    return text.toStdWString();
}

QString fallbackReason()
{
    return QStringLiteral(
        "qt_schannel_requires_exportable_key_copy");
}

class CertContext final {
public:
    CertContext() = default;

    explicit CertContext(
        PCCERT_CONTEXT context)
        : context_(context)
    {
    }

    ~CertContext()
    {
        reset();
    }

    CertContext(const CertContext&) = delete;
    CertContext& operator=(const CertContext&) =
        delete;

    CertContext(CertContext&& other) noexcept
        : context_(std::exchange(
              other.context_,
              nullptr))
    {
    }

    CertContext& operator=(
        CertContext&& other) noexcept
    {
        if (this != &other) {
            reset();
            context_ = std::exchange(
                other.context_,
                nullptr);
        }
        return *this;
    }

    PCCERT_CONTEXT get() const noexcept
    {
        return context_;
    }

    PCCERT_CONTEXT release() noexcept
    {
        return std::exchange(context_, nullptr);
    }

    explicit operator bool() const noexcept
    {
        return context_ != nullptr;
    }

private:
    void reset()
    {
        if (context_ != nullptr) {
            CertFreeCertificateContext(
                context_);
            context_ = nullptr;
        }
    }

    PCCERT_CONTEXT context_ = nullptr;
};

class CertStore final {
public:
    explicit CertStore(HCERTSTORE store = nullptr)
        : store_(store)
    {
    }

    ~CertStore()
    {
        if (store_ != nullptr) {
            CertCloseStore(store_, 0);
        }
    }

    CertStore(const CertStore&) = delete;
    CertStore& operator=(const CertStore&) =
        delete;

    HCERTSTORE get() const noexcept
    {
        return store_;
    }

    explicit operator bool() const noexcept
    {
        return store_ != nullptr;
    }

private:
    HCERTSTORE store_ = nullptr;
};

class NCryptProvider final {
public:
    ~NCryptProvider()
    {
        if (provider_ != 0) {
            NCryptFreeObject(provider_);
        }
    }

    NCRYPT_PROV_HANDLE* put()
    {
        return &provider_;
    }

    NCRYPT_PROV_HANDLE get() const noexcept
    {
        return provider_;
    }

private:
    NCRYPT_PROV_HANDLE provider_ = 0;
};

class NCryptKey final {
public:
    explicit NCryptKey(
        NCRYPT_KEY_HANDLE key = 0)
        : key_(key)
    {
    }

    ~NCryptKey()
    {
        reset();
    }

    NCryptKey(const NCryptKey&) = delete;
    NCryptKey& operator=(const NCryptKey&) =
        delete;

    NCryptKey(NCryptKey&& other) noexcept
        : key_(std::exchange(other.key_, 0))
    {
    }

    NCryptKey& operator=(
        NCryptKey&& other) noexcept
    {
        if (this != &other) {
            reset();
            key_ = std::exchange(other.key_, 0);
        }
        return *this;
    }

    NCRYPT_KEY_HANDLE* put()
    {
        reset();
        return &key_;
    }

    NCRYPT_KEY_HANDLE get() const noexcept
    {
        return key_;
    }

    NCRYPT_KEY_HANDLE release() noexcept
    {
        return std::exchange(key_, 0);
    }

private:
    void reset()
    {
        if (key_ != 0) {
            NCryptFreeObject(key_);
            key_ = 0;
        }
    }

    NCRYPT_KEY_HANDLE key_ = 0;
};

CertStore currentUserMyStore()
{
    return CertStore(
        CertOpenStore(
            CERT_STORE_PROV_SYSTEM_W,
            0,
            0,
            CERT_SYSTEM_STORE_CURRENT_USER,
            L"MY"));
}

bool certificateIsCurrent(
    PCCERT_CONTEXT context)
{
    return context != nullptr
        && CertVerifyTimeValidity(
               nullptr,
               context->pCertInfo)
               == 0;
}

std::optional<std::wstring> keyContainer(
    PCCERT_CONTEXT context)
{
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(
            context,
            CERT_KEY_PROV_INFO_PROP_ID,
            nullptr,
            &size)
        || size == 0) {
        return std::nullopt;
    }
    QByteArray storage(
        static_cast<qsizetype>(size),
        '\0');
    if (!CertGetCertificateContextProperty(
            context,
            CERT_KEY_PROV_INFO_PROP_ID,
            storage.data(),
            &size)) {
        return std::nullopt;
    }
    const auto* info =
        reinterpret_cast<
            const CRYPT_KEY_PROV_INFO*>(
            storage.constData());
    if (info->pwszContainerName == nullptr) {
        return std::nullopt;
    }
    return std::wstring(info->pwszContainerName);
}

bool canSign(PCCERT_CONTEXT context)
{
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD keySpec = 0;
    BOOL callerFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            context,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG
                | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr,
            &key,
            &keySpec,
            &callerFree)) {
        return false;
    }

    bool ok = false;
    if (keySpec == CERT_NCRYPT_KEY_SPEC) {
        std::array<BYTE, 32> hash{};
        DWORD signatureBytes = 0;
        SECURITY_STATUS status = NCryptSignHash(
            static_cast<NCRYPT_KEY_HANDLE>(key),
            nullptr,
            hash.data(),
            static_cast<DWORD>(hash.size()),
            nullptr,
            0,
            &signatureBytes,
            0);
        ok = status == ERROR_SUCCESS
            && signatureBytes > 0;
    }
    if (callerFree) {
        if (keySpec == CERT_NCRYPT_KEY_SPEC) {
            NCryptFreeObject(
                static_cast<NCRYPT_KEY_HANDLE>(
                    key));
        } else {
            CryptReleaseContext(
                static_cast<HCRYPTPROV>(key),
                0);
        }
    }
    return ok;
}

CertContext findCertificate(
    const std::wstring& keyName)
{
    CertStore store = currentUserMyStore();
    if (!store) {
        return {};
    }

    PCCERT_CONTEXT previous = nullptr;
    while (true) {
        PCCERT_CONTEXT current =
            CertEnumCertificatesInStore(
                store.get(),
                previous);
        if (current == nullptr) {
            break;
        }
        previous = current;
        const auto container =
            keyContainer(current);
        if (container.has_value()
            && container.value() == keyName
            && certificateIsCurrent(current)
            && canSign(current)) {
            return CertContext(
                CertDuplicateCertificateContext(
                    current));
        }
    }
    return {};
}

Result<NCryptKey> createKey(
    const std::wstring& keyName,
    bool exportable)
{
    NCryptProvider provider;
    SECURITY_STATUS status =
        NCryptOpenStorageProvider(
            provider.put(),
            MS_KEY_STORAGE_PROVIDER,
            0);
    if (status != ERROR_SUCCESS) {
        return Result<NCryptKey>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_provider_failed"),
                QStringLiteral(
                    "Windows could not open the CNG key provider."),
                QStringLiteral(
                    "NCryptOpenStorageProvider"),
                status));
    }

    NCRYPT_KEY_HANDLE existing = 0;
    if (NCryptOpenKey(
            provider.get(),
            &existing,
            keyName.c_str(),
            0,
            0)
        == ERROR_SUCCESS) {
        (void)NCryptDeleteKey(existing, 0);
    }

    NCryptKey key;
    status = NCryptCreatePersistedKey(
        provider.get(),
        key.put(),
        NCRYPT_ECDSA_P256_ALGORITHM,
        keyName.c_str(),
        0,
        0);
    if (status != ERROR_SUCCESS) {
        return Result<NCryptKey>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_create_failed"),
                QStringLiteral(
                    "Windows could not create the Companion TLS key."),
                QStringLiteral(
                    "NCryptCreatePersistedKey"),
                status));
    }

    DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
    status = NCryptSetProperty(
        key.get(),
        NCRYPT_KEY_USAGE_PROPERTY,
        reinterpret_cast<PBYTE>(&usage),
        sizeof(usage),
        0);
    if (status != ERROR_SUCCESS) {
        return Result<NCryptKey>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_property_failed"),
                QStringLiteral(
                    "Windows could not configure the Companion TLS key."),
                QStringLiteral(
                    "NCryptSetProperty"),
                status));
    }

    DWORD exportPolicy =
        exportable
        ? (NCRYPT_ALLOW_EXPORT_FLAG
           | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG)
        : 0;
    status = NCryptSetProperty(
        key.get(),
        NCRYPT_EXPORT_POLICY_PROPERTY,
        reinterpret_cast<PBYTE>(&exportPolicy),
        sizeof(exportPolicy),
        0);
    if (status != ERROR_SUCCESS) {
        return Result<NCryptKey>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_property_failed"),
                QStringLiteral(
                    "Windows could not configure the Companion TLS key."),
                QStringLiteral(
                    "NCryptSetProperty"),
                status));
    }

    status = NCryptFinalizeKey(key.get(), 0);
    if (status != ERROR_SUCCESS) {
        return Result<NCryptKey>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_finalize_failed"),
                QStringLiteral(
                    "Windows could not finalize the Companion TLS key."),
                QStringLiteral(
                    "NCryptFinalizeKey"),
                status));
    }
    return Result<NCryptKey>::success(
        std::move(key));
}

Result<QByteArray> encodedSubject(
    const QString& installationId)
{
    const std::wstring subject =
        wide(QStringLiteral("CN=Codex Companion ")
             + installationId);
    DWORD size = 0;
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            nullptr,
            &size,
            nullptr)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.subject_failed"),
                QStringLiteral(
                    "Windows could not encode the Companion TLS certificate subject."),
                QStringLiteral(
                    "CertStrToName"),
                GetLastError()));
    }
    QByteArray encoded(
        static_cast<qsizetype>(size),
        '\0');
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            reinterpret_cast<BYTE*>(
                encoded.data()),
            &size,
            nullptr)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.subject_failed"),
                QStringLiteral(
                    "Windows could not encode the Companion TLS certificate subject."),
                QStringLiteral(
                    "CertStrToName"),
                GetLastError()));
    }
    encoded.resize(static_cast<qsizetype>(size));
    return Result<QByteArray>::success(
        std::move(encoded));
}

Result<QByteArray> serverAuthEku()
{
    LPSTR usage =
        const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_ENHKEY_USAGE eku{
        1,
        &usage,
    };
    DWORD size = 0;
    if (!CryptEncodeObject(
            X509_ASN_ENCODING,
            X509_ENHANCED_KEY_USAGE,
            &eku,
            nullptr,
            &size)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.eku_failed"),
                QStringLiteral(
                    "Windows could not encode the Companion TLS certificate usage."),
                QStringLiteral(
                    "CryptEncodeObject"),
                GetLastError()));
    }
    QByteArray encoded(
        static_cast<qsizetype>(size),
        '\0');
    if (!CryptEncodeObject(
            X509_ASN_ENCODING,
            X509_ENHANCED_KEY_USAGE,
            &eku,
            reinterpret_cast<BYTE*>(
                encoded.data()),
            &size)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.eku_failed"),
                QStringLiteral(
                    "Windows could not encode the Companion TLS certificate usage."),
                QStringLiteral(
                    "CryptEncodeObject"),
                GetLastError()));
    }
    encoded.resize(static_cast<qsizetype>(size));
    return Result<QByteArray>::success(
        std::move(encoded));
}

Result<CertContext> createCertificate(
    const WindowsTlsIdentityRequest& request,
    const std::wstring& keyName,
    bool exportable)
{
    const auto key = createKey(keyName, exportable);
    if (!key.hasValue()) {
        return Result<CertContext>::failure(
            key.error());
    }
    const auto subject =
        encodedSubject(request.installationId);
    if (!subject.hasValue()) {
        return Result<CertContext>::failure(
            subject.error());
    }
    const auto eku = serverAuthEku();
    if (!eku.hasValue()) {
        return Result<CertContext>::failure(
            eku.error());
    }

    CERT_NAME_BLOB subjectBlob{
        static_cast<DWORD>(
            subject.value().size()),
        reinterpret_cast<BYTE*>(
            const_cast<char*>(
                subject.value().constData())),
    };
    CERT_EXTENSION extension{
        const_cast<LPSTR>(
            szOID_ENHANCED_KEY_USAGE),
        FALSE,
        {static_cast<DWORD>(
             eku.value().size()),
         reinterpret_cast<BYTE*>(
             const_cast<char*>(
                 eku.value().constData()))},
    };
    CERT_EXTENSIONS extensions{
        1,
        &extension,
    };
    CRYPT_ALGORITHM_IDENTIFIER signature{};
    signature.pszObjId =
        const_cast<LPSTR>(szOID_ECDSA_SHA256);

    SYSTEMTIME start{};
    SYSTEMTIME end{};
    GetSystemTime(&start);
    end = start;
    end.wYear = static_cast<WORD>(
        end.wYear + request.validityYears);

    CertContext context(
        CertCreateSelfSignCertificate(
            key.value().get(),
            &subjectBlob,
            0,
            nullptr,
            &signature,
            &start,
            &end,
            &extensions));
    if (!context) {
        return Result<CertContext>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.certificate_create_failed"),
                QStringLiteral(
                    "Windows could not create the Companion TLS certificate."),
                QStringLiteral(
                    "CertCreateSelfSignCertificate"),
                GetLastError()));
    }

    std::wstring providerName =
        MS_KEY_STORAGE_PROVIDER;
    CRYPT_KEY_PROV_INFO providerInfo{};
    providerInfo.pwszContainerName =
        const_cast<LPWSTR>(keyName.c_str());
    providerInfo.pwszProvName =
        providerName.data();
    providerInfo.dwProvType = 0;
    providerInfo.dwKeySpec =
        CERT_NCRYPT_KEY_SPEC;
    if (!CertSetCertificateContextProperty(
            context.get(),
            CERT_KEY_PROV_INFO_PROP_ID,
            0,
            &providerInfo)) {
        return Result<CertContext>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_binding_failed"),
                QStringLiteral(
                    "Windows could not bind the Companion TLS certificate to its key."),
                QStringLiteral(
                    "CertSetCertificateContextProperty"),
                GetLastError()));
    }

    CertStore store = currentUserMyStore();
    if (!store) {
        return Result<CertContext>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.store_unavailable"),
                QStringLiteral(
                    "Windows could not open the CurrentUser certificate store."),
                QStringLiteral(
                    "CertOpenStore"),
                GetLastError()));
    }
    PCCERT_CONTEXT stored = nullptr;
    if (!CertAddCertificateContextToStore(
            store.get(),
            context.get(),
            CERT_STORE_ADD_REPLACE_EXISTING,
            &stored)) {
        return Result<CertContext>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.store_write_failed"),
                QStringLiteral(
                    "Windows could not save the Companion TLS certificate."),
                QStringLiteral(
                    "CertAddCertificateContextToStore"),
                GetLastError()));
    }
    return Result<CertContext>::success(
        CertContext(stored));
}

class AcquiredCertificateKey final {
public:
    AcquiredCertificateKey() = default;

    ~AcquiredCertificateKey()
    {
        reset();
    }

    AcquiredCertificateKey(
        const AcquiredCertificateKey&) =
        delete;
    AcquiredCertificateKey& operator=(
        const AcquiredCertificateKey&) =
        delete;

    bool acquire(PCCERT_CONTEXT context)
    {
        reset();
        return CryptAcquireCertificatePrivateKey(
            context,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG
                | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr,
            &key_,
            &keySpec_,
            &callerFree_);
    }

    NCRYPT_KEY_HANDLE key() const noexcept
    {
        return static_cast<
            NCRYPT_KEY_HANDLE>(key_);
    }

    bool isNcryptKey() const noexcept
    {
        return keySpec_
            == CERT_NCRYPT_KEY_SPEC;
    }

private:
    void reset()
    {
        if (key_ != 0 && callerFree_) {
            if (keySpec_
                == CERT_NCRYPT_KEY_SPEC) {
                NCryptFreeObject(
                    static_cast<
                        NCRYPT_KEY_HANDLE>(
                        key_));
            } else {
                CryptReleaseContext(
                    static_cast<HCRYPTPROV>(
                        key_),
                    0);
            }
        }
        key_ = 0;
        keySpec_ = 0;
        callerFree_ = FALSE;
    }

    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key_ = 0;
    DWORD keySpec_ = 0;
    BOOL callerFree_ = FALSE;
};

Result<QByteArray> exportEcPrivateKey(
    PCCERT_CONTEXT context)
{
    AcquiredCertificateKey acquired;
    if (!acquired.acquire(context)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_acquire_failed"),
                QStringLiteral(
                    "Windows could not open the Companion TLS fallback key."),
                QStringLiteral(
                    "CryptAcquireCertificatePrivateKey"),
                GetLastError()));
    }
    if (!acquired.isNcryptKey()) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_provider_unsupported"),
                QStringLiteral(
                    "The Companion TLS fallback key does not use the Windows CNG provider."),
                QStringLiteral(
                    "CryptAcquireCertificatePrivateKey")));
    }

    DWORD size = 0;
    SECURITY_STATUS status =
        NCryptExportKey(
            acquired.key(),
            0,
            BCRYPT_ECCPRIVATE_BLOB,
            nullptr,
            nullptr,
            0,
            &size,
            0);
    if (status != ERROR_SUCCESS
        || size == 0) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_export_failed"),
                QStringLiteral(
                    "Windows could not export the Qt TLS fallback key."),
                QStringLiteral(
                    "NCryptExportKey(ECCPRIVATEBLOB)"),
                status));
    }

    QByteArray key(
        static_cast<qsizetype>(size),
        '\0');
    status = NCryptExportKey(
        acquired.key(),
        0,
        BCRYPT_ECCPRIVATE_BLOB,
        nullptr,
        reinterpret_cast<PBYTE>(
            key.data()),
        size,
        &size,
        0);
    if (status != ERROR_SUCCESS) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_export_failed"),
                QStringLiteral(
                    "Windows could not export the Qt TLS fallback key."),
                QStringLiteral(
                    "NCryptExportKey(ECCPRIVATEBLOB)"),
                status));
    }
    key.resize(
        static_cast<qsizetype>(size));

    if (key.size()
            < qsizetype(
                sizeof(BCRYPT_ECCKEY_BLOB))) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_blob_invalid"),
                QStringLiteral(
                    "Windows returned an invalid Companion TLS fallback key."),
                QStringLiteral(
                    "BCRYPT_ECCPRIVATE_BLOB")));
    }

    const auto* header =
        reinterpret_cast<
            const BCRYPT_ECCKEY_BLOB*>(
            key.constData());
    const qsizetype coordinateBytes =
        static_cast<qsizetype>(
            header->cbKey);
    const qsizetype expectedBytes =
        qsizetype(
            sizeof(BCRYPT_ECCKEY_BLOB))
        + coordinateBytes * 3;
    if (header->dwMagic
            != BCRYPT_ECDSA_PRIVATE_P256_MAGIC
        || coordinateBytes != 32
        || key.size() != expectedBytes) {
        CompanionError error =
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_blob_invalid"),
                QStringLiteral(
                    "Windows returned an unsupported Companion TLS fallback key."),
                QStringLiteral(
                    "BCRYPT_ECCPRIVATE_BLOB"));
        error.context.insert(
            QStringLiteral("bytes"),
            key.size());
        error.context.insert(
            QStringLiteral("coordinateBytes"),
            coordinateBytes);
        error.context.insert(
            QStringLiteral("magic"),
            qulonglong(header->dwMagic));
        return Result<QByteArray>::failure(
            std::move(error));
    }

    const auto* coordinates =
        reinterpret_cast<const BYTE*>(
            key.constData()
            + sizeof(BCRYPT_ECCKEY_BLOB));
    const auto* privateValue =
        coordinates
        + coordinateBytes * 2;
    QByteArray publicPoint(
        1 + coordinateBytes * 2,
        '\0');
    publicPoint[0] = char(0x04);
    std::copy_n(
        reinterpret_cast<const char*>(
            coordinates),
        coordinateBytes * 2,
        publicPoint.data() + 1);

    CRYPT_ECC_PRIVATE_KEY_INFO info{};
    info.dwVersion =
        CRYPT_ECC_PRIVATE_KEY_INFO_v1;
    info.PrivateKey.cbData =
        static_cast<DWORD>(
            coordinateBytes);
    info.PrivateKey.pbData =
        const_cast<PBYTE>(
            privateValue);
    info.szCurveOid =
        const_cast<LPSTR>(
            szOID_ECC_CURVE_P256);
    info.PublicKey.cbData =
        static_cast<DWORD>(
            publicPoint.size());
    info.PublicKey.pbData =
        reinterpret_cast<PBYTE>(
            publicPoint.data());
    info.PublicKey.cUnusedBits = 0;

    DWORD encodedBytes = 0;
    if (!CryptEncodeObject(
            X509_ASN_ENCODING,
            X509_ECC_PRIVATE_KEY,
            &info,
            nullptr,
            &encodedBytes)
        || encodedBytes == 0) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_encode_failed"),
                QStringLiteral(
                    "Windows could not encode the Qt TLS fallback key."),
                QStringLiteral(
                    "CryptEncodeObject(X509_ECC_PRIVATE_KEY)"),
                GetLastError()));
    }

    QByteArray encoded(
        static_cast<qsizetype>(
            encodedBytes),
        '\0');
    if (!CryptEncodeObject(
            X509_ASN_ENCODING,
            X509_ECC_PRIVATE_KEY,
            &info,
            reinterpret_cast<PBYTE>(
                encoded.data()),
            &encodedBytes)) {
        return Result<QByteArray>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_encode_failed"),
                QStringLiteral(
                    "Windows could not encode the Qt TLS fallback key."),
                QStringLiteral(
                    "CryptEncodeObject(X509_ECC_PRIVATE_KEY)"),
                GetLastError()));
    }
    encoded.resize(
        static_cast<qsizetype>(
            encodedBytes));
    return Result<QByteArray>::success(
        std::move(encoded));
}

Result<WindowsTlsIdentityMaterial>
materialFromExportedKey(
    PCCERT_CONTEXT context,
    bool reusedExisting)
{
    const auto encodedKey =
        exportEcPrivateKey(context);
    if (!encodedKey.hasValue()) {
        return Result<WindowsTlsIdentityMaterial>::
            failure(encodedKey.error());
    }

    const QByteArray certificateDer(
        reinterpret_cast<const char*>(
            context->pbCertEncoded),
        static_cast<qsizetype>(
            context->cbCertEncoded));
    const QSslCertificate certificate(
        certificateDer,
        QSsl::Der);
    if (certificate.isNull()) {
        return Result<WindowsTlsIdentityMaterial>::
            failure(
                tlsError(
                    QStringLiteral(
                        "mobile.tls.certificate_import_failed"),
                    QStringLiteral(
                        "Qt could not import the Companion TLS certificate."),
                    QStringLiteral(
                        "QSslCertificate(DER)")));
    }

    const QSslKey key(
        encodedKey.value(),
        QSsl::Ec,
        QSsl::Der,
        QSsl::PrivateKey);
    if (key.isNull()) {
        CompanionError error =
            tlsError(
                QStringLiteral(
                    "mobile.tls.key_import_failed"),
                QStringLiteral(
                    "Qt could not import the Companion TLS fallback key."),
                QStringLiteral(
                    "QSslKey(ECPrivateKey)"));
        error.context.insert(
            QStringLiteral("bytes"),
            encodedKey.value().size());
        error.context.insert(
            QStringLiteral("formatPrefix"),
            QString::fromLatin1(
                encodedKey.value()
                    .first(8)
                    .toHex()));
        return Result<WindowsTlsIdentityMaterial>::
            failure(std::move(error));
    }

    QSslConfiguration configuration =
        QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(certificate);
    configuration.setPrivateKey(key);

    return Result<WindowsTlsIdentityMaterial>::
        success({
            certificateDer,
            configuration,
            {
                reusedExisting,
                false,
                true,
                fallbackReason(),
            },
        });
}

class WindowsNativeTlsIdentityBackend final
    : public IWindowsTlsIdentityBackend {
public:
    Result<WindowsTlsIdentityMaterial> loadOrCreate(
        const WindowsTlsIdentityRequest& request) override
    {
        if (request.installationId.trimmed().isEmpty()) {
            return Result<WindowsTlsIdentityMaterial>::
                failure(
                    tlsError(
                        QStringLiteral(
                            "mobile.tls.invalid_installation"),
                        QStringLiteral(
                            "A TLS identity requires an installation identifier."),
                        QStringLiteral(
                            "WindowsTlsIdentityStore::loadOrCreate")));
        }

        const std::wstring nativeKey =
            wide(request.keyName);
        CertContext native =
            findCertificate(nativeKey);
        if (!native) {
            const auto created =
                createCertificate(
                    request,
                    nativeKey,
                    false);
            if (!created.hasValue()) {
                return Result<WindowsTlsIdentityMaterial>::
                    failure(created.error());
            }
            native = CertContext(
                CertDuplicateCertificateContext(
                    created.value().get()));
        }

        const std::wstring fallbackKey =
            nativeKey + kFallbackSuffix;
        CertContext fallback =
            findCertificate(fallbackKey);
        bool reusedFallback = true;
        if (!fallback) {
            const auto created =
                createCertificate(
                    request,
                    fallbackKey,
                    true);
            if (!created.hasValue()) {
                return Result<WindowsTlsIdentityMaterial>::
                    failure(created.error());
            }
            fallback = CertContext(
                CertDuplicateCertificateContext(
                    created.value().get()));
            reusedFallback = false;
        }

        return materialFromExportedKey(
            fallback.get(),
            reusedFallback);
    }
};

} // namespace

WindowsTlsIdentityStore::WindowsTlsIdentityStore(
    IWindowsTlsIdentityBackend* backend)
    : ownedBackend_(backend == nullptr
                        ? std::make_unique<
                              WindowsNativeTlsIdentityBackend>()
                        : nullptr),
      backend_(backend == nullptr ? ownedBackend_.get()
                                  : backend)
{
}

WindowsTlsIdentityStore::~WindowsTlsIdentityStore() =
    default;

Result<WindowsTlsIdentity>
WindowsTlsIdentityStore::loadOrCreate(
    const QString& installationId)
{
    const auto material =
        backend_->loadOrCreate({
            installationId,
            QStringLiteral(
                "Codex Companion Nearby TLS v1"),
            5,
        });
    if (!material.hasValue()) {
        return Result<WindowsTlsIdentity>::failure(
            material.error());
    }
    if (material.value().certificateDer.isEmpty()) {
        return Result<WindowsTlsIdentity>::failure(
            tlsError(
                QStringLiteral(
                    "mobile.tls.missing_certificate"),
                QStringLiteral(
                    "The Companion TLS identity did not include a certificate."),
                QStringLiteral(
                    "IWindowsTlsIdentityBackend::loadOrCreate")));
    }

    const QString fingerprint =
        QString::fromLatin1(
            QCryptographicHash::hash(
                material.value().certificateDer,
                QCryptographicHash::Sha256)
                .toHex());
    return Result<WindowsTlsIdentity>::success({
        material.value().configuration,
        material.value().certificateDer,
        fingerprint.toLower(),
        material.value().diagnostics,
    });
}

} // namespace companion
