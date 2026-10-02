-- A Luau script that UdonSharp code calls (see InteropSharpCaller.cs).
-- UdonSharp sees it as the class UdonLuau.Scripts.InteropCounter: public
-- fields keep their names, @networkcallable functions keep their names, and
-- other public functions get a leading underscore (Add becomes _Add).

export local count: int = 0

export function Add(amount: int): int
    count = count + amount
    print(`InteropCounter: count is {count}`)
    return count
end

-- @networkcallable
export function ResetCount()
    count = 0
    print("InteropCounter: reset")
end
