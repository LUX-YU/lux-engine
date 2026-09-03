local state = { value = 0 }

---@lux.requires lux.simulation.delay
---@lux.requires lux.test.lua_runtime

---@lux.method
---@lux.lifecycle begin_play
---@return void
function initialize_portable()
    state.value = 1
    lux.LuaRuntimeTest.writeValue(state.value)
end

---@lux.method
---@lux.lifecycle end_play
---@param reason lux.simulation.ScriptEndPlayReason
---@return void
function retire_portable(reason)
    if reason < 0 or state.value ~= 1234 then
        error("portable lifecycle state mismatch")
    end
    lux.LuaRuntimeTest.writeValue(-1)
end

---@lux.method
---@lux.coroutine
---@return void
function tick_portable()
    if lux.LuaRuntimeTest.echoBool(true) ~= true then error("bool mismatch") end
    if lux.LuaRuntimeTest.echoI32(-2147483648) ~= -2147483648 then error("i32 mismatch") end
    if lux.LuaRuntimeTest.echoU32(4294967295) ~= 4294967295 then error("u32 mismatch") end
    if lux.LuaRuntimeTest.echoF32(-12.5) ~= -12.5 then error("f32 mismatch") end
    if lux.LuaRuntimeTest.echoF64(1234.125) ~= 1234.125 then error("f64 mismatch") end
    local borrowed_copy = lux.LuaRuntimeTest.borrowValue()
    local eager = lux.LuaRuntimeTest.beginOperation(borrowed_copy)
    lux.Delay.nextStep()
    lux.Delay.simulationSeconds(0.000000002)
    state.value = eager + borrowed_copy + 1231
    lux.LuaRuntimeTest.writeValue(state.value)
end

return {
    initialize_portable = initialize_portable,
    retire_portable = retire_portable,
    tick_portable = tick_portable
}
