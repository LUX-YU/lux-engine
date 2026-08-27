local EnemyBehavior = {}

---@lux.method
---@param delta lux.f32
---@return lux.i32
function EnemyBehavior:tick(delta)
    return delta > 0 and 1 or 0
end

function EnemyBehavior:helper()
    return 0
end

return EnemyBehavior
