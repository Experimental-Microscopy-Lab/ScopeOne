# DriverHost protocol

ScopeOne talks to every driver, and to each camera of a Micro-Manager configuration with more
than one camera, through a `ScopeOne_DriverHost` process. This file describes what crosses that
process boundary: control messages on a local socket, camera frames
in shared memory, and the chunks of signal sources and DAQ devices in stream rings. Both ends are part of ScopeOne, so the protocol is internal and changes
with it; drivers are written against `scopeone::DriverSDK` instead. It is version
7, `kProtocolVersion` in `ScopeOneCore/internal/DriverHostProtocol.h`, which holds the names used
below.

## Process

ScopeOne starts one host for each driver and for each camera of a Micro-Manager configuration
with more than one camera:

```text
ScopeOne_DriverHost --provider <providerId> --driver <driver library> --hostKey <key>
                    --launcherPid <pid> [--option <JSON object>]
ScopeOne_DriverHost --provider micro-manager --micromanagerCamera <JSON object> --hostKey <key>
                    --launcherPid <pid>
```

`--option` carries the options from the Drivers dialog. `--micromanagerCamera` carries
`cameraId`, `adapter`, `device`, `exposureMs`, and `preInitProperties` and `properties`, arrays of
`{"name", "value"}` objects. The host loads the camera on a core of its own, as two cameras of the
same model in one process would share the state of one vendor library. The host loads the
driver or the camera, creates the devices and listens on the local socket
`ScopeOne.<hash>.ctrl`, where `<hash>` is the first 16 hexadecimal digits of the SHA-1 of the host
key, as a Unix socket path on macOS holds at most 104 characters (a named pipe on Windows, a Unix
domain socket elsewhere). ScopeOne connects to it, and the host exits when the
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
and `daqDevices`, for a provider with stages `defaultXYStage` and `defaultZStage`, for one with
shutters `defaultShutter`, the shutter that lights the sample for its cameras, and for one with an
autofocus `defaultAutofocus`.
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
| `eventSequence` | cameras only, whether the camera takes a series of images in one sequence acquisition |
| `pixelSizeUm` | cameras only, the size of a pixel in the sample, 0 when unknown; `GetPixelSize` reads it again after a setting changed |

`Shutdown` stops the devices, answers, and ends the host.

## Commands

Every command except the configuration, automatic shutter, signal and DAQ ones names its device
with `deviceId`. A command for a
capability the device lacks is answered with `ok` false.

| Capability | Command | Request fields | Response fields |
|---|---|---|---|
| `Camera` | `StartPreview`, `StopPreview` | | |
| | `SetFrameDeliveryMode` | `mode`: `PreviewLatest`, `LatestOnly` or `AllFrames` | `mode` |
| | `SetFrameDeliveryPaused` | `paused`; the host writes no frames while paused, and a Micro-Manager camera starts again the live acquisition an event capture stopped | |
| | `GetExposure` | | `exposureMs` |
| | `SetExposure` | `value` in ms | `exposureMs`, the value the camera took |
| | `SetROI` | `x`, `y`, `width`, `height` | `x`, `y`, `width`, `height` |
| | `ClearROI`, `GetROI` | | `x`, `y`, `width`, `height` |
| | `CaptureEvent` | `timeoutMs`, default 1500 | `frameIndex` |
| | `CaptureEventSequence` | `count`, `timeoutMs`; the images wait in the host | |
| | `SendEventFrames` | `first`, `count`; writes these images of the last sequence to the frame ring, as it holds fewer than a sequence can have | |
| | `GetPixelSize` | | `pixelSizeUm`, 0 when unknown |
| `Properties` | `ListProperties` | | `properties` |
| | `GetProperty` | `name`, `fromCache` | `value`, `propertyType`, `readOnly`, `preInit`, `allowedValues`, `hasLimits`, `lowerLimit`, `upperLimit` |
| | `SetProperty` | `name`, `value` | |
| | `GetPropertySequenceCapacity` | `name` | `capacity`, 0 when it cannot be sequenced |
| | `StartPropertySequence` | `name`, `values` | |
| | `StopPropertySequence` | `name` | |
| `Stage` | `GetXYPosition` | | `x`, `y` |
| | `GetZPosition` | | `z` |
| | `SetXYPosition`, `SetRelativeXYPosition` | `x`, `y` | |
| | `SetZPosition`, `SetRelativeZPosition` | `z` | |
| | `GetStageSequenceCapacity` | | `capacity`, 0 when it cannot be sequenced |
| | `StartStageSequence` | `positions` | |
| | `StopStageSequence` | | |
| `Shutter` | `GetShutterOpen` | | `open` |
| | `SetShutterOpen` | `open` | |
| | `GetAutoShutter`, provider wide | | `enabled` |
| | `SetAutoShutter`, provider wide | `enabled` | |
| `Autofocus` | `RunAutofocus` | | |
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
| | `GetConfigSettings` | `group`, `config` | `settings`, an array of `device`, `property`, `value` |

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
`latestSlotIndex` with release order and sends `FrameAvailable`. The host writes a frame while the
frame sink of the driver is called, so a driver may hand over a frame that points into its own
buffer. In `PreviewLatest` mode it writes every frame but sends `FrameAvailable` at most every
16 ms, and a frame that came in between is announced when the 16 ms are up. The reader takes a slot by
changing its `state` from 2 to 3, copies the frame, and stores 2 again. A slot being read is
skipped by the writer, so the reader never sees a frame change under it.

## Stream rings

Each `StartSignal` and `StartDaq` gets a new ring in shared memory under the `sharedMemoryKey` of
its response, sized for about two seconds of that acquisition. The host writes the chunks of the
source or session into it, and ScopeOne reads them after each `StreamAvailable`. After
`StopSignal` or `StopDaq`, both ends keep the ring until the `SignalState` or `DaqState` event
with `Idle` or `Error`, which follows the last record. ScopeOne reads what is left and both ends
then release the ring. A source that ends with `Error` releases its ring without a stop. The
layout is `ScopeOneCore/internal/StreamRing.h`.

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
