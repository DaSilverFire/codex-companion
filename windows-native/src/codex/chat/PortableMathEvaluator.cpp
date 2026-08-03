#include "codex/chat/PortableMathEvaluator.h"

#include <QVariantMap>

#include <cmath>
#include <numbers>
#include <utility>

namespace companion {
namespace {

CompanionError mathError(
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

class MathParser final {
public:
    explicit MathParser(QString expression)
        : expression_(std::move(expression))
    {
    }

    Result<double> parse()
    {
        const Result<double> result =
            parseExpression();
        if (!result.hasValue()) {
            return result;
        }
        skipWhitespace();
        if (!atEnd()) {
            return invalidExpression();
        }
        if (!std::isfinite(result.value())) {
            return nonFiniteResult();
        }
        return result;
    }

private:
    Result<double> parseExpression()
    {
        Result<double> parsed =
            parseTerm();
        if (!parsed.hasValue()) {
            return parsed;
        }
        double value = parsed.value();
        while (true) {
            if (consume(QLatin1Char('+'))) {
                parsed = parseTerm();
                if (!parsed.hasValue()) {
                    return parsed;
                }
                value += parsed.value();
            } else if (
                consume(QLatin1Char('-'))) {
                parsed = parseTerm();
                if (!parsed.hasValue()) {
                    return parsed;
                }
                value -= parsed.value();
            } else {
                return finite(value);
            }
        }
    }

    Result<double> parseTerm()
    {
        Result<double> parsed =
            parsePower();
        if (!parsed.hasValue()) {
            return parsed;
        }
        double value = parsed.value();
        while (true) {
            if (consume(QLatin1Char('*'))) {
                parsed = parsePower();
                if (!parsed.hasValue()) {
                    return parsed;
                }
                value *= parsed.value();
            } else if (
                consume(QLatin1Char('/'))) {
                parsed = parsePower();
                if (!parsed.hasValue()) {
                    return parsed;
                }
                if (parsed.value() == 0.0) {
                    return divisionByZero();
                }
                value /= parsed.value();
            } else if (
                consume(QLatin1Char('%'))) {
                parsed = parsePower();
                if (!parsed.hasValue()) {
                    return parsed;
                }
                if (parsed.value() == 0.0) {
                    return divisionByZero();
                }
                value = std::fmod(
                    value,
                    parsed.value());
            } else {
                return finite(value);
            }
        }
    }

    Result<double> parsePower()
    {
        Result<double> base =
            parseUnary();
        if (!base.hasValue()) {
            return base;
        }
        if (!consume(QLatin1Char('^'))) {
            return base;
        }
        const Result<double> exponent =
            parsePower();
        if (!exponent.hasValue()) {
            return exponent;
        }
        return finite(
            std::pow(
                base.value(),
                exponent.value()));
    }

    Result<double> parseUnary()
    {
        if (consume(QLatin1Char('+'))) {
            return parseUnary();
        }
        if (consume(QLatin1Char('-'))) {
            Result<double> value =
                parseUnary();
            if (!value.hasValue()) {
                return value;
            }
            return finite(-value.value());
        }
        return parsePrimary();
    }

    Result<double> parsePrimary()
    {
        skipWhitespace();
        if (consume(QLatin1Char('('))) {
            Result<double> value =
                parseExpression();
            if (!value.hasValue()) {
                return value;
            }
            if (!consume(QLatin1Char(')'))) {
                return invalidExpression();
            }
            return value;
        }

        const QChar next = peek();
        if (!next.isNull()
            && next.isLetter()) {
            const QString identifier =
                parseIdentifier().toLower();
            if (identifier
                == QStringLiteral("pi")) {
                return Result<double>::success(
                    std::numbers::pi);
            }
            if (identifier
                == QStringLiteral("e")) {
                return Result<double>::success(
                    std::numbers::e);
            }
            if (!consume(QLatin1Char('('))) {
                return invalidExpression();
            }
            Result<double> argument =
                parseExpression();
            if (!argument.hasValue()) {
                return argument;
            }
            if (!consume(QLatin1Char(')'))) {
                return invalidExpression();
            }
            return applyFunction(
                identifier,
                argument.value());
        }

        return parseNumber();
    }

    Result<double> parseNumber()
    {
        skipWhitespace();
        const qsizetype start = index_;
        bool sawDecimalPoint = false;
        while (!atEnd()) {
            const QChar character = peek();
            if (character.isDigit()) {
                ++index_;
            } else if (
                character == QLatin1Char('.')
                && !sawDecimalPoint) {
                sawDecimalPoint = true;
                ++index_;
            } else {
                break;
            }
        }

        if (!atEnd()
            && (peek() == QLatin1Char('e')
                || peek()
                    == QLatin1Char('E'))) {
            ++index_;
            if (!atEnd()
                && (peek()
                        == QLatin1Char('+')
                    || peek()
                        == QLatin1Char('-'))) {
                ++index_;
            }
            const qsizetype exponentStart =
                index_;
            while (!atEnd()
                   && peek().isDigit()) {
                ++index_;
            }
            if (index_ == exponentStart) {
                return invalidExpression();
            }
        }

        if (index_ == start) {
            return invalidExpression();
        }
        bool valid = false;
        const double value =
            expression_
                .mid(start, index_ - start)
                .toDouble(&valid);
        if (!valid) {
            return invalidExpression();
        }
        return finite(value);
    }

    QString parseIdentifier()
    {
        skipWhitespace();
        const qsizetype start = index_;
        while (!atEnd()) {
            const QChar character = peek();
            if (!character.isLetterOrNumber()
                && character
                    != QLatin1Char('_')) {
                break;
            }
            ++index_;
        }
        return expression_.mid(
            start,
            index_ - start);
    }

    Result<double> applyFunction(
        const QString& function,
        double value)
    {
        if (function
            == QStringLiteral("sqrt")) {
            return value < 0.0
                ? nonFiniteResult()
                : finite(std::sqrt(value));
        }
        if (function
            == QStringLiteral("abs")) {
            return finite(std::abs(value));
        }
        if (function
            == QStringLiteral("sin")) {
            return finite(std::sin(value));
        }
        if (function
            == QStringLiteral("cos")) {
            return finite(std::cos(value));
        }
        if (function
            == QStringLiteral("tan")) {
            return finite(std::tan(value));
        }
        if (function
            == QStringLiteral("ln")) {
            return value <= 0.0
                ? nonFiniteResult()
                : finite(std::log(value));
        }
        if (function
                == QStringLiteral("log")
            || function
                == QStringLiteral("log10")) {
            return value <= 0.0
                ? nonFiniteResult()
                : finite(std::log10(value));
        }
        if (function
            == QStringLiteral("exp")) {
            return finite(std::exp(value));
        }
        if (function
            == QStringLiteral("floor")) {
            return finite(std::floor(value));
        }
        if (function
            == QStringLiteral("ceil")) {
            return finite(std::ceil(value));
        }
        if (function
            == QStringLiteral("round")) {
            return finite(std::round(value));
        }
        return Result<double>::failure(
            mathError(
                QStringLiteral(
                    "portable_tool.math_unsupported_function"),
                QStringLiteral(
                    "The requested math function is not supported."),
                {
                    {
                        QStringLiteral("function"),
                        function,
                    },
                }));
    }

    bool consume(QChar expected)
    {
        skipWhitespace();
        if (peek() != expected) {
            return false;
        }
        ++index_;
        return true;
    }

    void skipWhitespace()
    {
        while (!atEnd()
               && peek().isSpace()) {
            ++index_;
        }
    }

    bool atEnd() const noexcept
    {
        return index_
            >= expression_.size();
    }

    QChar peek() const
    {
        return atEnd()
            ? QChar()
            : expression_.at(index_);
    }

    static Result<double> finite(
        double value)
    {
        return std::isfinite(value)
            ? Result<double>::success(value)
            : nonFiniteResult();
    }

    static Result<double>
    invalidExpression()
    {
        return Result<double>::failure(
            mathError(
                QStringLiteral(
                    "portable_tool.math_invalid_expression"),
                QStringLiteral(
                    "The expression is not valid.")));
    }

    static Result<double> divisionByZero()
    {
        return Result<double>::failure(
            mathError(
                QStringLiteral(
                    "portable_tool.math_division_by_zero"),
                QStringLiteral(
                    "The expression divides by zero.")));
    }

    static Result<double> nonFiniteResult()
    {
        return Result<double>::failure(
            mathError(
                QStringLiteral(
                    "portable_tool.math_non_finite_result"),
                QStringLiteral(
                    "The expression does not have a finite result.")));
    }

    QString expression_;
    qsizetype index_ = 0;
};

} // namespace

Result<double> PortableMathEvaluator::evaluate(
    QStringView expression)
{
    QString normalized =
        expression.toString();
    normalized.replace(
        QChar(0x00d7),
        QLatin1Char('*'));
    normalized.replace(
        QChar(0x00f7),
        QLatin1Char('/'));
    normalized.replace(
        QChar(0x2212),
        QLatin1Char('-'));
    normalized.replace(
        QString(QChar(0x221a))
            + QLatin1Char('('),
        QStringLiteral("sqrt("));
    if (normalized.toUtf8().size() > 256) {
        return Result<double>::failure(
            mathError(
                QStringLiteral(
                    "portable_tool.math_expression_too_long"),
                QStringLiteral(
                    "The expression is too long.")));
    }
    return MathParser(
               std::move(normalized))
        .parse();
}

QString PortableMathEvaluator::formatted(
    double value)
{
    const double normalized =
        std::abs(value) < 1e-12
        ? 0.0
        : value;
    return QString::number(
        normalized,
        'g',
        12);
}

Result<QString>
PortableMathEvaluator::toolSummary(
    QStringView expression)
{
    const Result<double> evaluated =
        evaluate(expression);
    if (!evaluated.hasValue()) {
        return Result<QString>::failure(
            evaluated.error());
    }
    return Result<QString>::success(
        QStringLiteral(
            "Exact calculator result: %1")
            .arg(formatted(
                evaluated.value())));
}

} // namespace companion
