local calls = 0

---@lux.requires lux.physics2d.query
---@lux.requires lux.simulation.delay
---@lux.event PhysicsBenchmark.pulse

---@lux.method
---@lux.coroutine
---@return void
function tick()
    local hit = lux.Physics2D.overlapsBox(0.0, 0.0, 0.25, 0.25)
    if type(hit) ~= "boolean" then
        error("Physics2D query did not return bool")
    end
    local payload = lux.Event.PhysicsBenchmark.pulse()
    if payload ~= 1 then
        error("Physics benchmark Event payload mismatch")
    end
    lux.Delay.nextStep()
    calls = calls + 1
end

-- NA1 companion steps preserve the original tick body above for the V4 comparison.
---@lux.method
---@return void
function task_query()
    local hit = lux.Physics2D.overlapsBox(0.0, 0.0, 0.25, 0.25)
    if type(hit) ~= "boolean" then
        error("Physics2D query did not return bool")
    end
end

---@lux.method
---@param payload lux.i32
---@return void
function task_check(payload)
    if payload ~= 1 then
        error("Physics benchmark Event payload mismatch")
    end
end

---@lux.method
---@return void
function task_finish()
    calls = calls + 1
end

return { tick = tick, task_query = task_query, task_check = task_check, task_finish = task_finish }
