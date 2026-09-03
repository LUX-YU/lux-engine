local EnemyBehavior = {}

---@lux.requires lux.simulation.delay
---@lux.event Gameplay.damage

---@lux.method
---@lux.lifecycle begin_play
---@return void
function EnemyBehavior:initialize()
    self.counter = 1
end

---@lux.method
---@lux.lifecycle end_play
---@param reason lux.simulation.ScriptEndPlayReason
---@return void
function EnemyBehavior:retire(reason)
    self.retired = reason
end

---@lux.method
---@lux.coroutine
---@return void
function EnemyBehavior:update_async()
    lux.Delay.nextStep()
    self.counter = self.counter + 10
end

---@lux.method
---@param step lux.simulation.SimulationStepInfo
---@param collision lux.test.CollisionEvent
---@param delta lux.f32
---@return lux.i32
function EnemyBehavior:tick(step, collision, delta)
    return delta > 0 and 1 or 0
end

function EnemyBehavior:helper()
    return 0
end

return EnemyBehavior
