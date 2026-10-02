-- Toggles a SampleDoor for everyone when a player interacts with this object.
-- Assign the door's GameObject in the inspector.

export local door: SampleDoor

function Interact()
    if not door then
        warn("SampleCaller: no door assigned")
        return
    end

    print(door:Describe("Before:"))
    if door.isOpen then
        Network.All(door):Close()
    else
        Network.All(door):Open()
    end
end
