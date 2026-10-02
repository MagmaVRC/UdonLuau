# UdonSharp Interop

UdonSharp calling Luau:

- **InteropCounter.lua** is a Luau script with a public field, a local-only function and a network-callable function.
- **InteropSharpCaller.cs** gets it with `GetComponent<InteropCounter>()`, calls `_Add`, reads `count` and sends `ResetCount` over the network.
- UdonSharp sees each Luau script as a class in the `UdonLuau.Scripts` namespace. Functions marked `@networkcallable` keep their name; other public functions get a leading underscore.

Luau calling UdonSharp:

- **InteropLuauCaller.lua** holds a typed reference to the UdonSharp class **InteropSharpTarget** and calls `Greet` when a player joins.

The first import compiles in two passes: the UdonSharp class that uses `InteropCounter` compiles once the Luau script has generated its class, a moment later.
