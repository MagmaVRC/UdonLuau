# UdonLuau

A compiler from [Luau](https://luau.org) to VRChat Udon bytecode, written in C++20.

UdonLuau knows nothing about any particular SDK version. The host fills a `Catalog` with the
types, extern signatures and events it exposes, normally by enumerating the live Udon wrapper
modules, and the compiler resolves every member access, operator and overload against it. When
VRChat exposes a new extern, it is usable from Luau the moment the host sees it.

## Installing in Unity

Requirements: Windows, Unity 2022.3 or Unity 6, and the VRChat Worlds SDK.

Download the latest release from [Releases](https://github.com/MagmaVRC/UdonLuau/releases) and use one of:

- **`dev.magmavrc.udonluau-<version>.zip`:** in Unity, open Window > Package Manager, choose + > Add package from tarball/disk... and pick the extracted `package.json`. Alternatively, extract it into your project's `Packages/` folder.
- **`UdonLuau-<version>.unitypackage`:** Assets > Import Package > Custom Package. It installs into `Packages/dev.magmavrc.udonluau`. Samples are only available from the `.zip` install.

Installing straight from the git URL is not supported: the native compiler DLL is a build output that only the release files contain.

**Updating:** install the new release over the old one; the editor can stay open. Unity never loads the package's `UdonLuau.dll` itself. It loads a private copy under `Library/UdonLuau/`, so the file is never locked, and the next script reload picks up the new compiler.

Then:

1. Right-click in the Project window and choose Create > VRChat > UdonLuau Script, or drop a `.lua` file into your project.
2. Drag the script onto a GameObject.
3. Install the [Luau Language Server](https://marketplace.visualstudio.com/items?itemName=JohnnyMorganz.luau-lsp) extension for VS Code. The package writes the definitions and workspace settings it needs, so the whole VRChat API autocompletes and type-checks.

## Layout

| path | contents |
|---|---|
| `include/UdonLuau/` | public C++ API (`Catalog.hpp`, `Compiler.hpp`, `Program.hpp`) and the C API (`UdonLuau.h`) |
| `src/` | the compiler |
| `tests/` | end-to-end tests that execute compiled programs on a reference interpreter |
| `projects/` | Visual Studio projects; open `UdonLuau.slnx` |
| `extern/luau` | Luau, as a submodule (only the parser is used) |

Projects:

- **UdonLuau.Core**: static library with the compiler.
- **Luau.Ast**: static library with the Luau parser. It is kept separate so hosts that already build Luau can link Core alone.
- **UdonLuau.Native**: `UdonLuau.dll`, exposing the C API for P/Invoke.
- **UdonLuau.Tests**: console test runner.

## Building

Requires Windows and Visual Studio 2022 or later with the C++ workload.

```
git clone --recursive https://github.com/MagmaVRC/UdonLuau
cd UdonLuau
pwsh tools/build.ps1
```

`tools/build.ps1` builds the static libraries, `UdonLuau.dll` and the tests into `build/Release`, then runs the tests. `tools/package.ps1` builds and produces the release files in `dist/`: the package `.zip`, the `.unitypackage`, a native SDK zip (DLL, import and static libraries, headers) and `SHA256SUMS.txt`. For development in Visual Studio, open `UdonLuau.slnx`.

Releases are published by pushing a `v<version>` tag that matches the package version. The release workflow builds, tests, packages and attaches the files.

## Unity package

`unity/dev.magmavrc.udonluau` is the editor integration for VRChat world projects (Windows, Unity 2022.3 and Unity 6, VRChat Worlds SDK).

- Builds the catalog from the Udon wrapper modules, types and event definitions the editor has loaded.
- Imports scripts as `.lua` files (`.luau` is accepted too). Assets > Create > VRChat > UdonLuau Script creates a script and its program asset.
- Recompiles when a script changes, after a domain reload, and before a world build. A build with script errors is blocked.
- Reports errors in the console with file and line, so double-clicking opens the script.
- Maps Udon runtime errors back to the Luau line that caused them, using a line table compiled into each program asset.
- Draws exported variables with `@header`, `@space`, `@tooltip`, `@range`, `@hideininspector` and `@multiline`.
- Applies the script's sync mode to the UdonBehaviour, and shows a sync method picker when the mode is `any`.
- Holds project-wide defines in Project Settings > UdonLuau, and the **Compatible exit return** option. It makes every entry point start with UdonSharp's `0xFFFFFFFF` exit marker and leave through the return trampoline, so tools that expect UdonSharp's layout (such as obfuscators) can read the programs. It costs a few instructions per event, so it is off by default.

When working from source, copy `build/Release/UdonLuau.dll` to `Editor/Plugins/x86_64/` in the package; `tools/package.ps1` does this too. The editor loads a private copy of it from `Library/UdonLuau/`, so a new build is picked up on the next domain reload, or through Tools > UdonLuau > Reload Native Compiler, without restarting Unity.

Each script also gets:

- a program asset next to it, created automatically;
- a generated component class named after it, which the inspector shows instead of a raw UdonBehaviour. This component lists the exported variables, the sync method picker when the mode is `any`, and a Methods foldout showing each public method's entry point and whether it is network callable;
- an abstract UdonSharp class with the same name, so UdonSharp code can call it directly (`GetComponent<Door>()`, `door.speed`, `door.Open()`).

These generated C# files change only when a script's public interface changes. Other edits recompile just the Luau program, and in play mode the running behaviours are hot-swapped.

Dragging a script onto a GameObject in the Hierarchy, Scene view or Inspector adds it. Projects that also contain unrelated `.lua` text files can turn off `.lua` handling in Project Settings > UdonLuau and use `.luau` instead.

## Usage

```cpp
UdonLuau::Catalog catalog;
catalog.AddStandardEvents();
catalog.AddType({ .fullName = "UnityEngine.Transform", .kind = UdonLuau::TypeKind::Class, .baseType = "UnityEngineComponent" });
catalog.AddExtern("UnityEngineTransform.__get_position__UnityEngineVector3", 2);

UdonLuau::CompileResult result = UdonLuau::Compile(catalog, source);
if (result.Succeeded()) {
    const UdonLuau::Program& program = *result.program;
    // program.ByteCode(), program.heap, program.entryPoints, program.sync
}
```

The operand count passed to `AddExtern` is what the wrapper module reports for the signature. It
tells the compiler whether the extern takes a receiver.

A `Program` holds everything needed to build an `IUdonProgram`:

- the big-endian bytecode;
- one heap slot per symbol, with its Udon type name and initial value;
- the exported entry points;
- the sync metadata.

## The language

UdonLuau compiles a statically typed subset of Luau. Each file becomes one UdonBehaviour program.

```lua
-- @header("Motion")
-- @range(0, 10)
export local speed: number = 2
-- @sync(linear)
local height: number = 0

local function wave(t: number): number
    return Mathf.Sin(t * speed)
end

function Update()
    height = wave(Time.time)
    local p = transform.position
    p.y = height
    transform.position = p
end

function OnPlayerJoined(player: VRCPlayerApi)
    print(`{player.displayName} joined`)
end

function Reset()
    height = 0
end
```

### Declarations

- Module-level `local`s are behaviour variables; their initializers must be constants.
- `export local` exposes a variable in the inspector, using Luau's export syntax.
- `-- @sync`, `-- @sync(linear)` and `-- @sync(smooth)` add sync metadata.
- `-- @syncmode(mode)` at the top of the file sets the behaviour sync mode. It uses UdonSharp's five modes and enforces the same rules at compile time:

  | mode | behaviour | rules |
  |---|---|---|
  | `any` (default) | The sync method is picked on the UdonBehaviour | Synced types and interpolation must be supported |
  | `none` | Sync method None | No synced variables, no `@networkcallable` methods |
  | `novariablesync` | Follows the other behaviours on the GameObject | No synced variables; network events still work |
  | `continuous` | Sync method Continuous | No synced arrays |
  | `manual` | Sync method Manual | No `linear`/`smooth` interpolation |

  Which types can be synced, and which support interpolation, comes from the SDK through the host (`Catalog::AddSyncableType`).
- `-- @onchange(OnScoreChanged)` above a variable calls that function whenever another behaviour or the network changes the variable (`SetProgramVariable`, or a synced value arriving). The function takes no parameters, or one: the previous value. Writes from the script's own code don't trigger it.
- Any other `-- @name(args)` annotation directly above a variable is kept on its heap slot for the host, for example `@range(0, 10)`, `@header("Motion")` or `@tooltip("...")`. The compiler does not interpret these; the editor decides what they mean.
- Functions:
  - A global function named after a VRChat event (`Start`, `Update`, `Interact`, `OnPlayerJoined`, ...) receives that event, with its parameters.
  - `export function Name(...)` is a public method that other behaviours can call. Its entry point is `_Name`, or `__0__Name` when it takes parameters. VRChat never runs entry points starting with `_` for network events, so a public method cannot be triggered by other clients unless marked as below.
  - Entry points, parameter symbols (`__0_value__param`) and result symbols (`__0___0__Name__ret`) follow UdonSharp's naming rules exactly, so UdonSharp code can call Luau methods natively.
  - `-- @networkcallable` above an `export function` makes it callable over the network. `-- @networkcallable(10)` also sets the rate limit in events per second.
    - The entry point is then `Name`, and the program carries VRChat's network-calling metadata.
    - It may take up to 8 parameters, which arrive with the event, and cannot return values.
  - `-- @entry("OnDataUpdated")` above an `export function` sets its exact entry point name, for callbacks other scripts send by a fixed name. Without `@networkcallable`, a name that doesn't start with `_` can still receive VRChat's legacy network events without arguments.
  - Events can return a value, as `OnOwnershipRequest` must: `function OnOwnershipRequest(requester: VRCPlayerApi, newOwner: VRCPlayerApi): boolean`. The value goes into `__returnValue`, where VRChat and UdonSharp read it.
  - The inspector lists each public method's entry point, parameter symbols and return symbol. Plain `UdonBehaviour` callers use those names with `SetProgramVariable`, `SendCustomEvent` and `GetProgramVariable`.
  - `local function` is private.
  - Parameters and return values need type annotations.
- `this`, `gameObject` and `transform` refer to the behaviour itself.

### Types

- **Numeric aliases:** `int`, `uint`, `long`, `ulong`, `short`, `ushort`, `byte`, `sbyte`, `float` (also `number`), `double`.
- **Other built-in types:** `boolean`, `string`, `any`.
- **Engine and SDK types:** short names (`Vector3`, `Transform`, `VRCPlayerApi`).
  - A name shared by several namespaces resolves through the preferred namespaces, highest priority first: UnityEngine, VRC.SDKBase, VRC.SDK3.Components, VRC.SDK3.Data, VRC.Udon, System. So `Object` is `UnityEngine.Object`; `System.Object` is `any`. Hosts can change the list.
  - Anything else is written qualified (`UnityEngine.Random`) or through an alias: `type UObject = UnityEngine.Object` for annotations, and `local SDK3 = VRC.SDK3.Components` for namespaces, which then also works in annotations (`x: SDK3.VRCPickup`).
  - Variables shadow type names, so a field called `Object` is just a field.
- **Arrays:** `{T}`, built with `{a, b, c}` or iterated with `for i, v in array do`. Indices start at 0, as everywhere in Udon.
- **Inference:** locals take the type of their initializer. Integer literals are `int` and other number literals are `float`, unless the context needs another numeric type.
- **Constant structs:** a struct produced by a constructor with literal arguments is a shared constant, so changing one of its fields directly is an error. Copy it into a local first; a local that is later modified gets its own copy automatically.
- **Conversions:** widening numeric conversions are implicit. Narrowing ones are written `value :: int`.
- **Characters:** a one-character string literal passes where a `char` is expected, and any string literal passes as a `char[]`, so `s:Split(",")` works.

### Expressions

- **Members:** `obj.Property`, `obj:Method(args)`, `Type.StaticMember`, `Type.new(args)` (constructors) and `Enum.Member`.
- **Generic methods:** pass the type as the last argument, `obj:GetComponent(Rigidbody)`, or explicitly with `obj:GetComponent<<Rigidbody>>()`.
- **Out parameters:** these become extra return values, for example `local hit, info = Physics.Raycast(origin, direction)`.
- **Operators:** they map to the operator externs, and constant folding gives the same results as run time.
  - `/` on integers divides as floats.
  - `//` truncates toward zero on integers and floors on floats.
  - `%` is the remainder with the sign of the dividend, as in C#.
  - `..` and backtick strings concatenate, and `#` gives `Length` or `Count`.
  - `and`, `or` and `not` short-circuit on booleans.
  - `value :: int` rounds a float the way `System.Convert` does.
- **Truthiness:** an object in a condition means "is not nil".
- **Built-ins:** `print`, `warn` and `tostring`, plus the standard library below.

### Standard library

Luau's `math`, `string`, `table`, `bit32` and `utf8` libraries are compiled onto Udon externs. Where Udon has no direct equivalent, the compiler emits a short inline routine instead. Array positions are 0-based, as everywhere else.

| Library | Functions | Notes |
|---|---|---|
| `math` | `abs`, `floor`, `ceil`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan(y [, x])`, `exp`, `log(x [, base])`, `log10`, `pow`, `min`, `max` (any count), `clamp`, `sign`, `round`, `fmod`, `lerp`, `map`, `random`, `noise`, `pi`, `huge` | `round` rounds halves away from zero, as in Luau. `random(m, n)` includes both bounds. `floor`, `ceil` and `round` of an integer return it unchanged. `noise(x [, y])` is `Mathf.PerlinNoise` scaled to -1..1. |
| `string` | `format`, `len`, `upper`, `lower`, `rep`, `split`, `sub`, `find`, `byte`, `char`, `reverse`, `trim`, `startswith`, `endswith` | `format` takes a literal format string. It is translated to `String.Format` when compiling and supports `%d %i %s %f %.Nf %g %e %x %X %%`, widths and `-`/`0` flags. `sub`, `find` and `byte` use Luau's 1-based positions, and `sub` accepts negative ones. `find` searches plain text only (pass `plain = true` for text with pattern characters) and returns `0, 0` when nothing is found. Comparisons are ordinal. For anything else, use the string's own methods (`s:Substring(0, 3)`). |
| `table` | `insert`, `remove`, `find`, `create`, `clone`, `clear`, `concat`, `sort`, `move` | Udon arrays have a fixed size. `insert` and `remove` build a new array and store it back into the variable or field you passed, so other references keep the old array. `find` returns -1 when the value is missing. `create(n, value)` fills by doubling copies, so it costs about 5·log2(n) externs rather than n. `sort(t, less)` takes an optional comparison, which must be a named local function; it then runs an insertion sort. |
| `bit32` | `band`, `bor`, `bxor`, `bnot`, `lshift`, `rshift`, `arshift`, `btest` | On `int`. `rshift` is logical and `arshift` arithmetic, as in Luau. Constant arguments are folded. |
| `utf8` | `len`, `char` | `len` counts code points, treating surrogate pairs as one. |

Every polyfilled function is listed in one registry, `src/Polyfills.hpp`, together with its Luau signature. The registry decides which library calls the compiler accepts, produces the "available: ..." list in error messages, and generates the luau-lsp declarations for libraries Luau does not have (such as `Delay`). To add a polyfill, add a registry entry and handle the call in the matching `Call*` function in `Compiler.cpp`.

### Delayed calls

Udon can delay an event with no arguments (`SendCustomEventDelayedSeconds`) or send a network event with arguments right away (`SendCustomNetworkEvent`). It cannot do both at once, and it cannot delay a call with arguments at all. `Delay` adds both:

```lua
export local door: Door

local function flash(times: int, color: Color)
    -- ...
end

function Interact()
    Delay.Seconds(2, this):flash(3, Color.red)              -- a local function, with arguments
    Delay.Frames(10, door):Open()                           -- another script's method
    Delay.Seconds(1.5, Network.All(door)):Hit(25)           -- a network event with arguments, sent later
    Delay.Seconds(0.5, this, EventTiming.LateUpdate):flash(1, Color.white)
end
```

- **Targets:**
  - `this`, for any function in the script;
  - a typed script reference, for its public methods;
  - a `Network.X(...)` target, for `@networkcallable` methods;
  - a plain `UdonBehaviour`, for custom events without arguments.
- **No-argument calls** to a public method compile straight to `SendCustomEventDelayedSeconds`/`Frames` and cost nothing extra.
- **Calling the SDK directly:** `this:SendCustomEventDelayedSeconds(name, t)` also works; the `EventTiming` defaults to `Update`. Sending a function's name instead of its entry point (`"Refresh"` for an entry point `_Refresh`) is a compile warning, because Udon would silently ignore it.
- **Calls with arguments:** each call site gets a queue for its arguments and a private stub event (`__delayN`). The arguments and the target are captured when the call is made, and the stub delivers them when the delay ends.
  - Delayed network calls are sent when the delay ends, by the client that made the call.
- **Ordering:** with a constant delay, calls from one site are delivered in the order they were made. When the delay is a variable, each call records its due time and the stub delivers the earliest one first.
  - A delayed call with arguments needs a constant `EventTiming`.
- **Repeating calls:** a function can delay a call to itself, as in a repeating tick. Functions used this way are compiled as real functions rather than inlined.
- **Cost:** queuing a call costs about 3 externs per argument. Delivering it costs about 4 per argument.

### Coroutines

Any event or function can wait in the middle of its body. The `task` library follows Roblox:

```lua
function Interact()
    door:Play("Open")
    task.wait(2)                 -- seconds; task.wait() waits one frame
    collider.enabled = false
    task.waitFrames(1, EventTiming.LateUpdate)
    task.waitUntil(door.closed)  -- re-checked once per frame
    sound:Play()
end

local function blink(times: int)
    for i = 1, times do
        light.enabled = not light.enabled
        task.wait(0.25)
    end
end

function Start()
    task.spawn(blink, 6)         -- runs now until its first wait, then Start continues
    task.delay(10, blink, 2)     -- starts in 10 seconds
    task.defer(blink, 1)         -- starts next frame
end
```

| Call | What it does | Cost |
|---|---|---|
| `task.wait(seconds [, timing])` | Suspends; returns the seconds actually waited when you use the result | 1 extern to suspend, plus 2 when the result is used |
| `task.waitFrames(n [, timing])` | Suspends for `n` frames | 1 extern |
| `task.waitUntil(condition)` | Checks the condition now, then once per frame until true | the condition, plus 1 extern per frame while waiting |
| `task.spawn(fn, ...)` | Runs `fn` now until its first wait, then returns | the arguments only |
| `task.defer(fn, ...)`, `task.delay(seconds, fn, ...)` | Starts `fn` next frame or after a delay, through `Delay` | as `Delay` |
| `task.cancel(fn)` | Stops `fn`'s suspended run | a few copies |

How it works: Udon locals already live on the behaviour's heap, so a suspended function keeps its state for free. Each wait schedules a private event (`__coN`) with `SendCustomEventDelayedSeconds`/`Frames` and ends the current run; that event jumps back to the statement after the wait. Nothing runs while a coroutine waits.

- **Where waits work:** in events, public methods, and functions they call. A function that waits makes its callers wait too; it is inlined into each caller, so two callers never share its locals. Mark it `-- @noinline` to compile it once instead: it then serves one caller at a time, a second caller skips the call (logged in debug builds), and only callers with `@reentry(ignore)` that are never cancelled may use it.
- **Triggered again while waiting:** a waiting function is still the same function with the same locals, so a second trigger has to be decided. `-- @reentry(mode)` above the function picks:

  | mode | second trigger | runtime cost |
  |---|---|---|
  | `ignore` (default) | Dropped. Debug builds log `Door.Interact ignored: still waiting at line 14`. | 2 copies per run |
  | `restart` | Stops the waiting run and starts again: a cooldown that resets, a timer that restarts | a few copies per run, about 4 more externs per timed wait |
  | `overlap(n)` | Starts another run, up to `n` at once; each run has its own locals and parameters. With all `n` busy, the trigger is dropped (logged in debug builds). | the function is compiled `n` times; picking a free run is one branch per run, with no externs |

  A run is never entered twice: a trigger that arrives while a run is executing (not waiting), for example through a `SendCustomEvent` back into the script, is dropped, or with `overlap(n)` takes another free run. `task.cancel(fn)` stops every run of `fn`. A run that gets cancelled by an event it triggered itself ends as soon as control comes back to it.
- **Event arguments** are copied when the event starts, so `player` in `OnPlayerJoined(player)` keeps its value across waits.
- **Public methods that wait cannot return values**, since callers get control back at the first wait. Waits are local to each client; they are not synced.
- **Differences from Roblox:** `task.cancel` takes the function rather than a thread. `task.defer` waits one frame. `task.waitUntil` is UdonLuau's own: the condition is an expression that is evaluated again every frame.

### Signals and events

A `Signal` is an event a script declares, fires and waits on, with typed values:

```lua
export local opened: Signal<VRCPlayerApi> = Signal()   -- export: other scripts can use it too
local hit: Signal<number, Vector3> = Signal()

local function onHit(damage: number, at: Vector3)
    health -= damage
end

function Start()
    hit:Connect(onHit)
end

export function Watch()
    while true do
        local damage, at = hit:Wait()
        print(`took {damage} at {at}`)
    end
end

function Interact()
    hit:Fire(10, transform.position)
    opened:Fire(Networking.LocalPlayer)
end
```

- `Fire` runs every waiting coroutine and connected function before it returns, in source order. A waiter that waits again on the same signal is resumed by the next `Fire`, not the current one.
- `Connect(fn)` and `Disconnect(fn)` take a named function of the script whose parameters match the signal. Connecting the same function twice connects it once.
- **Cost within a script:** signals have no heap value of their own. The compiler knows every place that waits or connects, so `Fire` is a few copies and one jump per waiter, with no externs and no event dispatch.
- **Across scripts:** `door.opened:Wait()`, `door.opened:Connect(fn)` and `door.opened:Fire(player)` work on another script's `export local` signal through a typed reference. The waiting script registers once per wait (2 `SetProgramVariable` and 1 `SendCustomEvent`); `Fire` costs one `SetProgramVariable` per value plus one `SendCustomEvent` per waiting or connected behaviour. Destroyed behaviours are skipped.
- **Firing during delivery:** firing a signal while that signal is still delivering a `Fire` (from a waiter or connected function) is skipped, and logged in debug builds. A handler that leads back into the function that fired is reported as recursion when compiling.

`Events` waits for VRChat events without writing a handler:

```lua
export function Greet()
    local player = Events.OnPlayerJoined:Wait()
    print(`hello {player.displayName}`)
end
```

- The script's own handler for the event, if it has one, runs first, then the waiters.
- `Events.Update:Wait()`, `LateUpdate`, `FixedUpdate` and `PostLateUpdate` are frame waits with the matching timing, so they add no per-frame event.
- Waiting for `Interact`, `OnCollision*`, `OnAnimatorMove`, `OnRenderObject` or `OnOwnershipRequest` needs a handler in the script, because declaring those events changes how VRChat treats the object (an `Interact` handler makes it interactable, and an ownership request needs an answer).
### Lists

`List<T>` is a growable list compiled to an array and a count, with the capacity doubling as it fills:

```lua
local enemies: List<Transform> = {}
local scores: List<int> = {10, 20}

function Start()
    local buffer: List<Vector3> = List.new(64)
    scores:Add(30)
    table.insert(scores, 0, 5)
    for i, score in scores do
        print(i, score)
    end
    print(#scores, scores:Contains(20))
end
```

| Operation | Externs |
|---|---|
| `list[i]`, `list[i] = v` | 1 |
| `#list`, `list.Count`, `list.Capacity` | 0 |
| `Add` | 3, plus a copy when the capacity doubles |
| `Insert`, `RemoveAt` | about 5, shifting the elements with one `Array.Copy` |
| `Remove`, `IndexOf`, `Contains` | 1-2 for the search |
| `Clear`, `ToArray`, `Sort`, `Reverse` | 1-2 |
| One step of `for i, v in list` | 3 |

`table.insert`, `table.remove`, `table.find`, `table.clear`, `table.sort` and `table.concat` also work on lists.

- **Element types:** struct elements such as `Vector3` stay unboxed.
- **Removing elements:** removed reference elements are cleared, so they can be garbage collected.
- **Index checks:** when `DEBUG` is defined, every index is checked against the count.

A list is two hidden variables, so a few restrictions apply:

- A list cannot be copied or reassigned from another list. `list = {}` or `list = List.new(n)` resets it, and `list:ToArray()` gives a copy.
- A list cannot be exported, synced or returned, and public methods cannot take one. Export or sync an array instead.
- Local functions can take a list if they are inlined. Mark larger ones `-- @inline`.

For lists shared between behaviours or turned into JSON, use VRChat's `DataList`.

Each `insert` or `remove` costs a constant 5-8 externs whatever the array's length. Each one also allocates a new array, so prefer `table.create` with a known size in hot loops.

### Other behaviours

Behaviour scripts, Luau or UdonSharp, are types. Name one to hold a typed reference, then use its public members directly:

```lua
export local door: Door

function Interact()
    door:Open()
    local total = door:Add(2, 40)
    door.speed = 5
    Network.All(door):Open()
end
```

These compile to what Udon understands:

| Luau | Udon |
|---|---|
| `door:Add(2, 40)` | `SetProgramVariable` for each argument, `SendCustomEvent("Add")`, `GetProgramVariable` for the result |
| `door.speed` | `GetProgramVariable("speed")` |
| `door.speed = 5` | `SetProgramVariable("speed", 5)` |
| `Network.All(door):Hit(7)` | `SendCustomNetworkEvent(NetworkEventTarget.All, "Hit", 7)`. Any `NetworkEventTarget` member works as the name. Only `@networkcallable` methods are allowed. |

UdonSharp properties read and write like fields (`manager.Ready`, `manager.Level = 2`); they call the compiled `get_`/`set_` methods. Overloaded UdonSharp methods are picked by their argument types, as C# does.

`Network.All(this):Ping(3)` calls a `@networkcallable` function of the script itself, with its argument types checked.

Names, argument counts and types are checked when compiling. Typed references are also usable in arrays (`{Door}`). Arrays of behaviours are stored as `Component[]`, as UdonSharp does, because Udon has no `UdonBehaviour[]` externs. A plain `UdonBehaviour` still has every SDK member (`SendCustomEvent`, `GetProgramVariable`, ...), and converts to a script type with `behaviour :: Door`.

`GetComponent(Door)`, `GetComponentInChildren(Door)` and `GetComponentInParent(Door)` return the behaviour running that script. A GameObject can hold several UdonBehaviours, so the compiler loops over them and compares each one's `__refl_typename`, the same type name UdonSharp uses.

UdonSharp code can't take a Luau script in an inspector field typed as the script's generated class, because UdonSharp's serializer clears it. Use a plain `UdonBehaviour` field and cast it once:

```csharp
public UdonBehaviour doorBehaviour;
private UdonLuau.Scripts.Door door;

void Start() => door = (UdonLuau.Scripts.Door)(Component)doorBehaviour;
```

The host registers scripts with `Catalog::AddScript`. `ExtractInterface` reads a Luau module's public interface without compiling it, so scripts that reference each other can all be registered before any is compiled.

### Singletons

A script marked `-- @singleton` anywhere above its first declaration exists once per scene. Every other script uses it by name, without holding a reference:

```lua
-- GameState.lua
-- @singleton

export local score: int = 0

export function AddScore(n: int)
    score += n
end
```

```lua
function Interact()
    GameState:AddScore(5)
    print(GameState.score)
end
```

Reads, writes and calls compile like a typed reference: `GetProgramVariable`, `SetProgramVariable`, `SendCustomEvent`. Constants declared in the singleton are folded into the callers.

- **The singleton's object:** the editor keeps one object per singleton the scene uses, at `__UdonLuauSingletons/<Name>`. That root must stay at the top of the scene hierarchy and stay active. The editor reactivates it, and it warns about duplicate roots and about singleton scripts placed anywhere else.
- **Finding it:**
  - Each script that uses a singleton gets one hidden reference to it, filled in when the scene is built or played, so no lookup happens at run time. Scripts that never use one get nothing.
  - Objects spawned at run time from prefabs can't hold scene references. They find the singleton once, on their first event, with `GameObject.Find("/__UdonLuauSingletons/<Name>")`. The leading `/` only matches the top-level root, so other objects named after the singleton are never picked up.
  - Every event checks a single flag to know the reference is ready, which costs no extern.
- **Start order:** Udon runs no script code before a behaviour is ready; there is no `Awake`. Every scene behaviour becomes ready at the same point, and singletons are given the lowest update order, so their `Start` runs first.
  - A script that calls singleton methods from `Start` or `OnEnable` checks that the singleton has started. If it hasn't, which can happen for objects spawned at run time, the event is rescheduled for the next frame instead of the call being dropped.
  - Fields can always be read and written, because their initial values are in place from scene load.

### Statics

Mark a module-level field or function `-- @static` to share it between every object running the script, like C# `static`:

```lua
-- Enemy.lua
-- @static
local alive: int = 0

-- @static
local function Register()
    alive += 1
end

export local health: int = 100

function Start()
    Register()
    print(`{alive} enemies`)
end
```

- **Compiled as a companion singleton.** Static fields and functions go into a companion named `<Script>.Static`, stored next to the script as `<Script>.Static.asset`. It follows all the singleton rules above: one per scene under `__UdonLuauSingletons`, wired at build time, and started before other scripts.
- **No prefix needed.** Instance code uses static names directly. A static read or write costs one `Get`/`SetProgramVariable`, and a static call one `SendCustomEvent` plus one extern per argument. An instance field is a plain heap read, so copy static values into a local in hot loops.
- **Static code can't use instance fields or functions.** It runs on the companion's own heap, so it can't reach a particular instance's fields; using one is a compile error. In static code, `this`, `gameObject` and `transform` are the companion.
- **Static events run once.** An event marked `-- @static` (`Update`, `Interact`, ...) runs on the companion, not on every instance.
- **Constants and type aliases are shared.** They're compile-time only, so both parts see them without marking.
- A `List` cannot be static.

### Compile time

- `const NAME = value` declares a constant that takes no heap slot. Module-level constants must be known at compile time. Constants include literals, defines, arithmetic on them, and struct constructors with literal arguments (`const UP = Vector3.new(0, 1, 0)`).
- `const State = {Idle = 0, Opening = 1, Open = 2}` is a compile-time table, used like an enum: `State.Open` is replaced by `2`, so it costs nothing at run time.
- Defines come from the host (`CompileOptions::defines`, or `ul_compile_with_defines`) or from `-- @define(NAME, value)` at the top of the file. A define is substituted wherever its name is used.
- Conditions known at compile time remove code: in `if DEBUG then ... end`, the branch that is not taken is never compiled, like `#if`.
- `assert(condition, "message")` with a constant condition is a compile-time check, like `static_assert`. Any other `assert` runs only when `DEBUG` is true and logs an error; otherwise it is removed with its arguments.
- `-- @inline` and `-- @noinline` above a local function force or forbid inlining. Inlined calls with constant arguments are evaluated at compile time, so small helpers behave like `constexpr` functions.

### Generated code

Every optimization targets what costs time in VRChat's own Udon VM. In order of cost: extern calls, then `COPY`s (struct copies allocate), then jumps (each runs the VM's time-limit check).

- Small local functions, and functions called once, are inlined. Parameters that are never assigned are bound directly to their arguments, so nothing is copied.
- Results are written straight into their destination: assignments, `return` values of inlined calls, if-expressions and boolean expressions.
- Locals that are never reassigned are not stored at all: they are replaced by their value.
- Struct constructors with literal arguments become constants built when the program is created, so they cost no extern at run time. The same applies to constant statics such as `Vector3.up` or `Color.red`, and to pure functions with literal arguments such as `Quaternion.Euler(0, 90, 0)` or `Mathf.Sqrt(2)`.
- Repeated reads of the same property, such as `transform.position` used three times in one expression, run the extern once. The value is reused until a write, a branch target, or a call that could change it.
- `not x` in a condition flips the branch instead of calling a negation extern.
- Operands are only copied before a call when that call could change them. Local functions that write no behaviour variables are recognised and skip the copy.
- Loops test their condition at the bottom, which saves one jump per iteration. Loops with constant bounds skip the first test.
- An `if`/`elseif` chain comparing one `int` against eight or more constants becomes a jump table: two bounds checks, an array read and an indirect jump.
- Events that no other code calls return with a single jump instead of the call/return trampoline.
- Coroutines and signals use the operand stack the way UdonSharp's return trampoline does: each run keeps one return address there, and waiting or finishing jumps to it. A same-script `Fire` jumps straight into each waiter instead of dispatching an event.

### Not supported

- Closures and anonymous functions.
- Lua's `coroutine` library; use `task` and `Signal` instead.
- Metatables.
- Varargs.
- Recursion.
- General tables used as maps.
- `ipairs` and `pairs`: iterate arrays directly instead.

## C API

`UdonLuau.h` exposes the same pipeline for hosts that cannot use C++:

1. Build a catalog with `ul_catalog_*`.
2. Compile with `ul_compile`.
3. Read the program back with the `ul_result_*` functions.

Every string a result returns stays valid until `ul_result_destroy`.
