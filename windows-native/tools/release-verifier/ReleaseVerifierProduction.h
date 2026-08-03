#pragma once

#include "ReleaseVerifier.h"

namespace companion {

ReleaseVerifierDependencies
makeProductionReleaseVerifierDependencies();

ReleaseEvidenceMetadata
productionReleaseEvidenceMetadata();

QString productionDefaultReleaseStagePath(
    QStringView installerPath);

QString productionDefaultReleaseMetadataPath(
    QStringView installerPath);

} // namespace companion
