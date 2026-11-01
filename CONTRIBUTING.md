# Development guidelines

These rules apply to everyone who changes ScopeOne. The README explains how to build; this file
explains how the code is meant to be written.

## Goals

- **Optimize for the reader.** Code is read far more often than it is written. Prefer the
  version a newcomer understands without asking.
- **Clear boundaries.** Every feature has one home, and the layers only depend downward.
- **Less code.** Code that does not exist has no bugs. Every line has to earn its place.
- **Consistency.** New code looks like the code around it, so the whole tree reads as one.

Each rule below says why. When a rule and its reason disagree in a specific case, follow the
reason and say so in the review.

## Boundaries

**Layers.** Dependencies point one way:

| Part | Folder | Namespace | May use |
|---|---|---|---|
| Core | `ScopeOneCore` | `scopeone::core`, `core::internal`, `core::imageops` | Qt Core, OpenCV, MMCore, ScopeWriter. No Qt Widgets. |
| Application | `src` | `scopeone::ui` | Core, Qt Widgets |
| MCP server | `src/ScopeOneMcpServer.cpp` | | Qt Core, Qt Network and the Local API protocol. Not Core. |
| Plugins | `plugins`, `ScopeOneCuda`, `ScopeOneInference` | their own | `scopeone::PluginSDK` |
| Python package | `ScopeOneCore/python/scopeone` | `scopeone` | the Local API protocol |

A lower layer never knows about a higher one. Core does not include from `src`, and plugins do
not include Core internals. This keeps each part buildable and usable without the ones above it.

**Entry points.** Code outside Core enters through the public headers in
`ScopeOneCore/include/scopeone` and the `ScopeOneCore` facade. `ScopeOneCore/internal` is for Core
alone. Plugins see only the SDK headers; a tool plugin gets `ScopeOneToolCore`, not the Core.
A narrow entry point is what lets Core change its insides without breaking callers.

**Decide the layer first.** Before writing a feature, decide where it belongs. Anything that
must work without a window (acquisition, hardware, processing, recording, data) goes into Core
and is reachable through the Local API before it gets a widget. Anything that only changes what
is shown goes into `src`. Hardware and optional processing go into plugins.

**Python API.** The `ScopeOne` client controls a running ScopeOne and moves frames in and out as
numpy arrays. It does not reimplement analysis that numpy already does. `scopeone.processing`
is the other direction: ScopeOne runs Python functions as pipeline modules.

**Namespaces.** Use the namespaces in the table. Do not add new levels.

**Threads.** The GUI thread never blocks: no waiting on hardware, files, the DriverHost or a
processing run there. Long work runs on a worker and reports back through a queued signal.
State that two threads touch is guarded by a mutex or an atomic, and the comment next to it
says which threads use it. Most hard bugs in an acquisition program are threading bugs.

## Code

**Problem first.** Write code for a problem someone has now, not for one that might come up.
Do not add API, options or extension points "for later". Speculative code is dead code that
has not been noticed yet.

**No dead code.** Delete functions without callers, signals without listeners and members that
are never read. Version control keeps the history.

**No defensive code.** Do not null-check objects that are always created, such as members set
in the constructor or managers wired when Core starts. Do not add fallback paths, branches for
old formats or versions, or `catch (...)`. A check that can never fail hides what the code
really guarantees.

**Handle real errors.** User input, files, hardware, the DriverHost, Python workers and the
network can fail. Check those, and return the error to the caller or write it to the log with a
message that says what failed and on which device or file. Never swallow an error.

**No duplication, within reason.** When the same logic appears a second time, move it into one
function in the lowest layer that both places already use. Do not move code down into Core only
to serve one distant caller; a small, local copy is better than a wrong dependency.

**Files.** `src` stays flat. Split files by responsibility. Do not create tiny files or new
subfolders.

**Comments.** A comment says why, not what the next line does. A public function gets a short
comment when its name does not tell the whole contract (units, ownership, threads, when it
fails). Match the comment density of the file you are in.

## C++ style

Match the layout of the surrounding code. Do not reformat lines you are not changing, and do
not send style-only changes.

- C++20.
- Four spaces, no tabs, braces on their own line, 100 columns.
- Headers start with `#pragma once`.
- Includes: the file's own header, then ScopeOne headers, then Qt and other libraries, then the
  standard library. Include what you use; forward-declare in headers where a pointer or
  reference is enough.
- Types are `PascalCase`, functions and variables `camelCase`, members `m_name`, constants
  `kName`.
- `enum class`, not plain `enum`.
- `auto` only when the type is obvious from the right-hand side or is an iterator or lambda.
- No `using namespace` in headers.
- QObjects are owned by their Qt parent; anything else that owns memory uses `std::unique_ptr`.
  No bare `delete`.
- Connections use the function-pointer form of `connect`. Signals are raised with `emit`.
- Constant strings use `QStringLiteral`; text the user sees goes through `tr()`.
- Our own code does not throw. MMCore, OpenCV and ONNX Runtime do, so catch their exceptions at
  the call site and turn them into an error value or a log message.
- Casts: `qobject_cast` for QObjects, `static_cast` otherwise. `dynamic_cast` only across plugin
  interfaces.
- Logging: `qWarning` for real problems. Do not leave `qDebug` behind.

## Versions

**Core.** The version is `MAJOR.MINOR.MMDD`: `MAJOR.MINOR` comes from `VERSION`, the last part is
the date of the last commit. Raise `MINOR` whenever a public SDK header changes. ScopeOne loads
a plugin only when its Core `MAJOR.MINOR` matches, and each plugin kind has its own
`scopeOneApi` contract version.

**Dependencies are pinned.** vcpkg packages, Python (`==3.14.*`) and Python packages
(`numpy==2.5.3`) use exact versions. Upgrade them together, on purpose.

**Data formats.** Documents and files carry a `schemaVersion`. A format change raises it, and a
file with another version is rejected with a clear message rather than read through a
compatibility path.