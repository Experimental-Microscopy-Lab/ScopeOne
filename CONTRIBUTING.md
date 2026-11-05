# Development guidelines

These rules apply to everyone who changes ScopeOne. The README explains how to build: on Windows,
`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --run` after installing Git and the
Visual Studio Build Tools, and on Linux and macOS `./scripts/build.sh`. This file explains
how the code is meant to be written. Each rule says why. When a rule and its reason
disagree in a specific case, follow the reason and say so in the review.

## Boundaries

**Layers.** Dependencies point one way:

| Part | Folder | Namespace | May use |
|---|---|---|---|
| Core | `ScopeOneCore` | `scopeone::core`, `core::internal`, `core::imageops`, `scopeone::cuda` and `scopeone::inference` for the CUDA and ONNX modules | Qt Core, OpenCV, MMCore, ScopeWriter, and CUDA and ONNX Runtime when they are built. No Qt Widgets. |
| Application | `src` | `scopeone::ui` | Core, Qt Widgets |
| MCP server | `src/ScopeOneMcpServer.cpp` | | Qt Core, Qt Network and the Local API protocol. Not Core. |
| Drivers | `drivers` | their own | `scopeone::DriverSDK` |
| Python package | `ScopeOneCore/python/scopeone` | `scopeone` | the Local API protocol |

A lower layer never knows about a higher one. Core does not include from `src`, and drivers do not include
Core internals. This keeps each part buildable and usable without the ones above it.

**Entry points.** Code outside Core enters through the public headers in
`ScopeOneCore/include/scopeone` and the `ScopeOneCore` facade. `ScopeOneCore/internal` is for Core
alone. Drivers see only `ScopeOneCore/sdk`, enforced by the build.
A narrow entry point is what lets Core change its insides without breaking callers.

**Decide the layer first.** Before writing a feature, decide where it belongs. Anything that
must work without a window (acquisition, hardware, processing, recording, data) goes into Core
and is reachable through the Local API before it gets a widget. Anything that only changes what
is shown goes into `src`. Hardware goes into drivers.

**Tools.** A window of the Tools menu, with its own state and workflow, goes into its own folder
under `src/tools`. It uses only the public Core headers and `ScopeOneToolContext` from
`src/tools/ToolContext.h`, and registers itself with `SCOPEONE_REGISTER_TOOL` at the end of its
source file, so adding a tool changes neither `MainWindow` nor CMake. Its algorithm belongs in Core.
A command that takes an image and parameters and returns a result is not a tool; it belongs in the
image menus.

**Extension boundary.** Drivers and Python processing modules are the only public
extension points, and both run outside the ScopeOne process. Every other processing method
belongs in Core, with its window in `src`. The CUDA and ONNX modules are in Core too, under
`src/cuda` and `src/inference`, and `SCOPEONE_ENABLE_CUDA` and `SCOPEONE_ENABLE_INFERENCE` build
Core without them. The
messages between ScopeOne and `ScopeOne_DriverHost` are internal too and are described in
`ScopeOneCore/DRIVERHOST_PROTOCOL.md`.

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

**Separate errors from invariants.** An error is something that can happen: user input, a
file, a device, the network, a worker. It is checked and reported. An invariant is something
the program guarantees, such as a member set in the constructor, a manager wired when Core
starts, or a value another function has already validated. An invariant is not checked again
at every use, and a branch that can never run is not written for it. Such a branch makes the
reader ask when it runs, and it hides the guarantee that the code relies on. State the
guarantee once, where it is made (see "State what must hold"). The same goes for fallback
paths, branches for formats or versions that are no longer produced, and `catch (...)` that
swallows what it does not understand.

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

**State what must hold.** Where code relies on a guarantee that the type system does not show,
write `Q_ASSERT` or `Q_ASSERT_X` with a message, and `Q_UNREACHABLE()` after a `switch` that
covers every case. An assertion documents the guarantee and fails loudly in a debug build. A
silent `if` that skips the case hides it. Input from outside is never asserted; it is checked
(see "Handle real errors").

**Keep functions short.** A function does one thing. Past about 60 lines, look for a part that
has its own name, such as a loop that decides something (make it a predicate), a block that
builds a widget or a response, or a lambda that grew. Long `setup` functions that only call
other setup functions are fine.

**Return early.** Handle the failing or trivial case first and return, so the main path is not
nested. Do not write `else` after a `return`, `continue` or `break`.

**One way to build a result.** Responses, messages, settings keys and file names are built by
one helper each, not typed out at every call site. A helper that exists is used; if it does not
fit, change it.

**Do not compute the same fact twice.** If two places must agree, such as which layer is the
active one or when a button is enabled, one function answers and both call it. A rule written
in two places drifts apart.

**Names say what a thing is.** No abbreviations that only the author knows, no `Manager` or
`Helper` for something that has a more exact name, no names that differ only in a number.
A boolean reads as a question (`isReady`, `hasCamera`).

**No leftovers.** No commented-out code, no `#if 0`, no `TODO` without a reason after it, no
debug output, no files that are not used. A `TODO` says what is missing and why it is not done.

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
- Casts: `qobject_cast` for QObjects, `static_cast` otherwise. `dynamic_cast` only across library
  interfaces.
- Logging: `qWarning` for real problems. Do not leave `qDebug` behind.

### Qt

- **Connect once.** A connection is made when the objects are built. Code that runs again and
  again, such as a handler for "the active viewer changed", does not call `connect`: every call
  adds one more connection and the slot then runs several times. If a connection must follow
  an object that changes, `disconnect` the old one first.
- **Name the receiver.** Connect a lambda with a context object (`connect(sender, &S::sig, this,
  [..] {..})`), so that it is removed when the receiver is destroyed. Capture `this` only with
  that context.
- **Signals are facts, slots are actions.** A signal says what happened (`layerRemoved`), not
  what to do about it. Do not emit a signal from a setter when the value did not change.
- **Block signals on purpose.** When code sets a widget to show state, use a `QSignalBlocker`
  so that setting it does not feed back into the model. Do this in one place per panel.
- **Do not hold a reference into a temporary.** `QMap::value()` and `QHash::value()` return a
  copy, so a reference taken from it dangles. Use `constFind`, `std::as_const` or a copy you
  name. Do not modify a container while you loop over it.
- **Widgets belong to the GUI thread.** Core signals that reach a widget from a worker thread
  are connected as queued, and the widget never touches a worker's data directly.
- Every `QObject` subclass has `Q_OBJECT`. Overrides are marked `override`. File-local functions
  are in an anonymous namespace. A variable does not shadow another one.
- Compare floating point values with `qFuzzyCompare`, and give both branches of `?:` the same
  type.
- Do not inherit from a Qt container.

## Hardware and the frame path

**Nothing moves without a request.** Loading a configuration, opening a window or reading a
value never moves a stage, opens a shutter or changes an exposure. Motion and writes to a
device follow from an action of the user or from an acquisition plan that the user started.
This applies to the Local API and the MCP server as well; a command that moves hardware or
destroys data needs an explicit confirmation from the caller.

**Hardware calls can fail and can be slow.** Every call into a device, MMCore or a DriverHost
has an error path that names the device, and a timeout that suits it. A timeout is a number
with a reason in the comment next to it, because a value that is too short fails only on slow
devices.

**The frame path is hot.** A frame is not copied, allocated or converted per frame unless the
code says why. Frames travel through the shared ring and are read from it by index. Anything
that runs for every frame, such as logging, string building or a lock held across a call, needs
a reason.

**Platforms.** The code builds and runs on Windows and Linux. A platform difference lives in a
small function of its own that says what each platform does, not in `#ifdef Q_OS_*` blocks
inside a long function. Paths are built with `QDir` and `QFileInfo`, not by joining strings
with a slash.

## Changes

**One purpose per change.** A change fixes one thing or adds one thing. Reformatting, renaming
and moving code are separate changes, because they hide the real change in review. Do not touch
lines you are not changing.

**Build everything you touched.** Core, the drivers and the application all build before you
send a change, with no new warnings. A change in a header builds every part that includes it.

**Run what you changed.** A change to behavior is run in the application, with a configuration
that has the devices involved (the demo configurations in `config` need no hardware). The
change says what was run. For a bug fix it says how to see the bug.

**Scripted and generated changes.** A change made by a script, a search and replace, or a code
generator can compile and still be wrong, for example a helper that now calls itself. Read the
result as if someone else had written it, and run it. The author of the change answers for it,
whoever or whatever wrote the lines.

**One public surface.** A Local API command is added to the server, its entry in
`capabilities`, the Python client and the MCP server in the same change. A command that exists
in only some of them is a bug.

**New dependencies.** A new library needs a reason that the standard library, Qt, OpenCV and
the existing dependencies cannot meet, a licence that allows redistribution with ScopeOne, and
a pinned version in `vcpkg.json`.

## Versions

**Core.** The version is `MAJOR.MINOR.MMDD`: `MAJOR.MINOR` comes from `VERSION`, the last part is
the date of the last commit. Raise `MINOR` whenever a public SDK header changes. ScopeOne loads
a driver only when its Core `MAJOR.MINOR` matches, and drivers have their own `scopeOneApi`
contract version.

**Dependencies are pinned.** vcpkg packages, Python (`==3.14.*`) and Python packages
(`numpy==2.5.3`) use exact versions. Upgrade them together, on purpose.

**Data formats.** Documents and files carry a `schemaVersion`. A format change raises it, and a
file with another version is rejected with a clear message rather than read through a
compatibility path.