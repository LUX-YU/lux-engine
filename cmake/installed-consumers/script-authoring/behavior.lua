local Common = {}
---@lux.requires consumer.authoring.counter

---@lux.method
---@lux.suggest hook Host.first
---@param value lux.i32
---@return void
function Common:update(value)
    self.sum = (self.sum or 0) + value
    lux.Counter.record(self.sum)
end
return Common
