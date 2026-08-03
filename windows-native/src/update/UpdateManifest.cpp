#include "update/UpdateManifest.h"

#include "update/ReleaseVersion.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QTime>
#include <QUrl>

namespace companion {
namespace {

CompanionError manifestError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
    };
}

CompanionError invalidManifest()
{
    return manifestError(
        QStringLiteral("update.invalid_manifest"),
        QStringLiteral(
            "The update manifest could not be decoded."));
}

class JsonDuplicateKeyScanner final {
public:
    explicit JsonDuplicateKeyScanner(QByteArrayView bytes)
        : bytes_(bytes.data(), bytes.size())
    {
    }

    bool scan()
    {
        skipWhitespace();
        if (!parseValue()) {
            return false;
        }
        skipWhitespace();
        return position_ == bytes_.size();
    }

    bool hasDuplicate() const noexcept
    {
        return duplicate_;
    }

private:
    bool parseValue()
    {
        skipWhitespace();
        if (position_ >= bytes_.size()) {
            return false;
        }

        const char current = bytes_.at(position_);
        if (current == '{') {
            return parseObject();
        }
        if (current == '[') {
            return parseArray();
        }
        if (current == '"') {
            return readString(nullptr);
        }
        if (current == '-'
            || (current >= '0' && current <= '9')) {
            return skipNumber();
        }
        return skipLiteral("true")
            || skipLiteral("false")
            || skipLiteral("null");
    }

    bool parseObject()
    {
        ++position_;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        QSet<QString> keys;
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)) {
                return false;
            }
            if (keys.contains(key)) {
                duplicate_ = true;
            }
            keys.insert(std::move(key));

            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            if (!parseValue()) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseArray()
    {
        ++position_;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }

        while (position_ < bytes_.size()) {
            if (!parseValue()) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    static bool isHexDigit(char value)
    {
        return (value >= '0' && value <= '9')
            || (value >= 'a' && value <= 'f')
            || (value >= 'A' && value <= 'F');
    }

    bool readString(QString* value)
    {
        if (position_ >= bytes_.size()
            || bytes_.at(position_) != '"') {
            return false;
        }

        const qsizetype start = position_;
        ++position_;
        while (position_ < bytes_.size()) {
            const char current = bytes_.at(position_++);
            if (current == '\\') {
                if (position_ >= bytes_.size()) {
                    return false;
                }
                const char escape = bytes_.at(position_++);
                if (escape == 'u') {
                    if (position_ + 4 > bytes_.size()) {
                        return false;
                    }
                    for (qsizetype index = 0;
                         index < 4;
                         ++index) {
                        if (!isHexDigit(
                                bytes_.at(position_ + index))) {
                            return false;
                        }
                    }
                    position_ += 4;
                } else if (escape != '"'
                           && escape != '\\'
                           && escape != '/'
                           && escape != 'b'
                           && escape != 'f'
                           && escape != 'n'
                           && escape != 'r'
                           && escape != 't') {
                    return false;
                }
            } else if (current == '"') {
                if (value != nullptr) {
                    QByteArray wrapped("[");
                    wrapped.append(
                        bytes_.mid(
                            start,
                            position_ - start));
                    wrapped.append(']');
                    QJsonParseError error;
                    const QJsonDocument document =
                        QJsonDocument::fromJson(
                            wrapped,
                            &error);
                    if (error.error
                            != QJsonParseError::NoError
                        || !document.isArray()
                        || document.array().size() != 1
                        || !document.array().at(0).isString()) {
                        return false;
                    }
                    *value =
                        document.array().at(0).toString();
                }
                return true;
            } else if (
                static_cast<unsigned char>(current)
                < 0x20U) {
                return false;
            }
        }
        return false;
    }

    bool skipNumber()
    {
        if (consume('-')
            && position_ >= bytes_.size()) {
            return false;
        }
        if (!consume('0')) {
            if (position_ >= bytes_.size()
                || bytes_.at(position_) < '1'
                || bytes_.at(position_) > '9') {
                return false;
            }
            ++position_;
            while (position_ < bytes_.size()
                   && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const qsizetype start = position_;
            while (position_ < bytes_.size()
                   && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        if (position_ < bytes_.size()
            && (bytes_.at(position_) == 'e'
                || bytes_.at(position_) == 'E')) {
            ++position_;
            if (!consume('+')) {
                consume('-');
            }
            const qsizetype start = position_;
            while (position_ < bytes_.size()
                   && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        return true;
    }

    bool skipLiteral(QByteArrayView literal)
    {
        if (bytes_.mid(position_, literal.size())
            != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool consume(char expected)
    {
        if (position_ >= bytes_.size()
            || bytes_.at(position_) != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void skipWhitespace()
    {
        while (position_ < bytes_.size()) {
            const char current = bytes_.at(position_);
            if (current != ' '
                && current != '\t'
                && current != '\r'
                && current != '\n') {
                break;
            }
            ++position_;
        }
    }

    QByteArray bytes_;
    qsizetype position_ = 0;
    bool duplicate_ = false;
};

bool hasExactFields(const QJsonObject& object)
{
    static const std::array expected{
        QStringLiteral("schemaVersion"),
        QStringLiteral("version"),
        QStringLiteral("build"),
        QStringLiteral("minimumSystemVersion"),
        QStringLiteral("publishedAt"),
        QStringLiteral("downloadURL"),
        QStringLiteral("sha256"),
        QStringLiteral("size"),
        QStringLiteral("signature"),
    };
    if (object.size()
        != static_cast<qsizetype>(expected.size())) {
        return false;
    }
    return std::all_of(
        expected.cbegin(),
        expected.cend(),
        [&object](const QString& key) {
            return object.contains(key);
        });
}

std::optional<qint64> integerValue(
    const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    constexpr qint64 lowFallback =
        std::numeric_limits<qint64>::min();
    constexpr qint64 highFallback =
        std::numeric_limits<qint64>::max();
    const qint64 withLowFallback =
        value.toInteger(lowFallback);
    const qint64 withHighFallback =
        value.toInteger(highFallback);
    if (withLowFallback != withHighFallback) {
        return std::nullopt;
    }
    return withLowFallback;
}

bool isAsciiHexDigest(QStringView value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        if (!((character >= QLatin1Char('0')
               && character <= QLatin1Char('9'))
              || (character >= QLatin1Char('a')
                  && character <= QLatin1Char('f'))
              || (character >= QLatin1Char('A')
                  && character <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

bool isRfc3339Utc(QStringView value)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.[0-9]+)?Z$)"));
    const QRegularExpressionMatch match =
        pattern.matchView(value);
    if (!match.hasMatch()) {
        return false;
    }

    const QDate date(
        match.capturedView(1).toInt(),
        match.capturedView(2).toInt(),
        match.capturedView(3).toInt());
    const QTime time(
        match.capturedView(4).toInt(),
        match.capturedView(5).toInt(),
        match.capturedView(6).toInt());
    return date.isValid() && time.isValid();
}

bool isWindowsDottedVersion(QStringView value)
{
    const QStringList components =
        value.toString().split(
            QLatin1Char('.'),
            Qt::KeepEmptyParts);
    if (components.size() < 2
        || components.size() > 4) {
        return false;
    }

    for (const QString& component : components) {
        if (component.isEmpty()) {
            return false;
        }
        for (const QChar character : component) {
            if (character < QLatin1Char('0')
                || character > QLatin1Char('9')) {
                return false;
            }
        }
        bool ok = false;
        const qulonglong number =
            component.toULongLong(&ok, 10);
        if (!ok
            || number
                > std::numeric_limits<quint32>::max()) {
            return false;
        }
    }
    return true;
}

bool isHttpsDownloadUrl(QStringView value)
{
    const QUrl url(
        value.toString(),
        QUrl::StrictMode);
    return url.isValid()
        && !url.isRelative()
        && url.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive) == 0
        && !url.host().isEmpty();
}

bool isAsciiDigit(QChar value)
{
    return value >= QLatin1Char('0')
        && value <= QLatin1Char('9');
}

std::strong_ordering compareNumericCaseInsensitive(
    QStringView left,
    QStringView right)
{
    const QString foldedLeft =
        left.toString().toCaseFolded();
    const QString foldedRight =
        right.toString().toCaseFolded();
    qsizetype leftIndex = 0;
    qsizetype rightIndex = 0;

    while (leftIndex < foldedLeft.size()
           && rightIndex < foldedRight.size()) {
        if (isAsciiDigit(foldedLeft.at(leftIndex))
            && isAsciiDigit(foldedRight.at(rightIndex))) {
            qsizetype leftEnd = leftIndex;
            while (leftEnd < foldedLeft.size()
                   && isAsciiDigit(
                       foldedLeft.at(leftEnd))) {
                ++leftEnd;
            }
            qsizetype rightEnd = rightIndex;
            while (rightEnd < foldedRight.size()
                   && isAsciiDigit(
                       foldedRight.at(rightEnd))) {
                ++rightEnd;
            }

            qsizetype leftSignificant = leftIndex;
            while (leftSignificant < leftEnd
                   && foldedLeft.at(leftSignificant)
                       == QLatin1Char('0')) {
                ++leftSignificant;
            }
            qsizetype rightSignificant = rightIndex;
            while (rightSignificant < rightEnd
                   && foldedRight.at(rightSignificant)
                       == QLatin1Char('0')) {
                ++rightSignificant;
            }
            const qsizetype leftDigits =
                leftEnd - leftSignificant;
            const qsizetype rightDigits =
                rightEnd - rightSignificant;
            if (leftDigits != rightDigits) {
                return leftDigits < rightDigits
                    ? std::strong_ordering::less
                    : std::strong_ordering::greater;
            }
            for (qsizetype offset = 0;
                 offset < leftDigits;
                 ++offset) {
                const QChar leftDigit =
                    foldedLeft.at(leftSignificant + offset);
                const QChar rightDigit =
                    foldedRight.at(
                        rightSignificant + offset);
                if (leftDigit != rightDigit) {
                    return leftDigit < rightDigit
                        ? std::strong_ordering::less
                        : std::strong_ordering::greater;
                }
            }
            leftIndex = leftEnd;
            rightIndex = rightEnd;
            continue;
        }

        const QChar leftCharacter =
            foldedLeft.at(leftIndex);
        const QChar rightCharacter =
            foldedRight.at(rightIndex);
        if (leftCharacter != rightCharacter) {
            return leftCharacter < rightCharacter
                ? std::strong_ordering::less
                : std::strong_ordering::greater;
        }
        ++leftIndex;
        ++rightIndex;
    }

    if (leftIndex == foldedLeft.size()
        && rightIndex == foldedRight.size()) {
        return std::strong_ordering::equal;
    }
    return leftIndex == foldedLeft.size()
        ? std::strong_ordering::less
        : std::strong_ordering::greater;
}

} // namespace

Result<UpdateManifest> UpdateManifest::decode(
    QByteArrayView bytes)
{
    JsonDuplicateKeyScanner scanner(bytes);
    if (!scanner.scan() || scanner.hasDuplicate()) {
        return Result<UpdateManifest>::failure(
            invalidManifest());
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            QByteArray(bytes.data(), bytes.size()),
            &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<UpdateManifest>::failure(
            invalidManifest());
    }

    const QJsonObject object = document.object();
    if (!hasExactFields(object)) {
        return Result<UpdateManifest>::failure(
            invalidManifest());
    }

    const QJsonValue schemaValue =
        object.value(QStringLiteral("schemaVersion"));
    const QJsonValue versionValue =
        object.value(QStringLiteral("version"));
    const QJsonValue buildValue =
        object.value(QStringLiteral("build"));
    const QJsonValue minimumValue =
        object.value(
            QStringLiteral("minimumSystemVersion"));
    const QJsonValue publishedValue =
        object.value(QStringLiteral("publishedAt"));
    const QJsonValue downloadValue =
        object.value(QStringLiteral("downloadURL"));
    const QJsonValue digestValue =
        object.value(QStringLiteral("sha256"));
    const QJsonValue sizeValue =
        object.value(QStringLiteral("size"));
    const QJsonValue signatureValue =
        object.value(QStringLiteral("signature"));

    const std::optional<qint64> schema =
        integerValue(schemaValue);
    const std::optional<qint64> build =
        integerValue(buildValue);
    const std::optional<qint64> size =
        integerValue(sizeValue);
    if (!schema.has_value()
        || *schema < std::numeric_limits<int>::min()
        || *schema > std::numeric_limits<int>::max()
        || !versionValue.isString()
        || !build.has_value()
        || !minimumValue.isString()
        || !publishedValue.isString()
        || !downloadValue.isString()
        || !digestValue.isString()
        || !size.has_value()
        || !signatureValue.isString()) {
        return Result<UpdateManifest>::failure(
            invalidManifest());
    }

    UpdateManifest manifest;
    manifest.schemaVersion =
        static_cast<int>(*schema);
    manifest.version = versionValue.toString();
    manifest.build = *build;
    manifest.minimumSystemVersion =
        minimumValue.toString();
    manifest.publishedAt =
        publishedValue.toString();
    manifest.downloadUrl =
        downloadValue.toString();
    manifest.sha256 =
        digestValue.toString();
    manifest.size = *size;
    manifest.signature =
        signatureValue.toString();

    if (manifest.schemaVersion != 1) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral(
                    "update.unsupported_schema"),
                QStringLiteral(
                    "The update manifest schema is unsupported.")));
    }
    if (manifest.build <= 0) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral("update.invalid_build"),
                QStringLiteral(
                    "The update manifest contains an invalid build number.")));
    }
    if (!isWindowsDottedVersion(
            manifest.minimumSystemVersion)) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral(
                    "update.invalid_minimum_system_version"),
                QStringLiteral(
                    "The update manifest contains an invalid minimum Windows version.")));
    }
    if (!isRfc3339Utc(manifest.publishedAt)) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral(
                    "update.invalid_published_at"),
                QStringLiteral(
                    "The update manifest contains an invalid publication time.")));
    }
    if (!isHttpsDownloadUrl(manifest.downloadUrl)) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral(
                    "update.insecure_download_url"),
                QStringLiteral(
                    "The update download must use HTTPS.")));
    }
    if (!isAsciiHexDigest(manifest.sha256)) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral("update.invalid_digest"),
                QStringLiteral(
                    "The update manifest contains an invalid SHA-256 digest.")));
    }
    if (manifest.size <= 0
        || manifest.size > maximumSignedSize) {
        return Result<UpdateManifest>::failure(
            manifestError(
                QStringLiteral("update.invalid_size"),
                QStringLiteral(
                    "The update manifest contains an invalid installer size.")));
    }

    return Result<UpdateManifest>::success(
        std::move(manifest));
}

QByteArray UpdateManifest::canonicalPayload() const
{
    QList<QByteArray> lines{
        QByteArray::number(schemaVersion),
        version.toUtf8(),
        QByteArray::number(build),
        minimumSystemVersion.toUtf8(),
        publishedAt.toUtf8(),
        downloadUrl.toUtf8(),
        sha256.toLower().toUtf8(),
        QByteArray::number(size),
    };
    return QByteArrayList(std::move(lines)).join('\n');
}

bool UpdateManifest::isNewerThan(
    QStringView currentVersion,
    qint64 currentBuild) const
{
    std::strong_ordering ordering;
    const auto release =
        ReleaseVersion::parse(version);
    const auto installed =
        ReleaseVersion::parse(currentVersion);
    if (release.has_value()
        && installed.has_value()) {
        ordering = *release <=> *installed;
    } else {
        ordering = compareNumericCaseInsensitive(
            version,
            currentVersion);
    }

    if (ordering == std::strong_ordering::greater) {
        return true;
    }
    if (ordering == std::strong_ordering::less) {
        return false;
    }
    return build > currentBuild;
}

} // namespace companion
