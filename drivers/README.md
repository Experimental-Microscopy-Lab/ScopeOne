# Drivers

A driver makes a device usable in ScopeOne. It is a C++ library written against
`scopeone::DriverSDK`, or a JSON description file that the Serial driver runs. ScopeOne runs each
device in a `ScopeOne_DriverHost` process of its own, so a crash in a driver or in a vendor library
does not take ScopeOne down.

| Driver | Folder | Device |
|---|---|---|
| Example Camera | `ExampleCamera` | a camera that renders test patterns, the template for camera drivers |
| Example PMT | `ExamplePmt` | a simulated PMT, the template for signal sources |
| NI-DAQmx | `NIDaqmx` | National Instruments DAQ devices, through the NI-DAQmx runtime, with each analog input also a signal source for the Signal Monitor; the inputs of one device run in one task on one sample clock |
| Serial Devices | `Serial` | devices described by JSON files in `profiles`, such as the Cobolt 05-01 lasers; each description file is a driver of its own |

Micro-Manager is the second provider. Its device adapters are loaded from the `.cfg` file, and they
are not ScopeOne drivers.

## Device model

`createDevice` returns a `Device`. It holds the properties of the device and implements the
capability interfaces that fit it:

| Interface | Device |
|---|---|
| `Camera` | frames, exposure, ROI and events |
| `Stage` | XY or Z position, with optional sequences |
| `Shutter` | open and close |
| `StateDevice` | positions with labels, such as a filter wheel |
| `Autofocus` | a run of the autofocus |
| `SignalSource` | chunks of samples or photon events with the scan markers |
| `Daq` | timed tasks of a DAQ session and the inputs they read |

A device that also implements `Hub` lists its peripherals in `peripherals()`, which `devices()`
returns, and every call names one of them by `deviceId`. A device without peripherals is one device, and its calls leave `deviceId` empty.

The SDK interfaces are in `ScopeOneCore/sdk/include/scopeone`, and `DriverSDK.h` includes all of
them. Driver API version 3 is `ScopeOneDriverApiVersion` in `scopeone/DriverManifest.h`.

## Declaring devices in the configuration

The configuration file declares the ScopeOne devices with lines that Micro-Manager skips, so the
same file still opens in Micro-Manager. Each `ScopeOneDevice` line starts one device.

```text
ScopeOneDevice,<label>,<driver id>,<device name>
ScopeOneProperty,<label>,<property>,<value>
ScopeOneConfigGroup,<group>,<preset>,<label>,<property>,<value>
```

The label is the device id in ScopeOne, and the driver id is the `id` of its manifest or the name of
its description file. The device name is one of the driver's `deviceNames()`, or an entry of `devices` in a
description file. A driver that reports several devices gives them the labels
`<label>.1`, `<label>.2` and so on. The properties with a `ScopeOneProperty` line are applied to the device with
that label when it starts. The pre-init properties are passed to `createDevice`, and the others are
set after the device is created. A `ScopeOneConfigGroup` line adds a preset to a config group, and it
merges with the Micro-Manager preset of the same group and preset name.

```text
ScopeOneDevice,Laser 488,cobolt-05-01,Laser
ScopeOneProperty,Laser 488,Port,COM3
ScopeOneProperty,Laser 488,BaudRate,115200
ScopeOneConfigGroup,Laser,Off,Laser 488,Laser,Off
ScopeOneConfigGroup,Laser,On,Laser 488,Laser,On
ScopeOneDevice,Example Camera,example.camera,Example Camera
ScopeOneConfigGroup,Channel,FITC,Example Camera.1,Pattern,Gradient
```

Only the devices in the file are loaded. `config/ScopeOne_MM_demo.cfg` has Micro-Manager and ScopeOne devices in one file, and `config/ScopeOne_demo.cfg` has ScopeOne devices only.

## Writing a C++ driver

A driver has one entry class, a `Driver`. ScopeOne_DriverHost loads the library, gets that object
from Qt's plugin loader and calls `createDevice` with one of its `deviceNames()`. A driver that
lists no names checks the name in `createDevice` itself.

The `scopeone::DriverSDK` target gives a driver the SDK headers and the small SDK library, and
nothing else of Core, so a driver that includes or calls any other part of Core fails to build.
The host does not link ScopeOneCore, so a driver process holds only the host, the SDK library, Qt
Core and Network, and the driver.

Build and install ScopeOneCore first (`scripts/build.sh` or `scripts\build.ps1`). A driver
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

Point `CMAKE_PREFIX_PATH` at `ScopeOneCore/install` and at the same Qt that ScopeOne uses,
`vcpkg_installed/<triplet>` or the Qt passed to the build scripts with `--qt`.

The drivers in this folder are built the same way, as a project of their own that finds only Qt
Core and the installed ScopeOneCore. The build scripts build it into `drivers/build` and install
the drivers into `ScopeOneCore/install/bin/drivers`, from where the GUI build copies them
(`scripts\build.ps1 --target drivers` builds only them).

`driver.json` sits beside `CMakeLists.txt`:

```json
{
  "id": "my.camera",
  "name": "My Camera",
  "version": "1.0.0",
  "scopeOneApi": 3,
  "kind": "driver"
}
```

`scopeOneApi` is the driver API version. `scopeone_add_driver` writes `scopeone_driver.json` from
this file with the Core version added, and the entry class names that file:

```cpp
class MyCameraDriver final : public QObject, public scopeone::core::Driver
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
    Q_INTERFACES(scopeone::core::Driver)

public:
    QString driverId() const override;
    QStringList deviceNames() const override;
    scopeone::core::DevicePtr createDevice(const QString& name,
                                           const QVariantMap& preInitProperties,
                                           QString* errorMessage) override;
};
```

The driver library is loaded by `ScopeOne_DriverHost`, which links the same SDK and Qt as
ScopeOne, so a driver is built with the same compiler, Qt and Core release (`MAJOR.MINOR` of
`VERSION`). ScopeOne refuses a driver built against another release and names the release it
needs.

## Serial description files

The Serial driver runs a device from a JSON file, so a serial device needs no C++. The file is
itself a driver, whose id is the file name without the extension, `cobolt-05-01` here. A file in the
`profiles` folder of the driver is installed beside the libraries. The Cobolt 05-01 laser:

```json
{
  "name": "Cobolt 05-01",
  "devices": ["Laser"],
  "serial": {"BaudRate": 115200, "parity": "none", "stopBits": 1, "flowControl": "none"},
  "terminator": {"write": "\r", "read": "\r\n"},
  "setReply": "OK",
  "error": "[Ee]rror",
  "AnswerTimeout": 500,
  "properties": {
    "Laser": {"type": "enum", "get": "l?", "set": "l{value}", "values": {"Off": "0", "On": "1"}},
    "Operating Mode": {"type": "enum", "get": "gom?",
                       "values": {"Off": "0", "Waiting for Temperatures": "1", "Waiting for Key": "2",
                                  "Warming Up": "3", "Completed": "4", "Fault": "5", "Aborted": "6",
                                  "Waiting for Remote": "7", "Standby": "8"}},
    "Power Setpoint": {"type": "float", "get": "glp?", "set": "slp {value}",
                       "min": 0, "max": {"get": "gmlp?"}},
    "Power Reading": {"type": "float", "get": "pa?"},
    "Fault": {"type": "enum", "get": "f?",
              "values": {"No Fault": "0", "TEC Fault": "1", "Interlock": "3", "Current Clamp": "4"}},
    "Clear Fault": {"type": "action", "set": "cf"},
    "Model": {"type": "string", "get": "glm?"},
    "Serial Number": {"type": "string", "get": "gsn?"},
    "Operating Hours": {"type": "string", "get": "hrs?"}
  }
}
```

The driver id is the file name and the device name is an entry of `devices`:

```text
ScopeOneDevice,Laser 488,cobolt-05-01,Laser
ScopeOneProperty,Laser 488,Port,COM3
```

The top level of the file has these fields.

| Field | Value |
|---|---|
| `name` | the name shown in Tools > Drivers |
| `devices` | the device names of the file; a `ScopeOneDevice` line picks one of them |
| `serial` | `BaudRate`, `parity` (`none`, `even` or `odd`), `stopBits` (1 or 2) and `flowControl` (`none`, `hardware` or `software`) |
| `terminator` | `write` is appended to every command, and `read` ends every line of a reply |
| `setReply` | the line the device answers to a set command |
| `error` | a regular expression; a reply or ack line that matches it is reported as the device's error |
| `ack` | optional; a second line that the device sends after a command, which must equal this text |
| `AnswerTimeout` | the time in ms to wait for each line of a reply, 500 by default |
| `properties` | the properties of the device, by name |
| `shutter` | optional; `property`, `open` and `closed` name the property that moves the shutter and its two values, and the device then has the `Shutter` interface |

Each command is sent with the write terminator, and the first line that is not its echo is the
reply. A reply that matches `error` fails the command, and the set commands must answer
`setReply`.

Every property has these fields.

| Field | Value |
|---|---|
| `type` | `float`, `int`, `enum`, `string` or `action` |
| `get` | the query that reads the value; a property without it is written only and keeps its last value |
| `set` | the command that sets the value, with `{value}` in place of the value; a property without it is read-only |
| `parse` | optional; a regular expression applied to the reply of `get`, whose first group is the value |
| `values` | for `enum`, the names shown in ScopeOne mapped to the tokens the device uses |
| `min`, `max` | for `float` and `int`, the limits of a set value; `max` can be `{"get": "..."}`, a query that the device answers once when the device opens |

An `enum` value goes out as its token, so `Laser` set to `On` sends `l1`. A `float` value is sent
without exponent and trailing zeros, and an `int` is rounded to a whole number. An `action` property
has the one value `Run`, which sends its `set` command. The pre-init properties of a serial device are
`Port`, `BaudRate` and `AnswerTimeout`, with the same names as the MM serial device adapters, and
`Description File`, which ScopeOne sets to the path of the file.

## Installing a driver

**Tools > Drivers** lists the drivers that ScopeOne finds, with their status and location, and the
version of the driver libraries. **Install Driver...** copies the files of a folder that holds one driver library,
and **Install Device Description...** copies one serial description file. Both go into the drivers
folder of the user's application data directory. A driver is also found when its library, or its
description file, is in `drivers/` beside the ScopeOne executable. Drivers are read when a
configuration is loaded, so a driver installed while ScopeOne runs is used by the next configuration
that declares it.
