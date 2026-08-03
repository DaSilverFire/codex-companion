#include "update/ReleaseVersion.h"

#include <algorithm>

namespace companion {
namespace {

bool isAsciiDigits(QStringView value)
{
    if (value.isEmpty()) {
        return false;
    }
    for (const QChar character : value) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ReleaseVersion> ReleaseVersion::parse(
    QStringView value)
{
    const QString original = value.toString();
    const qsizetype metadataSeparator =
        original.indexOf(QLatin1Char('+'));
    const QString withoutMetadata =
        metadataSeparator < 0
        ? original
        : original.left(metadataSeparator);

    const qsizetype prereleaseSeparator =
        withoutMetadata.indexOf(QLatin1Char('-'));
    const QString coreText =
        prereleaseSeparator < 0
        ? withoutMetadata
        : withoutMetadata.left(prereleaseSeparator);
    const QString prereleaseText =
        prereleaseSeparator < 0
        ? QString()
        : withoutMetadata.sliced(prereleaseSeparator + 1);

    const QStringList coreParts =
        coreText.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (coreParts.size() < 2) {
        return std::nullopt;
    }

    ReleaseVersion parsed;
    parsed.core_.reserve(coreParts.size());
    for (const QString& part : coreParts) {
        if (!isAsciiDigits(part)) {
            return std::nullopt;
        }
        bool ok = false;
        const qint64 component = part.toLongLong(&ok, 10);
        if (!ok) {
            return std::nullopt;
        }
        parsed.core_.append(component);
    }

    if (prereleaseSeparator < 0) {
        parsed.prerelease_ = std::nullopt;
        return parsed;
    }

    const QStringList parts =
        prereleaseText.split(
            QLatin1Char('.'),
            Qt::KeepEmptyParts);
    if (parts.isEmpty()) {
        return std::nullopt;
    }

    QVector<Identifier> identifiers;
    identifiers.reserve(parts.size());
    for (const QString& part : parts) {
        if (part.isEmpty()) {
            return std::nullopt;
        }

        Identifier identifier;
        if (isAsciiDigits(part)) {
            bool ok = false;
            const qint64 number = part.toLongLong(&ok, 10);
            if (ok) {
                identifier.number = number;
            }
        }
        identifier.text = part;
        identifiers.append(std::move(identifier));
    }
    parsed.prerelease_ = std::move(identifiers);
    return parsed;
}

std::strong_ordering operator<=>(
    const ReleaseVersion& left,
    const ReleaseVersion& right) noexcept
{
    const auto compareIdentifiers = [](
        const ReleaseVersion::Identifier& leftIdentifier,
        const ReleaseVersion::Identifier& rightIdentifier) {
        if (leftIdentifier.number.has_value()
            && rightIdentifier.number.has_value()) {
            return *leftIdentifier.number
                <=> *rightIdentifier.number;
        }
        if (leftIdentifier.number.has_value()) {
            return std::strong_ordering::less;
        }
        if (rightIdentifier.number.has_value()) {
            return std::strong_ordering::greater;
        }
        const int comparison = QString::compare(
            leftIdentifier.text,
            rightIdentifier.text,
            Qt::CaseSensitive);
        if (comparison < 0) {
            return std::strong_ordering::less;
        }
        if (comparison > 0) {
            return std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    };

    const qsizetype componentCount =
        std::max(left.core_.size(), right.core_.size());
    for (qsizetype index = 0;
         index < componentCount;
         ++index) {
        const qint64 leftComponent =
            index < left.core_.size()
            ? left.core_.at(index)
            : 0;
        const qint64 rightComponent =
            index < right.core_.size()
            ? right.core_.at(index)
            : 0;
        if (leftComponent != rightComponent) {
            return leftComponent <=> rightComponent;
        }
    }

    if (!left.prerelease_.has_value()
        && !right.prerelease_.has_value()) {
        return std::strong_ordering::equal;
    }
    if (left.prerelease_.has_value()
        && !right.prerelease_.has_value()) {
        return std::strong_ordering::less;
    }
    if (!left.prerelease_.has_value()
        && right.prerelease_.has_value()) {
        return std::strong_ordering::greater;
    }

    const QVector<ReleaseVersion::Identifier>& leftParts =
        *left.prerelease_;
    const QVector<ReleaseVersion::Identifier>& rightParts =
        *right.prerelease_;
    const qsizetype sharedCount =
        std::min(leftParts.size(), rightParts.size());
    for (qsizetype index = 0;
         index < sharedCount;
         ++index) {
        const std::strong_ordering comparison =
            compareIdentifiers(
                leftParts.at(index),
                rightParts.at(index));
        if (comparison != std::strong_ordering::equal) {
            return comparison;
        }
    }
    return leftParts.size() <=> rightParts.size();
}

bool operator==(
    const ReleaseVersion& left,
    const ReleaseVersion& right) noexcept
{
    return (left <=> right) == std::strong_ordering::equal;
}

} // namespace companion
