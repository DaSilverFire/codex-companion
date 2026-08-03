#pragma once

#include "core/Result.h"

#include <compare>
#include <functional>
#include <optional>

#include <QString>
#include <QStringView>
#include <QtGlobal>

namespace companion {

struct WindowsVersion final {
    quint32 major = 0;
    quint32 minor = 0;
    quint32 build = 0;
    quint32 revision = 0;

    static std::optional<WindowsVersion> parse(
        QStringView value);

    QString toString() const;

    friend auto operator<=>(
        const WindowsVersion&,
        const WindowsVersion&) = default;
};

class WindowsVersionProvider final {
public:
    using Query =
        std::function<Result<WindowsVersion>()>;

    WindowsVersionProvider();
    explicit WindowsVersionProvider(Query query);

    Result<WindowsVersion> current() const;

private:
    Query query_;
};

} // namespace companion
