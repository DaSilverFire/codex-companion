#pragma once

#include "core/Result.h"

#include <QString>
#include <QStringView>

namespace companion {

struct InstallerMetadata final {
    QString productName;
    QString productVersionMarker;
    QString originalFilename;

    friend bool operator==(
        const InstallerMetadata&,
        const InstallerMetadata&) = default;
};

class InstallerMetadataReader final {
public:
    Result<InstallerMetadata> read(
        QStringView path) const;
};

} // namespace companion
