#include "platform/windows/AuthenticodeVerifier.h"

#include "update/UpdateBuildConfiguration.h"

#include <utility>
#include <vector>

#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>

#define NOMINMAX
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

namespace companion {
namespace {

CompanionError trustError(
    QString code,
    QString message,
    QStringView path,
    QVariantMap context = {})
{
    context.insert(
        QStringLiteral("path"),
        path.toString());
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

void closeTrustState(
    GUID* action,
    WINTRUST_DATA* trustData)
{
    trustData->dwStateAction =
        WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(
        nullptr,
        action,
        trustData);
}

Result<AuthenticodeIdentity>
verifyNativeAuthenticode(
    QStringView path,
    bool cacheOnlyUrlRetrieval)
{
    const std::wstring nativePath =
        path.toString().toStdWString();
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath =
        nativePath.c_str();

    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks =
        WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice =
        WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction =
        WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags =
        WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    if (cacheOnlyUrlRetrieval) {
        trustData.dwProvFlags |=
            WTD_CACHE_ONLY_URL_RETRIEVAL;
    }

    GUID action =
        WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status =
        WinVerifyTrust(
            nullptr,
            &action,
            &trustData);
    if (status != ERROR_SUCCESS) {
        closeTrustState(
            &action,
            &trustData);
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.authenticode_invalid"),
                QStringLiteral(
                    "The update installer has no valid trusted signature."),
                path,
                {
                    {
                        QStringLiteral("trustStatus"),
                        QVariant::fromValue<qlonglong>(
                            status),
                    },
                }));
    }

    CRYPT_PROVIDER_DATA* providerData =
        WTHelperProvDataFromStateData(
            trustData.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer =
        providerData == nullptr
        ? nullptr
        : WTHelperGetProvSignerFromChain(
              providerData,
              0,
              FALSE,
              0);
    CRYPT_PROVIDER_CERT* providerCertificate =
        signer == nullptr
        ? nullptr
        : WTHelperGetProvCertFromChain(
              signer,
              0);
    PCCERT_CONTEXT certificate =
        providerCertificate == nullptr
        ? nullptr
        : providerCertificate->pCert;
    if (certificate == nullptr) {
        closeTrustState(
            &action,
            &trustData);
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.authenticode_identity_unavailable"),
                QStringLiteral(
                    "The update signer identity could not be read."),
                path));
    }

    DWORD digestSize = 0;
    if (!CertGetCertificateContextProperty(
            certificate,
            CERT_SHA256_HASH_PROP_ID,
            nullptr,
            &digestSize)
        || digestSize == 0) {
        const DWORD error = GetLastError();
        closeTrustState(
            &action,
            &trustData);
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.authenticode_identity_unavailable"),
                QStringLiteral(
                    "The update signer thumbprint could not be read."),
                path,
                {
                    {
                        QStringLiteral("win32Error"),
                        QVariant::fromValue<qulonglong>(
                            error),
                    },
                }));
    }

    QByteArray digest(
        qsizetype(digestSize),
        Qt::Uninitialized);
    if (!CertGetCertificateContextProperty(
            certificate,
            CERT_SHA256_HASH_PROP_ID,
            digest.data(),
            &digestSize)) {
        const DWORD error = GetLastError();
        closeTrustState(
            &action,
            &trustData);
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.authenticode_identity_unavailable"),
                QStringLiteral(
                    "The update signer thumbprint could not be read."),
                path,
                {
                    {
                        QStringLiteral("win32Error"),
                        QVariant::fromValue<qulonglong>(
                            error),
                    },
                }));
    }
    digest.resize(qsizetype(digestSize));

    const DWORD subjectCharacters =
        CertGetNameStringW(
            certificate,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            nullptr,
            nullptr,
            0);
    std::vector<wchar_t> subjectBuffer(
        subjectCharacters > 0
            ? subjectCharacters
            : 1);
    if (subjectCharacters > 0) {
        (void)CertGetNameStringW(
            certificate,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            nullptr,
            subjectBuffer.data(),
            subjectCharacters);
    }

    AuthenticodeIdentity identity{
        QString::fromLatin1(
            digest.toHex())
            .toUpper(),
        subjectCharacters > 1
            ? QString::fromWCharArray(
                  subjectBuffer.data(),
                  int(subjectCharacters - 1))
                  .trimmed()
            : QString(),
    };
    closeTrustState(
        &action,
        &trustData);
    return Result<AuthenticodeIdentity>::success(
        std::move(identity));
}

} // namespace

AuthenticodePolicy
AuthenticodePolicy::fromBuildConfiguration()
{
    AuthenticodePolicy policy;
    const QString configured =
        QString::fromLatin1(
            COMPANION_WINDOWS_SIGNER_SHA256);
    policy.allowedSignerSha256 =
        configured.split(
            QLatin1Char(','),
            Qt::SkipEmptyParts);
    return policy;
}

AuthenticodeVerifier::AuthenticodeVerifier()
    : verifier_(verifyNativeAuthenticode)
{
}

AuthenticodeVerifier::AuthenticodeVerifier(
    NativeVerifier verifier)
    : verifier_(std::move(verifier))
{
}

QString
AuthenticodeVerifier::normalizeThumbprint(
    QStringView value)
{
    QString normalized =
        value.toString().toUpper();
    normalized.remove(
        QRegularExpression(
            QStringLiteral("[\\s:-]")));
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9A-F]{64}$"));
    return pattern.match(normalized).hasMatch()
        ? normalized
        : QString();
}

Result<AuthenticodeIdentity>
AuthenticodeVerifier::verify(
    QStringView path,
    const AuthenticodePolicy& policy) const
{
    if (!verifier_) {
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.authenticode_unavailable"),
                QStringLiteral(
                    "Authenticode verification is unavailable."),
                path));
    }

    const auto verified =
        verifier_(
            path,
            policy.cacheOnlyUrlRetrieval);
    if (!verified.hasValue()) {
        return verified;
    }

    AuthenticodeIdentity identity =
        verified.value();
    identity.sha256Thumbprint =
        normalizeThumbprint(
            identity.sha256Thumbprint);
    if (identity.sha256Thumbprint.isEmpty()) {
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.invalid_signer_thumbprint"),
                QStringLiteral(
                    "The update signer thumbprint is invalid."),
                path));
    }
    if (policy.allowedSignerSha256.isEmpty()) {
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.signer_policy_unconfigured"),
                QStringLiteral(
                    "No trusted Windows update signer is configured."),
                path));
    }

    QSet<QString> allowed;
    for (const QString& thumbprint :
         policy.allowedSignerSha256) {
        const QString normalized =
            normalizeThumbprint(thumbprint);
        if (normalized.isEmpty()) {
            return Result<AuthenticodeIdentity>::failure(
                trustError(
                    QStringLiteral(
                        "update.invalid_signer_policy"),
                    QStringLiteral(
                        "The Windows update signer policy is invalid."),
                    path));
        }
        allowed.insert(normalized);
    }

    if (!allowed.contains(
            identity.sha256Thumbprint)) {
        return Result<AuthenticodeIdentity>::failure(
            trustError(
                QStringLiteral(
                    "update.untrusted_signer"),
                QStringLiteral(
                    "The update installer was signed by an untrusted publisher."),
                path,
                {
                    {
                        QStringLiteral(
                            "signerSha256"),
                        identity.sha256Thumbprint,
                    },
                }));
    }
    return Result<AuthenticodeIdentity>::success(
        std::move(identity));
}

} // namespace companion
