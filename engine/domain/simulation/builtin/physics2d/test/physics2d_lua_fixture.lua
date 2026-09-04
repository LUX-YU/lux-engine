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

return { tick = tick }
