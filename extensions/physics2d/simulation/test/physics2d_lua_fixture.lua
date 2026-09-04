local calls = 0

---@lux.requires lux.physics2d.query

---@lux.method
---@return void
function tick()
    local hit = lux.Physics2D.overlapsBox(0.0, 0.0, 0.25, 0.25)
    if type(hit) ~= "boolean" then
        error("Physics2D query did not return bool")
    end
    calls = calls + 1
end

return { tick = tick }
