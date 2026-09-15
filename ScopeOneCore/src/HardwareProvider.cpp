#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/HardwareProvider.h"

namespace scopeone::core
{
    HardwareProvider::~HardwareProvider() = default;
    DevicePropertyProvider::~DevicePropertyProvider() = default;
    StageProvider::~StageProvider() = default;
    ShutterProvider::~ShutterProvider() = default;
    StateProvider::~StateProvider() = default;
    ConfigurationProvider::~ConfigurationProvider() = default;
    CameraProvider::~CameraProvider() = default;
}
