#include "platform/windows/ChatGPTAccentThemeReader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace {

using companion::ChatGPTAccentTheme;

struct ThemeToken {
    QByteArrayView token;
    ChatGPTAccentTheme theme;
};

struct ThemeKeyMarker {
    QByteArrayView token;
    qsizetype valueSearchWindow;
    bool requiresAppsContext;
};

constexpr std::array<ThemeToken, 8> kThemeTokens{{
    {"\"blue\"", ChatGPTAccentTheme::Blue},
    {"\"green\"", ChatGPTAccentTheme::Green},
    {"\"orange\"", ChatGPTAccentTheme::Orange},
    {"\"pink\"", ChatGPTAccentTheme::Pink},
    {"\"purple\"", ChatGPTAccentTheme::Purple},
    {"\"red\"", ChatGPTAccentTheme::Red},
    {"\"teal\"", ChatGPTAccentTheme::Teal},
    {"\"yellow\"", ChatGPTAccentTheme::Yellow},
}};

constexpr std::array<ThemeKeyMarker, 2>
    kThemeKeyMarkers{{
        {"chattheme/", 1024, false},
        {"theme/user-", 256, true},
    }};

constexpr QByteArrayView kAppsContext("/apps/");
constexpr qsizetype kAppsContextLookBehind = 96;
constexpr qsizetype kReadChunkSize = 1024 * 1024;
constexpr qsizetype kReadOverlap = 2048;

struct StorageCandidate {
    QString path;
    QDateTime lastWrite;
};

bool hasAppsContext(
    const QByteArray& normalized,
    qsizetype markerOffset)
{
    const qsizetype contextOffset =
        normalized.lastIndexOf(
            kAppsContext,
            markerOffset - 1);
    return contextOffset >= 0
        && contextOffset
            >= std::max(
                qsizetype{0},
                markerOffset
                    - kAppsContextLookBehind);
}

qsizetype lastValidMarkerOffset(
    const QByteArray& normalized,
    const ThemeKeyMarker& marker,
    qsizetype from)
{
    qsizetype markerOffset =
        normalized.lastIndexOf(marker.token, from);
    while (markerOffset >= 0
           && marker.requiresAppsContext
           && !hasAppsContext(
               normalized,
               markerOffset)) {
        if (markerOffset == 0) {
            return -1;
        }
        markerOffset = normalized.lastIndexOf(
            marker.token,
            markerOffset - 1);
    }
    return markerOffset;
}

std::optional<ChatGPTAccentTheme>
nearestThemeAfter(
    const QByteArray& normalized,
    qsizetype searchStart,
    qsizetype searchWindow)
{
    const qsizetype searchEnd =
        std::min(
            normalized.size(),
            searchStart + searchWindow);
    qsizetype nearestOffset =
        std::numeric_limits<qsizetype>::max();
    std::optional<ChatGPTAccentTheme> nearestTheme;

    const qsizetype defaultOffset =
        normalized.indexOf(
            QByteArrayView("\"default\""),
            searchStart);
    if (defaultOffset >= 0
        && defaultOffset < searchEnd) {
        nearestOffset = defaultOffset;
        nearestTheme = ChatGPTAccentTheme::Blue;
    }

    for (const ThemeToken& candidate : kThemeTokens) {
        const qsizetype valueOffset =
            normalized.indexOf(
                candidate.token,
                searchStart);
        if (valueOffset >= 0
            && valueOffset < searchEnd
            && valueOffset < nearestOffset) {
            nearestOffset = valueOffset;
            nearestTheme = candidate.theme;
        }
    }
    return nearestTheme;
}

std::optional<ChatGPTAccentTheme> themeInFile(
    const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QByteArray overlap;
    std::optional<ChatGPTAccentTheme> result;
    while (!file.atEnd()) {
        QByteArray payload = overlap;
        payload.append(file.read(kReadChunkSize));
        if (const auto theme =
                companion::ChatGPTAccentThemeReader::
                    themeInPayload(payload)) {
            result = theme;
        }
        overlap = payload.right(kReadOverlap);
    }
    return result;
}

std::optional<ChatGPTAccentTheme>
themeInStorageDirectoryIfPresent(
    const QString& directoryPath)
{
    const QFileInfoList files =
        QDir(directoryPath).entryInfoList(
            {
                QStringLiteral("*.log"),
                QStringLiteral("*.ldb"),
            },
            QDir::Files | QDir::Readable,
            QDir::Time);
    for (const QFileInfo& file : files) {
        if (const auto theme =
                themeInFile(
                    file.absoluteFilePath())) {
            return theme;
        }
    }
    return std::nullopt;
}

QDateTime latestStorageWrite(
    const QString& directoryPath)
{
    const QFileInfoList files =
        QDir(directoryPath).entryInfoList(
            {
                QStringLiteral("*.log"),
                QStringLiteral("*.ldb"),
            },
            QDir::Files | QDir::Readable,
            QDir::Time);
    return files.isEmpty()
        ? QFileInfo(directoryPath).lastModified()
        : files.constFirst().lastModified();
}

QStringList candidateStorageDirectories(
    const QString& localAppData,
    const QString& appData)
{
    QList<StorageCandidate> candidates;
    QSet<QString> seen;
    const auto appendCandidate =
        [&candidates, &seen](const QString& path) {
            const QString normalized =
                QDir::cleanPath(path);
            const QString key =
                normalized.toCaseFolded();
            if (seen.contains(key)
                || !QDir(normalized).exists()) {
                return;
            }
            seen.insert(key);
            candidates.append({
                normalized,
                latestStorageWrite(normalized),
            });
        };

    if (!localAppData.isEmpty()) {
        const QDir packages(
            QDir(localAppData).filePath(
                QStringLiteral("Packages")));
        const QFileInfoList installations =
            packages.entryInfoList(
                {
                    QStringLiteral(
                        "OpenAI.Codex_*"),
                    QStringLiteral(
                        "OpenAI.CodexBeta_*"),
                    QStringLiteral(
                        "OpenAI.ChatGPT-Desktop_*"),
                },
                QDir::Dirs | QDir::NoDotAndDotDot,
                QDir::Time);
        for (const QFileInfo& installation :
             installations) {
            const QDir packageRoot(
                installation.absoluteFilePath());
            if (installation.fileName().startsWith(
                    QStringLiteral(
                        "OpenAI.ChatGPT-Desktop_"),
                    Qt::CaseInsensitive)) {
                appendCandidate(
                    packageRoot.filePath(
                        QStringLiteral(
                            "LocalCache/Roaming/ChatGPT/"
                            "Local Storage/leveldb")));
                continue;
            }

            const QStringList codexStoragePaths{
                QStringLiteral(
                    "LocalCache/Roaming/Codex/"
                    "Local Storage/leveldb"),
                QStringLiteral(
                    "LocalCache/Roaming/Codex/web/"
                    "Codex/Default/Local Storage/"
                    "leveldb"),
                QStringLiteral(
                    "LocalCache/Roaming/Codex/web/"
                    "Codex/Default/Partitions/"
                    "codex-browser-app/Local Storage/"
                    "leveldb"),
            };
            for (const QString& relativePath :
                 codexStoragePaths) {
                appendCandidate(
                    packageRoot.filePath(
                        relativePath));
            }
        }
    }

    if (!appData.isEmpty()) {
        appendCandidate(
            QDir(appData).filePath(
                QStringLiteral(
                    "ChatGPT/Local Storage/leveldb")));
        appendCandidate(
            QDir(appData).filePath(
                QStringLiteral(
                    "OpenAI/ChatGPT/Local Storage/"
                    "leveldb")));
        appendCandidate(
            QDir(appData).filePath(
                QStringLiteral(
                    "Codex/Local Storage/leveldb")));
        appendCandidate(
            QDir(appData).filePath(
                QStringLiteral(
                    "OpenAI/Codex/Local Storage/"
                    "leveldb")));
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const StorageCandidate& lhs,
           const StorageCandidate& rhs) {
            if (lhs.lastWrite == rhs.lastWrite) {
                return lhs.path < rhs.path;
            }
            return lhs.lastWrite > rhs.lastWrite;
        });

    QStringList paths;
    paths.reserve(candidates.size());
    for (const StorageCandidate& candidate :
         std::as_const(candidates)) {
        paths.append(candidate.path);
    }
    return paths;
}

} // namespace

namespace companion {

ChatGPTAccentTheme
ChatGPTAccentThemeReader::currentTheme()
{
    return currentThemeInRoots(
        qEnvironmentVariable("LOCALAPPDATA"),
        qEnvironmentVariable("APPDATA"));
}

ChatGPTAccentTheme
ChatGPTAccentThemeReader::currentThemeInRoots(
    const QString& localAppData,
    const QString& appData)
{
    for (const QString& directory :
         candidateStorageDirectories(
             localAppData,
             appData)) {
        if (const auto theme =
                themeInStorageDirectoryIfPresent(
                    directory)) {
            return *theme;
        }
    }
    return ChatGPTAccentTheme::Blue;
}

ChatGPTAccentTheme
ChatGPTAccentThemeReader::themeInStorageDirectory(
    const QString& directoryPath)
{
    return themeInStorageDirectoryIfPresent(
               directoryPath)
        .value_or(ChatGPTAccentTheme::Blue);
}

std::optional<ChatGPTAccentTheme>
ChatGPTAccentThemeReader::themeInPayload(
    const QByteArray& payload)
{
    const QByteArray normalized = payload.toLower();
    qsizetype searchBefore = normalized.size() - 1;
    while (searchBefore >= 0) {
        const ThemeKeyMarker* latestMarker = nullptr;
        qsizetype latestMarkerOffset = -1;
        for (const ThemeKeyMarker& marker :
             kThemeKeyMarkers) {
            const qsizetype markerOffset =
                lastValidMarkerOffset(
                    normalized,
                    marker,
                    searchBefore);
            if (markerOffset > latestMarkerOffset) {
                latestMarker = &marker;
                latestMarkerOffset = markerOffset;
            }
        }

        if (latestMarker == nullptr) {
            return std::nullopt;
        }

        if (const auto theme = nearestThemeAfter(
                normalized,
                latestMarkerOffset
                    + latestMarker->token.size(),
                latestMarker->valueSearchWindow)) {
            return theme;
        }
        searchBefore = latestMarkerOffset - 1;
    }
    return std::nullopt;
}

QString ChatGPTAccentThemeReader::colorName(
    ChatGPTAccentTheme theme)
{
    switch (theme) {
    case ChatGPTAccentTheme::Blue:
        return QStringLiteral("#297af5");
    case ChatGPTAccentTheme::Green:
        return QStringLiteral("#21ad66");
    case ChatGPTAccentTheme::Orange:
        return QStringLiteral("#ff6e14");
    case ChatGPTAccentTheme::Pink:
        return QStringLiteral("#f04f94");
    case ChatGPTAccentTheme::Purple:
        return QStringLiteral("#965ceb");
    case ChatGPTAccentTheme::Red:
        return QStringLiteral("#eb3d40");
    case ChatGPTAccentTheme::Teal:
        return QStringLiteral("#1aabab");
    case ChatGPTAccentTheme::Yellow:
        return QStringLiteral("#edb324");
    }
    return QStringLiteral("#297af5");
}

} // namespace companion
