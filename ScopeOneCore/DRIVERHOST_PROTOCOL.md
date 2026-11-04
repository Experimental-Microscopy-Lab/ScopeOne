# DriverHost protocol

ScopeOne talks to every hardware plugin through a `ScopeOne_DriverHost` process. This file
describes what crosses that process boundary: control messages on a local socket and camera
frames in shared memory. Both ends are part of ScopeOne, so the protocol is internal and changes
with it; hardware plugins are written against `scopeone::HardwarePluginSDK` instead. It is version
5, `kProtocolVersion` in `ScopeOneCore/internal/DriverHostProtocol.h`, which holds the names used
below.

## Process

ScopeOne starts one host for each device:

```text
ScopeOne_DriverHost --provider <providerId> --plugin <plugin library> --hostKey <key>
                    --launcherPid <pid> [--option <JSON object>]
```

`--option` carries the options from the Plugin Manager. The host loads the plugin, creates the
devices and listens on the local socket `ScopeOne.DriverHost.<hostKey>.ctrl` (a named pipe on
Windows, a Unix domain socket elsewhere). ScopeOne connects to it, and the host exits when the
process `--launcherPid` is gone.

## Messages

Each message is a 4-byte little-endian length followed by that many bytes of compact UTF-8 JSON.
A message is at most 256 KiB. Every message is an object with

| Field | Value |
|---|---|
| `kind` | `Request`, `Response` or `Event` |
| `version` | the protocol version |
| `type` | the command or event name |
| `requestId` | a decimal string, in requests and their responses |

ScopeOne sends requests. The host answers each one with a response of the same `type` and
`requestId` that has `ok` and, when `ok` is false, `error`. Events come from the host at any time
and carry `providerId`.

64-bit integers such as `requestId` and `frameIndex` are sent as decimal strings.

## Describe and shutdown

`Describe` returns `providerId`, `providerName`, `providerVersion` and `devices`, and for a provider
with stages `defaultXYStage` and `defaultZStage`. Each device is an object with

| Field | Value |
|---|---|
| `deviceId` | the id ScopeOne uses in requests |
| `providerDeviceId`, `hardwareId`, `deviceName` | the plugin's own names |
| `deviceKind` | `Camera`, `XYStage`, `ZStage`, `Shutter`, `State`, `Hub`, `Serial`, `Generic`, ... |
| `deviceState` | the numeric `HardwareDeviceState` |
| `deviceProperties` | an object of fixed device information |
| `capabilities` | the capability names below that the device supports |
| `sharedMemoryKey` | cameras only, the key of the frame ring |

`Shutdown` stops the devices, answers, and ends the host.

## Commands

Every command except the configuration ones names its device with `deviceId`. A command for a
capability the device lacks is answered with `ok` false.

| Capability | Command | Request fields | Response fields |
|---|---|---|---|
| `Camera` | `StartPreview`, `StopPreview` | | |
| | `SetFrameDeliveryMode` | `mode`: `PreviewLatest`, `LatestOnly` or `AllFrames` | `mode` |
| | `GetExposure` | | `exposureMs` |
| | `SetExposure` | `value` in ms | `exposureMs`, the value the camera took |
| | `SetROI` | `x`, `y`, `width`, `height` | `x`, `y`, `width`, `height` |
| | `ClearROI`, `GetROI` | | `x`, `y`, `width`, `height` |
| | `CaptureEvent` | `timeoutMs`, default 1500 | `frameIndex` |
| `Properties` | `ListProperties` | | `properties` |
| | `GetProperty` | `name`, `fromCache` | `value`, `propertyType`, `readOnly`, `preInit`, `allowedValues`, `hasLimits`, `lowerLimit`, `upperLimit` |
| | `SetProperty` | `name`, `value` | |
| `Stage` | `GetXYPosition` | | `x`, `y` |
| | `GetZPosition` | | `z` |
| | `SetXYPosition`, `SetRelativeXYPosition` | `x`, `y` | |
| | `SetZPosition`, `SetRelativeZPosition` | `z` | |
| `Shutter` | `GetShutterOpen` | | `open` |
| | `SetShutterOpen` | `open` | |
| `State` | `GetState` | | `state`, `label` |
| | `SetState` | `state` | |
| | `GetStateLabel` | `state` | `label` |
| `Configuration` | `ListConfigGroups` | | `groups` |
| | `ListConfigs` | `group` | `configs` |
| | `GetCurrentConfig` | `group` | `config` |
| | `SetConfig` | `group`, `config` | |

## Events

| Event | Fields | Sent when |
|---|---|---|
| `Hello` | | a client has connected |
| `FrameAvailable` | `deviceId`, `frameIndex` | a frame is in the ring |
| `PreviewState` | `deviceId`, `running` | preview starts or stops |
| `DriverHostError` | `error` | the provider reports an error |

## Camera frames

Each camera has one ring in shared memory, created by the host under `sharedMemoryKey`. On Windows
the key is the name of a file mapping. On Linux and macOS it is the path of a file in the user's
runtime directory, which `QSharedMemory` turns into a System V key.

The ring is a 64-byte control block followed by 32 slots. A slot is a 64-byte header and room for
`4 * 2048 * 2048` bytes of pixels. All fields are little-endian.

| Offset | Control block field |
|---|---|
| 0 | `latestSlotIndex`, u32, the slot written last |

| Offset | Slot header field |
|---|---|
| 0 | `state`, u32: 0 empty, 1 being written, 2 ready, 3 being read |
| 4 | `width`, u32 |
| 8 | `height`, u32 |
| 12 | `stride`, u32, bytes per row |
| 16 | `pixelFormat`, u32: 0 Mono8, 1 Mono16 |
| 20 | `bitsPerSample`, u16 |
| 22 | `channels`, u16, always 1 |
| 24 | `frameIndex`, u64 |
| 32 | `timestampNs`, u64 |
| 40 | `sourceRoiX`, `sourceRoiY`, `sourceRoiWidth`, `sourceRoiHeight`, i32 each |
| 56 | `sourceRoiValid`, u32 |

The pixels start at offset 64 of the slot, `stride * height` bytes.

The writer takes a slot whose `state` is 0 or 2 by changing it to 1 with a compare-and-swap,
fills the header and pixels, stores 2 with release order, then stores the slot index in
`latestSlotIndex` with release order and sends `FrameAvailable`. The reader takes a slot by
changing its `state` from 2 to 3, copies the frame, and stores 2 again. A slot being read is
skipped by the writer, so the reader never sees a frame change under it.
