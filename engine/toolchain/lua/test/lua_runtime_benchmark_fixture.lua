local BenchmarkBehavior = {}

---@lux.requires lux.benchmark.value
---@lux.requires lux.simulation.delay

---@lux.method
---@lux.lifecycle begin_play
---@return void
function BenchmarkBehavior:admit()
    self.value = 1
end

---@lux.method
---@lux.lifecycle end_play
---@param reason lux.simulation.ScriptEndPlayReason
---@return void
function BenchmarkBehavior:retire(reason)
    self.retired = reason
end

---@lux.method
---@return void
function BenchmarkBehavior:update()
    self.value = self.value + 1
    local value = lux.BenchmarkValue.read(self.value)
    lux.BenchmarkValue.write(value)
end

---@lux.method
---@return void
function BenchmarkBehavior:query_only()
    self.value = lux.BenchmarkValue.read(self.value)
end

---@lux.method
---@return void
function BenchmarkBehavior:update_plain()
    self.value = self.value + 1
end

---@lux.method
---@lux.coroutine
---@return void
function BenchmarkBehavior:update_async()
    self.value = self.value + 1
    lux.Delay.nextStep()
    self.value = self.value + 10
end

return BenchmarkBehavior
