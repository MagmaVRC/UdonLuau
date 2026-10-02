-- Drag this script onto any GameObject to make it spin.

-- @header("Motion")
-- @range(0, 360)
-- @tooltip("Degrees per second")
export local speed: number = 45

-- @tooltip("Leave empty to spin this object")
export local target: Transform

function Start()
    if not target then
        target = transform
    end
end

function Update()
    target:Rotate(Vector3.up, speed * Time.deltaTime)
end
