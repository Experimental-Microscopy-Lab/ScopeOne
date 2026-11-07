#include "scopeone/Camera.h"
#include "scopeone/Device.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/SignalSource.h"

namespace scopeone::core
{
    Device::~Device() = default;
    Hub::~Hub() = default;
    Stage::~Stage() = default;
    Shutter::~Shutter() = default;
    Autofocus::~Autofocus() = default;
    StateDevice::~StateDevice() = default;
    Camera::~Camera() = default;
    SignalSource::~SignalSource() = default;
    Daq::~Daq() = default;

    QList<HardwareDeviceDescriptor> Device::devices() const
    {
        const auto* const hub = dynamic_cast<const Hub*>(this);
        return hub ? hub->peripherals() : QList<HardwareDeviceDescriptor>{};
    }
}
