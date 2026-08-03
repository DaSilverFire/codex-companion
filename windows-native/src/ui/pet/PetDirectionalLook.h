#pragma once

#include <QPointF>
#include <QRectF>
#include <optional>

namespace companion {

struct PetDirectionalLookFrame final {
    int row = 0;
    int column = 0;

    friend bool operator==(
        const PetDirectionalLookFrame&,
        const PetDirectionalLookFrame&) = default;
};

class PetDirectionalLook final {
public:
    static std::optional<
        PetDirectionalLookFrame>
    resolve(
        QPointF pointer,
        QRectF petFrame,
        int startRow = 9) noexcept;
};

} // namespace companion
