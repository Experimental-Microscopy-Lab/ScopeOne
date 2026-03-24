#pragma once

#include "scopeone/ImageFrame.h"

namespace scopeone::core::internal {

bool convertFrameToMono8(const scopeone::core::ImageFrame& src,
                         scopeone::core::ImageFrame& dst);

scopeone::core::ImageFrame makeMono8Frame(const QString& cameraId,
                                          int width,
                                          int height,
                                          QByteArray bytes);

}
