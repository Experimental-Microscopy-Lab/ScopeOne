# Drivers

A driver makes a device usable in ScopeOne. It is a C++ library written against
`scopeone::DriverSDK`, and ScopeOne runs it in a `ScopeOne_DriverHost` process of its own, one per
device, so a crash in a driver or in a vendor library does not take ScopeOne down.

| Driver | Folder | Device |
|---|---|---|
| Example Camera | `ExampleCamera` | a camera that renders test patterns, the template for camera drivers |
| Example PMT | `ExamplePmt` | a simulated PMT, the template for signal sources |
| NI-DAQmx | `NIDaqmx` | National Instruments DAQ devices, through the NI-DAQmx runtime, with each analog input also a signal source for the Signal Monitor; the inputs of one device run in one task on one sample clock |

Micro-Manager remains the built-in camera provider and loads its device adapters from `.cfg`
files. Those adapters are not ScopeOne drivers.

## Writing a driver

A driver has one entry class, a `Driver`. ScopeOne_DriverHost loads the library, gets that object
from Qt's plugin loader and calls `createProvider`, which returns a `HardwareProvider` with the
capabilities its devices have (camera, stage, shutter, state, configuration, signal source, DAQ).
A signal source, such as a PMT or a photon counter, puts out one value over time:
`SignalSourceProvider` hands over its chunks of samples or photon events with the scan markers,
and ScopeOne shows them on the oscilloscope screen of the Signal Monitor, up to four sources at
once on one time axis, and assembles scan images from them. A
`DaqProvider` runs the timed tasks of a DAQ session and hands over the inputs it reads. From then
on ScopeOne only exchanges messages and data with the host. The host does not link ScopeOneCore,
so a driver process holds only the host, the SDK library, Qt Core and Network, and the driver.

The `scopeone::DriverSDK` target gives a driver the SDK headers (`scopeone/DriverSDK.h` includes
them all) and the small SDK library, and nothing else of Core, so a driver that includes or calls
any other part of Core fails to build.

Build and install ScopeOneCore first (`scripts/build-unix.sh` or `scripts\build.ps1`). A driver
is then a separate CMake project:

```cmake
cmake_minimum_required(VERSION 3.23)
project(MyCamera LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(ScopeOneCore CONFIG REQUIRED)

add_library(MyCamera MODULE MyCamera.cpp driver.json)
target_link_libraries(MyCamera PRIVATE scopeone::DriverSDK Qt6::Core)
scopeone_add_driver(MyCamera)
```

Point `CMAKE_PREFIX_PATH` at `ScopeOneCore/install` and at the same Qt that ScopeOne uses
(`vcpkg_installed/<triplet>`).

`driver.json` sits beside `CMakeLists.txt`:

```json
{
  "id": "my.camera",
  "name": "My Camera",
  "version": "1.0.0",
  "scopeOneApi": 1,
  "kind": "driver",
  "providerId": "my.camera",
  "autoLoad": false
}
```

`scopeOneApi` is `ScopeOneDriverApiVersion` in `scopeone/PluginManifest.h`.
`scopeone_add_driver` writes `scopeone_driver.json` from it with the Core version added, and the
entry class names that file:

```cpp
class MyCameraDriver final : public QObject, public scopeone::core::Driver
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
    Q_INTERFACES(scopeone::core::Driver)

public:
    QString providerId() const override;
    scopeone::core::HardwareProviderPtr createProvider(const QJsonObject& options,
                                                       QString* errorMessage) override;
};
```

The driver library is loaded by `ScopeOne_DriverHost`, which links the same SDK and Qt as
ScopeOne, so a driver is built with the same compiler, Qt and Core release (`MAJOR.MINOR` of
`VERSION`). ScopeOne refuses a driver built against another release and names the release it
needs.

## Installing a driver

Copy the built library into `drivers/` beside the ScopeOne executable, or install it with
**Tools > Drivers and Plugins**, which puts it in the user's application data directory. Drivers
are enabled and configured there and take effect after a restart.
