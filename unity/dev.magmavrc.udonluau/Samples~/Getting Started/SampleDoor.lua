-- A door other scripts can open. Public fields and functions are visible to
-- other Luau and UdonSharp scripts that hold a typed reference to it.

-- @header("Door")
-- @tooltip("Degrees the door turns when opened")
export local openAngle: number = 90
export local isOpen: boolean = false

-- @hideininspector
export local timesOpened: int = 0

-- Callable by every player over the network.
-- @networkcallable
export function Open()
    if isOpen then
        return
    end
    isOpen = true
    timesOpened = timesOpened + 1
    transform:Rotate(Vector3.up, openAngle)
    print(`{gameObject.name} opened ({timesOpened} times)`)
end

-- @networkcallable
export function Close()
    if not isOpen then
        return
    end
    isOpen = false
    transform:Rotate(Vector3.up, -openAngle)
    print(`{gameObject.name} closed`)
end

-- A local-only method with parameters and a return value.
export function Describe(prefix: string): string
    return `{prefix} {gameObject.name} is {if isOpen then "open" else "closed"}`
end
