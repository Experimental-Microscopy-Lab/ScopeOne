# DriverHost protocol

ScopeOne talks to every driver through a `ScopeOne_DriverHost` process. This file
describes what crosses that process boundary: control messages on a local socket, camera frames
in shared memory, and the chunks of signal sources and DAQ devices in stream rings. Both ends are part of ScopeOne, so the protocol is internal and changes
with it; drivers are written against `scopeone::DriverSDK` instead. It is version
6, `kProtocolVersion` in `ScopeOneCore/internal/DriverHostProtocol.h`, which holds the names used
below.

## Process

ScopeOne starts one host for each device:

```text
ScopeOne_DriverHost --provider <providerId> --driver <driver library> --hostKey <key>
                    --launcherPid <pid> [--option <JSON object>]
```

`--option` carries the options from Drivers and Plugins. The host loads the driver, creates the
devices and listens on the local socket `ScopeOne.DriverHost.<hostKey>.ctrl` (a named pipe on
Windows, a Unix domain socket elsewhere). ScopeOne connects to it, and the host exits when the
process `--launcherPid` is gone.

## Messages

Each message is a 4-byte little-endian length followed by that many bytes of compact UTF-8 JSON.
A message is at most 64 MiB, because a DAQ session carries its output waveforms. Every message is an object with

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

`Describe` returns `providerId`, `providerName`, `providerVersion`, `devices`, `signalSources`
and `daqDevices`, and for a provider with stages `defaultXYStage` and `defaultZStage`.
`signalSources` and `daqDevices` hold the JSON form of `SignalSourceDescriptor` and
`DaqDeviceDescriptor`, written by `ScopeOneCore/src/HardwareStreamCodec.cpp`. Each device is an object with

| Field | Value |
|---|---|
| `deviceId` | the id ScopeOne uses in requests |
| `providerDeviceId`, `hardwareId`, `deviceName` | the driver's own names |
| `deviceKind` | `Camera`, `XYStage`, `ZStage`, `Shutter`, `State`, `Hub`, `Serial`, `Generic`, ... |
| `deviceState` | the numeric `HardwareDeviceState` |
| `deviceProperties` | an object of fixed device information |
| `capabilities` | the capability names below that the device supports |
| `sharedMemoryKey` | cameras only, the key of the frame ring |

`Shutdown` stops the devices, answers, and ends the host.

## Commands

Every command except the configuration, signal and DAQ ones names its device with `deviceId`. A command for a
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
| `SignalSource` | `StartSignal` | `config`, a `SignalAcquisitionConfig` | `streamId`, `sharedMemoryKey` |
| | `StopSignal` | `sourceId` | |
| `Daq` | `StartDaq` | `config`, a `DaqSessionConfig` with its raster scans expanded | `streamId`, `sharedMemoryKey` |
| | `StopDaq` | `deviceId` | |
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
| `StreamAvailable` | `streamId` | records are in a stream ring; one event can stand for several |
| `SignalState` | `sourceId`, `state`, `message` | a signal source changes its `SignalSourceState` |
| `DaqState` | `deviceId`, `state`, `message` | a DAQ device changes its `DaqState` |

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

## Stream rings

Each `StartSignal` and `StartDaq` gets a new ring in shared memory under the `sharedMemoryKey` of
its response, sized for about two seconds of that acquisition. The host writes the chunks of the
source or session into it, and ScopeOne reads them after each `StreamAvailable`. The layout is
`ScopeOneCore/internal/StreamRing.h`.

The ring begins with a control block of three 64-byte lines: `magic` u32 and `version` u32 and
`capacity` u64 in the first, `writeCursor` u64 in the second and `readCursor` u64 in the third.
The cursors count bytes from the start of the stream and never wrap; the data area of `capacity`
bytes follows the control block. Each record starts on 8 bytes with a 32-byte header

| Offset | Record header field |
|---|---|
| 0 | `bytes`, u32, the record with its header and padding |
| 4 | `type`, u32: 0 skip, 1 time series, 2 timestamped events, 3 DAQ input |
| 8 | `sequence`, u64 |
| 16 | `droppedBefore`, u64, records dropped right before this one |
| 24 | `payloadBytes`, u64 |

followed by the payload. A record that would cross the end of the data area is written at its
beginning after a skip record that fills the rest. The writer never overwrites unread bytes: when
the ring is full it drops the record and reports the count in `droppedBefore` of the next one.
It publishes a record by storing `writeCursor` with release order, and the reader frees it by
storing `readCursor` the same way. The payloads are the chunks in native byte order, as
`HardwareStreamCodec.cpp` writes them.
