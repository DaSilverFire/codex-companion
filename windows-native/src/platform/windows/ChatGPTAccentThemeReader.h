#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace companion {

enum class ChatGPTAccentTheme {
    Blue,
    Green,
    Orange,
    Pink,
    Purple,
    Red,
    Teal,
    Yellow,
};

class ChatGPTAccentThemeReader final {
public:
    static ChatGPTAccentTheme currentTheme();
    static ChatGPTAccentTheme currentThemeInRoots(
        const QString& localAppData,
        const QString& appData);
    static ChatGPTAccentTheme themeInStorageDirectory(
        const QString& directoryPath);
    static std::optional<ChatGPTAccentTheme>
    themeInPayload(const QByteArray& payload);
    static QString colorName(ChatGPTAccentTheme theme);
};

} // namespace companion
