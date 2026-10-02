# UdonLuau

Write VRChat world behaviours in [Luau](https://luau.org). UdonLuau compiles `.lua` scripts (`.luau` is also accepted) to Udon programs inside the Unity editor. It uses whatever externs, types and events your installed VRChat SDK exposes, so new SDK features work without updating UdonLuau.

- Scripts appear on GameObjects as their own components, like UdonSharp behaviours.
- Public variables show in the inspector, with `@header`, `@tooltip`, `@range`, `@space`, `@hideininspector` and `@multiline` annotations.
- Luau and UdonSharp scripts can hold typed references to each other and call each other's public functions.
- `@networkcallable` functions, `@sync` variables and `@syncmode` follow VRChat's rules and are checked when compiling.
- Edits to function bodies are hot-reloaded in play mode.
- Generates a definitions file and VS Code settings for the [luau-lsp](https://marketplace.visualstudio.com/items?itemName=JohnnyMorganz.luau-lsp) extension.

Requires Windows, Unity 2022.3 or later, and the VRChat Worlds SDK 3.9.0 or later.

## Install

Add the package to your world project through the VRChat Creator Companion, or with Window > Package Manager > + > Add package from disk.

## Quick start

1. Assets > Create > VRChat > UdonLuau Script creates `NewLuauScript.lua`.
2. Drag the script onto a GameObject in the Hierarchy, the Scene view or the Inspector.
3. Enter play mode.

```lua
-- @range(0, 360)
export local speed: number = 45

function Update()
    transform:Rotate(Vector3.up, speed * Time.deltaTime)
end
```

The samples (Package Manager > UdonLuau > Samples) show typed references between scripts, network calls and UdonSharp interop.

Settings are in Project Settings > UdonLuau. Tools > UdonLuau has commands to reload the native compiler and regenerate the editor definitions.

## Documentation

The language reference and compiler documentation are at [github.com/MagmaVRC/UdonLuau](https://github.com/MagmaVRC/UdonLuau).

## License

MIT, see [LICENSE.md](LICENSE.md). Third-party components are listed in [Third Party Notices.md](Third%20Party%20Notices.md).
