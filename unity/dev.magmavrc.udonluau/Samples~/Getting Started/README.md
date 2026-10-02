# Getting Started

- **SampleSpinner.lua**: drag onto any GameObject to make it spin.
- **SampleDoor.lua**: a door with inspector fields and network-callable `Open` and `Close` functions.
- **SampleCaller.lua**: put it on an object with a collider and assign the door's GameObject. Interacting toggles the door for everyone with `Network.All(door)`.

Each script gets its own component (for example `SampleDoor`) once it has compiled. Drag a script onto a GameObject, or use Add Component > UdonLuau.
