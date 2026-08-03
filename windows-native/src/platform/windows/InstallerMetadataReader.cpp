#include "platform/windows/InstallerMetadataReader.h"

#include <utility>
#include <vector>

#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

namespace companion {
namespace {

struct VersionTranslation final {
    WORD language;
    WORD codePage;
};

CompanionError metadataError(
    QString code,
    QString message,
    QStringView path,
    DWORD win32Error = ERROR_SUCCESS)
{
    QVariantMap context{
        {
            QStringLiteral("path"),
            path.toString(),
        },
    };
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

QString queryVersionString(
    const std::vector<BYTE>& versionInfo,
    const QVector<VersionTranslation>&
        translations,
    QStringView key)
{
    for (const VersionTranslation translation :
         translations) {
        const QString query =
            QStringLiteral(
                "\\StringFileInfo\\%1%2\\%3")
                .arg(
                    translation.language,
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(
                    translation.codePage,
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(key);
        const std::wstring nativeQuery =
            query.toStdWString();
        void* rawValue = nullptr;
        UINT valueCharacters = 0;
        if (!VerQueryValueW(
                versionInfo.data(),
                nativeQuery.c_str(),
                &rawValue,
                &valueCharacters)
            || rawValue == nullptr
            || valueCharacters == 0) {
            continue;
        }

        return QString::fromWCharArray(
                   static_cast<const wchar_t*>(
                       rawValue),
                   int(valueCharacters - 1))
            .trimmed();
    }
    return {};
}

} // namespace

Result<InstallerMetadata>
InstallerMetadataReader::read(
    QStringView path) const
{
    const std::wstring nativePath =
        path.toString().toStdWString();
    DWORD ignoredHandle = 0;
    const DWORD versionBytes =
        GetFileVersionInfoSizeW(
            nativePath.c_str(),
            &ignoredHandle);
    if (versionBytes == 0) {
        return Result<InstallerMetadata>::failure(
            metadataError(
                QStringLiteral(
                    "update.installer_metadata_unavailable"),
                QStringLiteral(
                    "The update installer version metadata is unavailable."),
                path,
                GetLastError()));
    }

    std::vector<BYTE> versionInfo(
        versionBytes);
    if (!GetFileVersionInfoW(
            nativePath.c_str(),
            0,
            versionBytes,
            versionInfo.data())) {
        return Result<InstallerMetadata>::failure(
            metadataError(
                QStringLiteral(
                    "update.installer_metadata_unavailable"),
                QStringLiteral(
                    "The update installer version metadata could not be read."),
                path,
                GetLastError()));
    }

    void* rawTranslations = nullptr;
    UINT translationBytes = 0;
    QVector<VersionTranslation> translations;
    if (VerQueryValueW(
            versionInfo.data(),
            L"\\VarFileInfo\\Translation",
            &rawTranslations,
            &translationBytes)
        && rawTranslations != nullptr
        && translationBytes
            >= sizeof(VersionTranslation)) {
        const auto* values =
            static_cast<const VersionTranslation*>(
                rawTranslations);
        const UINT count =
            translationBytes
            / sizeof(VersionTranslation);
        translations.reserve(int(count));
        for (UINT index = 0;
             index < count;
             ++index) {
            translations.append(values[index]);
        }
    }
    if (translations.isEmpty()) {
        translations.append(
            {0x0409, 1200});
    }

    InstallerMetadata metadata{
        queryVersionString(
            versionInfo,
            translations,
            QStringLiteral("ProductName")),
        queryVersionString(
            versionInfo,
            translations,
            QStringLiteral("ProductVersion")),
        queryVersionString(
            versionInfo,
            translations,
            QStringLiteral("OriginalFilename")),
    };
    if (metadata.productName.isEmpty()
        || metadata.productVersionMarker.isEmpty()
        || metadata.originalFilename.isEmpty()) {
        return Result<InstallerMetadata>::failure(
            metadataError(
                QStringLiteral(
                    "update.installer_metadata_missing"),
                QStringLiteral(
                    "The update installer identity metadata is incomplete."),
                path));
    }

    return Result<InstallerMetadata>::success(
        std::move(metadata));
}

} // namespace companion
