-- A Luau script calling an UdonSharp behaviour by its class name.
-- Assign the GameObject that has InteropSharpTarget in the inspector.

export local target: InteropSharpTarget

function OnPlayerJoined(player: VRCPlayerApi)
    if not target then
        return
    end

    local length = target:Greet(player.displayName)
    print(`InteropLuauCaller: greeting was {length} characters, {target.greetings} so far`)
end
