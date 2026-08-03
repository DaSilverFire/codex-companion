#pragma once

#include "core/Result.h"

#include <QString>
#include <QStringView>

namespace companion {

class PortableMathEvaluator final {
public:
    static Result<double> evaluate(
        QStringView expression);
    static QString formatted(double value);
    static Result<QString> toolSummary(
        QStringView expression);
};

} // namespace companion
