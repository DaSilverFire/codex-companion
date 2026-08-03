#pragma once

#include <compare>
#include <optional>

#include <QString>
#include <QStringView>
#include <QVector>

namespace companion {

class ReleaseVersion final {
public:
    static std::optional<ReleaseVersion> parse(
        QStringView value);

    friend std::strong_ordering operator<=>(
        const ReleaseVersion& left,
        const ReleaseVersion& right) noexcept;
    friend bool operator==(
        const ReleaseVersion& left,
        const ReleaseVersion& right) noexcept;

private:
    struct Identifier final {
        std::optional<qint64> number;
        QString text;
    };

    QVector<qint64> core_;
    std::optional<QVector<Identifier>> prerelease_;
};

} // namespace companion
