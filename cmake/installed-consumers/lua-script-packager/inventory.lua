local InventoryBehavior = {}

---@lux.requires consumer.inventory.lua
---@lux.event Inventory.changed

---@lux.method
---@lux.lifecycle begin_play
---@return void
function InventoryBehavior:initialize()
    self.count = 0
end

---@lux.method
---@lux.lifecycle end_play
---@param reason lux.simulation.ScriptEndPlayReason
---@return void
function InventoryBehavior:retire(reason)
    self.reason = reason
end

---@lux.method
---@lux.coroutine
---@return void
function InventoryBehavior:update()
    self.count = lux.Inventory.countLater(7)
end

return InventoryBehavior
