# UdonLuau

A compiler from [Luau](https://luau.org) to VRChat Udon bytecode, written in C++20.

UdonLuau knows nothing about any particular SDK version. The host fills a `Catalog` with the
types, extern signatures and events it exposes, normally by enumerating the live Udon wrapper
modules, and the compiler resolves every member access, operator and overload against it. When
VRChat exposes a new extern, it is usable from Luau the moment the host sees it.

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

Requires Visual Studio 2022 or later with the C++ workload, and Windows.

```
git clone --recursive https://github.com/MagmaVRC/UdonLuau
```

Open `UdonLuau.slnx`, choose `Release|x64` and build.

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
- Any other `-- @name(args)` annotation directly above a variable is kept on its heap slot for the host, for example `@range(0, 10)`, `@header("Motion")` or `@tooltip("...")`. The compiler does not interpret these; the editor decides what they mean.
- Global functions are entry points:
  - a function named after a VRChat event (`Start`, `Update`, `Interact`, `OnPlayerJoined`, ...) receives that event, with its parameters;
  - any other global function is a custom event, callable through `SendCustomEvent`.
- `local function`s are internal and may take parameters and return values. Parameters and return values need type annotations.
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

### Expressions

- **Members:** `obj.Property`, `obj:Method(args)`, `Type.StaticMember`, `Type.new(args)` (constructors) and `Enum.Member`.
- **Generic methods:** pass the type as the last argument, `obj:GetComponent(Rigidbody)`, or explicitly with `obj:GetComponent<<Rigidbody>>()`.
- **Out parameters:** these become extra return values, for example `local hit, info = Physics.Raycast(origin, direction)`.
- **Operators:** they map to the operator externs. `/` on integers divides as floats, `//` divides as integers, `..` and backtick strings concatenate, `#` gives `Length` or `Count`, and `and`, `or` and `not` short-circuit on booleans.
- **Truthiness:** an object in a condition means "is not nil".
- **Built-ins:** `print`, `warn` and `tostring`.

### Compile time

- `const NAME = value` declares a constant that takes no heap slot. Module-level constants must be known at compile time. Constants include literals, defines, arithmetic on them, and struct constructors with literal arguments (`const UP = Vector3.new(0, 1, 0)`).
- Defines come from the host (`CompileOptions::defines`, or `ul_compile_with_defines`) or from `-- @define(NAME, value)` at the top of the file. A define is substituted wherever its name is used.
- Conditions known at compile time remove code: in `if DEBUG then ... end`, the branch that is not taken is never compiled, like `#if`.
- `assert(condition, "message")` with a constant condition is a compile-time check, like `static_assert`. Any other `assert` runs only when `DEBUG` is true and logs an error; otherwise it is removed with its arguments.
- `-- @inline` and `-- @noinline` above a local function force or forbid inlining. Inlined calls with constant arguments are evaluated at compile time, so small helpers behave like `constexpr` functions.

### Generated code

Every optimization targets what costs time in VRChat's own Udon VM. In order of cost: extern calls, then `COPY`s (struct copies allocate), then jumps (each runs the VM's time-limit check).

- Small local functions, and functions called once, are inlined. Parameters that are never assigned are bound directly to their arguments, so nothing is copied.
- Results are written straight into their destination: assignments, `return` values of inlined calls, if-expressions and boolean expressions.
- Locals that are never reassigned are not stored at all: they are replaced by their value.
- Struct constructors with literal arguments become constants built when the program is created, so they cost no extern at run time.
- Loops test their condition at the bottom, which saves one jump per iteration. Loops with constant bounds skip the first test.
- An `if`/`elseif` chain comparing one `int` against eight or more constants becomes a jump table: two bounds checks, an array read and an indirect jump.
- Events that no other code calls return with a single jump instead of the call/return trampoline.

### Not supported

- Closures and anonymous functions.
- Coroutines.
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
