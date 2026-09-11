local Steps = {}
---@lux.requires installed.na1.probe
---@lux.requires lux.simulation.delay
---@lux.event Gameplay.pulse
---@lux.method
---@lux.lifecycle begin_play
---@return void
function Steps:begin_life()
    self.value = 1
    lux.Probe.hit(1)
end
---@lux.method
---@lux.lifecycle end_play
---@param reason lux.simulation.ScriptEndPlayReason
---@return void
function Steps:end_life(reason)
    lux.Probe.hit(2)
end
---@lux.method
---@lux.coroutine
---@return void
function Steps:run()
    local value = lux.Event.Gameplay.pulse()
    lux.Delay.nextStep()
    self.value = self.value + value
    lux.Probe.hit(self.value)
end
---@lux.method
---@param value lux.i32
---@return lux.i32
function Steps:apply(value)
    self.value = self.value + value
    lux.Probe.hit(self.value)
    return self.value
end
---@lux.method
---@param value lux.i32
---@return lux.i32
function Steps:bad(value)
    error("installed sync step failure")
end
return Steps
