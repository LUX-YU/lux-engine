local PhysicsConsumer = {}

---@lux.requires lux.physics2d.query

---@lux.method
---@return void
function PhysicsConsumer:tick()
    local hit = lux.Physics2D.overlapsBox(0.0, 0.0, 0.25, 0.25)
    if type(hit) ~= "boolean" then
        error("Physics2D query did not return bool")
    end
end

return PhysicsConsumer
