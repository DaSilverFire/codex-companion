#include "codex/models/BridgeJsonCodec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaType>
#include <QSet>
#include <QVariant>

namespace companion {
namespace {

constexpr double kSwiftReferenceDateUnixSeconds = 978307200.0;

CompanionError invalidJson(QString message, qsizetype offset = 0)
{
    return {
        QStringLiteral("bridge.invalid_json"),
        std::move(message),
        false,
        {{QStringLiteral("offset"), offset}},
    };
}

CompanionError invalidField(QString path, QString detail = QString())
{
    if (detail.isEmpty()) {
        detail = QStringLiteral("Invalid bridge field");
    }
    return {
        QStringLiteral("bridge.invalid_field"),
        detail + QStringLiteral(": ") + path,
        false,
        {{QStringLiteral("path"), path}},
    };
}

QString childPath(const QString& path, QStringView key)
{
    return path.isEmpty() ? key.toString() : path + QLatin1Char('.') + key;
}

QString rawNumberChildPath(const QString& path, QStringView key)
{
    QString escaped;
    escaped.reserve(key.size());
    for (const QChar character : key) {
        switch (character.unicode()) {
        case '%': escaped += QStringLiteral("%25"); break;
        case '.': escaped += QStringLiteral("%2E"); break;
        case '[': escaped += QStringLiteral("%5B"); break;
        case ']': escaped += QStringLiteral("%5D"); break;
        default: escaped += character; break;
        }
    }
    return childPath(path, escaped);
}

class RawNumberIndex final {
public:
    explicit RawNumberIndex(QByteArrayView bytes) : bytes_(bytes.data(), bytes.size()) {}

    bool index()
    {
        skipWhitespace();
        if (!parseValue({}, true, &root_)) {
            return false;
        }
        skipWhitespace();
        return position_ == bytes_.size() && root_.isObject();
    }

    QByteArray token(const QString& path) const { return numbers_.value(path); }
    QJsonObject rootObject() const { return root_.toObject(); }

private:
    bool parseValue(
        const QString& path,
        bool captureNumber,
        QJsonValue* parsedValue)
    {
        skipWhitespace();
        if (position_ >= bytes_.size()) {
            return false;
        }

        const char current = bytes_.at(position_);
        if (current == '{') {
            return parseObject(path, captureNumber, parsedValue);
        }
        if (current == '[') {
            return parseArray(path, captureNumber, parsedValue);
        }
        if (current == '"') {
            QString value;
            if (!readString(&value)) {
                return false;
            }
            if (parsedValue != nullptr) {
                *parsedValue = value;
            }
            return true;
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            const int start = position_;
            if (!skipNumber()) {
                return false;
            }
            const QByteArray token = bytes_.mid(start, position_ - start);
            if (captureNumber) {
                numbers_.insert(path, token);
            }
            if (parsedValue != nullptr) {
                QByteArray wrapped("[");
                wrapped.append(token);
                wrapped.append(']');
                const QJsonDocument document = QJsonDocument::fromJson(wrapped);
                if (!document.isArray() || document.array().size() != 1
                    || !document.array().at(0).isDouble()) {
                    return false;
                }
                *parsedValue = document.array().at(0);
            }
            return true;
        }
        if (skipLiteral("true")) {
            if (parsedValue != nullptr) {
                *parsedValue = true;
            }
            return true;
        }
        if (skipLiteral("false")) {
            if (parsedValue != nullptr) {
                *parsedValue = false;
            }
            return true;
        }
        if (skipLiteral("null")) {
            if (parsedValue != nullptr) {
                *parsedValue = QJsonValue(QJsonValue::Null);
            }
            return true;
        }
        return false;
    }

    bool parseObject(
        const QString& path,
        bool captureNumbers,
        QJsonValue* parsedValue)
    {
        ++position_;
        skipWhitespace();
        if (consume('}')) {
            if (parsedValue != nullptr) {
                *parsedValue = QJsonObject{};
            }
            return true;
        }

        QJsonObject object;
        QSet<QString> seenKeys;
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            const bool firstOccurrence = !seenKeys.contains(key);
            seenKeys.insert(key);
            QJsonValue value;
            if (!parseValue(
                    rawNumberChildPath(path, key),
                    captureNumbers && firstOccurrence,
                    firstOccurrence && parsedValue != nullptr ? &value : nullptr)) {
                return false;
            }
            if (firstOccurrence && parsedValue != nullptr) {
                object.insert(key, value);
            }
            skipWhitespace();
            if (consume('}')) {
                if (parsedValue != nullptr) {
                    *parsedValue = object;
                }
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(
        const QString& path,
        bool captureNumbers,
        QJsonValue* parsedValue)
    {
        ++position_;
        skipWhitespace();
        if (consume(']')) {
            if (parsedValue != nullptr) {
                *parsedValue = QJsonArray{};
            }
            return true;
        }

        QJsonArray array;
        qsizetype index = 0;
        while (position_ < bytes_.size()) {
            QJsonValue value;
            if (!parseValue(
                    path + QLatin1Char('[') + QString::number(index)
                        + QLatin1Char(']'),
                    captureNumbers,
                    parsedValue != nullptr ? &value : nullptr)) {
                return false;
            }
            if (parsedValue != nullptr) {
                array.append(value);
            }
            ++index;
            skipWhitespace();
            if (consume(']')) {
                if (parsedValue != nullptr) {
                    *parsedValue = array;
                }
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
        if (position_ >= bytes_.size() || bytes_.at(position_) != '"') {
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
                    for (qsizetype index = 0; index < 4; ++index) {
                        if (!isHexDigit(bytes_.at(position_ + index))) {
                            return false;
                        }
                    }
                    position_ += 4;
                } else if (escape != '"' && escape != '\\' && escape != '/'
                           && escape != 'b' && escape != 'f' && escape != 'n'
                           && escape != 'r' && escape != 't') {
                    return false;
                }
            } else if (current == '"') {
                if (value != nullptr) {
                    QByteArray wrapped("[");
                    wrapped.append(bytes_.mid(start, position_ - start));
                    wrapped.append(']');

                    QJsonParseError error;
                    const QJsonDocument document =
                        QJsonDocument::fromJson(wrapped, &error);
                    if (error.error != QJsonParseError::NoError
                        || !document.isArray() || document.array().size() != 1
                        || !document.array().at(0).isString()) {
                        return false;
                    }
                    *value = document.array().at(0).toString();
                }
                return true;
            } else if (static_cast<unsigned char>(current) < 0x20U) {
                return false;
            }
        }
        return false;
    }

    bool skipString()
    {
        return readString(nullptr);
    }

    bool skipNumber()
    {
        if (consume('-') && position_ >= bytes_.size()) {
            return false;
        }
        if (consume('0')) {
        } else {
            if (position_ >= bytes_.size() || bytes_.at(position_) < '1'
                || bytes_.at(position_) > '9') {
                return false;
            }
            ++position_;
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const int fractionalStart = position_;
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == fractionalStart) {
                return false;
            }
        }
        if (position_ < bytes_.size()
            && (bytes_.at(position_) == 'e' || bytes_.at(position_) == 'E')) {
            ++position_;
            consume('+');
            consume('-');
            const int exponentStart = position_;
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0'
                   && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == exponentStart) {
                return false;
            }
        }
        return true;
    }

    bool skipLiteral(const char* literal)
    {
        const QByteArrayView candidate(literal);
        if (bytes_.mid(position_, candidate.size()) != candidate) {
            return false;
        }
        position_ += candidate.size();
        return true;
    }

    bool consume(char expected)
    {
        if (position_ < bytes_.size() && bytes_.at(position_) == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (position_ < bytes_.size()
               && (bytes_.at(position_) == ' ' || bytes_.at(position_) == '\n'
                   || bytes_.at(position_) == '\r' || bytes_.at(position_) == '\t')) {
            ++position_;
        }
    }

    QByteArray bytes_;
    qsizetype position_ = 0;
    QHash<QString, QByteArray> numbers_;
    QJsonValue root_;
};

struct DecodeContext final {
    BridgeWireProfile profile;
    const RawNumberIndex& rawNumbers;
};

class SwiftDecimalMantissa final {
public:
    bool multiplyBy10AndAdd(quint16 digit)
    {
        SwiftDecimalMantissa candidate = *this;
        quint32 carry = digit;
        for (quint16& limb : candidate.limbs_) {
            const quint32 value = quint32(limb) * 10U + carry;
            limb = quint16(value & 0xFFFFU);
            carry = value >> 16U;
        }
        if (carry != 0) {
            return false;
        }
        *this = candidate;
        return true;
    }

    quint16 divideBy10()
    {
        quint32 remainder = 0;
        for (qsizetype index = qsizetype(limbs_.size()) - 1;
             index >= 0;
             --index) {
            const quint32 value = (remainder << 16U) + limbs_.at(index);
            limbs_.at(index) = quint16(value / 10U);
            remainder = value % 10U;
        }
        return quint16(remainder);
    }

    void compact(int& exponent)
    {
        while (!isZero() && exponent < 127) {
            SwiftDecimalMantissa divided = *this;
            if (divided.divideBy10() != 0) {
                return;
            }
            *this = divided;
            ++exponent;
        }
    }

    std::optional<quint64> toUInt64() const
    {
        quint64 value = 0;
        for (qsizetype index = qsizetype(limbs_.size()) - 1;
             index >= 0;
             --index) {
            const quint64 limb = limbs_.at(index);
            if (value > (std::numeric_limits<quint64>::max() - limb) / 65536U) {
                return std::nullopt;
            }
            value = value * 65536U + limb;
        }
        return value;
    }

private:
    bool isZero() const
    {
        return std::all_of(
            limbs_.cbegin(), limbs_.cend(),
            [](quint16 limb) { return limb == 0; });
    }

    std::array<quint16, 8> limbs_{};
};

std::optional<qint64> parseSwiftDecimalInteger(const QByteArray& token)
{
    qsizetype position = 0;
    bool negative = false;
    if (token.at(position) == '-') {
        negative = true;
        ++position;
    }

    SwiftDecimalMantissa mantissa;
    int exponent = 0;
    bool tooBigToFit = false;
    const auto incrementExponent = [&exponent]() {
        if (exponent == 127) {
            return false;
        }
        ++exponent;
        return true;
    };

    while (position < token.size()
           && token.at(position) >= '0' && token.at(position) <= '9') {
        const quint16 digit = quint16(token.at(position) - '0');
        if (tooBigToFit) {
            if (!incrementExponent()) {
                return std::nullopt;
            }
        } else if (!mantissa.multiplyBy10AndAdd(digit)) {
            tooBigToFit = true;
            if (!incrementExponent()) {
                return std::nullopt;
            }
        }
        ++position;
    }

    if (position < token.size() && token.at(position) == '.') {
        ++position;
        while (position < token.size()
               && token.at(position) >= '0' && token.at(position) <= '9') {
            if (!tooBigToFit) {
                const quint16 digit = quint16(token.at(position) - '0');
                if (!mantissa.multiplyBy10AndAdd(digit)) {
                    tooBigToFit = true;
                } else {
                    if (exponent == -128) {
                        return std::nullopt;
                    }
                    --exponent;
                }
            }
            ++position;
        }
    }

    if (position < token.size()
        && (token.at(position) == 'e' || token.at(position) == 'E')) {
        ++position;
        bool negativeExponent = false;
        if (position < token.size()
            && (token.at(position) == '+' || token.at(position) == '-')) {
            negativeExponent = token.at(position) == '-';
            ++position;
        }
        int explicitExponent = 0;
        while (position < token.size()
               && token.at(position) >= '0' && token.at(position) <= '9') {
            explicitExponent =
                explicitExponent * 10 + int(token.at(position) - '0');
            if (explicitExponent > 254) {
                return std::nullopt;
            }
            ++position;
        }
        if (negativeExponent) {
            explicitExponent = -explicitExponent;
        }
        const int combinedExponent = exponent + explicitExponent;
        if (combinedExponent < -128 || combinedExponent > 127) {
            return std::nullopt;
        }
        exponent = combinedExponent;
    }

    if (position != token.size()) {
        return std::nullopt;
    }

    mantissa.compact(exponent);
    auto magnitude = mantissa.toUInt64();
    if (!magnitude.has_value()) {
        return std::nullopt;
    }
    if (exponent < 0) {
        for (int index = 0; index < -exponent; ++index) {
            *magnitude /= 10U;
        }
    } else {
        for (int index = 0; index < exponent; ++index) {
            if (*magnitude > std::numeric_limits<quint64>::max() / 10U) {
                return std::nullopt;
            }
            *magnitude *= 10U;
        }
    }

    constexpr quint64 kPositiveLimit =
        quint64(std::numeric_limits<qint64>::max());
    if (*magnitude > kPositiveLimit) {
        return std::nullopt;
    }
    const qint64 signedMagnitude = qint64(*magnitude);
    return negative ? -signedMagnitude : signedMagnitude;
}

std::optional<qint64> parseSwiftIntegerToken(
    const QByteArray& token,
    double projectedValue)
{
    if (token.isEmpty()) {
        return std::nullopt;
    }

    const bool usesFloatingSyntax =
        token.contains('.') || token.contains('e') || token.contains('E');
    if (usesFloatingSyntax) {
        constexpr double kExclusiveInt64Limit = 9223372036854775808.0;
        if (!std::isfinite(projectedValue)
            || projectedValue < -kExclusiveInt64Limit
            || projectedValue >= kExclusiveInt64Limit
            || std::trunc(projectedValue) != projectedValue) {
            return std::nullopt;
        }
        const qint64 projectedInteger = static_cast<qint64>(projectedValue);
        constexpr double kExactDoubleIntegerLimit = 9007199254740992.0;
        if (std::abs(projectedValue) < kExactDoubleIntegerLimit) {
            return projectedInteger;
        }
        return parseSwiftDecimalInteger(token);
    }

    qsizetype position = 0;
    bool negative = false;
    if (token.at(position) == '-') {
        negative = true;
        ++position;
        if (position == token.size()) {
            return std::nullopt;
        }
    }

    QByteArray digits;
    if (token.at(position) == '0') {
        digits.append('0');
        ++position;
    } else {
        if (token.at(position) < '1' || token.at(position) > '9') {
            return std::nullopt;
        }
        while (position < token.size()
               && token.at(position) >= '0' && token.at(position) <= '9') {
            digits.append(token.at(position++));
        }
    }

    qint64 fractionalDigits = 0;
    if (position < token.size() && token.at(position) == '.') {
        ++position;
        const qsizetype fractionalStart = position;
        while (position < token.size()
               && token.at(position) >= '0' && token.at(position) <= '9') {
            digits.append(token.at(position++));
            ++fractionalDigits;
        }
        if (position == fractionalStart) {
            return std::nullopt;
        }
    }

    qint64 exponent = 0;
    bool negativeExponent = false;
    if (position < token.size()
        && (token.at(position) == 'e' || token.at(position) == 'E')) {
        ++position;
        if (position < token.size()
            && (token.at(position) == '+' || token.at(position) == '-')) {
            negativeExponent = token.at(position) == '-';
            ++position;
        }
        const qsizetype exponentStart = position;
        while (position < token.size()
               && token.at(position) >= '0' && token.at(position) <= '9') {
            constexpr qint64 kExponentSaturation = 1'000'000;
            if (exponent < kExponentSaturation) {
                exponent = std::min(
                    kExponentSaturation,
                    exponent * 10 + qint64(token.at(position) - '0'));
            }
            ++position;
        }
        if (position == exponentStart) {
            return std::nullopt;
        }
    }

    if (position != token.size()) {
        return std::nullopt;
    }

    qsizetype firstNonzero = 0;
    while (firstNonzero < digits.size() && digits.at(firstNonzero) == '0') {
        ++firstNonzero;
    }
    if (firstNonzero == digits.size()) {
        return qint64{0};
    }
    digits.remove(0, firstNonzero);

    qint64 scale = (negativeExponent ? -exponent : exponent) - fractionalDigits;
    if (scale < 0) {
        const qint64 digitsToRemove = -scale;
        if (digitsToRemove > digits.size()) {
            return std::nullopt;
        }
        for (qint64 index = 0; index < digitsToRemove; ++index) {
            if (digits.at(digits.size() - 1 - index) != '0') {
                return std::nullopt;
            }
        }
        digits.chop(qsizetype(digitsToRemove));
        scale = 0;
    }

    firstNonzero = 0;
    while (firstNonzero < digits.size() && digits.at(firstNonzero) == '0') {
        ++firstNonzero;
    }
    if (firstNonzero == digits.size()) {
        return qint64{0};
    }
    digits.remove(0, firstNonzero);

    if (scale > 19 || digits.size() + scale > 19) {
        return std::nullopt;
    }

    constexpr quint64 kPositiveLimit =
        quint64(std::numeric_limits<qint64>::max());
    constexpr quint64 kNegativeLimit = kPositiveLimit + 1;
    const quint64 limit = negative ? kNegativeLimit : kPositiveLimit;

    quint64 magnitude = 0;
    const auto appendDigit = [&](quint64 digit) -> bool {
        if (magnitude > (limit - digit) / 10) {
            return false;
        }
        magnitude = magnitude * 10 + digit;
        return true;
    };

    for (char digit : digits) {
        if (!appendDigit(quint64(digit - '0'))) {
            return std::nullopt;
        }
    }
    for (qint64 index = 0; index < scale; ++index) {
        if (!appendDigit(0)) {
            return std::nullopt;
        }
    }

    if (!negative) {
        return qint64(magnitude);
    }
    if (magnitude == kNegativeLimit) {
        return std::numeric_limits<qint64>::min();
    }
    return -qint64(magnitude);
}

Result<QJsonObject> requireObject(const QJsonValue& value, const QString& path)
{
    if (!value.isObject()) {
        return Result<QJsonObject>::failure(invalidField(path));
    }
    return Result<QJsonObject>::success(value.toObject());
}

Result<QString> requireString(const QJsonValue& value, const QString& path)
{
    if (!value.isString()) {
        return Result<QString>::failure(invalidField(path));
    }
    return Result<QString>::success(value.toString());
}

Result<bool> requireBool(const QJsonValue& value, const QString& path)
{
    if (!value.isBool()) {
        return Result<bool>::failure(invalidField(path));
    }
    return Result<bool>::success(value.toBool());
}

Result<qint64> requireInteger(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    if (!value.isDouble()) {
        return Result<qint64>::failure(invalidField(path));
    }
    const QByteArray token = context.rawNumbers.token(path);
    const auto parsed = parseSwiftIntegerToken(token, value.toDouble());
    if (!parsed.has_value()) {
        return Result<qint64>::failure(invalidField(path));
    }
    return Result<qint64>::success(*parsed);
}

Result<qint64> requireNonNegativeInteger(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto result =
        requireInteger(value, path, context);
    if (!result.hasValue()) {
        return result;
    }
    if (result.value() < 0) {
        return Result<qint64>::failure(
            invalidField(path));
    }
    return result;
}

Result<double> requireFiniteDouble(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    if (!value.isDouble()) {
        return Result<double>::failure(invalidField(path));
    }
    bool valid = false;
    const double parsed = context.rawNumbers.token(path).toDouble(&valid);
    if (!valid || !std::isfinite(parsed)) {
        return Result<double>::failure(invalidField(path));
    }
    return Result<double>::success(parsed);
}

Result<QUuid> requireUuid(const QJsonValue& value, const QString& path)
{
    const auto string = requireString(value, path);
    if (!string.hasValue()) {
        return Result<QUuid>::failure(string.error());
    }

    const QString& text = string.value();
    if (text.size() != 36) {
        return Result<QUuid>::failure(invalidField(path));
    }
    for (qsizetype index = 0; index < text.size(); ++index) {
        const bool separator =
            index == 8 || index == 13 || index == 18 || index == 23;
        if (separator) {
            if (text.at(index) != QLatin1Char('-')) {
                return Result<QUuid>::failure(invalidField(path));
            }
            continue;
        }
        const QChar character = text.at(index);
        const bool hexadecimal =
            (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
            || (character >= QLatin1Char('a') && character <= QLatin1Char('f'))
            || (character >= QLatin1Char('A') && character <= QLatin1Char('F'));
        if (!hexadecimal) {
            return Result<QUuid>::failure(invalidField(path));
        }
    }

    const QUuid uuid(string.value());
    return Result<QUuid>::success(uuid);
}

QString uuidText(const QUuid& value)
{
    return value.toString(QUuid::WithoutBraces).toUpper();
}

Result<QByteArray> requireBase64(const QJsonValue& value, const QString& path)
{
    const auto string = requireString(value, path);
    if (!string.hasValue()) {
        return Result<QByteArray>::failure(string.error());
    }
    const QByteArray encoded = string.value().toUtf8();
    if (encoded.isEmpty()) {
        return Result<QByteArray>::success({});
    }

    qsizetype lastNonPadding = encoded.size() - 1;
    while (lastNonPadding >= 0 && encoded.at(lastNonPadding) == '=') {
        --lastNonPadding;
    }
    if (lastNonPadding < 0) {
        return encoded.size() >= 4
            ? Result<QByteArray>::success(QByteArray(1, '\0'))
            : Result<QByteArray>::failure(invalidField(path));
    }

    const qsizetype nonPaddedLength = lastNonPadding + 1;
    const qsizetype bytesToParse = ((nonPaddedLength + 3) / 4) * 4;
    if (bytesToParse > encoded.size()) {
        return Result<QByteArray>::failure(invalidField(path));
    }

    const auto sextet = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };
    const auto appendChunk = [&sextet](
                                 QByteArray& output,
                                 char a0,
                                 char a1,
                                 std::optional<char> a2,
                                 std::optional<char> a3) {
        const int v0 = sextet(a0);
        const int v1 = sextet(a1);
        const int v2 = a2.has_value() ? sextet(*a2) : 0;
        const int v3 = a3.has_value() ? sextet(*a3) : 0;
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return false;
        }
        output.append(char((v0 << 2) | (v1 >> 4)));
        if (a2.has_value()) {
            output.append(char((v1 << 4) | (v2 >> 2)));
        }
        if (a3.has_value()) {
            output.append(char((v2 << 6) | v3));
        }
        return true;
    };

    QByteArray decoded;
    decoded.reserve(((bytesToParse + 3) / 4) * 3);
    const qsizetype fullChunks = bytesToParse / 4 - 1;
    for (qsizetype chunk = 0; chunk < fullChunks; ++chunk) {
        const qsizetype offset = chunk * 4;
        if (!appendChunk(
                decoded,
                encoded.at(offset),
                encoded.at(offset + 1),
                encoded.at(offset + 2),
                encoded.at(offset + 3))) {
            return Result<QByteArray>::failure(invalidField(path));
        }
    }

    const qsizetype offset = fullChunks * 4;
    const std::optional<char> a2 =
        encoded.at(offset + 2) == '='
        ? std::nullopt
        : std::optional<char>(encoded.at(offset + 2));
    const std::optional<char> a3 =
        encoded.at(offset + 3) == '='
        ? std::nullopt
        : std::optional<char>(encoded.at(offset + 3));
    if (!appendChunk(
            decoded,
            encoded.at(offset),
            encoded.at(offset + 1),
            a2,
            a3)) {
        return Result<QByteArray>::failure(invalidField(path));
    }
    return Result<QByteArray>::success(std::move(decoded));
}

Result<BridgeDate> requireDate(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto number = requireFiniteDouble(value, path, context);
    if (!number.hasValue()) {
        return Result<BridgeDate>::failure(number.error());
    }
    const double referenceDate = context.profile == BridgeWireProfile::NearbyV1Milliseconds
        ? number.value() / 1000.0 - kSwiftReferenceDateUnixSeconds
        : number.value();
    if (!std::isfinite(referenceDate)) {
        return Result<BridgeDate>::failure(invalidField(path));
    }
    return Result<BridgeDate>::success({referenceDate});
}

template <typename T, typename Decoder>
Result<std::optional<T>> optionalField(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    Decoder decoder)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<std::optional<T>>::success(std::nullopt);
    }
    const auto decoded = decoder(*iterator, childPath(path, key));
    if (!decoded.hasValue()) {
        return Result<std::optional<T>>::failure(decoded.error());
    }
    return Result<std::optional<T>>::success(decoded.value());
}

template <typename T, typename Decoder>
Result<QVector<T>> requireArray(
    const QJsonValue& value,
    const QString& path,
    Decoder decoder)
{
    if (!value.isArray()) {
        return Result<QVector<T>>::failure(invalidField(path));
    }
    QVector<T> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (qsizetype index = 0; index < array.size(); ++index) {
        const auto decoded = decoder(
            array.at(index), path + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!decoded.hasValue()) {
            return Result<QVector<T>>::failure(decoded.error());
        }
        result.append(decoded.value());
    }
    return Result<QVector<T>>::success(std::move(result));
}

template <typename T, typename Decoder>
Result<std::optional<QVector<T>>> optionalArray(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    Decoder decoder)
{
    return optionalField<QVector<T>>(
        object, key, path,
        [&decoder](const QJsonValue& value, const QString& nestedPath) {
            return requireArray<T>(value, nestedPath, decoder);
        });
}

Result<BridgeOperation> parseOperation(const QString& value, const QString& path)
{
    if (value == QStringLiteral("handshake")) return Result<BridgeOperation>::success(BridgeOperation::Handshake);
    if (value == QStringLiteral("listTasks")) return Result<BridgeOperation>::success(BridgeOperation::ListTasks);
    if (value == QStringLiteral("loadMessages")) return Result<BridgeOperation>::success(BridgeOperation::LoadMessages);
    if (value == QStringLiteral("sendMessage")) return Result<BridgeOperation>::success(BridgeOperation::SendMessage);
    if (value == QStringLiteral("respondToApproval")) return Result<BridgeOperation>::success(BridgeOperation::RespondToApproval);
    if (value == QStringLiteral("createTask")) return Result<BridgeOperation>::success(BridgeOperation::CreateTask);
    if (value == QStringLiteral("loadCapabilities")) return Result<BridgeOperation>::success(BridgeOperation::LoadCapabilities);
    if (value == QStringLiteral("sendCasualChat")) return Result<BridgeOperation>::success(BridgeOperation::SendCasualChat);
    if (value == QStringLiteral("loadUsage")) return Result<BridgeOperation>::success(BridgeOperation::LoadUsage);
    if (value == QStringLiteral("consumeUsageReset")) return Result<BridgeOperation>::success(BridgeOperation::ConsumeUsageReset);
    if (value == QStringLiteral("createGoal")) return Result<BridgeOperation>::success(BridgeOperation::CreateGoal);
    if (value == QStringLiteral("resumeGoal")) return Result<BridgeOperation>::success(BridgeOperation::ResumeGoal);
    if (value == QStringLiteral("updateGoal")) return Result<BridgeOperation>::success(BridgeOperation::UpdateGoal);
    if (value == QStringLiteral("loadPresencePetManifest")) return Result<BridgeOperation>::success(BridgeOperation::LoadPresencePetManifest);
    if (value == QStringLiteral("loadPresencePetChunk")) return Result<BridgeOperation>::success(BridgeOperation::LoadPresencePetChunk);
    return Result<BridgeOperation>::failure(invalidField(path));
}

Result<BridgeFeature> parseFeature(
    const QString& value,
    const QString& path)
{
    if (value == QStringLiteral("task_stream_v1")) {
        return Result<BridgeFeature>::success(
            BridgeFeature::TaskStreamV1);
    }
    if (value
        == QStringLiteral(
            "presence_pet_package_v1")) {
        return Result<BridgeFeature>::success(
            BridgeFeature::PresencePetPackageV1);
    }
    if (value
        == QStringLiteral("attachment_upload_v1")) {
        return Result<BridgeFeature>::success(
            BridgeFeature::AttachmentUploadV1);
    }
    return Result<BridgeFeature>::failure(
        invalidField(path));
}

Result<BridgePresencePetState> parsePresencePetState(
    const QString& value,
    const QString& path)
{
    if (value == QStringLiteral("idle")) {
        return Result<BridgePresencePetState>::success(
            BridgePresencePetState::Idle);
    }
    if (value == QStringLiteral("thinking")) {
        return Result<BridgePresencePetState>::success(
            BridgePresencePetState::Thinking);
    }
    if (value == QStringLiteral("talking")) {
        return Result<BridgePresencePetState>::success(
            BridgePresencePetState::Talking);
    }
    return Result<BridgePresencePetState>::failure(
        invalidField(path));
}

Result<SendAction> parseSendAction(const QString& value, const QString& path)
{
    if (value == QStringLiteral("reply")) return Result<SendAction>::success(SendAction::Reply);
    if (value == QStringLiteral("steer")) return Result<SendAction>::success(SendAction::Steer);
    return Result<SendAction>::failure(invalidField(path));
}

Result<ChatProvider> parseChatProvider(const QString& value, const QString& path)
{
    if (value == QStringLiteral("onDevice")) return Result<ChatProvider>::success(ChatProvider::OnDevice);
    if (value == QStringLiteral("openAIAPI")) return Result<ChatProvider>::success(ChatProvider::OpenAIAPI);
    if (value == QStringLiteral("lumoAPI")) return Result<ChatProvider>::success(ChatProvider::LumoAPI);
    return Result<ChatProvider>::failure(invalidField(path));
}

Result<ApprovalDecision> parseApprovalDecision(const QString& value, const QString& path)
{
    if (value == QStringLiteral("approveOnce")) return Result<ApprovalDecision>::success(ApprovalDecision::ApproveOnce);
    if (value == QStringLiteral("approveSimilar")) return Result<ApprovalDecision>::success(ApprovalDecision::ApproveSimilar);
    if (value == QStringLiteral("decline")) return Result<ApprovalDecision>::success(ApprovalDecision::Decline);
    return Result<ApprovalDecision>::failure(invalidField(path));
}

Result<TaskStatus> parseTaskStatus(const QString& value, const QString& path)
{
    if (value == QStringLiteral("running")) return Result<TaskStatus>::success(TaskStatus::Running);
    if (value == QStringLiteral("waiting")) return Result<TaskStatus>::success(TaskStatus::Waiting);
    if (value == QStringLiteral("completed")) return Result<TaskStatus>::success(TaskStatus::Completed);
    if (value == QStringLiteral("failed")) return Result<TaskStatus>::success(TaskStatus::Failed);
    return Result<TaskStatus>::failure(invalidField(path));
}

Result<GoalStatus> parseGoalStatus(const QString& value, const QString& path)
{
    if (value == QStringLiteral("active")) return Result<GoalStatus>::success(GoalStatus::Active);
    if (value == QStringLiteral("paused")) return Result<GoalStatus>::success(GoalStatus::Paused);
    if (value == QStringLiteral("blocked")) return Result<GoalStatus>::success(GoalStatus::Blocked);
    if (value == QStringLiteral("usageLimited")) return Result<GoalStatus>::success(GoalStatus::UsageLimited);
    if (value == QStringLiteral("budgetLimited")) return Result<GoalStatus>::success(GoalStatus::BudgetLimited);
    if (value == QStringLiteral("complete")) return Result<GoalStatus>::success(GoalStatus::Complete);
    return Result<GoalStatus>::failure(invalidField(path));
}

Result<TaskGroupKind> parseTaskGroupKind(const QString& value, const QString& path)
{
    if (value == QStringLiteral("chats")) return Result<TaskGroupKind>::success(TaskGroupKind::Chats);
    if (value == QStringLiteral("project")) return Result<TaskGroupKind>::success(TaskGroupKind::Project);
    return Result<TaskGroupKind>::failure(invalidField(path));
}

Result<AttachmentKind> parseAttachmentKind(const QString& value, const QString& path)
{
    if (value == QStringLiteral("file")) return Result<AttachmentKind>::success(AttachmentKind::File);
    if (value == QStringLiteral("image")) return Result<AttachmentKind>::success(AttachmentKind::Image);
    return Result<AttachmentKind>::failure(invalidField(path));
}

Result<MessageRole> parseMessageRole(const QString& value, const QString& path)
{
    if (value == QStringLiteral("user")) return Result<MessageRole>::success(MessageRole::User);
    if (value == QStringLiteral("assistant")) return Result<MessageRole>::success(MessageRole::Assistant);
    return Result<MessageRole>::failure(invalidField(path));
}

Result<TimelineKind> parseTimelineKind(const QString& value, const QString& path)
{
    if (value == QStringLiteral("message")) return Result<TimelineKind>::success(TimelineKind::Message);
    if (value == QStringLiteral("reasoning")) return Result<TimelineKind>::success(TimelineKind::Reasoning);
    if (value == QStringLiteral("tool")) return Result<TimelineKind>::success(TimelineKind::Tool);
    if (value == QStringLiteral("status")) return Result<TimelineKind>::success(TimelineKind::Status);
    if (value == QStringLiteral("compaction")) return Result<TimelineKind>::success(TimelineKind::Compaction);
    return Result<TimelineKind>::failure(invalidField(path));
}

Result<TimelineStatus> parseTimelineStatus(const QString& value, const QString& path)
{
    if (value == QStringLiteral("inProgress")) return Result<TimelineStatus>::success(TimelineStatus::InProgress);
    if (value == QStringLiteral("completed")) return Result<TimelineStatus>::success(TimelineStatus::Completed);
    if (value == QStringLiteral("failed")) return Result<TimelineStatus>::success(TimelineStatus::Failed);
    return Result<TimelineStatus>::failure(invalidField(path));
}

Result<TimelinePhase> parseTimelinePhase(const QString& value, const QString& path)
{
    if (value == QStringLiteral("commentary")) return Result<TimelinePhase>::success(TimelinePhase::Commentary);
    if (value == QStringLiteral("final")) return Result<TimelinePhase>::success(TimelinePhase::Final);
    return Result<TimelinePhase>::failure(invalidField(path));
}

Result<MediaKind> parseMediaKind(const QString& value, const QString& path)
{
    if (value == QStringLiteral("image")) return Result<MediaKind>::success(MediaKind::Image);
    return Result<MediaKind>::failure(invalidField(path));
}

QString operationText(BridgeOperation value)
{
    switch (value) {
    case BridgeOperation::Handshake: return QStringLiteral("handshake");
    case BridgeOperation::ListTasks: return QStringLiteral("listTasks");
    case BridgeOperation::LoadMessages: return QStringLiteral("loadMessages");
    case BridgeOperation::SendMessage: return QStringLiteral("sendMessage");
    case BridgeOperation::RespondToApproval: return QStringLiteral("respondToApproval");
    case BridgeOperation::CreateTask: return QStringLiteral("createTask");
    case BridgeOperation::LoadCapabilities: return QStringLiteral("loadCapabilities");
    case BridgeOperation::SendCasualChat: return QStringLiteral("sendCasualChat");
    case BridgeOperation::LoadUsage: return QStringLiteral("loadUsage");
    case BridgeOperation::ConsumeUsageReset: return QStringLiteral("consumeUsageReset");
    case BridgeOperation::CreateGoal: return QStringLiteral("createGoal");
    case BridgeOperation::ResumeGoal: return QStringLiteral("resumeGoal");
    case BridgeOperation::UpdateGoal: return QStringLiteral("updateGoal");
    case BridgeOperation::LoadPresencePetManifest: return QStringLiteral("loadPresencePetManifest");
    case BridgeOperation::LoadPresencePetChunk: return QStringLiteral("loadPresencePetChunk");
    }
    return {};
}

QString featureText(BridgeFeature value)
{
    switch (value) {
    case BridgeFeature::TaskStreamV1:
        return QStringLiteral("task_stream_v1");
    case BridgeFeature::PresencePetPackageV1:
        return QStringLiteral(
            "presence_pet_package_v1");
    case BridgeFeature::AttachmentUploadV1:
        return QStringLiteral("attachment_upload_v1");
    }
    return {};
}

QString presencePetStateText(
    BridgePresencePetState value)
{
    switch (value) {
    case BridgePresencePetState::Idle:
        return QStringLiteral("idle");
    case BridgePresencePetState::Thinking:
        return QStringLiteral("thinking");
    case BridgePresencePetState::Talking:
        return QStringLiteral("talking");
    }
    return {};
}

QString sendActionText(SendAction value)
{
    return value == SendAction::Reply ? QStringLiteral("reply") : QStringLiteral("steer");
}

QString chatProviderText(ChatProvider value)
{
    switch (value) {
    case ChatProvider::OnDevice: return QStringLiteral("onDevice");
    case ChatProvider::OpenAIAPI: return QStringLiteral("openAIAPI");
    case ChatProvider::LumoAPI: return QStringLiteral("lumoAPI");
    }
    return {};
}

QString approvalDecisionText(ApprovalDecision value)
{
    switch (value) {
    case ApprovalDecision::ApproveOnce: return QStringLiteral("approveOnce");
    case ApprovalDecision::ApproveSimilar: return QStringLiteral("approveSimilar");
    case ApprovalDecision::Decline: return QStringLiteral("decline");
    }
    return {};
}

QString taskStatusText(TaskStatus value)
{
    switch (value) {
    case TaskStatus::Running: return QStringLiteral("running");
    case TaskStatus::Waiting: return QStringLiteral("waiting");
    case TaskStatus::Completed: return QStringLiteral("completed");
    case TaskStatus::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString goalStatusText(GoalStatus value)
{
    switch (value) {
    case GoalStatus::Active: return QStringLiteral("active");
    case GoalStatus::Paused: return QStringLiteral("paused");
    case GoalStatus::Blocked: return QStringLiteral("blocked");
    case GoalStatus::UsageLimited: return QStringLiteral("usageLimited");
    case GoalStatus::BudgetLimited: return QStringLiteral("budgetLimited");
    case GoalStatus::Complete: return QStringLiteral("complete");
    }
    return {};
}

QString taskGroupKindText(TaskGroupKind value)
{
    return value == TaskGroupKind::Chats ? QStringLiteral("chats") : QStringLiteral("project");
}

QString attachmentKindText(AttachmentKind value)
{
    return value == AttachmentKind::File ? QStringLiteral("file") : QStringLiteral("image");
}

QString messageRoleText(MessageRole value)
{
    return value == MessageRole::User ? QStringLiteral("user") : QStringLiteral("assistant");
}

QString timelineKindText(TimelineKind value)
{
    switch (value) {
    case TimelineKind::Message: return QStringLiteral("message");
    case TimelineKind::Reasoning: return QStringLiteral("reasoning");
    case TimelineKind::Tool: return QStringLiteral("tool");
    case TimelineKind::Status: return QStringLiteral("status");
    case TimelineKind::Compaction: return QStringLiteral("compaction");
    }
    return {};
}

QString timelineStatusText(TimelineStatus value)
{
    switch (value) {
    case TimelineStatus::InProgress: return QStringLiteral("inProgress");
    case TimelineStatus::Completed: return QStringLiteral("completed");
    case TimelineStatus::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString timelinePhaseText(TimelinePhase value)
{
    return value == TimelinePhase::Commentary ? QStringLiteral("commentary") : QStringLiteral("final");
}

QString mediaKindText(MediaKind)
{
    return QStringLiteral("image");
}

template <typename T>
Result<T> requiredField(const QJsonObject& object, QStringView key, const QString& path, const DecodeContext& context);

template <>
Result<QString> requiredField<QString>(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    const DecodeContext&)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<QString>::failure(invalidField(childPath(path, key)));
    }
    return requireString(*iterator, childPath(path, key));
}

template <>
Result<bool> requiredField<bool>(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    const DecodeContext&)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<bool>::failure(invalidField(childPath(path, key)));
    }
    return requireBool(*iterator, childPath(path, key));
}

template <>
Result<qint64> requiredField<qint64>(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    const DecodeContext& context)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<qint64>::failure(invalidField(childPath(path, key)));
    }
    return requireInteger(*iterator, childPath(path, key), context);
}

template <>
Result<QUuid> requiredField<QUuid>(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    const DecodeContext&)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<QUuid>::failure(invalidField(childPath(path, key)));
    }
    return requireUuid(*iterator, childPath(path, key));
}

template <>
Result<BridgeDate> requiredField<BridgeDate>(
    const QJsonObject& object,
    QStringView key,
    const QString& path,
    const DecodeContext& context)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd() || iterator->isNull()) {
        return Result<BridgeDate>::failure(invalidField(childPath(path, key)));
    }
    return requireDate(*iterator, childPath(path, key), context);
}

Result<BridgeAttachment> decodeAttachment(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeGoal> decodeGoal(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeTaskGroup> decodeTaskGroup(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeTask> decodeTask(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeMessage> decodeMessage(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeMedia> decodeMedia(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeTimelineItem> decodeTimelineItem(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeSubagent> decodeSubagent(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeContextUsage> decodeContextUsage(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeUsageWindow> decodeUsageWindow(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeResetCredit> decodeResetCredit(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeUsageGroup> decodeUsageGroup(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeUsageSnapshot> decodeUsageSnapshot(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeReasoningEffort> decodeReasoningEffort(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeModel> decodeModel(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeSkill> decodeSkill(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePlugin> decodePlugin(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeChatAgent> decodeChatAgent(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeChatModel> decodeChatModel(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgeCapabilities> decodeCapabilities(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetFile> decodePresencePetFile(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetAtlas> decodePresencePetAtlas(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetAnimation>
decodePresencePetAnimation(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetManifest>
decodePresencePetManifest(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetCatalogEntry>
decodePresencePetCatalogEntry(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);
Result<BridgePresencePetChunk> decodePresencePetChunk(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context);

Result<BridgeAttachment> decodeAttachment(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeAttachment>::failure(object.error());
    const auto id = requiredField<QUuid>(object.value(), u"id", path, context);
    const auto kindValue = requiredField<QString>(object.value(), u"kind", path, context);
    const auto filename = requiredField<QString>(object.value(), u"filename", path, context);
    const auto dataValue = object.value().value(u"data");
    if (!id.hasValue()) return Result<BridgeAttachment>::failure(id.error());
    if (!kindValue.hasValue()) return Result<BridgeAttachment>::failure(kindValue.error());
    if (!filename.hasValue()) return Result<BridgeAttachment>::failure(filename.error());
    if (dataValue.isUndefined() || dataValue.isNull()) {
        return Result<BridgeAttachment>::failure(invalidField(childPath(path, u"data")));
    }
    const auto kind = parseAttachmentKind(kindValue.value(), childPath(path, u"kind"));
    const auto data = requireBase64(dataValue, childPath(path, u"data"));
    const auto mimeType = optionalField<QString>(
        object.value(), u"mimeType", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    if (!kind.hasValue()) return Result<BridgeAttachment>::failure(kind.error());
    if (!data.hasValue()) return Result<BridgeAttachment>::failure(data.error());
    if (!mimeType.hasValue()) return Result<BridgeAttachment>::failure(mimeType.error());
    return Result<BridgeAttachment>::success(
        {id.value(), kind.value(), filename.value(), mimeType.value(), data.value()});
}

Result<BridgeGoal> decodeGoal(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeGoal>::failure(object.error());
    const auto threadId = requiredField<QString>(object.value(), u"threadID", path, context);
    const auto objective = requiredField<QString>(object.value(), u"objective", path, context);
    const auto statusText = requiredField<QString>(object.value(), u"status", path, context);
    const auto tokensUsed = requiredField<qint64>(object.value(), u"tokensUsed", path, context);
    const auto elapsedSeconds = requiredField<qint64>(object.value(), u"elapsedSeconds", path, context);
    const auto createdAt = requiredField<qint64>(object.value(), u"createdAt", path, context);
    const auto updatedAt = requiredField<qint64>(object.value(), u"updatedAt", path, context);
    const auto tokenBudget = optionalField<qint64>(
        object.value(), u"tokenBudget", path,
        [&context](const QJsonValue& item, const QString& itemPath) {
            return requireInteger(item, itemPath, context);
        });
    if (!threadId.hasValue()) return Result<BridgeGoal>::failure(threadId.error());
    if (!objective.hasValue()) return Result<BridgeGoal>::failure(objective.error());
    if (!statusText.hasValue()) return Result<BridgeGoal>::failure(statusText.error());
    if (!tokensUsed.hasValue()) return Result<BridgeGoal>::failure(tokensUsed.error());
    if (!elapsedSeconds.hasValue()) return Result<BridgeGoal>::failure(elapsedSeconds.error());
    if (!createdAt.hasValue()) return Result<BridgeGoal>::failure(createdAt.error());
    if (!updatedAt.hasValue()) return Result<BridgeGoal>::failure(updatedAt.error());
    if (!tokenBudget.hasValue()) return Result<BridgeGoal>::failure(tokenBudget.error());
    const auto status = parseGoalStatus(statusText.value(), childPath(path, u"status"));
    if (!status.hasValue()) return Result<BridgeGoal>::failure(status.error());
    return Result<BridgeGoal>::success({
        threadId.value(), objective.value(), status.value(), tokenBudget.value(),
        tokensUsed.value(), elapsedSeconds.value(), createdAt.value(), updatedAt.value(),
    });
}

Result<BridgeTaskGroup> decodeTaskGroup(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeTaskGroup>::failure(object.error());
    const auto kindText = requiredField<QString>(object.value(), u"kind", path, context);
    const auto title = requiredField<QString>(object.value(), u"title", path, context);
    const auto groupPath = optionalField<QString>(
        object.value(), u"path", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    if (!kindText.hasValue()) return Result<BridgeTaskGroup>::failure(kindText.error());
    if (!title.hasValue()) return Result<BridgeTaskGroup>::failure(title.error());
    if (!groupPath.hasValue()) return Result<BridgeTaskGroup>::failure(groupPath.error());
    const auto kind = parseTaskGroupKind(kindText.value(), childPath(path, u"kind"));
    if (!kind.hasValue()) return Result<BridgeTaskGroup>::failure(kind.error());
    return Result<BridgeTaskGroup>::success({kind.value(), title.value(), groupPath.value()});
}

Result<BridgeTask> decodeTask(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeTask>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto title = requiredField<QString>(object.value(), u"title", path, context);
    const auto preview = requiredField<QString>(object.value(), u"preview", path, context);
    const auto updatedAt = requiredField<BridgeDate>(object.value(), u"updatedAt", path, context);
    const auto statusText = requiredField<QString>(object.value(), u"status", path, context);
    const auto needsApproval = requiredField<bool>(object.value(), u"needsApproval", path, context);
    if (!id.hasValue()) return Result<BridgeTask>::failure(id.error());
    if (!title.hasValue()) return Result<BridgeTask>::failure(title.error());
    if (!preview.hasValue()) return Result<BridgeTask>::failure(preview.error());
    if (!updatedAt.hasValue()) return Result<BridgeTask>::failure(updatedAt.error());
    if (!statusText.hasValue()) return Result<BridgeTask>::failure(statusText.error());
    if (!needsApproval.hasValue()) return Result<BridgeTask>::failure(needsApproval.error());
    const auto status = parseTaskStatus(statusText.value(), childPath(path, u"status"));
    if (!status.hasValue()) return Result<BridgeTask>::failure(status.error());
    const auto cwd = optionalField<QString>(object.value(), u"cwd", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto activeTurn = optionalField<QString>(object.value(), u"activeTurnID", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto model = optionalField<QString>(object.value(), u"model", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto effort = optionalField<QString>(object.value(), u"reasoningEffort", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto taskGroup = optionalField<BridgeTaskGroup>(object.value(), u"taskGroup", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeTaskGroup(item, itemPath, context); });
    const auto goal = optionalField<BridgeGoal>(object.value(), u"goal", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeGoal(item, itemPath, context); });
    if (!cwd.hasValue()) return Result<BridgeTask>::failure(cwd.error());
    if (!activeTurn.hasValue()) return Result<BridgeTask>::failure(activeTurn.error());
    if (!model.hasValue()) return Result<BridgeTask>::failure(model.error());
    if (!effort.hasValue()) return Result<BridgeTask>::failure(effort.error());
    if (!taskGroup.hasValue()) return Result<BridgeTask>::failure(taskGroup.error());
    if (!goal.hasValue()) return Result<BridgeTask>::failure(goal.error());
    return Result<BridgeTask>::success({
        id.value(), title.value(), preview.value(), updatedAt.value(), cwd.value(),
        status.value(), needsApproval.value(), activeTurn.value(), model.value(),
        effort.value(), taskGroup.value(), goal.value(),
    });
}

Result<BridgeMessage> decodeMessage(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeMessage>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto roleText = requiredField<QString>(object.value(), u"role", path, context);
    const auto text = requiredField<QString>(object.value(), u"text", path, context);
    if (!id.hasValue()) return Result<BridgeMessage>::failure(id.error());
    if (!roleText.hasValue()) return Result<BridgeMessage>::failure(roleText.error());
    if (!text.hasValue()) return Result<BridgeMessage>::failure(text.error());
    const auto role = parseMessageRole(roleText.value(), childPath(path, u"role"));
    if (!role.hasValue()) return Result<BridgeMessage>::failure(role.error());
    const auto createdAt = optionalField<BridgeDate>(object.value(), u"createdAt", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return requireDate(item, itemPath, context); });
    const auto attachments = optionalArray<BridgeAttachment>(object.value(), u"attachments", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeAttachment(item, itemPath, context); });
    if (!createdAt.hasValue()) return Result<BridgeMessage>::failure(createdAt.error());
    if (!attachments.hasValue()) return Result<BridgeMessage>::failure(attachments.error());
    return Result<BridgeMessage>::success(
        {id.value(), role.value(), text.value(), createdAt.value(), attachments.value()});
}

Result<BridgeMedia> decodeMedia(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeMedia>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto kindText = requiredField<QString>(object.value(), u"kind", path, context);
    const auto mimeType = requiredField<QString>(object.value(), u"mimeType", path, context);
    if (!id.hasValue()) return Result<BridgeMedia>::failure(id.error());
    if (!kindText.hasValue()) return Result<BridgeMedia>::failure(kindText.error());
    if (!mimeType.hasValue()) return Result<BridgeMedia>::failure(mimeType.error());
    const auto kind = parseMediaKind(kindText.value(), childPath(path, u"kind"));
    const auto dataValue = object.value().value(u"data");
    if (!kind.hasValue()) return Result<BridgeMedia>::failure(kind.error());
    if (dataValue.isUndefined() || dataValue.isNull()) {
        return Result<BridgeMedia>::failure(invalidField(childPath(path, u"data")));
    }
    const auto data = requireBase64(dataValue, childPath(path, u"data"));
    if (!data.hasValue()) return Result<BridgeMedia>::failure(data.error());
    return Result<BridgeMedia>::success({id.value(), kind.value(), mimeType.value(), data.value()});
}

Result<BridgeTimelineItem> decodeTimelineItem(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeTimelineItem>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto kindText = requiredField<QString>(object.value(), u"kind", path, context);
    const auto statusText = requiredField<QString>(object.value(), u"status", path, context);
    const auto mediaValue = object.value().value(u"media");
    if (!id.hasValue()) return Result<BridgeTimelineItem>::failure(id.error());
    if (!kindText.hasValue()) return Result<BridgeTimelineItem>::failure(kindText.error());
    if (!statusText.hasValue()) return Result<BridgeTimelineItem>::failure(statusText.error());
    if (mediaValue.isUndefined() || mediaValue.isNull()) {
        return Result<BridgeTimelineItem>::failure(invalidField(childPath(path, u"media")));
    }
    const auto kind = parseTimelineKind(kindText.value(), childPath(path, u"kind"));
    const auto status = parseTimelineStatus(statusText.value(), childPath(path, u"status"));
    const auto media = requireArray<BridgeMedia>(mediaValue, childPath(path, u"media"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeMedia(item, itemPath, context); });
    if (!kind.hasValue()) return Result<BridgeTimelineItem>::failure(kind.error());
    if (!status.hasValue()) return Result<BridgeTimelineItem>::failure(status.error());
    if (!media.hasValue()) return Result<BridgeTimelineItem>::failure(media.error());
    const auto role = optionalField<MessageRole>(object.value(), u"role", path,
        [](const QJsonValue& item, const QString& itemPath) {
            const auto text = requireString(item, itemPath);
            return text.hasValue() ? parseMessageRole(text.value(), itemPath)
                                   : Result<MessageRole>::failure(text.error());
        });
    const auto title = optionalField<QString>(object.value(), u"title", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto text = optionalField<QString>(object.value(), u"text", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto detail = optionalField<QString>(object.value(), u"detail", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto phase = optionalField<TimelinePhase>(object.value(), u"phase", path,
        [](const QJsonValue& item, const QString& itemPath) {
            const auto phaseText = requireString(item, itemPath);
            return phaseText.hasValue() ? parseTimelinePhase(phaseText.value(), itemPath)
                                        : Result<TimelinePhase>::failure(phaseText.error());
        });
    const auto createdAt = optionalField<BridgeDate>(object.value(), u"createdAt", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return requireDate(item, itemPath, context); });
    const auto turnId = optionalField<QString>(object.value(), u"turnID", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto callId = optionalField<QString>(object.value(), u"callID", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    if (!role.hasValue()) return Result<BridgeTimelineItem>::failure(role.error());
    if (!title.hasValue()) return Result<BridgeTimelineItem>::failure(title.error());
    if (!text.hasValue()) return Result<BridgeTimelineItem>::failure(text.error());
    if (!detail.hasValue()) return Result<BridgeTimelineItem>::failure(detail.error());
    if (!phase.hasValue()) return Result<BridgeTimelineItem>::failure(phase.error());
    if (!createdAt.hasValue()) return Result<BridgeTimelineItem>::failure(createdAt.error());
    if (!turnId.hasValue()) return Result<BridgeTimelineItem>::failure(turnId.error());
    if (!callId.hasValue()) return Result<BridgeTimelineItem>::failure(callId.error());
    return Result<BridgeTimelineItem>::success({
        id.value(), kind.value(), status.value(), role.value(), title.value(), text.value(),
        detail.value(), phase.value(), createdAt.value(), turnId.value(), callId.value(), media.value(),
    });
}

Result<BridgeSubagent> decodeSubagent(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeSubagent>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto name = requiredField<QString>(object.value(), u"name", path, context);
    const auto title = requiredField<QString>(object.value(), u"title", path, context);
    const auto updatedAt = requiredField<BridgeDate>(object.value(), u"updatedAt", path, context);
    const auto statusText = requiredField<QString>(object.value(), u"status", path, context);
    if (!id.hasValue()) return Result<BridgeSubagent>::failure(id.error());
    if (!name.hasValue()) return Result<BridgeSubagent>::failure(name.error());
    if (!title.hasValue()) return Result<BridgeSubagent>::failure(title.error());
    if (!updatedAt.hasValue()) return Result<BridgeSubagent>::failure(updatedAt.error());
    if (!statusText.hasValue()) return Result<BridgeSubagent>::failure(statusText.error());
    const auto status = parseTaskStatus(statusText.value(), childPath(path, u"status"));
    if (!status.hasValue()) return Result<BridgeSubagent>::failure(status.error());
    const auto role = optionalField<QString>(object.value(), u"role", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto needsApproval = optionalField<bool>(object.value(), u"needsApproval", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireBool(item, itemPath); });
    if (!role.hasValue()) return Result<BridgeSubagent>::failure(role.error());
    if (!needsApproval.hasValue()) return Result<BridgeSubagent>::failure(needsApproval.error());
    return Result<BridgeSubagent>::success({
        id.value(), name.value(), title.value(), role.value(), updatedAt.value(),
        status.value(), needsApproval.value(),
    });
}

Result<BridgeContextUsage> decodeContextUsage(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeContextUsage>::failure(object.error());
    const auto usedTokens = requiredField<qint64>(object.value(), u"usedTokens", path, context);
    const auto contextWindow = requiredField<qint64>(object.value(), u"contextWindow", path, context);
    if (!usedTokens.hasValue()) return Result<BridgeContextUsage>::failure(usedTokens.error());
    if (!contextWindow.hasValue()) return Result<BridgeContextUsage>::failure(contextWindow.error());
    return Result<BridgeContextUsage>::success({usedTokens.value(), contextWindow.value()});
}

Result<BridgeUsageWindow> decodeUsageWindow(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeUsageWindow>::failure(object.error());
    const auto remainingPercentValue = object.value().value(u"remainingPercent");
    if (remainingPercentValue.isUndefined() || remainingPercentValue.isNull()) {
        return Result<BridgeUsageWindow>::failure(invalidField(childPath(path, u"remainingPercent")));
    }
    const auto remainingPercent = requireFiniteDouble(
        remainingPercentValue,
        childPath(path, u"remainingPercent"),
        context);
    const auto durationLabel = requiredField<QString>(object.value(), u"durationLabel", path, context);
    const auto resetsAt = optionalField<BridgeDate>(object.value(), u"resetsAt", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return requireDate(item, itemPath, context); });
    if (!remainingPercent.hasValue()) return Result<BridgeUsageWindow>::failure(remainingPercent.error());
    if (!durationLabel.hasValue()) return Result<BridgeUsageWindow>::failure(durationLabel.error());
    if (!resetsAt.hasValue()) return Result<BridgeUsageWindow>::failure(resetsAt.error());
    return Result<BridgeUsageWindow>::success(
        {remainingPercent.value(), durationLabel.value(), resetsAt.value()});
}

Result<BridgeResetCredit> decodeResetCredit(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeResetCredit>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto displayTitle = requiredField<QString>(object.value(), u"displayTitle", path, context);
    if (!id.hasValue()) return Result<BridgeResetCredit>::failure(id.error());
    if (!displayTitle.hasValue()) return Result<BridgeResetCredit>::failure(displayTitle.error());
    const auto detail = optionalField<QString>(object.value(), u"detail", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    const auto expiresAt = optionalField<BridgeDate>(object.value(), u"expiresAt", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return requireDate(item, itemPath, context); });
    if (!detail.hasValue()) return Result<BridgeResetCredit>::failure(detail.error());
    if (!expiresAt.hasValue()) return Result<BridgeResetCredit>::failure(expiresAt.error());
    return Result<BridgeResetCredit>::success(
        {id.value(), displayTitle.value(), detail.value(), expiresAt.value()});
}

Result<BridgeUsageGroup> decodeUsageGroup(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeUsageGroup>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto title = requiredField<QString>(object.value(), u"title", path, context);
    if (!id.hasValue()) return Result<BridgeUsageGroup>::failure(id.error());
    if (!title.hasValue()) return Result<BridgeUsageGroup>::failure(title.error());
    const auto shortWindow = optionalField<BridgeUsageWindow>(object.value(), u"shortWindow", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeUsageWindow(item, itemPath, context); });
    const auto weeklyWindow = optionalField<BridgeUsageWindow>(object.value(), u"weeklyWindow", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeUsageWindow(item, itemPath, context); });
    if (!shortWindow.hasValue()) return Result<BridgeUsageGroup>::failure(shortWindow.error());
    if (!weeklyWindow.hasValue()) return Result<BridgeUsageGroup>::failure(weeklyWindow.error());
    return Result<BridgeUsageGroup>::success(
        {id.value(), title.value(), shortWindow.value(), weeklyWindow.value()});
}

Result<BridgeUsageSnapshot> decodeUsageSnapshot(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeUsageSnapshot>::failure(object.error());
    const auto groupsValue = object.value().value(u"groups");
    const auto creditsValue = object.value().value(u"availableResetCredits");
    if (groupsValue.isUndefined() || groupsValue.isNull()) {
        return Result<BridgeUsageSnapshot>::failure(invalidField(childPath(path, u"groups")));
    }
    if (creditsValue.isUndefined() || creditsValue.isNull()) {
        return Result<BridgeUsageSnapshot>::failure(invalidField(childPath(path, u"availableResetCredits")));
    }
    const auto groups = requireArray<BridgeUsageGroup>(groupsValue, childPath(path, u"groups"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeUsageGroup(item, itemPath, context); });
    const auto availableResetCount = requiredField<qint64>(object.value(), u"availableResetCount", path, context);
    const auto credits = requireArray<BridgeResetCredit>(creditsValue, childPath(path, u"availableResetCredits"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeResetCredit(item, itemPath, context); });
    const auto updatedAt = requiredField<BridgeDate>(object.value(), u"updatedAt", path, context);
    const auto planType = optionalField<QString>(object.value(), u"planType", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    if (!groups.hasValue()) return Result<BridgeUsageSnapshot>::failure(groups.error());
    if (!availableResetCount.hasValue()) return Result<BridgeUsageSnapshot>::failure(availableResetCount.error());
    if (!credits.hasValue()) return Result<BridgeUsageSnapshot>::failure(credits.error());
    if (!updatedAt.hasValue()) return Result<BridgeUsageSnapshot>::failure(updatedAt.error());
    if (!planType.hasValue()) return Result<BridgeUsageSnapshot>::failure(planType.error());
    return Result<BridgeUsageSnapshot>::success({
        planType.value(), groups.value(), availableResetCount.value(), credits.value(), updatedAt.value(),
    });
}

Result<BridgeReasoningEffort> decodeReasoningEffort(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeReasoningEffort>::failure(object.error());
    const auto effort = requiredField<QString>(object.value(), u"value", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    if (!effort.hasValue()) return Result<BridgeReasoningEffort>::failure(effort.error());
    if (!description.hasValue()) return Result<BridgeReasoningEffort>::failure(description.error());
    return Result<BridgeReasoningEffort>::success({effort.value(), description.value()});
}

Result<BridgeModel> decodeModel(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeModel>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto model = requiredField<QString>(object.value(), u"model", path, context);
    const auto displayName = requiredField<QString>(object.value(), u"displayName", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    const auto isDefault = requiredField<bool>(object.value(), u"isDefault", path, context);
    const auto defaultReasoningEffort = requiredField<QString>(object.value(), u"defaultReasoningEffort", path, context);
    const auto effortsValue = object.value().value(u"supportedReasoningEfforts");
    if (!id.hasValue()) return Result<BridgeModel>::failure(id.error());
    if (!model.hasValue()) return Result<BridgeModel>::failure(model.error());
    if (!displayName.hasValue()) return Result<BridgeModel>::failure(displayName.error());
    if (!description.hasValue()) return Result<BridgeModel>::failure(description.error());
    if (!isDefault.hasValue()) return Result<BridgeModel>::failure(isDefault.error());
    if (!defaultReasoningEffort.hasValue()) return Result<BridgeModel>::failure(defaultReasoningEffort.error());
    if (effortsValue.isUndefined() || effortsValue.isNull()) {
        return Result<BridgeModel>::failure(invalidField(childPath(path, u"supportedReasoningEfforts")));
    }
    const auto efforts = requireArray<BridgeReasoningEffort>(effortsValue, childPath(path, u"supportedReasoningEfforts"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeReasoningEffort(item, itemPath, context); });
    if (!efforts.hasValue()) return Result<BridgeModel>::failure(efforts.error());
    return Result<BridgeModel>::success({
        id.value(), model.value(), displayName.value(), description.value(), isDefault.value(),
        defaultReasoningEffort.value(), efforts.value(),
    });
}

Result<BridgeSkill> decodeSkill(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeSkill>::failure(object.error());
    const auto name = requiredField<QString>(object.value(), u"name", path, context);
    const auto displayName = requiredField<QString>(object.value(), u"displayName", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    const auto skillPath = requiredField<QString>(object.value(), u"path", path, context);
    const auto scope = requiredField<QString>(object.value(), u"scope", path, context);
    const auto defaultPrompt = optionalField<QString>(object.value(), u"defaultPrompt", path,
        [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    if (!name.hasValue()) return Result<BridgeSkill>::failure(name.error());
    if (!displayName.hasValue()) return Result<BridgeSkill>::failure(displayName.error());
    if (!description.hasValue()) return Result<BridgeSkill>::failure(description.error());
    if (!skillPath.hasValue()) return Result<BridgeSkill>::failure(skillPath.error());
    if (!scope.hasValue()) return Result<BridgeSkill>::failure(scope.error());
    if (!defaultPrompt.hasValue()) return Result<BridgeSkill>::failure(defaultPrompt.error());
    return Result<BridgeSkill>::success({
        name.value(), displayName.value(), description.value(), skillPath.value(),
        scope.value(), defaultPrompt.value(),
    });
}

Result<BridgePlugin> decodePlugin(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgePlugin>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto name = requiredField<QString>(object.value(), u"name", path, context);
    const auto displayName = requiredField<QString>(object.value(), u"displayName", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    const auto enabled = requiredField<bool>(object.value(), u"enabled", path, context);
    const auto installed = requiredField<bool>(object.value(), u"installed", path, context);
    if (!id.hasValue()) return Result<BridgePlugin>::failure(id.error());
    if (!name.hasValue()) return Result<BridgePlugin>::failure(name.error());
    if (!displayName.hasValue()) return Result<BridgePlugin>::failure(displayName.error());
    if (!description.hasValue()) return Result<BridgePlugin>::failure(description.error());
    if (!enabled.hasValue()) return Result<BridgePlugin>::failure(enabled.error());
    if (!installed.hasValue()) return Result<BridgePlugin>::failure(installed.error());
    return Result<BridgePlugin>::success({
        id.value(), name.value(), displayName.value(), description.value(),
        enabled.value(), installed.value(),
    });
}

Result<BridgeChatAgent> decodeChatAgent(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeChatAgent>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto name = requiredField<QString>(object.value(), u"name", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    const auto symbolName = requiredField<QString>(object.value(), u"symbolName", path, context);
    if (!id.hasValue()) return Result<BridgeChatAgent>::failure(id.error());
    if (!name.hasValue()) return Result<BridgeChatAgent>::failure(name.error());
    if (!description.hasValue()) return Result<BridgeChatAgent>::failure(description.error());
    if (!symbolName.hasValue()) return Result<BridgeChatAgent>::failure(symbolName.error());
    return Result<BridgeChatAgent>::success({
        id.value(), name.value(), description.value(), symbolName.value(),
    });
}

Result<BridgeChatModel> decodeChatModel(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeChatModel>::failure(object.error());
    const auto id = requiredField<QString>(object.value(), u"id", path, context);
    const auto providerText = requiredField<QString>(object.value(), u"provider", path, context);
    const auto model = requiredField<QString>(object.value(), u"model", path, context);
    const auto displayName = requiredField<QString>(object.value(), u"displayName", path, context);
    const auto description = requiredField<QString>(object.value(), u"description", path, context);
    const auto isDefault = requiredField<bool>(object.value(), u"isDefault", path, context);
    const auto isAvailable = requiredField<bool>(object.value(), u"isAvailable", path, context);
    const auto supportsAttachments = requiredField<bool>(object.value(), u"supportsAttachments", path, context);
    if (!id.hasValue()) return Result<BridgeChatModel>::failure(id.error());
    if (!providerText.hasValue()) return Result<BridgeChatModel>::failure(providerText.error());
    if (!model.hasValue()) return Result<BridgeChatModel>::failure(model.error());
    if (!displayName.hasValue()) return Result<BridgeChatModel>::failure(displayName.error());
    if (!description.hasValue()) return Result<BridgeChatModel>::failure(description.error());
    if (!isDefault.hasValue()) return Result<BridgeChatModel>::failure(isDefault.error());
    if (!isAvailable.hasValue()) return Result<BridgeChatModel>::failure(isAvailable.error());
    if (!supportsAttachments.hasValue()) return Result<BridgeChatModel>::failure(supportsAttachments.error());
    const auto provider = parseChatProvider(providerText.value(), childPath(path, u"provider"));
    if (!provider.hasValue()) return Result<BridgeChatModel>::failure(provider.error());
    return Result<BridgeChatModel>::success({
        id.value(), provider.value(), model.value(), displayName.value(), description.value(),
        isDefault.value(), isAvailable.value(), supportsAttachments.value(),
    });
}

Result<BridgeCapabilities> decodeCapabilities(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) return Result<BridgeCapabilities>::failure(object.error());
    const auto modelsValue = object.value().value(u"models");
    const auto skillsValue = object.value().value(u"skills");
    const auto pluginsValue = object.value().value(u"plugins");
    const auto agentsValue = object.value().value(u"chatAgents");
    if (modelsValue.isUndefined() || modelsValue.isNull()) return Result<BridgeCapabilities>::failure(invalidField(childPath(path, u"models")));
    if (skillsValue.isUndefined() || skillsValue.isNull()) return Result<BridgeCapabilities>::failure(invalidField(childPath(path, u"skills")));
    if (pluginsValue.isUndefined() || pluginsValue.isNull()) return Result<BridgeCapabilities>::failure(invalidField(childPath(path, u"plugins")));
    if (agentsValue.isUndefined() || agentsValue.isNull()) return Result<BridgeCapabilities>::failure(invalidField(childPath(path, u"chatAgents")));
    const auto models = requireArray<BridgeModel>(modelsValue, childPath(path, u"models"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeModel(item, itemPath, context); });
    const auto skills = requireArray<BridgeSkill>(skillsValue, childPath(path, u"skills"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeSkill(item, itemPath, context); });
    const auto plugins = requireArray<BridgePlugin>(pluginsValue, childPath(path, u"plugins"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodePlugin(item, itemPath, context); });
    const auto agents = requireArray<BridgeChatAgent>(agentsValue, childPath(path, u"chatAgents"),
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeChatAgent(item, itemPath, context); });
    const auto chatModels = optionalArray<BridgeChatModel>(object.value(), u"chatModels", path,
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeChatModel(item, itemPath, context); });
    if (!models.hasValue()) return Result<BridgeCapabilities>::failure(models.error());
    if (!skills.hasValue()) return Result<BridgeCapabilities>::failure(skills.error());
    if (!plugins.hasValue()) return Result<BridgeCapabilities>::failure(plugins.error());
    if (!agents.hasValue()) return Result<BridgeCapabilities>::failure(agents.error());
    if (!chatModels.hasValue()) return Result<BridgeCapabilities>::failure(chatModels.error());
    return Result<BridgeCapabilities>::success({
        models.value(), skills.value(), plugins.value(), agents.value(), chatModels.value(),
    });
}

Result<BridgePresencePetFile> decodePresencePetFile(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<BridgePresencePetFile>::failure(
            object.error());
    }
    const auto name = requiredField<QString>(
        object.value(), u"name", path, context);
    const auto sha256 = requiredField<QString>(
        object.value(), u"sha256", path, context);
    const auto byteCount = requiredField<qint64>(
        object.value(), u"byteCount", path, context);
    if (!name.hasValue()) {
        return Result<BridgePresencePetFile>::failure(
            name.error());
    }
    if (!sha256.hasValue()) {
        return Result<BridgePresencePetFile>::failure(
            sha256.error());
    }
    if (!byteCount.hasValue()) {
        return Result<BridgePresencePetFile>::failure(
            byteCount.error());
    }
    if (byteCount.value() < 0) {
        return Result<BridgePresencePetFile>::failure(
            invalidField(
                childPath(path, u"byteCount")));
    }
    return Result<BridgePresencePetFile>::success({
        name.value(),
        sha256.value(),
        byteCount.value(),
    });
}

Result<BridgePresencePetAtlas> decodePresencePetAtlas(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            object.error());
    }
    const QJsonValue fileValue =
        object.value().value(u"file");
    if (fileValue.isUndefined()
        || fileValue.isNull()) {
        return Result<BridgePresencePetAtlas>::failure(
            invalidField(childPath(path, u"file")));
    }
    const auto file = decodePresencePetFile(
        fileValue,
        childPath(path, u"file"),
        context);
    const auto cellWidth = requiredField<qint64>(
        object.value(), u"cellWidth", path, context);
    const auto cellHeight = requiredField<qint64>(
        object.value(), u"cellHeight", path, context);
    const auto columns = requiredField<qint64>(
        object.value(), u"columns", path, context);
    const auto rows = requiredField<qint64>(
        object.value(), u"rows", path, context);
    if (!file.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            file.error());
    }
    if (!cellWidth.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            cellWidth.error());
    }
    if (!cellHeight.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            cellHeight.error());
    }
    if (!columns.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            columns.error());
    }
    if (!rows.hasValue()) {
        return Result<BridgePresencePetAtlas>::failure(
            rows.error());
    }
    if (cellWidth.value() < 0) {
        return Result<BridgePresencePetAtlas>::failure(
            invalidField(
                childPath(path, u"cellWidth")));
    }
    if (cellHeight.value() < 0) {
        return Result<BridgePresencePetAtlas>::failure(
            invalidField(
                childPath(path, u"cellHeight")));
    }
    if (columns.value() < 0) {
        return Result<BridgePresencePetAtlas>::failure(
            invalidField(
                childPath(path, u"columns")));
    }
    if (rows.value() < 0) {
        return Result<BridgePresencePetAtlas>::failure(
            invalidField(childPath(path, u"rows")));
    }
    return Result<BridgePresencePetAtlas>::success({
        file.value(),
        cellWidth.value(),
        cellHeight.value(),
        columns.value(),
        rows.value(),
    });
}

Result<BridgePresencePetAnimation>
decodePresencePetAnimation(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            object.error());
    }
    const auto stateValue = requiredField<QString>(
        object.value(), u"state", path, context);
    const auto row = requiredField<qint64>(
        object.value(), u"row", path, context);
    const auto frameCount = requiredField<qint64>(
        object.value(), u"frameCount", path, context);
    const auto posterFrame = requiredField<qint64>(
        object.value(), u"posterFrame", path, context);
    const QJsonValue durationsValue =
        object.value().value(
            u"frameDurationsMilliseconds");
    if (!stateValue.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            stateValue.error());
    }
    if (!row.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            row.error());
    }
    if (!frameCount.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            frameCount.error());
    }
    if (!posterFrame.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            posterFrame.error());
    }
    if (durationsValue.isUndefined()
        || durationsValue.isNull()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            invalidField(childPath(
                path,
                u"frameDurationsMilliseconds")));
    }
    const auto state = parsePresencePetState(
        stateValue.value(),
        childPath(path, u"state"));
    const auto durations = requireArray<qint64>(
        durationsValue,
        childPath(
            path,
            u"frameDurationsMilliseconds"),
        [&context](
            const QJsonValue& item,
            const QString& itemPath) {
            return requireNonNegativeInteger(
                item,
                itemPath,
                context);
        });
    if (!state.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            state.error());
    }
    if (!durations.hasValue()) {
        return Result<
            BridgePresencePetAnimation>::failure(
            durations.error());
    }
    if (row.value() < 0) {
        return Result<
            BridgePresencePetAnimation>::failure(
            invalidField(childPath(path, u"row")));
    }
    if (frameCount.value() < 0) {
        return Result<
            BridgePresencePetAnimation>::failure(
            invalidField(
                childPath(path, u"frameCount")));
    }
    if (posterFrame.value() < 0) {
        return Result<
            BridgePresencePetAnimation>::failure(
            invalidField(
                childPath(path, u"posterFrame")));
    }
    return Result<
        BridgePresencePetAnimation>::success({
        state.value(),
        row.value(),
        frameCount.value(),
        durations.value(),
        posterFrame.value(),
    });
}

Result<BridgePresencePetManifest>
decodePresencePetManifest(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            object.error());
    }
    const auto schemaVersion = requiredField<qint64>(
        object.value(),
        u"schemaVersion",
        path,
        context);
    const auto packageId = requiredField<QString>(
        object.value(), u"packageID", path, context);
    const auto petId = requiredField<QString>(
        object.value(), u"petID", path, context);
    const auto displayName = requiredField<QString>(
        object.value(),
        u"displayName",
        path,
        context);
    const auto assetVersion = requiredField<QString>(
        object.value(),
        u"assetVersion",
        path,
        context);
    const auto contentHash = requiredField<QString>(
        object.value(),
        u"contentHash",
        path,
        context);
    const QJsonValue atlasValue =
        object.value().value(u"atlas");
    const QJsonValue thumbnailValue =
        object.value().value(u"thumbnail");
    const QJsonValue animationsValue =
        object.value().value(u"animations");
    if (!schemaVersion.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            schemaVersion.error());
    }
    if (!packageId.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            packageId.error());
    }
    if (!petId.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            petId.error());
    }
    if (!displayName.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            displayName.error());
    }
    if (!assetVersion.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            assetVersion.error());
    }
    if (!contentHash.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            contentHash.error());
    }
    if (atlasValue.isUndefined()
        || atlasValue.isNull()) {
        return Result<
            BridgePresencePetManifest>::failure(
            invalidField(childPath(path, u"atlas")));
    }
    if (thumbnailValue.isUndefined()
        || thumbnailValue.isNull()) {
        return Result<
            BridgePresencePetManifest>::failure(
            invalidField(
                childPath(path, u"thumbnail")));
    }
    if (animationsValue.isUndefined()
        || animationsValue.isNull()) {
        return Result<
            BridgePresencePetManifest>::failure(
            invalidField(
                childPath(path, u"animations")));
    }
    const auto atlas = decodePresencePetAtlas(
        atlasValue,
        childPath(path, u"atlas"),
        context);
    const auto thumbnail = decodePresencePetFile(
        thumbnailValue,
        childPath(path, u"thumbnail"),
        context);
    const auto animations =
        requireArray<BridgePresencePetAnimation>(
            animationsValue,
            childPath(path, u"animations"),
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return decodePresencePetAnimation(
                    item,
                    itemPath,
                    context);
            });
    if (!atlas.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            atlas.error());
    }
    if (!thumbnail.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            thumbnail.error());
    }
    if (!animations.hasValue()) {
        return Result<
            BridgePresencePetManifest>::failure(
            animations.error());
    }
    if (schemaVersion.value() < 0) {
        return Result<
            BridgePresencePetManifest>::failure(
            invalidField(childPath(
                path,
                u"schemaVersion")));
    }
    return Result<
        BridgePresencePetManifest>::success({
        schemaVersion.value(),
        packageId.value(),
        petId.value(),
        displayName.value(),
        assetVersion.value(),
        atlas.value(),
        thumbnail.value(),
        animations.value(),
        contentHash.value(),
    });
}

Result<BridgePresencePetCatalogEntry>
decodePresencePetCatalogEntry(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            object.error());
    }
    const auto packageId = requiredField<QString>(
        object.value(), u"packageID", path, context);
    const auto petId = requiredField<QString>(
        object.value(), u"petID", path, context);
    const auto displayName = requiredField<QString>(
        object.value(),
        u"displayName",
        path,
        context);
    const auto assetVersion = requiredField<QString>(
        object.value(),
        u"assetVersion",
        path,
        context);
    const auto contentHash = requiredField<QString>(
        object.value(),
        u"contentHash",
        path,
        context);
    const auto byteCount = requiredField<qint64>(
        object.value(), u"byteCount", path, context);
    const QJsonValue thumbnailValue =
        object.value().value(u"thumbnail");
    if (!packageId.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            packageId.error());
    }
    if (!petId.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            petId.error());
    }
    if (!displayName.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            displayName.error());
    }
    if (!assetVersion.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            assetVersion.error());
    }
    if (!contentHash.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            contentHash.error());
    }
    if (!byteCount.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            byteCount.error());
    }
    if (thumbnailValue.isUndefined()
        || thumbnailValue.isNull()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            invalidField(
                childPath(path, u"thumbnail")));
    }
    const auto thumbnail = decodePresencePetFile(
        thumbnailValue,
        childPath(path, u"thumbnail"),
        context);
    if (!thumbnail.hasValue()) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            thumbnail.error());
    }
    if (byteCount.value() < 0) {
        return Result<
            BridgePresencePetCatalogEntry>::failure(
            invalidField(
                childPath(path, u"byteCount")));
    }
    return Result<
        BridgePresencePetCatalogEntry>::success({
        packageId.value(),
        petId.value(),
        displayName.value(),
        assetVersion.value(),
        contentHash.value(),
        byteCount.value(),
        thumbnail.value(),
    });
}

Result<BridgePresencePetChunk> decodePresencePetChunk(
    const QJsonValue& value,
    const QString& path,
    const DecodeContext& context)
{
    const auto object = requireObject(value, path);
    if (!object.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            object.error());
    }
    const auto packageId = requiredField<QString>(
        object.value(), u"packageID", path, context);
    const auto contentHash = requiredField<QString>(
        object.value(),
        u"contentHash",
        path,
        context);
    const auto fileName = requiredField<QString>(
        object.value(), u"fileName", path, context);
    const auto offset = requiredField<qint64>(
        object.value(), u"offset", path, context);
    const auto nextOffset = requiredField<qint64>(
        object.value(), u"nextOffset", path, context);
    const auto isComplete = requiredField<bool>(
        object.value(), u"isComplete", path, context);
    const QJsonValue dataValue =
        object.value().value(u"data");
    if (!packageId.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            packageId.error());
    }
    if (!contentHash.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            contentHash.error());
    }
    if (!fileName.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            fileName.error());
    }
    if (!offset.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            offset.error());
    }
    if (!nextOffset.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            nextOffset.error());
    }
    if (!isComplete.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            isComplete.error());
    }
    if (dataValue.isUndefined()
        || dataValue.isNull()) {
        return Result<BridgePresencePetChunk>::failure(
            invalidField(childPath(path, u"data")));
    }
    const auto data = requireBase64(
        dataValue,
        childPath(path, u"data"));
    if (!data.hasValue()) {
        return Result<BridgePresencePetChunk>::failure(
            data.error());
    }
    if (offset.value() < 0) {
        return Result<BridgePresencePetChunk>::failure(
            invalidField(childPath(path, u"offset")));
    }
    if (nextOffset.value() < 0) {
        return Result<BridgePresencePetChunk>::failure(
            invalidField(
                childPath(path, u"nextOffset")));
    }
    return Result<BridgePresencePetChunk>::success({
        packageId.value(),
        contentHash.value(),
        fileName.value(),
        offset.value(),
        data.value(),
        nextOffset.value(),
        isComplete.value(),
    });
}

Result<BridgeRequest> decodeRequestObject(const QJsonObject& object, const DecodeContext& context)
{
    const auto id = requiredField<QUuid>(object, u"id", {}, context);
    const auto protocolVersion = requiredField<qint64>(object, u"protocolVersion", {}, context);
    const auto operationTextValue = requiredField<QString>(object, u"operation", {}, context);
    if (!id.hasValue()) return Result<BridgeRequest>::failure(id.error());
    if (!protocolVersion.hasValue()) return Result<BridgeRequest>::failure(protocolVersion.error());
    if (!operationTextValue.hasValue()) return Result<BridgeRequest>::failure(operationTextValue.error());
    const auto operation = parseOperation(operationTextValue.value(), QStringLiteral("operation"));
    if (!operation.hasValue()) return Result<BridgeRequest>::failure(operation.error());

    const auto stringOptional = [&object](QStringView key) {
        return optionalField<QString>(object, key, {},
            [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    };
    const auto cursor = stringOptional(u"cursor");
    const auto threadId = stringOptional(u"threadID");
    const auto goalObjective = stringOptional(u"goalObjective");
    const auto text = stringOptional(u"text");
    const auto cwd = stringOptional(u"cwd");
    const auto model = stringOptional(u"model");
    const auto reasoningEffort = stringOptional(u"reasoningEffort");
    const auto skillName = stringOptional(u"skillName");
    const auto skillPath = stringOptional(u"skillPath");
    const auto chatAgentId = stringOptional(u"chatAgentID");
    const auto chatModelId = stringOptional(u"chatModelID");
    const auto resetCreditId = stringOptional(u"resetCreditID");
    const auto integerOptional = [&object, &context](QStringView key) {
        return optionalField<qint64>(object, key, {},
            [&context](const QJsonValue& item, const QString& itemPath) {
                return requireInteger(item, itemPath, context);
            });
    };
    const auto limit = integerOptional(u"limit");
    const auto goalTokenBudget = integerOptional(u"goalTokenBudget");
    const auto sendAction = optionalField<SendAction>(object, u"sendAction", {},
        [](const QJsonValue& item, const QString& itemPath) {
            const auto textValue = requireString(item, itemPath);
            return textValue.hasValue() ? parseSendAction(textValue.value(), itemPath)
                                        : Result<SendAction>::failure(textValue.error());
        });
    const auto approvalDecision = optionalField<ApprovalDecision>(object, u"approvalDecision", {},
        [](const QJsonValue& item, const QString& itemPath) {
            const auto textValue = requireString(item, itemPath);
            return textValue.hasValue() ? parseApprovalDecision(textValue.value(), itemPath)
                                        : Result<ApprovalDecision>::failure(textValue.error());
        });
    const auto chatProvider = optionalField<ChatProvider>(object, u"chatProvider", {},
        [](const QJsonValue& item, const QString& itemPath) {
            const auto textValue = requireString(item, itemPath);
            return textValue.hasValue() ? parseChatProvider(textValue.value(), itemPath)
                                        : Result<ChatProvider>::failure(textValue.error());
        });
    const auto presencePetPackageId =
        stringOptional(u"presencePetPackageID");
    const auto presencePetContentHash =
        stringOptional(u"presencePetContentHash");
    const auto presencePetFileName =
        stringOptional(u"presencePetFileName");
    const auto presencePetOffset =
        optionalField<qint64>(
            object,
            u"presencePetOffset",
            {},
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return requireNonNegativeInteger(
                    item,
                    itemPath,
                    context);
            });
    const auto presencePetLength =
        optionalField<qint64>(
            object,
            u"presencePetLength",
            {},
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return requireNonNegativeInteger(
                    item,
                    itemPath,
                    context);
            });
    const auto attachments = optionalArray<BridgeAttachment>(object, u"attachments", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeAttachment(item, itemPath, context); });
    const auto idempotencyKey = optionalField<QUuid>(object, u"idempotencyKey", {},
        [](const QJsonValue& item, const QString& itemPath) { return requireUuid(item, itemPath); });
    const bool valid = cursor.hasValue() && limit.hasValue() && threadId.hasValue()
        && goalObjective.hasValue() && goalTokenBudget.hasValue() && text.hasValue()
        && cwd.hasValue() && sendAction.hasValue() && approvalDecision.hasValue()
        && model.hasValue() && reasoningEffort.hasValue() && skillName.hasValue()
        && skillPath.hasValue() && chatAgentId.hasValue() && chatProvider.hasValue()
        && chatModelId.hasValue() && resetCreditId.hasValue() && attachments.hasValue()
        && idempotencyKey.hasValue() && presencePetPackageId.hasValue()
        && presencePetContentHash.hasValue() && presencePetFileName.hasValue()
        && presencePetOffset.hasValue() && presencePetLength.hasValue();
    if (!valid) {
        const auto firstError = [&]() -> CompanionError {
            if (!cursor.hasValue()) return cursor.error();
            if (!limit.hasValue()) return limit.error();
            if (!threadId.hasValue()) return threadId.error();
            if (!goalObjective.hasValue()) return goalObjective.error();
            if (!goalTokenBudget.hasValue()) return goalTokenBudget.error();
            if (!text.hasValue()) return text.error();
            if (!cwd.hasValue()) return cwd.error();
            if (!sendAction.hasValue()) return sendAction.error();
            if (!approvalDecision.hasValue()) return approvalDecision.error();
            if (!model.hasValue()) return model.error();
            if (!reasoningEffort.hasValue()) return reasoningEffort.error();
            if (!skillName.hasValue()) return skillName.error();
            if (!skillPath.hasValue()) return skillPath.error();
            if (!chatAgentId.hasValue()) return chatAgentId.error();
            if (!chatProvider.hasValue()) return chatProvider.error();
            if (!chatModelId.hasValue()) return chatModelId.error();
            if (!resetCreditId.hasValue()) return resetCreditId.error();
            if (!attachments.hasValue()) return attachments.error();
            if (!idempotencyKey.hasValue()) return idempotencyKey.error();
            if (!presencePetPackageId.hasValue()) return presencePetPackageId.error();
            if (!presencePetContentHash.hasValue()) return presencePetContentHash.error();
            if (!presencePetFileName.hasValue()) return presencePetFileName.error();
            if (!presencePetOffset.hasValue()) return presencePetOffset.error();
            return presencePetLength.error();
        };
        return Result<BridgeRequest>::failure(firstError());
    }
    return Result<BridgeRequest>::success({
        id.value(), protocolVersion.value(), operation.value(), cursor.value(), limit.value(),
        threadId.value(), goalObjective.value(), goalTokenBudget.value(), text.value(),
        cwd.value(), sendAction.value(), approvalDecision.value(), model.value(),
        reasoningEffort.value(), skillName.value(), skillPath.value(), chatAgentId.value(),
        chatProvider.value(), chatModelId.value(), resetCreditId.value(), attachments.value(),
        idempotencyKey.value(), presencePetPackageId.value(),
        presencePetContentHash.value(), presencePetFileName.value(),
        presencePetOffset.value(), presencePetLength.value(),
    });
}

Result<BridgeResponse> decodeResponseObject(const QJsonObject& object, const DecodeContext& context)
{
    const auto id = requiredField<QUuid>(object, u"id", {}, context);
    const auto protocolVersion = requiredField<qint64>(object, u"protocolVersion", {}, context);
    const auto operationTextValue = requiredField<QString>(object, u"operation", {}, context);
    const auto succeeded = requiredField<bool>(object, u"succeeded", {}, context);
    if (!id.hasValue()) return Result<BridgeResponse>::failure(id.error());
    if (!protocolVersion.hasValue()) return Result<BridgeResponse>::failure(protocolVersion.error());
    if (!operationTextValue.hasValue()) return Result<BridgeResponse>::failure(operationTextValue.error());
    if (!succeeded.hasValue()) return Result<BridgeResponse>::failure(succeeded.error());
    const auto operation = parseOperation(operationTextValue.value(), QStringLiteral("operation"));
    if (!operation.hasValue()) return Result<BridgeResponse>::failure(operation.error());

    const auto stringOptional = [&object](QStringView key) {
        return optionalField<QString>(object, key, {},
            [](const QJsonValue& item, const QString& itemPath) { return requireString(item, itemPath); });
    };
    const auto errorCode = stringOptional(u"errorCode");
    const auto message = stringOptional(u"message");
    const auto macName = stringOptional(u"macName");
    const auto macDeviceId = stringOptional(u"macDeviceID");
    const auto relayUrlString = stringOptional(u"relayURLString");
    const auto nextCursor = stringOptional(u"nextCursor");
    const auto threadId = stringOptional(u"threadID");
    const auto revision = stringOptional(u"revision");
    const auto timelineNextCursor = stringOptional(u"timelineNextCursor");
    const auto pairingSecret = optionalField<QByteArray>(object, u"pairingSecret", {},
        [](const QJsonValue& item, const QString& itemPath) { return requireBase64(item, itemPath); });
    const auto tasks = optionalArray<BridgeTask>(object, u"tasks", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeTask(item, itemPath, context); });
    const auto messages = optionalArray<BridgeMessage>(object, u"messages", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeMessage(item, itemPath, context); });
    const auto capabilities = optionalField<BridgeCapabilities>(object, u"capabilities", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeCapabilities(item, itemPath, context); });
    const auto chatMessage = optionalField<BridgeMessage>(object, u"chatMessage", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeMessage(item, itemPath, context); });
    const auto timelineItems = optionalArray<BridgeTimelineItem>(object, u"timelineItems", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeTimelineItem(item, itemPath, context); });
    const auto subagents = optionalArray<BridgeSubagent>(object, u"subagents", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeSubagent(item, itemPath, context); });
    const auto contextUsage = optionalField<BridgeContextUsage>(object, u"contextUsage", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeContextUsage(item, itemPath, context); });
    const auto usageSnapshot = optionalField<BridgeUsageSnapshot>(object, u"usageSnapshot", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeUsageSnapshot(item, itemPath, context); });
    const auto goal = optionalField<BridgeGoal>(object, u"goal", {},
        [&context](const QJsonValue& item, const QString& itemPath) { return decodeGoal(item, itemPath, context); });
    const auto features = optionalArray<BridgeFeature>(
        object,
        u"features",
        {},
        [](const QJsonValue& item, const QString& itemPath) {
            const auto textValue =
                requireString(item, itemPath);
            return textValue.hasValue()
                ? parseFeature(
                    textValue.value(), itemPath)
                : Result<BridgeFeature>::failure(
                    textValue.error());
        });
    const auto selectedDesktopPetId =
        stringOptional(u"selectedDesktopPetID");
    const auto presencePetCatalog =
        optionalArray<BridgePresencePetCatalogEntry>(
            object,
            u"presencePetCatalog",
            {},
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return decodePresencePetCatalogEntry(
                    item,
                    itemPath,
                    context);
            });
    const auto presencePetManifest =
        optionalField<BridgePresencePetManifest>(
            object,
            u"presencePetManifest",
            {},
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return decodePresencePetManifest(
                    item,
                    itemPath,
                    context);
            });
    const auto presencePetChunk =
        optionalField<BridgePresencePetChunk>(
            object,
            u"presencePetChunk",
            {},
            [&context](
                const QJsonValue& item,
                const QString& itemPath) {
                return decodePresencePetChunk(
                    item,
                    itemPath,
                    context);
            });
    const bool valid = errorCode.hasValue() && message.hasValue() && macName.hasValue()
        && macDeviceId.hasValue() && pairingSecret.hasValue() && relayUrlString.hasValue()
        && tasks.hasValue() && messages.hasValue() && nextCursor.hasValue() && threadId.hasValue()
        && capabilities.hasValue() && chatMessage.hasValue() && timelineItems.hasValue()
        && revision.hasValue() && timelineNextCursor.hasValue() && subagents.hasValue()
        && contextUsage.hasValue() && usageSnapshot.hasValue() && goal.hasValue()
        && features.hasValue() && selectedDesktopPetId.hasValue()
        && presencePetCatalog.hasValue() && presencePetManifest.hasValue()
        && presencePetChunk.hasValue();
    if (!valid) {
        const auto firstError = [&]() -> CompanionError {
            if (!errorCode.hasValue()) return errorCode.error();
            if (!message.hasValue()) return message.error();
            if (!macName.hasValue()) return macName.error();
            if (!macDeviceId.hasValue()) return macDeviceId.error();
            if (!pairingSecret.hasValue()) return pairingSecret.error();
            if (!relayUrlString.hasValue()) return relayUrlString.error();
            if (!tasks.hasValue()) return tasks.error();
            if (!messages.hasValue()) return messages.error();
            if (!nextCursor.hasValue()) return nextCursor.error();
            if (!threadId.hasValue()) return threadId.error();
            if (!capabilities.hasValue()) return capabilities.error();
            if (!chatMessage.hasValue()) return chatMessage.error();
            if (!timelineItems.hasValue()) return timelineItems.error();
            if (!revision.hasValue()) return revision.error();
            if (!timelineNextCursor.hasValue()) return timelineNextCursor.error();
            if (!subagents.hasValue()) return subagents.error();
            if (!contextUsage.hasValue()) return contextUsage.error();
            if (!usageSnapshot.hasValue()) return usageSnapshot.error();
            if (!goal.hasValue()) return goal.error();
            if (!features.hasValue()) return features.error();
            if (!selectedDesktopPetId.hasValue()) return selectedDesktopPetId.error();
            if (!presencePetCatalog.hasValue()) return presencePetCatalog.error();
            if (!presencePetManifest.hasValue()) return presencePetManifest.error();
            return presencePetChunk.error();
        };
        return Result<BridgeResponse>::failure(firstError());
    }
    return Result<BridgeResponse>::success({
        id.value(), protocolVersion.value(), operation.value(), succeeded.value(), errorCode.value(),
        message.value(), macName.value(), macDeviceId.value(), pairingSecret.value(),
        relayUrlString.value(), tasks.value(), messages.value(), nextCursor.value(), threadId.value(),
        capabilities.value(), chatMessage.value(), timelineItems.value(), revision.value(),
        timelineNextCursor.value(), subagents.value(), contextUsage.value(), usageSnapshot.value(),
        goal.value(), features.value(), selectedDesktopPetId.value(),
        presencePetCatalog.value(), presencePetManifest.value(),
        presencePetChunk.value(),
    });
}

QJsonValue encodeFiniteNumber(double value)
{
    // QJsonValue normalizes -0.0 to +0.0. Null is an internal-only marker;
    // bridge model encoding never emits JSON null.
    return value == 0.0 && std::signbit(value)
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(value);
}

Result<QJsonValue> encodeDate(BridgeDate value, BridgeWireProfile profile, const QString& path)
{
    if (!std::isfinite(value.secondsSinceReferenceDate)) {
        return Result<QJsonValue>::failure(invalidField(path));
    }
    const double wireDate = profile == BridgeWireProfile::NearbyV1Milliseconds
        ? (value.secondsSinceReferenceDate + kSwiftReferenceDateUnixSeconds) * 1000.0
        : value.secondsSinceReferenceDate;
    if (!std::isfinite(wireDate)) {
        return Result<QJsonValue>::failure(invalidField(path));
    }
    return Result<QJsonValue>::success(encodeFiniteNumber(wireDate));
}

void putOptional(QJsonObject& object, QStringView key, const std::optional<QString>& value)
{
    if (value.has_value()) object.insert(key, *value);
}

void putOptional(QJsonObject& object, QStringView key, const std::optional<qint64>& value)
{
    if (value.has_value()) object.insert(key, QJsonValue(*value));
}

void putOptional(QJsonObject& object, QStringView key, const std::optional<bool>& value)
{
    if (value.has_value()) object.insert(key, *value);
}

void putOptional(QJsonObject& object, QStringView key, const std::optional<QUuid>& value)
{
    if (value.has_value()) object.insert(key, uuidText(*value));
}

void putOptional(QJsonObject& object, QStringView key, const std::optional<QByteArray>& value)
{
    if (value.has_value()) object.insert(key, QString::fromLatin1(value->toBase64()));
}

Result<QJsonObject> encodeAttachment(const BridgeAttachment& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"id", uuidText(value.id));
    object.insert(u"kind", attachmentKindText(value.kind));
    object.insert(u"filename", value.filename);
    putOptional(object, u"mimeType", value.mimeType);
    object.insert(u"data", QString::fromLatin1(value.data.toBase64()));
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeGoal(const BridgeGoal& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"threadID", value.threadId);
    object.insert(u"objective", value.objective);
    object.insert(u"status", goalStatusText(value.status));
    putOptional(object, u"tokenBudget", value.tokenBudget);
    object.insert(u"tokensUsed", QJsonValue(value.tokensUsed));
    object.insert(u"elapsedSeconds", QJsonValue(value.elapsedSeconds));
    object.insert(u"createdAt", QJsonValue(value.createdAt));
    object.insert(u"updatedAt", QJsonValue(value.updatedAt));
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeTaskGroup(const BridgeTaskGroup& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"kind", taskGroupKindText(value.kind));
    object.insert(u"title", value.title);
    putOptional(object, u"path", value.path);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeTask(const BridgeTask& value, BridgeWireProfile profile, const QString& path)
{
    const auto updatedAt = encodeDate(value.updatedAt, profile, childPath(path, u"updatedAt"));
    if (!updatedAt.hasValue()) return Result<QJsonObject>::failure(updatedAt.error());
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"title", value.title);
    object.insert(u"preview", value.preview);
    object.insert(u"updatedAt", updatedAt.value());
    putOptional(object, u"cwd", value.cwd);
    object.insert(u"status", taskStatusText(value.status));
    object.insert(u"needsApproval", value.needsApproval);
    putOptional(object, u"activeTurnID", value.activeTurnId);
    putOptional(object, u"model", value.model);
    putOptional(object, u"reasoningEffort", value.reasoningEffort);
    if (value.taskGroup.has_value()) {
        const auto taskGroup = encodeTaskGroup(*value.taskGroup, profile, childPath(path, u"taskGroup"));
        if (!taskGroup.hasValue()) return Result<QJsonObject>::failure(taskGroup.error());
        object.insert(u"taskGroup", taskGroup.value());
    }
    if (value.goal.has_value()) {
        const auto goal = encodeGoal(*value.goal, profile, childPath(path, u"goal"));
        if (!goal.hasValue()) return Result<QJsonObject>::failure(goal.error());
        object.insert(u"goal", goal.value());
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeMessage(const BridgeMessage& value, BridgeWireProfile profile, const QString& path)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"role", messageRoleText(value.role));
    object.insert(u"text", value.text);
    if (value.createdAt.has_value()) {
        const auto createdAt = encodeDate(*value.createdAt, profile, childPath(path, u"createdAt"));
        if (!createdAt.hasValue()) return Result<QJsonObject>::failure(createdAt.error());
        object.insert(u"createdAt", createdAt.value());
    }
    if (value.attachments.has_value()) {
        QJsonArray attachments;
        for (qsizetype index = 0; index < value.attachments->size(); ++index) {
            const auto encoded = encodeAttachment(
                value.attachments->at(index), profile,
                childPath(path, u"attachments") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!encoded.hasValue()) return Result<QJsonObject>::failure(encoded.error());
            attachments.append(encoded.value());
        }
        object.insert(u"attachments", attachments);
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeMedia(const BridgeMedia& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"kind", mediaKindText(value.kind));
    object.insert(u"mimeType", value.mimeType);
    object.insert(u"data", QString::fromLatin1(value.data.toBase64()));
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeTimelineItem(
    const BridgeTimelineItem& value,
    BridgeWireProfile profile,
    const QString& path)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"kind", timelineKindText(value.kind));
    object.insert(u"status", timelineStatusText(value.status));
    if (value.role.has_value()) object.insert(u"role", messageRoleText(*value.role));
    putOptional(object, u"title", value.title);
    putOptional(object, u"text", value.text);
    putOptional(object, u"detail", value.detail);
    if (value.phase.has_value()) object.insert(u"phase", timelinePhaseText(*value.phase));
    if (value.createdAt.has_value()) {
        const auto createdAt = encodeDate(*value.createdAt, profile, childPath(path, u"createdAt"));
        if (!createdAt.hasValue()) return Result<QJsonObject>::failure(createdAt.error());
        object.insert(u"createdAt", createdAt.value());
    }
    putOptional(object, u"turnID", value.turnId);
    putOptional(object, u"callID", value.callId);
    QJsonArray media;
    for (qsizetype index = 0; index < value.media.size(); ++index) {
        const auto encoded = encodeMedia(
            value.media.at(index), profile,
            childPath(path, u"media") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!encoded.hasValue()) return Result<QJsonObject>::failure(encoded.error());
        media.append(encoded.value());
    }
    object.insert(u"media", media);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeSubagent(const BridgeSubagent& value, BridgeWireProfile profile, const QString& path)
{
    const auto updatedAt = encodeDate(value.updatedAt, profile, childPath(path, u"updatedAt"));
    if (!updatedAt.hasValue()) return Result<QJsonObject>::failure(updatedAt.error());
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"name", value.name);
    object.insert(u"title", value.title);
    putOptional(object, u"role", value.role);
    object.insert(u"updatedAt", updatedAt.value());
    object.insert(u"status", taskStatusText(value.status));
    putOptional(object, u"needsApproval", value.needsApproval);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeContextUsage(const BridgeContextUsage& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"usedTokens", QJsonValue(value.usedTokens));
    object.insert(u"contextWindow", QJsonValue(value.contextWindow));
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeUsageWindow(
    const BridgeUsageWindow& value,
    BridgeWireProfile profile,
    const QString& path)
{
    if (!std::isfinite(value.remainingPercent)) {
        return Result<QJsonObject>::failure(invalidField(childPath(path, u"remainingPercent")));
    }
    QJsonObject object;
    object.insert(u"remainingPercent", encodeFiniteNumber(value.remainingPercent));
    object.insert(u"durationLabel", value.durationLabel);
    if (value.resetsAt.has_value()) {
        const auto resetsAt = encodeDate(*value.resetsAt, profile, childPath(path, u"resetsAt"));
        if (!resetsAt.hasValue()) return Result<QJsonObject>::failure(resetsAt.error());
        object.insert(u"resetsAt", resetsAt.value());
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeResetCredit(
    const BridgeResetCredit& value,
    BridgeWireProfile profile,
    const QString& path)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"displayTitle", value.displayTitle);
    putOptional(object, u"detail", value.detail);
    if (value.expiresAt.has_value()) {
        const auto expiresAt = encodeDate(*value.expiresAt, profile, childPath(path, u"expiresAt"));
        if (!expiresAt.hasValue()) return Result<QJsonObject>::failure(expiresAt.error());
        object.insert(u"expiresAt", expiresAt.value());
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeUsageGroup(
    const BridgeUsageGroup& value,
    BridgeWireProfile profile,
    const QString& path)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"title", value.title);
    if (value.shortWindow.has_value()) {
        const auto shortWindow = encodeUsageWindow(*value.shortWindow, profile, childPath(path, u"shortWindow"));
        if (!shortWindow.hasValue()) return Result<QJsonObject>::failure(shortWindow.error());
        object.insert(u"shortWindow", shortWindow.value());
    }
    if (value.weeklyWindow.has_value()) {
        const auto weeklyWindow = encodeUsageWindow(*value.weeklyWindow, profile, childPath(path, u"weeklyWindow"));
        if (!weeklyWindow.hasValue()) return Result<QJsonObject>::failure(weeklyWindow.error());
        object.insert(u"weeklyWindow", weeklyWindow.value());
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeUsageSnapshot(
    const BridgeUsageSnapshot& value,
    BridgeWireProfile profile,
    const QString& path)
{
    const auto updatedAt = encodeDate(value.updatedAt, profile, childPath(path, u"updatedAt"));
    if (!updatedAt.hasValue()) return Result<QJsonObject>::failure(updatedAt.error());
    QJsonObject object;
    putOptional(object, u"planType", value.planType);
    QJsonArray groups;
    for (qsizetype index = 0; index < value.groups.size(); ++index) {
        const auto group = encodeUsageGroup(
            value.groups.at(index), profile,
            childPath(path, u"groups") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!group.hasValue()) return Result<QJsonObject>::failure(group.error());
        groups.append(group.value());
    }
    object.insert(u"groups", groups);
    object.insert(u"availableResetCount", QJsonValue(value.availableResetCount));
    QJsonArray credits;
    for (qsizetype index = 0; index < value.availableResetCredits.size(); ++index) {
        const auto credit = encodeResetCredit(
            value.availableResetCredits.at(index), profile,
            childPath(path, u"availableResetCredits") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!credit.hasValue()) return Result<QJsonObject>::failure(credit.error());
        credits.append(credit.value());
    }
    object.insert(u"availableResetCredits", credits);
    object.insert(u"updatedAt", updatedAt.value());
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeReasoningEffort(const BridgeReasoningEffort& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"value", value.value);
    object.insert(u"description", value.description);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeModel(const BridgeModel& value, BridgeWireProfile profile, const QString& path)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"model", value.model);
    object.insert(u"displayName", value.displayName);
    object.insert(u"description", value.description);
    object.insert(u"isDefault", value.isDefault);
    object.insert(u"defaultReasoningEffort", value.defaultReasoningEffort);
    QJsonArray efforts;
    for (qsizetype index = 0; index < value.supportedReasoningEfforts.size(); ++index) {
        const auto effort = encodeReasoningEffort(
            value.supportedReasoningEfforts.at(index), profile,
            childPath(path, u"supportedReasoningEfforts") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!effort.hasValue()) return Result<QJsonObject>::failure(effort.error());
        efforts.append(effort.value());
    }
    object.insert(u"supportedReasoningEfforts", efforts);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeSkill(const BridgeSkill& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"name", value.name);
    object.insert(u"displayName", value.displayName);
    object.insert(u"description", value.description);
    object.insert(u"path", value.path);
    object.insert(u"scope", value.scope);
    putOptional(object, u"defaultPrompt", value.defaultPrompt);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodePlugin(const BridgePlugin& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"name", value.name);
    object.insert(u"displayName", value.displayName);
    object.insert(u"description", value.description);
    object.insert(u"enabled", value.enabled);
    object.insert(u"installed", value.installed);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeChatAgent(const BridgeChatAgent& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"name", value.name);
    object.insert(u"description", value.description);
    object.insert(u"symbolName", value.symbolName);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeChatModel(const BridgeChatModel& value, BridgeWireProfile, const QString&)
{
    QJsonObject object;
    object.insert(u"id", value.id);
    object.insert(u"provider", chatProviderText(value.provider));
    object.insert(u"model", value.model);
    object.insert(u"displayName", value.displayName);
    object.insert(u"description", value.description);
    object.insert(u"isDefault", value.isDefault);
    object.insert(u"isAvailable", value.isAvailable);
    object.insert(u"supportsAttachments", value.supportsAttachments);
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodeCapabilities(
    const BridgeCapabilities& value,
    BridgeWireProfile profile,
    const QString& path)
{
    QJsonObject object;
    QJsonArray models;
    for (qsizetype index = 0; index < value.models.size(); ++index) {
        const auto model = encodeModel(value.models.at(index), profile,
            childPath(path, u"models") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!model.hasValue()) return Result<QJsonObject>::failure(model.error());
        models.append(model.value());
    }
    object.insert(u"models", models);
    QJsonArray skills;
    for (qsizetype index = 0; index < value.skills.size(); ++index) {
        const auto skill = encodeSkill(value.skills.at(index), profile,
            childPath(path, u"skills") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!skill.hasValue()) return Result<QJsonObject>::failure(skill.error());
        skills.append(skill.value());
    }
    object.insert(u"skills", skills);
    QJsonArray plugins;
    for (qsizetype index = 0; index < value.plugins.size(); ++index) {
        const auto plugin = encodePlugin(value.plugins.at(index), profile,
            childPath(path, u"plugins") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!plugin.hasValue()) return Result<QJsonObject>::failure(plugin.error());
        plugins.append(plugin.value());
    }
    object.insert(u"plugins", plugins);
    QJsonArray agents;
    for (qsizetype index = 0; index < value.chatAgents.size(); ++index) {
        const auto agent = encodeChatAgent(value.chatAgents.at(index), profile,
            childPath(path, u"chatAgents") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
        if (!agent.hasValue()) return Result<QJsonObject>::failure(agent.error());
        agents.append(agent.value());
    }
    object.insert(u"chatAgents", agents);
    if (value.chatModels.has_value()) {
        QJsonArray modelsValue;
        for (qsizetype index = 0; index < value.chatModels->size(); ++index) {
            const auto model = encodeChatModel(value.chatModels->at(index), profile,
                childPath(path, u"chatModels") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!model.hasValue()) return Result<QJsonObject>::failure(model.error());
            modelsValue.append(model.value());
        }
        object.insert(u"chatModels", modelsValue);
    }
    return Result<QJsonObject>::success(std::move(object));
}

Result<QJsonObject> encodePresencePetFile(
    const BridgePresencePetFile& value,
    BridgeWireProfile,
    const QString& path)
{
    if (value.byteCount < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"byteCount")));
    }
    QJsonObject object;
    object.insert(u"name", value.name);
    object.insert(u"sha256", value.sha256);
    object.insert(
        u"byteCount",
        QJsonValue(value.byteCount));
    return Result<QJsonObject>::success(
        std::move(object));
}

Result<QJsonObject> encodePresencePetAtlas(
    const BridgePresencePetAtlas& value,
    BridgeWireProfile profile,
    const QString& path)
{
    if (value.cellWidth < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"cellWidth")));
    }
    if (value.cellHeight < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"cellHeight")));
    }
    if (value.columns < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"columns")));
    }
    if (value.rows < 0) {
        return Result<QJsonObject>::failure(
            invalidField(childPath(path, u"rows")));
    }
    const auto file = encodePresencePetFile(
        value.file,
        profile,
        childPath(path, u"file"));
    if (!file.hasValue()) {
        return Result<QJsonObject>::failure(
            file.error());
    }
    QJsonObject object;
    object.insert(u"file", file.value());
    object.insert(
        u"cellWidth",
        QJsonValue(value.cellWidth));
    object.insert(
        u"cellHeight",
        QJsonValue(value.cellHeight));
    object.insert(
        u"columns",
        QJsonValue(value.columns));
    object.insert(u"rows", QJsonValue(value.rows));
    return Result<QJsonObject>::success(
        std::move(object));
}

Result<QJsonObject> encodePresencePetAnimation(
    const BridgePresencePetAnimation& value,
    BridgeWireProfile,
    const QString& path)
{
    if (value.row < 0) {
        return Result<QJsonObject>::failure(
            invalidField(childPath(path, u"row")));
    }
    if (value.frameCount < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"frameCount")));
    }
    if (value.posterFrame < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"posterFrame")));
    }
    QJsonArray durations;
    for (qsizetype index = 0;
         index
         < value.frameDurationsMilliseconds.size();
         ++index) {
        const qint64 duration =
            value.frameDurationsMilliseconds.at(index);
        if (duration < 0) {
            return Result<QJsonObject>::failure(
                invalidField(
                    childPath(
                        path,
                        u"frameDurationsMilliseconds")
                    + QLatin1Char('[')
                    + QString::number(index)
                    + QLatin1Char(']')));
        }
        durations.append(QJsonValue(duration));
    }
    QJsonObject object;
    object.insert(
        u"state",
        presencePetStateText(value.state));
    object.insert(u"row", QJsonValue(value.row));
    object.insert(
        u"frameCount",
        QJsonValue(value.frameCount));
    object.insert(
        u"frameDurationsMilliseconds",
        durations);
    object.insert(
        u"posterFrame",
        QJsonValue(value.posterFrame));
    return Result<QJsonObject>::success(
        std::move(object));
}

Result<QJsonObject> encodePresencePetManifest(
    const BridgePresencePetManifest& value,
    BridgeWireProfile profile,
    const QString& path)
{
    if (value.schemaVersion < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"schemaVersion")));
    }
    const auto atlas = encodePresencePetAtlas(
        value.atlas,
        profile,
        childPath(path, u"atlas"));
    if (!atlas.hasValue()) {
        return Result<QJsonObject>::failure(
            atlas.error());
    }
    const auto thumbnail = encodePresencePetFile(
        value.thumbnail,
        profile,
        childPath(path, u"thumbnail"));
    if (!thumbnail.hasValue()) {
        return Result<QJsonObject>::failure(
            thumbnail.error());
    }
    QJsonArray animations;
    for (qsizetype index = 0;
         index < value.animations.size();
         ++index) {
        const auto animation =
            encodePresencePetAnimation(
                value.animations.at(index),
                profile,
                childPath(path, u"animations")
                    + QLatin1Char('[')
                    + QString::number(index)
                    + QLatin1Char(']'));
        if (!animation.hasValue()) {
            return Result<QJsonObject>::failure(
                animation.error());
        }
        animations.append(animation.value());
    }
    QJsonObject object;
    object.insert(
        u"schemaVersion",
        QJsonValue(value.schemaVersion));
    object.insert(u"packageID", value.packageId);
    object.insert(u"petID", value.petId);
    object.insert(u"displayName", value.displayName);
    object.insert(u"assetVersion", value.assetVersion);
    object.insert(u"atlas", atlas.value());
    object.insert(u"thumbnail", thumbnail.value());
    object.insert(u"animations", animations);
    object.insert(u"contentHash", value.contentHash);
    return Result<QJsonObject>::success(
        std::move(object));
}

Result<QJsonObject> encodePresencePetCatalogEntry(
    const BridgePresencePetCatalogEntry& value,
    BridgeWireProfile profile,
    const QString& path)
{
    if (value.byteCount < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"byteCount")));
    }
    const auto thumbnail = encodePresencePetFile(
        value.thumbnail,
        profile,
        childPath(path, u"thumbnail"));
    if (!thumbnail.hasValue()) {
        return Result<QJsonObject>::failure(
            thumbnail.error());
    }
    QJsonObject object;
    object.insert(u"packageID", value.packageId);
    object.insert(u"petID", value.petId);
    object.insert(u"displayName", value.displayName);
    object.insert(u"assetVersion", value.assetVersion);
    object.insert(u"contentHash", value.contentHash);
    object.insert(
        u"byteCount",
        QJsonValue(value.byteCount));
    object.insert(u"thumbnail", thumbnail.value());
    return Result<QJsonObject>::success(
        std::move(object));
}

Result<QJsonObject> encodePresencePetChunk(
    const BridgePresencePetChunk& value,
    BridgeWireProfile,
    const QString& path)
{
    if (value.offset < 0) {
        return Result<QJsonObject>::failure(
            invalidField(childPath(path, u"offset")));
    }
    if (value.nextOffset < 0) {
        return Result<QJsonObject>::failure(
            invalidField(
                childPath(path, u"nextOffset")));
    }
    QJsonObject object;
    object.insert(u"packageID", value.packageId);
    object.insert(u"contentHash", value.contentHash);
    object.insert(u"fileName", value.fileName);
    object.insert(u"offset", QJsonValue(value.offset));
    object.insert(
        u"data",
        QString::fromLatin1(value.data.toBase64()));
    object.insert(
        u"nextOffset",
        QJsonValue(value.nextOffset));
    object.insert(u"isComplete", value.isComplete);
    return Result<QJsonObject>::success(
        std::move(object));
}

QByteArray encodeCanonicalString(const QString& value)
{
    QJsonArray scalar;
    scalar.append(value);
    const QByteArray bytes =
        QJsonDocument(scalar).toJson(QJsonDocument::Compact);
    QByteArray encoded = bytes.mid(1, bytes.size() - 2);
    encoded.replace("/", "\\/");
    return encoded;
}

QByteArray encodeSwiftDouble(double value)
{
    QJsonArray scalar;
    scalar.append(value);
    const QByteArray encoded =
        QJsonDocument(scalar).toJson(QJsonDocument::Compact);
    QByteArray token = encoded.mid(1, encoded.size() - 2);

    constexpr double kLargestFixedSwiftInteger = 9007199254740992.0;
    if (std::abs(value) <= kLargestFixedSwiftInteger
        || token.contains('e') || token.contains('E')) {
        return token;
    }

    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::scientific);
    Q_ASSERT(error == std::errc());
    return error == std::errc()
        ? QByteArray(buffer.data(), end - buffer.data())
        : token;
}

QByteArray encodeJson(const QJsonValue& value, bool sortObjectKeys)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QStringList keys;
        keys.reserve(object.size());
        for (auto iterator = object.constBegin(); iterator != object.constEnd();
             ++iterator) {
            keys.append(iterator.key());
        }
        if (sortObjectKeys) {
            std::sort(keys.begin(), keys.end());
        }
        QByteArray output("{");
        for (qsizetype index = 0; index < keys.size(); ++index) {
            if (index > 0) output.append(',');
            output.append(encodeCanonicalString(keys.at(index)));
            output.append(':');
            output.append(
                encodeJson(object.value(keys.at(index)), sortObjectKeys));
        }
        output.append('}');
        return output;
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        QByteArray output("[");
        for (qsizetype index = 0; index < array.size(); ++index) {
            if (index > 0) output.append(',');
            output.append(encodeJson(array.at(index), sortObjectKeys));
        }
        output.append(']');
        return output;
    }
    if (value.isString()) {
        return encodeCanonicalString(value.toString());
    }
    if (value.isNull()) {
        return QByteArrayLiteral("-0");
    }
    if (value.isDouble()
        && value.toVariant().metaType().id() == QMetaType::Double) {
        return encodeSwiftDouble(value.toDouble());
    }
    QJsonArray scalar;
    scalar.append(value);
    const QByteArray bytes = QJsonDocument(scalar).toJson(QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2);
}

Result<QByteArray> encodeRequestObject(const BridgeRequest& value, BridgeWireProfile profile)
{
    if (value.presencePetOffset.has_value()
        && *value.presencePetOffset < 0) {
        return Result<QByteArray>::failure(
            invalidField(
                QStringLiteral("presencePetOffset")));
    }
    if (value.presencePetLength.has_value()
        && *value.presencePetLength < 0) {
        return Result<QByteArray>::failure(
            invalidField(
                QStringLiteral("presencePetLength")));
    }
    QJsonObject object;
    object.insert(u"id", uuidText(value.id));
    object.insert(u"protocolVersion", QJsonValue(value.protocolVersion));
    object.insert(u"operation", operationText(value.operation));
    putOptional(object, u"cursor", value.cursor);
    putOptional(object, u"limit", value.limit);
    putOptional(object, u"threadID", value.threadId);
    putOptional(object, u"goalObjective", value.goalObjective);
    putOptional(object, u"goalTokenBudget", value.goalTokenBudget);
    putOptional(object, u"text", value.text);
    putOptional(object, u"cwd", value.cwd);
    if (value.sendAction.has_value()) object.insert(u"sendAction", sendActionText(*value.sendAction));
    if (value.approvalDecision.has_value()) object.insert(u"approvalDecision", approvalDecisionText(*value.approvalDecision));
    putOptional(object, u"model", value.model);
    putOptional(object, u"reasoningEffort", value.reasoningEffort);
    putOptional(object, u"skillName", value.skillName);
    putOptional(object, u"skillPath", value.skillPath);
    putOptional(object, u"chatAgentID", value.chatAgentId);
    if (value.chatProvider.has_value()) object.insert(u"chatProvider", chatProviderText(*value.chatProvider));
    putOptional(object, u"chatModelID", value.chatModelId);
    putOptional(object, u"resetCreditID", value.resetCreditId);
    if (value.attachments.has_value()) {
        QJsonArray attachments;
        for (qsizetype index = 0; index < value.attachments->size(); ++index) {
            const auto attachment = encodeAttachment(value.attachments->at(index), profile,
                QStringLiteral("attachments") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!attachment.hasValue()) return Result<QByteArray>::failure(attachment.error());
            attachments.append(attachment.value());
        }
        object.insert(u"attachments", attachments);
    }
    putOptional(object, u"idempotencyKey", value.idempotencyKey);
    putOptional(
        object,
        u"presencePetPackageID",
        value.presencePetPackageId);
    putOptional(
        object,
        u"presencePetContentHash",
        value.presencePetContentHash);
    putOptional(
        object,
        u"presencePetFileName",
        value.presencePetFileName);
    putOptional(
        object,
        u"presencePetOffset",
        value.presencePetOffset);
    putOptional(
        object,
        u"presencePetLength",
        value.presencePetLength);
    return Result<QByteArray>::success(encodeJson(
        object, profile == BridgeWireProfile::RelayV1Canonical));
}

Result<QByteArray> encodeResponseObject(const BridgeResponse& value, BridgeWireProfile profile)
{
    QJsonObject object;
    object.insert(u"id", uuidText(value.id));
    object.insert(u"protocolVersion", QJsonValue(value.protocolVersion));
    object.insert(u"operation", operationText(value.operation));
    object.insert(u"succeeded", value.succeeded);
    putOptional(object, u"errorCode", value.errorCode);
    putOptional(object, u"message", value.message);
    putOptional(object, u"macName", value.macName);
    putOptional(object, u"macDeviceID", value.macDeviceId);
    putOptional(object, u"pairingSecret", value.pairingSecret);
    putOptional(object, u"relayURLString", value.relayUrlString);
    if (value.tasks.has_value()) {
        QJsonArray tasks;
        for (qsizetype index = 0; index < value.tasks->size(); ++index) {
            const auto task = encodeTask(value.tasks->at(index), profile,
                QStringLiteral("tasks") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!task.hasValue()) return Result<QByteArray>::failure(task.error());
            tasks.append(task.value());
        }
        object.insert(u"tasks", tasks);
    }
    if (value.messages.has_value()) {
        QJsonArray messages;
        for (qsizetype index = 0; index < value.messages->size(); ++index) {
            const auto message = encodeMessage(value.messages->at(index), profile,
                QStringLiteral("messages") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!message.hasValue()) return Result<QByteArray>::failure(message.error());
            messages.append(message.value());
        }
        object.insert(u"messages", messages);
    }
    putOptional(object, u"nextCursor", value.nextCursor);
    putOptional(object, u"threadID", value.threadId);
    if (value.capabilities.has_value()) {
        const auto capabilities = encodeCapabilities(*value.capabilities, profile, QStringLiteral("capabilities"));
        if (!capabilities.hasValue()) return Result<QByteArray>::failure(capabilities.error());
        object.insert(u"capabilities", capabilities.value());
    }
    if (value.chatMessage.has_value()) {
        const auto chatMessage = encodeMessage(*value.chatMessage, profile, QStringLiteral("chatMessage"));
        if (!chatMessage.hasValue()) return Result<QByteArray>::failure(chatMessage.error());
        object.insert(u"chatMessage", chatMessage.value());
    }
    if (value.timelineItems.has_value()) {
        QJsonArray items;
        for (qsizetype index = 0; index < value.timelineItems->size(); ++index) {
            const auto item = encodeTimelineItem(value.timelineItems->at(index), profile,
                QStringLiteral("timelineItems") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!item.hasValue()) return Result<QByteArray>::failure(item.error());
            items.append(item.value());
        }
        object.insert(u"timelineItems", items);
    }
    putOptional(object, u"revision", value.revision);
    putOptional(object, u"timelineNextCursor", value.timelineNextCursor);
    if (value.subagents.has_value()) {
        QJsonArray subagents;
        for (qsizetype index = 0; index < value.subagents->size(); ++index) {
            const auto subagent = encodeSubagent(value.subagents->at(index), profile,
                QStringLiteral("subagents") + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'));
            if (!subagent.hasValue()) return Result<QByteArray>::failure(subagent.error());
            subagents.append(subagent.value());
        }
        object.insert(u"subagents", subagents);
    }
    if (value.contextUsage.has_value()) {
        const auto usage = encodeContextUsage(*value.contextUsage, profile, QStringLiteral("contextUsage"));
        if (!usage.hasValue()) return Result<QByteArray>::failure(usage.error());
        object.insert(u"contextUsage", usage.value());
    }
    if (value.usageSnapshot.has_value()) {
        const auto usage = encodeUsageSnapshot(*value.usageSnapshot, profile, QStringLiteral("usageSnapshot"));
        if (!usage.hasValue()) return Result<QByteArray>::failure(usage.error());
        object.insert(u"usageSnapshot", usage.value());
    }
    if (value.goal.has_value()) {
        const auto goal = encodeGoal(*value.goal, profile, QStringLiteral("goal"));
        if (!goal.hasValue()) return Result<QByteArray>::failure(goal.error());
        object.insert(u"goal", goal.value());
    }
    if (value.features.has_value()) {
        QJsonArray features;
        for (const BridgeFeature feature : *value.features) {
            features.append(featureText(feature));
        }
        object.insert(u"features", features);
    }
    putOptional(
        object,
        u"selectedDesktopPetID",
        value.selectedDesktopPetId);
    if (value.presencePetCatalog.has_value()) {
        QJsonArray catalog;
        for (qsizetype index = 0;
             index < value.presencePetCatalog->size();
             ++index) {
            const auto entry =
                encodePresencePetCatalogEntry(
                    value.presencePetCatalog->at(index),
                    profile,
                    QStringLiteral(
                        "presencePetCatalog")
                        + QLatin1Char('[')
                        + QString::number(index)
                        + QLatin1Char(']'));
            if (!entry.hasValue()) {
                return Result<QByteArray>::failure(
                    entry.error());
            }
            catalog.append(entry.value());
        }
        object.insert(u"presencePetCatalog", catalog);
    }
    if (value.presencePetManifest.has_value()) {
        const auto manifest =
            encodePresencePetManifest(
                *value.presencePetManifest,
                profile,
                QStringLiteral(
                    "presencePetManifest"));
        if (!manifest.hasValue()) {
            return Result<QByteArray>::failure(
                manifest.error());
        }
        object.insert(
            u"presencePetManifest",
            manifest.value());
    }
    if (value.presencePetChunk.has_value()) {
        const auto chunk = encodePresencePetChunk(
            *value.presencePetChunk,
            profile,
            QStringLiteral("presencePetChunk"));
        if (!chunk.hasValue()) {
            return Result<QByteArray>::failure(
                chunk.error());
        }
        object.insert(
            u"presencePetChunk",
            chunk.value());
    }
    return Result<QByteArray>::success(encodeJson(
        object, profile == BridgeWireProfile::RelayV1Canonical));
}

template <typename T, typename Decoder>
Result<T> decodeJson(
    QByteArrayView bytes,
    BridgeWireProfile profile,
    Decoder decoder)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<T>::failure(invalidJson(
            error.error == QJsonParseError::NoError
                ? QStringLiteral("Bridge payload root must be an object")
                : error.errorString(),
            error.offset));
    }
    RawNumberIndex rawNumbers(bytes);
    if (!rawNumbers.index()) {
        return Result<T>::failure(invalidJson(QStringLiteral("Malformed bridge JSON")));
    }
    return decoder(rawNumbers.rootObject(), DecodeContext{profile, rawNumbers});
}

} // namespace

Result<BridgeRequest> BridgeJsonCodec::decodeRequest(
    QByteArrayView bytes,
    BridgeWireProfile profile)
{
    return decodeJson<BridgeRequest>(bytes, profile, decodeRequestObject);
}

Result<QByteArray> BridgeJsonCodec::encodeRequest(
    const BridgeRequest& request,
    BridgeWireProfile profile)
{
    return encodeRequestObject(request, profile);
}

Result<BridgeResponse> BridgeJsonCodec::decodeResponse(
    QByteArrayView bytes,
    BridgeWireProfile profile)
{
    return decodeJson<BridgeResponse>(bytes, profile, decodeResponseObject);
}

Result<QByteArray> BridgeJsonCodec::encodeResponse(
    const BridgeResponse& response,
    BridgeWireProfile profile)
{
    return encodeResponseObject(response, profile);
}

} // namespace companion
