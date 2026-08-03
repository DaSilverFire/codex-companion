#pragma once

#include <QString>
#include <QStringView>
#include <QUuid>

#include <optional>

namespace companion {

struct CodexAccountProfile final {
    QUuid id;
    QString label;

    friend bool operator==(
        const CodexAccountProfile&,
        const CodexAccountProfile&) =
        default;
};

inline QString codexAccountProfileIdString(
    const QUuid& id)
{
    return id.toString(
                 QUuid::WithoutBraces)
        .toLower();
}

inline QString
codexAccountProfileDirectoryName(
    const QUuid& id)
{
    QString result =
        codexAccountProfileIdString(id);
    result.remove(QLatin1Char('-'));
    return result;
}

inline std::optional<QUuid>
parseCodexAccountProfileId(
    QStringView value)
{
    const QUuid id(value.toString());
    if (id.isNull()) {
        return std::nullopt;
    }
    return id;
}

} // namespace companion
