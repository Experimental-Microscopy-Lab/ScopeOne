#pragma once

#include <QString>

#include "scopeone/ExperimentDocument.h"
#include "scopeone/ImageFrame.h"

namespace scopeone::core::internal
{
    ImageFrame readOmeZarrFrame(const QString& rootPath,
                                const QString& cameraId,
                                int frameIndex,
                                const ExperimentDocument& document);
}
