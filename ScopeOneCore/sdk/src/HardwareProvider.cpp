#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/DaqProvider.h"
#include "scopeone/HardwareProvider.h"
#include "scopeone/SignalSourceProvider.h"

namespace scopeone::core
{
    HardwareProvider::~HardwareProvider() = default;
    DevicePropertyProvider::~DevicePropertyProvider() = default;
    StageProvider::~StageProvider() = default;
    ShutterProvider::~ShutterProvider() = default;
    AutofocusProvider::~AutofocusProvider() = default;
    StateProvider::~StateProvider() = default;
    ConfigurationProvider::~ConfigurationProvider() = default;
    CameraProvider::~CameraProvider() = default;
    SignalSourceProvider::~SignalSourceProvider() = default;
    DaqProvider::~DaqProvider() = default;
}
