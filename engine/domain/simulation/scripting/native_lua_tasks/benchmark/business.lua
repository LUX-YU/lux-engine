local BenchmarkBehavior = {}

---@lux.requires lux.benchmark.value
---@lux.requires lux.simulation.delay
---@lux.event Benchmark.event

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

---@lux.method
---@lux.coroutine
---@return void
function BenchmarkBehavior:wait_event()
    local payload = lux.Event.Benchmark.event()
    self.value = self.value + payload
end

---@lux.method
---@lux.coroutine
---@return void
function BenchmarkBehavior:sequence()
    self.value = self.value + 1
    lux.Delay.nextStep()
    local payload = lux.Event.Benchmark.event()
    lux.Delay.simulationSeconds(0.001)
    lux.BenchmarkValue.write(self.value + payload)
end

-- Diagnostic readback runs only after timing. It does not mutate the script instance.
---@lux.method
---@return void
function BenchmarkBehavior:read_value()
    lux.BenchmarkValue.write(self.value)
end

---@lux.method
---@lux.coroutine
---@return void
function BenchmarkBehavior:long_task()
    for i = 1, 32 do
        local payload = lux.Event.Benchmark.event()
        self.value = self.value + payload
    end
end

---@lux.method
---@lux.coroutine
---@param input lux.test.lua.pose
---@return void
function BenchmarkBehavior:pose_task(input)
    self.value = self.value + input.key + input.velocity.x + input.velocity.y + input.mode
    local payload = lux.Event.Benchmark.event()
    self.value = self.value + input.key + input.velocity.x + input.velocity.y + input.mode + payload
end

---@lux.method
---@param payload lux.i32
---@return void
function BenchmarkBehavior:apply_event(payload)
    self.value = self.value + payload
end

---@lux.method
---@return void
function BenchmarkBehavior:before_wait()
    self.value = self.value + 1
end

---@lux.method
---@return void
function BenchmarkBehavior:after_next()
    self.value = self.value + 10
end

---@lux.method
---@param payload lux.i32
---@return void
function BenchmarkBehavior:after_sequence(payload)
    lux.BenchmarkValue.write(self.value + payload)
end

---@lux.method
---@param input lux.test.lua.pose
---@return void
function BenchmarkBehavior:before_pose(input)
    self.value = self.value + input.key + input.velocity.x + input.velocity.y + input.mode
end

---@lux.method
---@param input lux.test.lua.pose
---@param payload lux.i32
---@return void
function BenchmarkBehavior:after_pose(input, payload)
    self.value = self.value + input.key + input.velocity.x + input.velocity.y + input.mode + payload
end

return BenchmarkBehavior
