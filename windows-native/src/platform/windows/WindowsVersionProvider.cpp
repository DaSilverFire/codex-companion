#include "platform/windows/WindowsVersionProvider.h"

#include <utility>

#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

namespace companion {
namespace {

using RtlGetVersionFunction =
    LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

CompanionError versionError(
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

Result<WindowsVersion> queryWindowsVersion()
{
    HMODULE module =
        GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
        return Result<WindowsVersion>::failure(
            versionError(
                QStringLiteral(
                    "update.windows_version_unavailable"),
                QStringLiteral(
                    "The Windows version module is unavailable."),
                {
                    {
                        QStringLiteral("win32Error"),
                        QVariant::fromValue<qulonglong>(
                            GetLastError()),
                    },
                }));
    }

    auto* query =
        reinterpret_cast<RtlGetVersionFunction>(
            GetProcAddress(module, "RtlGetVersion"));
    if (query == nullptr) {
        return Result<WindowsVersion>::failure(
            versionError(
                QStringLiteral(
                    "update.windows_version_unavailable"),
                QStringLiteral(
                    "The Windows version API is unavailable."),
                {
                    {
                        QStringLiteral("win32Error"),
                        QVariant::fromValue<qulonglong>(
                            GetLastError()),
                    },
                }));
    }

    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    const LONG status = query(&version);
    if (status < 0) {
        return Result<WindowsVersion>::failure(
            versionError(
                QStringLiteral(
                    "update.windows_version_unavailable"),
                QStringLiteral(
                    "The current Windows version could not be read."),
                {
                    {
                        QStringLiteral("ntstatus"),
                        QVariant::fromValue<qlonglong>(
                            status),
                    },
                }));
    }

    return Result<WindowsVersion>::success(
        {
            version.dwMajorVersion,
            version.dwMinorVersion,
            version.dwBuildNumber,
            0,
        });
}

} // namespace

std::optional<WindowsVersion>
WindowsVersion::parse(
    QStringView value)
{
    const QList<QStringView> parts =
        value.split(
            QLatin1Char('.'),
            Qt::KeepEmptyParts);
    if (parts.size() < 2
        || parts.size() > 4) {
        return std::nullopt;
    }

    WindowsVersion result;
    quint32* components[] = {
        &result.major,
        &result.minor,
        &result.build,
        &result.revision,
    };
    for (qsizetype index = 0;
         index < parts.size();
         ++index) {
        const QStringView part = parts.at(index);
        if (part.isEmpty()) {
            return std::nullopt;
        }
        for (const QChar character : part) {
            if (character < QLatin1Char('0')
                || character > QLatin1Char('9')) {
                return std::nullopt;
            }
        }

        bool valid = false;
        const quint32 parsed =
            part.toUInt(&valid, 10);
        if (!valid) {
            return std::nullopt;
        }
        *components[index] = parsed;
    }
    return result;
}

QString WindowsVersion::toString() const
{
    return QStringLiteral("%1.%2.%3.%4")
        .arg(major)
        .arg(minor)
        .arg(build)
        .arg(revision);
}

WindowsVersionProvider::WindowsVersionProvider()
    : query_(queryWindowsVersion)
{
}

WindowsVersionProvider::WindowsVersionProvider(
    Query query)
    : query_(std::move(query))
{
}

Result<WindowsVersion>
WindowsVersionProvider::current() const
{
    if (!query_) {
        return Result<WindowsVersion>::failure(
            versionError(
                QStringLiteral(
                    "update.windows_version_unavailable"),
                QStringLiteral(
                    "The Windows version query is unavailable.")));
    }
    return query_();
}

} // namespace companion
