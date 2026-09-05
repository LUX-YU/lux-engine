#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp>

#include <lux/engine/function/script/lua/Lua.hpp>
#include <lux/engine/function/script/lua/detail/LuaVmCompatibility.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptContractValidation.hpp>

#include <lua.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    struct LuaScriptBackend::State final
    {
        struct VmAllocationTracker final
        {
            lua_Alloc original{};
            void* context{};
            LuaVmAllocationStats stats;
            static void* allocate(void* opaque, void* pointer, std::size_t old_size, std::size_t size) noexcept
            {
                auto& self = *static_cast<VmAllocationTracker*>(opaque);
                auto* result = self.original(self.context, pointer, old_size, size);
                if (size != 0U && result == nullptr)
                {
                    ++self.stats.failures;
                    return nullptr;
                }
                if (size == 0U)
                    self.stats.frees += pointer != nullptr ? 1U : 0U;
                else if (pointer == nullptr)
                    ++self.stats.allocations;
                else
                    ++self.stats.reallocations;
                self.stats.requested_bytes += size;
                self.stats.released_bytes += pointer != nullptr ? old_size : 0U;
                return result;
            }
        };
        static constexpr std::size_t kMaxAbilityArguments = 8U;
        static constexpr std::size_t kMaxAbilityResults = 4U;
        static constexpr std::int32_t kInvalidCall = -1;
        static constexpr std::int32_t kMarshalFailure = -3;
        static constexpr std::int32_t kLuaFailure = -4;
        static constexpr std::int32_t kInvalidResult = -5;
        static constexpr std::int32_t kInvalidResume = -6;
        static constexpr std::int32_t kContinuationCapacity = -7;
        static constexpr std::int32_t kExecutionDepthCapacity = -8;
        static constexpr std::int32_t kEventWaitFailure = -9;

        enum class EPendingOperation : std::uint8_t
        {
            NONE,
            ABILITY,
            EVENT,
        };

        struct HostHandle final
        {
            State* owner{};
            ScriptBehavior* host{};
            bool alive{};
        };

        struct PreparedSpan final
        {
            static constexpr std::uint32_t InvalidSlot{(std::numeric_limits<std::uint32_t>::max)()};
            std::uint32_t first{InvalidSlot};
            std::uint32_t count{};

            [[nodiscard]] bool valid() const noexcept
            {
                return first != InvalidSlot;
            }
        };

        struct Prototype final
        {
            int table_ref{LUA_NOREF};
            int environment_ref{LUA_NOREF};
            const void* layout_token{};
            std::vector<std::uint32_t> ability_ordinals;
            std::vector<std::uint32_t> event_ordinals;
        };

        struct Instance final
        {
            State* owner{};
            lux::asset::AssetId asset;
            int table_ref{LUA_NOREF};
            bool entity_scope{};
            HostHandle* host_handle{};
            Prototype* prototype{};
            std::span<const lux::script::ScriptSymbolId> suspension_capable_exports;
            std::size_t slot{};
            std::size_t active_continuations{};
            PreparedSpan prepared_abilities;
            PreparedSpan prepared_events;
            bool active{};
        };

        struct FunctionKey final
        {
            lux::asset::AssetId asset;
            lux::script::ScriptSymbolId symbol{};

            [[nodiscard]] bool operator==(const FunctionKey&) const noexcept = default;
        };

        struct FunctionKeyHash final
        {
            [[nodiscard]] std::size_t operator()(const FunctionKey& key) const noexcept
            {
                const auto asset_hash = std::hash<lux::asset::AssetId>{}(key.asset);
                const auto symbol_hash = std::hash<lux::script::ScriptSymbolId>{}(key.symbol);
                return asset_hash ^ (symbol_hash + 0x9E3779B9U + (asset_hash << 6U) + (asset_hash >> 2U));
            }
        };

        struct LuaFunctionBinding final
        {
            lux::rdesc::ScriptFunction signature;
            int function_ref{LUA_NOREF};
            std::vector<const LuaRecordMarshaller*> argument_marshallers;
        };

        struct PreparedCall final
        {
            Instance* instance{};
            const LuaFunctionBinding* function{};
            bool active{};
        };

        struct AbilityMethod final
        {
            const lux::script::ScriptAbilityDescription* ability{};
            const lux::script::ScriptAbilityMethodDescription* method{};
            int (*entry)(lua_State*) noexcept{};
        };

        struct PreparedAbility final
        {
            void* context{};
            const void* dispatch{};
            const lux::script::ScriptAbilityErasedMethodBinding* method{};
            const lux::script::ScriptAbilityMethodDescription* semantic{};
        };

        struct PreparedEventSource final
        {
            const lux::script::ScriptEventSourceDescription* source{};
        };

        template <class Value>
        class PreparedSpanArena final
        {
        public:
            static constexpr std::uint32_t InvalidSlot{(std::numeric_limits<std::uint32_t>::max)()};

            using Span = PreparedSpan;

            struct Stats final
            {
                std::size_t active{};
                std::size_t high_water{};
                std::size_t storage_bytes{};
            };

            explicit PreparedSpanArena(std::size_t capacity)
            {
                if (capacity == 0U)
                    return;
                if (capacity > (std::numeric_limits<std::uint32_t>::max)())
                {
                    valid_ = false;
                    return;
                }
                entries_.resize(capacity);
                free_spans_.reserve(capacity + 1U);
                free_spans_.push_back({0U, static_cast<std::uint32_t>(capacity)});
            }

            [[nodiscard]] Span allocate(std::size_t count) noexcept
            {
                if (count == 0U)
                    return {0U, 0U};
                if (count > (std::numeric_limits<std::uint32_t>::max)())
                    return {};
                for (std::size_t index{}; index < free_spans_.size(); ++index)
                {
                    auto& free = free_spans_[index];
                    if (free.count < count)
                        continue;
                    const Span result{free.first, static_cast<std::uint32_t>(count)};
                    free.first += result.count;
                    free.count -= result.count;
                    if (free.count == 0U)
                        free_spans_.erase(free_spans_.begin() + static_cast<std::ptrdiff_t>(index));
                    active_ += result.count;
                    high_water_ = (std::max)(high_water_, active_);
                    return result;
                }
                return {};
            }

            void release(Span& span) noexcept
            {
                if (!span.valid() || span.count == 0U)
                {
                    span = {};
                    return;
                }
                const auto end = static_cast<std::size_t>(span.first) + span.count;
                if (end > entries_.size())
                    std::terminate();
                for (std::size_t index = span.first; index < end; ++index)
                    entries_[index] = {};
                const FreeSpan returned{span.first, span.count};
                auto position = free_spans_.begin();
                while (position != free_spans_.end() && position->first < returned.first)
                    ++position;
                const auto inserted = free_spans_.insert(position, returned);
                coalesce(static_cast<std::size_t>(inserted - free_spans_.begin()));
                active_ -= span.count;
                span = {};
            }

            [[nodiscard]] Value* at(Span span, std::size_t local_slot) noexcept
            {
                if (!span.valid() || local_slot >= span.count)
                    return nullptr;
                return std::addressof(entries_[span.first + local_slot]);
            }

            [[nodiscard]] const Value* at(Span span, std::size_t local_slot) const noexcept
            {
                return const_cast<PreparedSpanArena*>(this)->at(span, local_slot);
            }

            [[nodiscard]] Stats stats() const noexcept
            {
                return {
                    active_,
                    high_water_,
                    entries_.size() * sizeof(Value) + free_spans_.capacity() * sizeof(FreeSpan)
                };
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return valid_;
            }

        private:
            struct FreeSpan final
            {
                std::uint32_t first{};
                std::uint32_t count{};
            };

            void coalesce(std::size_t index) noexcept
            {
                if (index != 0U)
                {
                    auto& previous = free_spans_[index - 1U];
                    auto& current = free_spans_[index];
                    if (previous.first + previous.count == current.first)
                    {
                        previous.count += current.count;
                        free_spans_.erase(free_spans_.begin() + static_cast<std::ptrdiff_t>(index));
                        --index;
                    }
                }
                if (index + 1U >= free_spans_.size())
                    return;
                auto& current = free_spans_[index];
                const auto& next = free_spans_[index + 1U];
                if (current.first + current.count == next.first)
                {
                    current.count += next.count;
                    free_spans_.erase(free_spans_.begin() + static_cast<std::ptrdiff_t>(index + 1U));
                }
            }

            std::vector<Value> entries_;
            std::vector<FreeSpan> free_spans_;
            std::size_t active_{};
            std::size_t high_water_{};
            bool valid_{true};
        };

        struct LuaContinuation final
        {
            State* owner{};
            Instance* instance{};
            PreparedCall* call{};
            lua_State* thread{};
            int thread_ref{LUA_NOREF};
            ScriptAwaitableId waiting_on;
            std::uint32_t pending_ordinal{};
            EPendingOperation pending_operation{EPendingOperation::NONE};
            std::int32_t failure_status{};
            bool active{};
        };

        struct ExecutionFrame final
        {
            lua_State* thread{};
            Instance* instance{};
            LuaContinuation* continuation{};
            ScriptStepContext* step{};
            ExecutionFrame* previous{};
        };

        class ExecutionScope final
        {
        public:
            ExecutionScope(State& owner, ExecutionFrame frame) noexcept
                : owner_(std::addressof(owner)), frame_(frame)
            {
                if (owner.execution_depth >= owner.execution_depth_capacity)
                    return;
                frame_.previous = owner.active_execution;
                owner.active_execution = std::addressof(frame_);
                ++owner.execution_depth;
                owner.execution_depth_high_water = (std::max)(
                    owner.execution_depth_high_water,
                    owner.execution_depth
                );
                active_ = true;
            }

            ExecutionScope(const ExecutionScope&) = delete;
            ExecutionScope& operator=(const ExecutionScope&) = delete;

            ~ExecutionScope()
            {
                if (!active_)
                    return;
                owner_->active_execution = frame_.previous;
                --owner_->execution_depth;
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return active_;
            }

        private:
            State* owner_{};
            ExecutionFrame frame_;
            bool active_{};
        };

        State(
            LuaScriptBackendConfig config
        )
            : state(engine.state()),
              instance_capacity(config.instance_capacity),
              prepared_call_capacity(config.prepared_call_capacity),
              continuation_capacity(config.continuation_capacity),
              execution_depth_capacity(config.execution_depth_capacity),
              prepared_abilities(config.prepared_ability_capacity),
              prepared_events(config.prepared_event_capacity)
        {
            if (!prepared_abilities.valid() || !prepared_events.valid())
                return;
            if (config.track_vm_allocations && state != nullptr)
            {
                vm_allocations.original = lua_getallocf(state, &vm_allocations.context);
                vm_allocations.stats.enabled = true;
                lua_setallocf(state, &VmAllocationTracker::allocate, &vm_allocations);
            }
            if (!lux::script::lua::detail::configureLuaVm(
                    state,
                    config.execution_policy,
                    runtime_info
                ))
            {
                return;
            }
            vm_configured = true;
            prototypes.reserve(config.instance_capacity);
            components.assign(
                config.components.begin(),
                config.components.end()
            );
            component_index.reserve(components.size());
            for (std::size_t index{}; index < components.size(); ++index)
                component_index.emplace(components[index].name, index);
            record_marshallers.assign(
                config.record_marshallers.begin(),
                config.record_marshallers.end()
            );
            record_marshaller_index.reserve(record_marshallers.size());
            for (std::size_t index{}; index < record_marshallers.size(); ++index)
            {
                record_marshaller_index.emplace(
                    record_marshallers[index].semantic_type,
                    index
                );
            }
            for (const auto& contribution : config.abilities)
            {
                for (std::size_t index{}; index < contribution.description->methods.size(); ++index)
                {
                    ability_methods.push_back({
                        contribution.description,
                        std::addressof(contribution.description->methods[index]),
                        contribution.methods[index].entry
                    });
                }
            }
            event_sources.assign(config.events.begin(), config.events.end());
            std::ranges::sort(event_sources, {}, [](const auto& source) noexcept {
                return std::pair{std::string_view(source.system_name), std::string_view(source.event_name)};
            });
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
            function_bindings.reserve(prepared_call_capacity);
            function_index.reserve(prepared_call_capacity);
            prepared_calls.resize(prepared_call_capacity);
            free_prepared_calls.reserve(prepared_call_capacity);
            for (std::size_t index = prepared_call_capacity; index > 0U; --index)
                free_prepared_calls.push_back(index - 1U);
            continuations.resize(continuation_capacity);
            free_continuations.reserve(continuation_capacity);
            for (std::size_t index = continuation_capacity; index > 0U; --index)
                free_continuations.push_back(index - 1U);
            lua_pushcfunction(state, &State::traceback);
            traceback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        }

        ~State()
        {
            for (auto& continuation : continuations)
            {
                if (continuation.active && continuation.thread_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, continuation.thread_ref);
            }
            for (const auto& [asset, prototype] : prototypes)
            {
                static_cast<void>(asset);
                if (prototype.table_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype.table_ref);
                if (prototype.environment_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype.environment_ref);
            }
            for (const auto& function : function_bindings)
            {
                if (function.function_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, function.function_ref);
            }
            if (state && traceback_ref != LUA_NOREF)
                luaL_unref(state, LUA_REGISTRYINDEX, traceback_ref);
        }

        [[nodiscard]] static bool identifier(std::string_view value) noexcept
        {
            if (value.empty())
                return false;
            const auto alpha = [](char character) noexcept {
                return (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') || character == '_';
            };
            const auto digit = [](char character) noexcept {
                return character >= '0' && character <= '9';
            };
            if (!alpha(value.front()))
                return false;
            const bool valid_characters = std::all_of(value.begin() + 1, value.end(), [&](char character) noexcept {
                return alpha(character) || digit(character);
            });
            if (!valid_characters)
                return false;
            constexpr std::array keywords{
                std::string_view{"and"}, std::string_view{"break"}, std::string_view{"do"},
                std::string_view{"else"}, std::string_view{"elseif"}, std::string_view{"end"},
                std::string_view{"false"}, std::string_view{"for"}, std::string_view{"function"},
                std::string_view{"goto"}, std::string_view{"if"}, std::string_view{"in"},
                std::string_view{"local"}, std::string_view{"nil"}, std::string_view{"not"},
                std::string_view{"or"}, std::string_view{"repeat"}, std::string_view{"return"},
                std::string_view{"then"}, std::string_view{"true"}, std::string_view{"until"},
                std::string_view{"while"}
            };
            return std::find(keywords.begin(), keywords.end(), value) == keywords.end();
        }

        [[nodiscard]] bool appendArtifactAbilities(
            Prototype& prototype,
            const lux::script::ScriptArtifact& artifact,
            int lux_index
        ) noexcept
        {
            for (const auto& requirement : artifact.description().api_requirements)
            {
                std::size_t first_method{};
                while (first_method < ability_methods.size())
                {
                    const auto* ability = ability_methods[first_method].ability;
                    if (ability->id.hash() == requirement.contract.hash() &&
                        ability->id.name() == requirement.contract.name() &&
                        ability->schema_hash == requirement.expected_schema_hash)
                        break;
                    ++first_method;
                }
                if (first_method == ability_methods.size())
                    return false;
                const auto* ability = ability_methods[first_method].ability;
                lua_newtable(state);
                const auto ability_index = lua_gettop(state);
                auto ordinal = first_method;
                while (ordinal < ability_methods.size() && ability_methods[ordinal].ability == ability)
                {
                    const auto local_slot = prototype.ability_ordinals.size();
                    if (local_slot > (std::numeric_limits<lua_Integer>::max)())
                        return false;
                    const auto* method = ability_methods[ordinal].method;
                    lua_pushlstring(state, method->name.data(), method->name.size());
                    lua_pushlightuserdata(state, this);
                    lua_pushinteger(state, static_cast<lua_Integer>(local_slot));
                    lua_pushlightuserdata(state, this);
                    lua_rawget(state, lux_index);
                    lua_pushcclosure(state, ability_methods[ordinal].entry, 3);
                    const bool is_async = method->kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION;
                    if (!wrapAbility(is_async, !method->results.empty()))
                        return false;
                    lua_settable(state, ability_index);
                    prototype.ability_ordinals.push_back(static_cast<std::uint32_t>(ordinal));
                    ++ordinal;
                }
                lua_pushlstring(state, ability->name.data(), ability->name.size());
                lua_pushvalue(state, ability_index);
                lua_settable(state, lux_index);
                lua_pop(state, 1);
            }
            return true;
        }

        [[nodiscard]] bool wrapAbility(bool is_async, bool has_result) noexcept
        {
            // Raise/yield in Lua only, after the typed C++ thunk (and its noexcept frames) has returned.
            constexpr std::string_view prefix =
                "return function(start) return function(...) local ok,value=start(...); "
                "if not ok then error(value,0) end; ";
            const auto suffix = is_async ? "return coroutine.yield() end end" :
                has_result ? "return value end end" : "end end";
            std::array<char, 256U> wrapper{};
            const auto suffix_size = std::strlen(suffix);
            std::memcpy(wrapper.data(), prefix.data(), prefix.size());
            std::memcpy(wrapper.data() + prefix.size(), suffix, suffix_size);
            if (luaL_loadbufferx(state, wrapper.data(), prefix.size() + suffix_size, "lux.Ability.wrapper", "t") !=
                    LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK)
                return false;
            lua_insert(state, -2);
            return lua_pcall(state, 1, 1, 0) == LUA_OK;
        }

        [[nodiscard]] bool appendArtifactEvents(
            Prototype& prototype,
            const lux::script::ScriptArtifact& artifact,
            int lux_index
        ) noexcept
        {
            if (artifact.description().event_requirements.empty())
                return true;
            static constexpr std::string_view wrapper_source =
                "return function(start) return function() local ok,message=start(); "
                "if not ok then error(message,0) end; return coroutine.yield() end end";
            lua_newtable(state);
            const auto event_index = lua_gettop(state);
            if (luaL_loadbufferx(
                    state,
                    wrapper_source.data(),
                    wrapper_source.size(),
                    "lux.Event.wrapper",
                    "t"
                ) != LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK || !lua_isfunction(state, -1))
            {
                return false;
            }
            const auto factory_index = lua_gettop(state);
            std::string_view active_system;
            int system_index{};
            for (const auto& requirement : artifact.description().event_requirements)
            {
                const auto found = std::ranges::find(event_sources, requirement);
                if (found == event_sources.end())
                    return false;
                if (active_system != requirement.system_name)
                {
                    if (system_index != 0)
                    {
                        lua_pushlstring(state, active_system.data(), active_system.size());
                        lua_pushvalue(state, system_index);
                        lua_settable(state, event_index);
                        lua_remove(state, system_index);
                    }
                    active_system = requirement.system_name;
                    lua_newtable(state);
                    system_index = lua_gettop(state);
                }
                const auto local_slot = prototype.event_ordinals.size();
                if (local_slot > (std::numeric_limits<lua_Integer>::max)())
                    return false;
                lua_pushlstring(state, requirement.event_name.data(), requirement.event_name.size());
                lua_pushvalue(state, factory_index);
                lua_pushlightuserdata(state, this);
                lua_pushinteger(state, static_cast<lua_Integer>(local_slot));
                lua_pushlightuserdata(state, this);
                lua_rawget(state, lux_index);
                lua_pushcclosure(state, &State::invokeEventWait, 3);
                if (lua_pcall(state, 1, 1, 0) != LUA_OK)
                    return false;
                lua_settable(state, system_index);
                prototype.event_ordinals.push_back(static_cast<std::uint32_t>(found - event_sources.begin()));
            }
            if (system_index != 0)
            {
                lua_pushlstring(state, active_system.data(), active_system.size());
                lua_pushvalue(state, system_index);
                lua_settable(state, event_index);
                lua_remove(state, system_index);
            }
            lua_remove(state, factory_index);
            lua_setfield(state, lux_index, "Event");
            return true;
        }

        [[nodiscard]] bool pushArtifactEnvironment(
            Prototype& prototype,
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            lua_newtable(state);
            const auto environment_index = lua_gettop(state);
            lua_newtable(state);
            lux::script::lua::detail::pushLuaGlobalEnvironment(state);
            lua_setfield(state, -2, "__index");
            lua_setmetatable(state, environment_index);
            lua_newtable(state);
            const auto lux_index = lua_gettop(state);
            // A full userdata is retained by the environment and every closure. An old reachable closure
            // therefore prevents reuse of its layout identity, independently of C++ prototype storage.
            lua_pushlightuserdata(state, this);
            prototype.layout_token = lua_newuserdata(state, 1U);
            lua_rawset(state, lux_index);
            if (!appendArtifactAbilities(prototype, artifact, lux_index) ||
                !appendArtifactEvents(prototype, artifact, lux_index))
            {
                lua_settop(state, environment_index - 1);
                return false;
            }
            lua_setfield(state, environment_index, "lux");
            return true;
        }

        static int traceback(lua_State* state)
        {
            const char* message = lua_tostring(state, 1);
            luaL_traceback(state, state, message ? message : "script error", 1);
            return 1;
        }

        [[nodiscard]] Prototype* prototypeFor(
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            const auto found = prototypes.find(context.asset);
            if (found != prototypes.end())
                return std::addressof(found->second);
            if (artifact.payload().empty())
                return nullptr;
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(
                std::addressof(artifact.description().body));
            if (!body)
                return nullptr;
            Prototype prototype;
            lua_rawgeti(state, LUA_REGISTRYINDEX, traceback_ref);
            const auto error_index = lua_gettop(state);
            if (!pushArtifactEnvironment(prototype, artifact))
            {
                lua_settop(state, error_index - 1);
                return nullptr;
            }
            const auto environment_index = lua_gettop(state);
            const auto* source = reinterpret_cast<const char*>(
                artifact.payload().data());
            if (luaL_loadbufferx(
                    state,
                    source,
                    artifact.payload().size(),
                    artifact.description().module_name.c_str(),
                    "t") != LUA_OK || !lux::script::lua::detail::setLuaChunkEnvironment(
                        state,
                        -1,
                        environment_index
                    ) ||
                lua_pcall(state, 0, 1, error_index) != LUA_OK)
            {
                lua_settop(state, error_index - 1);
                return nullptr;
            }
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                lua_getfield(state, environment_index, body->entry.c_str());
            }
            if (!lua_istable(state, -1))
            {
                lua_settop(state, error_index - 1);
                return nullptr;
            }
            prototype.table_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_pushvalue(state, environment_index);
            prototype.environment_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_settop(state, error_index - 1);
            try
            {
                const auto inserted = prototypes.emplace(context.asset, std::move(prototype));
                if (!inserted.second)
                    return nullptr;
                return std::addressof(inserted.first->second);
            }
            catch (const std::bad_alloc&)
            {
                if (prototype.table_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype.table_ref);
                if (prototype.environment_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype.environment_ref);
                return nullptr;
            }
        }

        [[nodiscard]] bool supportedType(
            const lux::rdesc::ScriptValueType& type
        ) const noexcept
        {
            if (!lux::rdesc::detail::validScriptValueType(type))
                return false;
            switch (type.abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                return type.size == sizeof(bool) && type.alignment == alignof(bool);
            case LUX_SCRIPT_VK_INT32:
                return type.size == sizeof(std::int32_t) && type.alignment == alignof(std::int32_t);
            case LUX_SCRIPT_VK_UINT32:
                return type.size == sizeof(std::uint32_t) && type.alignment == alignof(std::uint32_t);
            case LUX_SCRIPT_VK_FLOAT:
                return type.size == sizeof(float) && type.alignment == alignof(float);
            case LUX_SCRIPT_VK_DOUBLE:
                return type.size == sizeof(double) && type.alignment == alignof(double);
            default:
                return false;
            }
        }

        [[nodiscard]] static bool supportedType(
            const lux::script::ScriptAbilityValueDescription& type
        ) noexcept
        {
            switch (type.abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                return type.size == sizeof(bool) && type.alignment == alignof(bool);
            case LUX_SCRIPT_VK_INT32:
                return type.size == sizeof(std::int32_t) && type.alignment == alignof(std::int32_t);
            case LUX_SCRIPT_VK_UINT32:
                return type.size == sizeof(std::uint32_t) && type.alignment == alignof(std::uint32_t);
            case LUX_SCRIPT_VK_FLOAT:
                return type.size == sizeof(float) && type.alignment == alignof(float);
            case LUX_SCRIPT_VK_DOUBLE:
                return type.size == sizeof(double) && type.alignment == alignof(double);
            default:
                return false;
            }
        }

        [[nodiscard]] EScriptBackendResult prepareAbilities(
            Instance& instance,
            const ScriptInstanceCreateContext& context
        ) noexcept
        {
            if (instance.prototype == nullptr)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            auto span = prepared_abilities.allocate(instance.prototype->ability_ordinals.size());
            if (!span.valid())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            instance.prepared_abilities = span;
            for (std::size_t local_slot{}; local_slot < span.count; ++local_slot)
            {
                const auto ordinal = instance.prototype->ability_ordinals[local_slot];
                if (ordinal >= ability_methods.size())
                {
                    prepared_abilities.release(instance.prepared_abilities);
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                }
                const auto& projected = ability_methods[ordinal];
                const auto capability = std::ranges::find_if(
                    context.capabilities,
                    [&](const auto& candidate) noexcept {
                        return candidate.contract.hash() == projected.ability->id.hash() &&
                            candidate.contract.name() == projected.ability->id.name();
                    }
                );
                if (capability == context.capabilities.end() ||
                    capability->schema_version != projected.ability->schema_version ||
                    capability->schema_hash != projected.ability->schema_hash)
                {
                    prepared_abilities.release(instance.prepared_abilities);
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                }
                const auto method = std::ranges::find_if(
                    capability->methods,
                    [&](const auto& candidate) noexcept {
                        return candidate.method.hash() == projected.method->id.hash() &&
                            candidate.method.name() == projected.method->id.name();
                    }
                );
                if (method == capability->methods.end() || !scriptAbilityMethodMatches(*projected.method, *method))
                {
                    prepared_abilities.release(instance.prepared_abilities);
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                }
                *prepared_abilities.at(instance.prepared_abilities, local_slot) = {
                    capability->context,
                    capability->dispatch,
                    std::addressof(*method),
                    projected.method
                };
            }
            return EScriptBackendResult::SUCCESS;
        }

        [[nodiscard]] EScriptBackendResult prepareEvents(
            Instance& instance,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            const auto& requirements = artifact.description().event_requirements;
            if (requirements.size() != context.events.size())
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            if (instance.prototype == nullptr || instance.prototype->event_ordinals.size() != requirements.size())
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            auto span = prepared_events.allocate(requirements.size());
            if (!span.valid())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            instance.prepared_events = span;
            for (std::size_t local_slot{}; local_slot < requirements.size(); ++local_slot)
            {
                const auto& requirement = requirements[local_slot];
                const auto resolved = std::ranges::find(context.events, requirement);
                if (resolved == context.events.end())
                {
                    prepared_events.release(instance.prepared_events);
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                }
                const auto ordinal = instance.prototype->event_ordinals[local_slot];
                if (ordinal >= event_sources.size() || event_sources[ordinal] != requirement)
                {
                    prepared_events.release(instance.prepared_events);
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                }
                *prepared_events.at(instance.prepared_events, local_slot) = {
                    std::addressof(event_sources[ordinal])
                };
            }
            return EScriptBackendResult::SUCCESS;
        }

        [[nodiscard]] const LuaRecordMarshaller* recordMarshaller(
            const lux::rdesc::ScriptValueType& type
        ) const noexcept
        {
            if (type.pass != lux::semantic::EValuePass::CONST_REF)
                return nullptr;
            const auto found = record_marshaller_index.find(type.type_id);
            if (found == record_marshaller_index.end())
                return nullptr;
            const auto& marshaller = record_marshallers[found->second];
            return marshaller.canonical_name == type.canonical_name
                ? std::addressof(marshaller)
                : nullptr;
        }

        [[nodiscard]] const LuaComponentBinding* component(
            std::string_view name
        ) const noexcept
        {
            const auto found = component_index.find(name);
            return found == component_index.end()
                ? nullptr
                : std::addressof(components[found->second]);
        }

        [[nodiscard]] static HostHandle* hostHandle(lua_State* state) noexcept
        {
            return static_cast<HostHandle*>(lua_touserdata(
                state,
                lua_upvalueindex(1)
            ));
        }

        static int hasComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            lua_pushboolean(
                state,
                binding && handle->host &&
                    handle->host->read(binding->component_type));
            return 1;
        }

        static bool pushComponentValue(
            lua_State* state,
            std::uint8_t kind,
            const void* value
        ) noexcept
        {
            if (!value)
                return false;
            switch (kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                lua_pushboolean(state, *static_cast<const bool*>(value));
                return true;
            case LUX_SCRIPT_VK_INT32:
                lua_pushnumber(state, static_cast<lua_Number>(*static_cast<const std::int32_t*>(value)));
                return true;
            case LUX_SCRIPT_VK_UINT32:
                lua_pushnumber(state, static_cast<lua_Number>(*static_cast<const std::uint32_t*>(value)));
                return true;
            case LUX_SCRIPT_VK_FLOAT:
                lua_pushnumber(state, *static_cast<const float*>(value));
                return true;
            case LUX_SCRIPT_VK_DOUBLE:
                lua_pushnumber(state, *static_cast<const double*>(value));
                return true;
            default:
                return false;
            }
        }

        static int getComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            const auto* value = binding && handle->host
                ? handle->host->read(binding->component_type)
                : nullptr;
            if (!binding || !pushComponentValue(state, binding->abi_kind, value))
                lua_pushnil(state);
            return 1;
        }

        template <class Type>
        [[nodiscard]] static bool readStrictNumber(
            lua_State* state,
            int index,
            Type& result
        ) noexcept
        {
            if (lua_type(state, index) != LUA_TNUMBER)
                return false;
            const auto value = lua_tonumber(state, index);
            if constexpr (std::is_integral_v<Type>)
            {
                if (!std::isfinite(value) || std::trunc(value) != value ||
                    value < static_cast<lua_Number>(
                        std::numeric_limits<Type>::lowest()) ||
                    value > static_cast<lua_Number>(
                        std::numeric_limits<Type>::max()))
                {
                    return false;
                }
            }
            else if (!std::isfinite(value) ||
                     value < -static_cast<lua_Number>(
                         std::numeric_limits<Type>::max()) ||
                     value > static_cast<lua_Number>(
                         std::numeric_limits<Type>::max()))
            {
                return false;
            }
            result = static_cast<Type>(value);
            return true;
        }

        static int patchComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            if (!binding || !handle->host)
            {
                lua_pushboolean(state, false);
                return 1;
            }
            alignas(std::uint64_t) std::byte storage[sizeof(std::uint64_t)]{};
            bool valid{};
            switch (binding->abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                if (lua_type(state, 3) == LUA_TBOOLEAN)
                {
                    *reinterpret_cast<bool*>(storage) =
                        lua_toboolean(state, 3) != 0;
                    valid = true;
                }
                break;
            case LUX_SCRIPT_VK_INT32:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<std::int32_t*>(storage));
                break;
            case LUX_SCRIPT_VK_UINT32:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<std::uint32_t*>(storage));
                break;
            case LUX_SCRIPT_VK_FLOAT:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<float*>(storage));
                break;
            case LUX_SCRIPT_VK_DOUBLE:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<double*>(storage));
                break;
            default:
                break;
            }
            lua_pushboolean(
                state,
                valid && handle->host->patch(
                    binding->component_type,
                    storage
                ));
            return 1;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (self.free_instances.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(
                std::addressof(artifact.description().body)
            );
            if (body == nullptr)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            for (const auto& binding : self.components)
            {
                ScriptHostComponentContract contract;
                if (!context.behavior || !context.behavior->componentContract(
                        binding.component_type,
                        contract) ||
                    contract.component_type != binding.component_type ||
                    contract.semantic_type != binding.semantic_type ||
                    contract.canonical_name != binding.canonical_name ||
                    contract.abi_kind != binding.abi_kind ||
                    contract.size != binding.size ||
                    contract.alignment != binding.alignment)
                {
                    return EScriptBackendResult::
                        HOST_COMPONENT_CONTRACT_MISMATCH;
                }
            }
            const auto prototype = self.prototypeFor(context, artifact);
            if (prototype == nullptr)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;

            const auto instance_slot = self.free_instances.back();
            auto* instance = std::addressof(self.instances[instance_slot]);
            *instance = Instance{
                std::addressof(self),
                context.asset,
                LUA_NOREF,
                std::holds_alternative<EntityScriptScope>(context.scope),
                nullptr,
                prototype,
                body->suspension_capable_exports,
                instance_slot
            };
            const auto ability_result = self.prepareAbilities(*instance, context);
            if (ability_result != EScriptBackendResult::SUCCESS)
            {
                *instance = {};
                return ability_result;
            }
            const auto event_result = self.prepareEvents(*instance, context, artifact);
            if (event_result != EScriptBackendResult::SUCCESS)
            {
                self.prepared_abilities.release(instance->prepared_abilities);
                *instance = {};
                return event_result;
            }
            self.free_instances.pop_back();
            instance->active = true;

            lua_newtable(self.state);
            const auto instance_index = lua_gettop(self.state);
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, prototype->table_ref);
            const auto prototype_index = lua_gettop(self.state);
            lua_pushnil(self.state);
            while (lua_next(self.state, prototype_index) != 0)
            {
                lua_pushvalue(self.state, -2);
                lua_pushvalue(self.state, -2);
                lua_settable(self.state, instance_index);
                lua_pop(self.state, 1);
            }
            lua_pop(self.state, 1);
            if (instance->entity_scope)
            {
                auto* handle = static_cast<HostHandle*>(
                    lua_newuserdata(self.state, sizeof(HostHandle)));
                *handle = HostHandle{
                    std::addressof(self),
                    context.behavior,
                    true};
                instance->host_handle = handle;
                const auto handle_index = lua_gettop(self.state);
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::hasComponent, 1);
                lua_setfield(self.state, instance_index, "has_component");
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::getComponent, 1);
                lua_setfield(self.state, instance_index, "get_component");
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::patchComponent, 1);
                lua_setfield(self.state, instance_index, "patch_component");
                lua_pop(self.state, 1);
            }
            const auto table_ref = luaL_ref(self.state, LUA_REGISTRYINDEX);
            instance->table_ref = table_ref;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void* opaque,
            ScriptBackendInstance instance_value,
            const lux::rdesc::ScriptFunction& function,
            ScriptBackendPreparedMethod& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;

            const FunctionKey key{instance->asset, function.symbol_id};
            const LuaFunctionBinding* function_binding{};
            const auto cached = self.function_index.find(key);
            if (cached != self.function_index.end())
            {
                function_binding = std::addressof(self.function_bindings[cached->second]);
                if (function_binding->signature != function)
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            else
            {
                if (self.function_bindings.size() >= self.prepared_calls.size())
                    return EScriptBackendResult::CAPACITY_EXCEEDED;
                std::vector<const LuaRecordMarshaller*> argument_marshallers;
                try
                {
                    argument_marshallers.reserve(function.args.size());
                    for (const auto& argument : function.args)
                    {
                        const auto* record = self.recordMarshaller(argument);
                        if (!self.supportedType(argument) && !record)
                            return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                        argument_marshallers.push_back(record);
                    }
                    for (const auto& return_type : function.returns)
                    {
                        const bool unsupported_return = !self.supportedType(return_type) ||
                            return_type.pass != lux::semantic::EValuePass::VALUE;
                        if (unsupported_return)
                            return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }

                const auto prototype = self.prototypes.find(instance->asset);
                if (prototype == self.prototypes.end())
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                lua_rawgeti(self.state, LUA_REGISTRYINDEX, prototype->second.table_ref);
                lua_getfield(self.state, -1, function.name.c_str());
                lua_remove(self.state, -2);
                if (!lua_isfunction(self.state, -1))
                {
                    lua_pop(self.state, 1);
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
                const auto function_ref = luaL_ref(self.state, LUA_REGISTRYINDEX);
                try
                {
                    const auto binding_index = self.function_bindings.size();
                    self.function_bindings.push_back(LuaFunctionBinding{
                        function,
                        function_ref,
                        std::move(argument_marshallers)
                    });
                    if (!self.function_index.emplace(key, binding_index).second)
                    {
                        self.function_bindings.pop_back();
                        luaL_unref(self.state, LUA_REGISTRYINDEX, function_ref);
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }
                    function_binding = std::addressof(self.function_bindings.back());
                }
                catch (const std::bad_alloc&)
                {
                    if (!self.function_bindings.empty() &&
                        self.function_bindings.back().function_ref == function_ref)
                    {
                        self.function_bindings.pop_back();
                    }
                    luaL_unref(self.state, LUA_REGISTRYINDEX, function_ref);
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }
            }

            const auto call_slot = self.free_prepared_calls.back();
            self.free_prepared_calls.pop_back();
            auto& call = self.prepared_calls[call_slot];
            call.instance = instance;
            call.function = function_binding;
            call.active = true;
            const bool resumable = std::binary_search(
                instance->suspension_capable_exports.begin(),
                instance->suspension_capable_exports.end(),
                function.symbol_id
            );
            if (resumable && !function.returns.empty())
            {
                call.instance = nullptr;
                call.function = nullptr;
                call.active = false;
                self.free_prepared_calls.push_back(call_slot);
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            }
            result = {
                std::addressof(call),
                lux::script::BoundScriptCall{&State::invoke, std::addressof(call)},
                resumable
                    ? BoundScriptStepCall{std::addressof(call), &invokePreparedStep}
                    : BoundScriptStepCall{}
            };
            return EScriptBackendResult::SUCCESS;
        }

        static bool pushArgument(
            lua_State* state,
            const lux_script_value_slot& value,
            const LuaRecordMarshaller* record
        ) noexcept
        {
            if (!value.data)
                return false;
            if (record)
            {
                const auto address = reinterpret_cast<std::uintptr_t>(
                    value.data
                );
                const bool valid_layout = value.kind ==
                        LUX_SCRIPT_VK_STRUCT_REF &&
                    value.type_id == record->semantic_type &&
                    value.size == record->size &&
                    address % record->alignment == 0U;
                return valid_layout && record->push && record->push(
                    record->context,
                    state,
                    value.data
                );
            }
            switch (value.kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                lua_pushboolean(state, *static_cast<const bool*>(value.data));
                return true;
            case LUX_SCRIPT_VK_INT32:
                lua_pushnumber(
                    state,
                    static_cast<lua_Number>(*static_cast<const std::int32_t*>(value.data)));
                return true;
            case LUX_SCRIPT_VK_UINT32:
                lua_pushnumber(
                    state,
                    static_cast<lua_Number>(*static_cast<const std::uint32_t*>(value.data)));
                return true;
            case LUX_SCRIPT_VK_FLOAT:
                lua_pushnumber(state, *static_cast<const float*>(value.data));
                return true;
            case LUX_SCRIPT_VK_DOUBLE:
                lua_pushnumber(state, *static_cast<const double*>(value.data));
                return true;
            default:
                return false;
            }
        }

        template <class Type>
        static bool writeNumber(
            lux_script_value_slot& slot,
            Type value
        ) noexcept
        {
            if (!slot.data || slot.size < sizeof(Type))
                return false;
            std::memcpy(slot.data, std::addressof(value), sizeof(Type));
            return true;
        }

        static bool readReturn(
            lua_State* state,
            int index,
            lux_script_value_slot& slot
        ) noexcept
        {
            switch (slot.kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                if (lua_type(state, index) != LUA_TBOOLEAN)
                    return false;
                return writeNumber<bool>(
                    slot,
                    lua_toboolean(state, index) != 0);
            case LUX_SCRIPT_VK_INT32:
            {
                std::int32_t value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_UINT32:
            {
                std::uint32_t value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_FLOAT:
            {
                float value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_DOUBLE:
            {
                double value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            default:
                return false;
            }
        }

        union ScalarStorage final
        {
            bool boolean;
            std::int32_t i32;
            std::uint32_t u32;
            std::int64_t i64;
            float f32;
            double f64;
        };

        [[nodiscard]] static bool readAbilityArgument(
            lua_State* state,
            int index,
            const lux::script::ScriptAbilityValueDescription& description,
            ScalarStorage& storage,
            lux::script::ScriptAbilityInputSlot& slot
        ) noexcept
        {
            bool valid{};
            switch (description.abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                if (lua_type(state, index) == LUA_TBOOLEAN)
                {
                    storage.boolean = lua_toboolean(state, index) != 0;
                    valid = true;
                }
                break;
            case LUX_SCRIPT_VK_INT32: valid = readStrictNumber(state, index, storage.i32); break;
            case LUX_SCRIPT_VK_UINT32: valid = readStrictNumber(state, index, storage.u32); break;
            case LUX_SCRIPT_VK_FLOAT: valid = readStrictNumber(state, index, storage.f32); break;
            case LUX_SCRIPT_VK_DOUBLE: valid = readStrictNumber(state, index, storage.f64); break;
            default: break;
            }
            if (!valid)
                return false;
            slot = {
                description.abi_kind,
                {},
                description.size,
                description.type_id,
                std::addressof(storage)
            };
            return true;
        }

        [[nodiscard]] static bool pushAbilityResult(
            lua_State* state,
            const lux::script::ScriptAbilityValueDescription& description,
            const void* value
        ) noexcept
        {
            return pushComponentValue(state, description.abi_kind, value);
        }

        static int abilityFailure(
            lua_State* state,
            LuaContinuation* continuation,
            std::int32_t status,
            const char* message
        ) noexcept
        {
            if (continuation != nullptr)
                continuation->failure_status = status;
            lua_pushboolean(state, false);
            lua_pushstring(state, message);
            return 2;
        }

        struct EventWaitAdmission final
        {
            ScriptAwaitableId waiting_on;
            std::int32_t failure{};
        };

        [[nodiscard]] static EventWaitAdmission admitEventWait(
            ScriptStepContext& step,
            const lux::script::ScriptEventSourceDescription& source
        ) noexcept
        {
            const auto route = source.route == lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                ? EEventRoute::SIMULATION_BROADCAST
                : EEventRoute::ENTITY_TARGETED;
            const auto waiting = step.event_waits.wait({
                lux::system::SystemInstanceId{source.system_id},
                EventPointId{source.event_id},
                route
            });
            if (!waiting)
            {
                return {
                    {},
                    kEventWaitFailure - static_cast<std::int32_t>(waiting.error())
                };
            }
            return {*waiting, 0};
        }

        static int invokeEventWait(lua_State* state) noexcept
        {
            auto* self = static_cast<State*>(lua_touserdata(state, lua_upvalueindex(1)));
            const auto raw_ordinal = lua_tointeger(state, lua_upvalueindex(2));
            const bool is_invalid_ordinal = self == nullptr || raw_ordinal < 0;
            if (is_invalid_ordinal)
                return abilityFailure(state, nullptr, kInvalidCall, "invalid Lux Script Event source");
            auto* execution = self->active_execution;
            const auto ordinal = static_cast<std::size_t>(raw_ordinal);
            const bool is_invalid_context = execution == nullptr || execution->thread != state ||
                execution->instance == nullptr ||
                execution->continuation == nullptr || execution->step == nullptr || lua_gettop(state) != 0;
            if (is_invalid_context)
            {
                return abilityFailure(
                    state,
                    execution != nullptr ? execution->continuation : nullptr,
                    kInvalidCall,
                    "Script Event wait requires a coroutine-capable export"
                );
            }
            const bool is_foreign_layout = execution->instance->owner != self ||
                execution->instance->prototype == nullptr ||
                lua_touserdata(state, lua_upvalueindex(3)) != execution->instance->prototype->layout_token;
            if (is_foreign_layout)
            {
                return abilityFailure(state, execution->continuation, kInvalidCall,
                    "Script Event closure belongs to a different prepared layout");
            }
            auto* prepared = self->prepared_events.at(execution->instance->prepared_events, ordinal);
            if (prepared == nullptr || prepared->source == nullptr)
            {
                return abilityFailure(
                    state,
                    execution->continuation,
                    kInvalidCall,
                    "Script did not declare this Event source"
                );
            }
            const auto& source = *prepared->source;
            const auto admission = admitEventWait(*execution->step, source);
            if (admission.failure != 0)
            {
                return abilityFailure(
                    state,
                    execution->continuation,
                    admission.failure,
                    "Script Event wait admission failed"
                );
            }
            execution->continuation->waiting_on = admission.waiting_on;
            execution->continuation->pending_ordinal = static_cast<std::uint32_t>(ordinal);
            execution->continuation->pending_operation = EPendingOperation::EVENT;
            lua_pushboolean(state, true);
            return 1;
        }

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            if (!frame || !frame->user_context)
                return -1;
            auto& call = *static_cast<PreparedCall*>(frame->user_context);
            if (!call.active || !call.instance || !call.function)
                return -1;
            auto& self = *call.instance->owner;
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, self.traceback_ref);
            const auto error_index = lua_gettop(self.state);
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, call.function->function_ref);
            std::uint32_t argument_count{};
            if (call.instance->entity_scope)
            {
                lua_rawgeti(
                    self.state,
                    LUA_REGISTRYINDEX,
                    call.instance->table_ref);
                ++argument_count;
            }
            for (std::uint32_t index{}; index < frame->arg_count; ++index)
            {
                const auto* record = index < call.function->argument_marshallers.size()
                    ? call.function->argument_marshallers[index]
                    : nullptr;
                if (!pushArgument(self.state, frame->args[index], record))
                {
                    lua_settop(self.state, error_index - 1);
                    return -3;
                }
                ++argument_count;
            }
            ExecutionScope execution{
                self,
                {self.state, call.instance, nullptr, nullptr, nullptr}
            };
            if (!execution)
            {
                lua_settop(self.state, error_index - 1);
                return kExecutionDepthCapacity;
            }
            if (lua_pcall(
                    self.state,
                    static_cast<int>(argument_count),
                    static_cast<int>(frame->return_count),
                    error_index) != LUA_OK)
            {
                lua_settop(self.state, error_index - 1);
                return kLuaFailure;
            }
            for (std::uint32_t index{}; index < frame->return_count; ++index)
            {
                const auto stack_index =
                    error_index + 1 + static_cast<int>(index);
                if (!readReturn(
                        self.state,
                        stack_index,
                        frame->returns[index]))
                {
                    lua_settop(self.state, error_index - 1);
                    return -5;
                }
            }
            lua_settop(self.state, error_index - 1);
            return 0;
        }

        [[nodiscard]] LuaContinuation* acquireContinuation(
            Instance& instance,
            PreparedCall& call
        ) noexcept
        {
            if (free_continuations.empty())
                return nullptr;
            lua_State* thread = lua_newthread(state);
            if (thread == nullptr)
                return nullptr;
            const auto thread_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            if (thread_ref == LUA_NOREF || thread_ref == LUA_REFNIL)
                return nullptr;
            ++vm_coroutine_creations;
            const auto slot = free_continuations.back();
            free_continuations.pop_back();
            auto& continuation = continuations[slot];
            continuation = {
                this,
                std::addressof(instance),
                std::addressof(call),
                thread,
                thread_ref,
                {},
                0U,
                EPendingOperation::NONE,
                0,
                true
            };
            ++instance.active_continuations;
            return std::addressof(continuation);
        }

        static void destroyLuaContinuation(LuaContinuation& continuation) noexcept
        {
            if (!continuation.active || continuation.owner == nullptr)
                return;
            auto* owner = continuation.owner;
            ++owner->vm_coroutine_releases;
            const auto slot = static_cast<std::size_t>(
                std::addressof(continuation) - owner->continuations.data()
            );
            if (continuation.thread != nullptr)
                lua_settop(continuation.thread, 0);
            if (continuation.thread_ref != LUA_NOREF)
                luaL_unref(owner->state, LUA_REGISTRYINDEX, continuation.thread_ref);
            if (continuation.instance == nullptr || continuation.instance->active_continuations == 0U)
                std::terminate();
            --continuation.instance->active_continuations;
            continuation = {};
            owner->free_continuations.push_back(slot);
        }

        static void destroyLuaContinuationErased(void* opaque) noexcept
        {
            if (opaque == nullptr)
                return;
            auto& continuation = *static_cast<LuaContinuation*>(opaque);
            if (continuation.owner == nullptr)
                return;
            destroyLuaContinuation(continuation);
        }

        [[nodiscard]] static bool pushResumeValue(
            LuaContinuation& continuation,
            const ScriptResumePacket& packet,
            int& argument_count
        ) noexcept
        {
            argument_count = 0;
            if (continuation.pending_operation == EPendingOperation::EVENT)
            {
                const auto* prepared = continuation.owner->prepared_events.at(
                    continuation.instance->prepared_events,
                    continuation.pending_ordinal
                );
                const bool is_invalid_prepared_event = prepared == nullptr || prepared->source == nullptr;
                const bool is_invalid_packet_value = packet.value == nullptr || !packet.value->type.valid();
                if (is_invalid_prepared_event || is_invalid_packet_value)
                {
                    return false;
                }
                const auto& expected = prepared->source->payload;
                const auto& actual = packet.value->type;
                const bool is_mismatch = actual.type_id != expected.type_id ||
                    actual.abi_kind != expected.abi_kind || actual.size != expected.size ||
                    actual.alignment != expected.alignment ||
                    packet.value->bytes.size() != expected.size;
                if (is_mismatch)
                    return false;
                if (expected.abi_kind == LUX_SCRIPT_VK_STRUCT_REF)
                {
                    const auto found = continuation.owner->record_marshaller_index.find(expected.type_id);
                    if (found == continuation.owner->record_marshaller_index.end())
                        return false;
                    const auto& marshaller = continuation.owner->record_marshallers[found->second];
                    if (!marshaller.push(
                            marshaller.context,
                            continuation.thread,
                            packet.value->bytes.data()
                        ))
                    {
                        return false;
                    }
                }
                else if (!pushComponentValue(
                             continuation.thread,
                             expected.abi_kind,
                             packet.value->bytes.data()
                         ))
                {
                    return false;
                }
                argument_count = 1;
                return true;
            }
            if (continuation.pending_operation != EPendingOperation::ABILITY)
                return false;
            const auto* prepared = continuation.owner->prepared_abilities.at(
                continuation.instance->prepared_abilities,
                continuation.pending_ordinal
            );
            if (prepared == nullptr || prepared->semantic == nullptr)
                return false;
            const auto& method = *prepared->semantic;
            if (method.results.empty())
            {
                const bool has_value = packet.value != nullptr &&
                    (packet.value->type.valid() || !packet.value->bytes.empty());
                return !has_value;
            }
            if (method.results.size() != 1U || packet.value == nullptr || !packet.value->type.valid() ||
                packet.value->bytes.size() != method.results.front().size)
            {
                return false;
            }
            const auto& expected = method.results.front();
            const auto& actual = packet.value->type;
            const bool is_mismatch = actual.type_id != expected.type_id ||
                actual.abi_kind != expected.abi_kind || actual.size != expected.size ||
                actual.alignment != expected.alignment;
            if (is_mismatch || !pushAbilityResult(continuation.thread, expected, packet.value->bytes.data()))
                return false;
            argument_count = 1;
            return true;
        }

        [[nodiscard]] static ScriptStepResult finishLuaStep(
            LuaContinuation& continuation,
            lux::script::lua::detail::LuaResumeResult resume,
            bool release_terminal
        ) noexcept
        {
            if (resume.status == LUA_YIELD)
            {
                if (continuation.waiting_on.valid() && resume.result_count == 0)
                    return ScriptStepResult::suspended(continuation.waiting_on);
                const auto failure = continuation.failure_status != 0
                    ? continuation.failure_status
                    : kInvalidCall;
                if (release_terminal)
                    destroyLuaContinuation(continuation);
                return ScriptStepResult::failed(failure);
            }
            if (resume.status != LUA_OK)
            {
                const auto failure = continuation.failure_status != 0
                    ? continuation.failure_status
                    : kLuaFailure;
                if (release_terminal)
                    destroyLuaContinuation(continuation);
                return ScriptStepResult::failed(failure);
            }
            if (resume.result_count != 0)
            {
                if (release_terminal)
                    destroyLuaContinuation(continuation);
                return ScriptStepResult::failed(kInvalidResult);
            }
            lua_settop(continuation.thread, 0);
            if (release_terminal)
                destroyLuaContinuation(continuation);
            return ScriptStepResult::completed();
        }

        static ScriptStepResult resumeLuaContinuation(
            void* opaque,
            ScriptStepContext& context,
            const ScriptResumePacket& packet
        ) noexcept
        {
            auto& continuation = *static_cast<LuaContinuation*>(opaque);
            if (continuation.owner == nullptr)
                return ScriptStepResult::failed(kInvalidResume);
            if (!continuation.active || continuation.owner == nullptr || continuation.instance == nullptr ||
                continuation.call == nullptr || continuation.thread == nullptr ||
                packet.awaitable != continuation.waiting_on)
            {
                return ScriptStepResult::failed(kInvalidResume);
            }
            if (packet.state != EScriptAwaitableState::READY)
            {
                return ScriptStepResult::failed(
                    packet.state == EScriptAwaitableState::FAILED && packet.error.valid()
                        ? packet.error.status
                        : kInvalidResume
                );
            }
            int argument_count{};
            if (!pushResumeValue(continuation, packet, argument_count))
                return ScriptStepResult::failed(kInvalidResume);
            continuation.waiting_on = {};
            continuation.pending_operation = EPendingOperation::NONE;
            continuation.failure_status = 0;
            ExecutionScope execution{
                *continuation.owner,
                {
                    continuation.thread,
                    continuation.instance,
                    std::addressof(continuation),
                    std::addressof(context),
                    nullptr
                }
            };
            if (!execution)
            {
                return ScriptStepResult::failed(kExecutionDepthCapacity);
            }
            ++continuation.owner->vm_coroutine_resumes;
            const auto resume = lux::script::lua::detail::resumeLuaVm(
                continuation.thread,
                nullptr,
                argument_count
            );
            return finishLuaStep(continuation, resume, false);
        }

        static ScriptStepResult invokePreparedStep(
            void* opaque,
            lux_script_call_frame& frame,
            ScriptStepContext& context,
            ScriptBackendContinuation& result
        ) noexcept
        {
            auto& call = *static_cast<PreparedCall*>(opaque);
            if (!call.active || call.instance == nullptr || call.function == nullptr || frame.return_count != 0U)
                return ScriptStepResult::failed(kInvalidCall);
            auto& self = *call.instance->owner;
            auto* continuation = self.acquireContinuation(*call.instance, call);
            if (continuation == nullptr)
                return ScriptStepResult::failed(kContinuationCapacity);

            lua_rawgeti(continuation->thread, LUA_REGISTRYINDEX, call.function->function_ref);
            std::uint32_t argument_count{};
            if (call.instance->entity_scope)
            {
                lua_rawgeti(continuation->thread, LUA_REGISTRYINDEX, call.instance->table_ref);
                ++argument_count;
            }
            for (std::uint32_t index{}; index < frame.arg_count; ++index)
            {
                const auto* record = index < call.function->argument_marshallers.size()
                    ? call.function->argument_marshallers[index]
                    : nullptr;
                if (!pushArgument(continuation->thread, frame.args[index], record))
                {
                    destroyLuaContinuation(*continuation);
                    return ScriptStepResult::failed(kMarshalFailure);
                }
                ++argument_count;
            }
            ExecutionScope execution{
                self,
                {continuation->thread, call.instance, continuation, std::addressof(context), nullptr}
            };
            if (!execution)
            {
                destroyLuaContinuation(*continuation);
                return ScriptStepResult::failed(kExecutionDepthCapacity);
            }
            const auto resume = lux::script::lua::detail::resumeLuaVm(
                continuation->thread,
                nullptr,
                static_cast<int>(argument_count)
            );
            const auto step_result = finishLuaStep(*continuation, resume, true);
            if (step_result.state == EScriptStepState::SUSPENDED && step_result.valid())
            {
                result = {continuation, &resumeLuaContinuation, &destroyLuaContinuationErased};
            }
            return step_result;
        }

        static void releaseMethod(
            void* opaque,
            ScriptBackendInstance,
            ScriptBackendPreparedMethod method
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* call = static_cast<PreparedCall*>(method.token);
            if (!call || !call->active)
                return;
            call->instance = nullptr;
            call->function = nullptr;
            call->active = false;
            const auto call_slot = static_cast<std::size_t>(call - self.prepared_calls.data());
            self.free_prepared_calls.push_back(call_slot);
        }

        static void destroyInstance(
            void* opaque,
            ScriptBackendInstance instance_value
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance)
                return;
            if (instance->active_continuations != 0U)
                std::terminate();
            if (instance->host_handle)
            {
                instance->host_handle->alive = false;
                instance->host_handle->host = nullptr;
                instance->host_handle->owner = nullptr;
                instance->host_handle = nullptr;
            }
            if (instance->table_ref != LUA_NOREF)
                luaL_unref(self.state, LUA_REGISTRYINDEX, instance->table_ref);
            const auto instance_slot = static_cast<std::size_t>(
                instance - self.instances.data()
            );
            self.prepared_abilities.release(instance->prepared_abilities);
            self.prepared_events.release(instance->prepared_events);
            *instance = {};
            self.free_instances.push_back(instance_slot);
        }

        // Must outlive ScriptEngine: lua_close may call the original allocator through this diagnostic wrapper.
        VmAllocationTracker vm_allocations;
        lux::script::lua::ScriptEngine engine;
        lua_State* state{};
        lux::script::lua::LuaRuntimeInfo runtime_info;
        bool vm_configured{};
        int traceback_ref{LUA_NOREF};
        std::size_t instance_capacity{};
        std::size_t prepared_call_capacity{};
        std::size_t continuation_capacity{};
        std::size_t execution_depth_capacity{};
        std::unordered_map<lux::asset::AssetId, Prototype> prototypes;
        std::vector<LuaComponentBinding> components;
        std::unordered_map<std::string_view, std::size_t> component_index;
        std::vector<LuaRecordMarshaller> record_marshallers;
        std::unordered_map<std::uint64_t, std::size_t>
            record_marshaller_index;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
        std::vector<LuaFunctionBinding> function_bindings;
        std::unordered_map<FunctionKey, std::size_t, FunctionKeyHash> function_index;
        std::vector<PreparedCall> prepared_calls;
        std::vector<std::size_t> free_prepared_calls;
        std::vector<AbilityMethod> ability_methods;
        PreparedSpanArena<PreparedAbility> prepared_abilities;
        std::vector<lux::script::ScriptEventSourceDescription> event_sources;
        PreparedSpanArena<PreparedEventSource> prepared_events;
        std::vector<LuaContinuation> continuations;
        std::vector<std::size_t> free_continuations;
        ExecutionFrame* active_execution{};
        std::size_t execution_depth{};
        std::size_t execution_depth_high_water{};
        std::size_t vm_coroutine_creations{};
        std::size_t vm_coroutine_resumes{};
        std::size_t vm_coroutine_releases{};
    };

    bool detail::LuaAbilityProjectionAccess::current(
        lua_State* state,
        LuaPreparedAbilityAccess& result
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::State*>(lua_touserdata(state, lua_upvalueindex(1)));
        const auto raw_slot = lua_tointeger(state, lua_upvalueindex(2));
        if (owner == nullptr || raw_slot < 0 || owner->active_execution == nullptr ||
            owner->active_execution->thread != state || owner->active_execution->instance == nullptr)
        {
            return false;
        }
        const auto* instance = owner->active_execution->instance;
        const bool is_foreign_layout = instance->owner != owner || instance->prototype == nullptr ||
            lua_touserdata(state, lua_upvalueindex(3)) != instance->prototype->layout_token;
        if (is_foreign_layout)
            return false;
        const auto local_slot = static_cast<std::size_t>(raw_slot);
        auto* prepared = owner->prepared_abilities.at(
            owner->active_execution->instance->prepared_abilities,
            local_slot
        );
        if (prepared == nullptr || prepared->context == nullptr || prepared->dispatch == nullptr)
            return false;
        result = {
            prepared->context,
            prepared->dispatch,
            owner->active_execution->step,
            static_cast<std::uint32_t>(local_slot)
        };
        return true;
    }

    int detail::LuaAbilityProjectionAccess::fail(
        lua_State* state,
        std::int32_t status,
        const char* message
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::State*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* continuation = owner != nullptr && owner->active_execution != nullptr
            ? owner->active_execution->continuation
            : nullptr;
        return LuaScriptBackend::State::abilityFailure(state, continuation, status, message);
    }

    int detail::LuaAbilityProjectionAccess::suspend(
        lua_State* state,
        ScriptStepResult result,
        std::uint32_t local_slot
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::State*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* execution = owner != nullptr ? owner->active_execution : nullptr;
        if (execution == nullptr || execution->thread != state || execution->continuation == nullptr ||
            result.state != EScriptStepState::SUSPENDED || !result.valid())
        {
            const auto status = result.error.valid() ? result.error.status : -1;
            return LuaScriptBackend::State::abilityFailure(
                state,
                execution != nullptr ? execution->continuation : nullptr,
                status,
                "async Script Ability admission failed"
            );
        }
        execution->continuation->waiting_on = result.waiting_on;
        execution->continuation->pending_ordinal = local_slot;
        execution->continuation->pending_operation = LuaScriptBackend::State::EPendingOperation::ABILITY;
        lua_pushboolean(state, true);
        return 1;
    }

    int detail::LuaAbilityProjectionAccess::succeed(lua_State* state, int results) noexcept
    {
        lua_pushboolean(state, true);
        lua_insert(state, -results - 1);
        return results + 1;
    }

    int detail::LuaAbilityProjectionAccess::argumentCount(lua_State* state) noexcept
    {
        return lua_gettop(state);
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, bool& value) noexcept
    {
        if (!lua_isboolean(state, index))
            return false;
        value = lua_toboolean(state, index) != 0;
        return true;
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, std::int32_t& value) noexcept
    {
        return LuaScriptBackend::State::readStrictNumber(state, index, value);
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, std::uint32_t& value) noexcept
    {
        return LuaScriptBackend::State::readStrictNumber(state, index, value);
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, float& value) noexcept
    {
        return LuaScriptBackend::State::readStrictNumber(state, index, value);
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, double& value) noexcept
    {
        return LuaScriptBackend::State::readStrictNumber(state, index, value);
    }

    void detail::LuaAbilityProjectionAccess::push(lua_State* state, bool value) noexcept
    {
        lua_pushboolean(state, value);
    }

    void detail::LuaAbilityProjectionAccess::push(lua_State* state, std::int32_t value) noexcept
    {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }

    void detail::LuaAbilityProjectionAccess::push(lua_State* state, std::uint32_t value) noexcept
    {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }

    void detail::LuaAbilityProjectionAccess::push(lua_State* state, float value) noexcept
    {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }

    void detail::LuaAbilityProjectionAccess::push(lua_State* state, double value) noexcept
    {
        lua_pushnumber(state, value);
    }

    lux::cxx::expected<
        LuaScriptBackend,
        ELuaScriptBindingBackendError> LuaScriptBackend::create(
            LuaScriptBackendConfig config
        ) noexcept
    {
        const bool has_invalid_capacity = config.instance_capacity == 0U ||
            config.prepared_call_capacity == 0U || config.continuation_capacity == 0U ||
            config.execution_depth_capacity == 0U || config.ability_catalog_method_capacity == 0U ||
            config.event_catalog_capacity == 0U;
        if (has_invalid_capacity)
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        for (std::size_t index{}; index < config.components.size(); ++index)
        {
            const auto& component = config.components[index];
            const auto* layout = lux::semantic::builtinLayout(
                component.semantic_type);
            const bool supported_kind = component.abi_kind ==
                    LUX_SCRIPT_VK_BOOL ||
                component.abi_kind == LUX_SCRIPT_VK_INT32 ||
                component.abi_kind == LUX_SCRIPT_VK_UINT32 ||
                component.abi_kind == LUX_SCRIPT_VK_FLOAT ||
                component.abi_kind == LUX_SCRIPT_VK_DOUBLE;
            if (component.name.empty() || component.component_type == 0U ||
                component.canonical_name.empty() ||
                component.semantic_type !=
                    lux::semantic::typeId(
                        component.canonical_name) ||
                !layout ||
                layout->canonical_name != component.canonical_name ||
                layout->abi_kind != component.abi_kind ||
                layout->size != component.size ||
                layout->alignment != component.alignment ||
                !supported_kind)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::
                        INVALID_COMPONENT_CONTRACT);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (config.components[previous].name == component.name)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            DUPLICATE_COMPONENT_NAME);
                }
                if (config.components[previous].component_type ==
                    component.component_type)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            INVALID_COMPONENT_CONTRACT);
                }
            }
        }
        for (std::size_t index{}; index < config.record_marshallers.size(); ++index)
        {
            const auto& marshaller = config.record_marshallers[index];
            const bool power_of_two_alignment = marshaller.alignment != 0U &&
                (marshaller.alignment & (marshaller.alignment - 1U)) == 0U;
            const bool valid_identity = marshaller.semantic_type != 0U &&
                !marshaller.canonical_name.empty() &&
                marshaller.semantic_type == lux::semantic::typeId(
                    marshaller.canonical_name
                );
            if (!valid_identity || marshaller.size == 0U ||
                !power_of_two_alignment || !marshaller.push)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::INVALID_RECORD_MARSHALLER
                );
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                const auto& candidate = config.record_marshallers[previous];
                if (candidate.semantic_type == marshaller.semantic_type ||
                    candidate.canonical_name == marshaller.canonical_name)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            DUPLICATE_RECORD_MARSHALLER
                    );
                }
            }
        }
        std::size_t ability_method_count{};
        for (std::size_t ability_index{}; ability_index < config.abilities.size(); ++ability_index)
        {
            const auto& contribution = config.abilities[ability_index];
            if (!contribution.valid() || contribution.description->methods.empty() ||
                !State::identifier(contribution.description->name) ||
                !lux::script::scriptAbilityMethodIdsUnique(contribution.description->methods))
            {
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_ABILITY_CONTRIBUTION);
            }
            for (std::size_t previous{}; previous < ability_index; ++previous)
            {
                const auto* candidate = config.abilities[previous].description;
                if (candidate->id == contribution.description->id)
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::DUPLICATE_ABILITY_CONTRACT);
                if (candidate->name == contribution.description->name)
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::DUPLICATE_ABILITY_NAME);
            }
            for (std::size_t method_index{}; method_index < contribution.description->methods.size(); ++method_index)
            {
                const auto& method = contribution.description->methods[method_index];
                const auto& projection = contribution.methods[method_index];
                if (!State::identifier(method.name) || method.parameters.size() > State::kMaxAbilityArguments ||
                    method.results.size() > State::kMaxAbilityResults ||
                    (method.kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION && method.results.size() > 1U) ||
                    projection.entry == nullptr || projection.method != method.id)
                {
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_ABILITY_CONTRIBUTION);
                }
                for (std::size_t previous{}; previous < method_index; ++previous)
                {
                    if (contribution.description->methods[previous].name == method.name)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::DUPLICATE_ABILITY_METHOD);
                }
                for (const auto& parameter : method.parameters)
                {
                    if (!State::supportedType(parameter.value))
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                }
                for (const auto& result : method.results)
                {
                    const bool is_invalid_async = method.kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION &&
                        (result.pass != lux::semantic::EValuePass::VALUE ||
                         result.lifetime != lux::script::EScriptAbilityValueLifetime::AWAITABLE);
                    if (!State::supportedType(result) || is_invalid_async)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                }
            }
            const bool has_method_count_overflow = ability_method_count >
                std::numeric_limits<std::size_t>::max() - contribution.description->methods.size();
            if (has_method_count_overflow)
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
            ability_method_count += contribution.description->methods.size();
        }
        if (ability_method_count > config.ability_catalog_method_capacity ||
            (ability_method_count != 0U && config.prepared_ability_capacity == 0U))
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        if (config.events.size() > config.event_catalog_capacity ||
            (!config.events.empty() && config.prepared_event_capacity == 0U))
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        for (std::size_t index{}; index < config.events.size(); ++index)
        {
            const auto& source = config.events[index];
            const bool is_supported_scalar = [&]() noexcept {
                switch (source.payload.abi_kind)
                {
                case LUX_SCRIPT_VK_BOOL:
                    return source.payload.size == sizeof(bool) && source.payload.alignment == alignof(bool);
                case LUX_SCRIPT_VK_INT32:
                    return source.payload.size == sizeof(std::int32_t) &&
                        source.payload.alignment == alignof(std::int32_t);
                case LUX_SCRIPT_VK_UINT32:
                    return source.payload.size == sizeof(std::uint32_t) &&
                        source.payload.alignment == alignof(std::uint32_t);
                case LUX_SCRIPT_VK_FLOAT:
                    return source.payload.size == sizeof(float) && source.payload.alignment == alignof(float);
                case LUX_SCRIPT_VK_DOUBLE:
                    return source.payload.size == sizeof(double) && source.payload.alignment == alignof(double);
                default: return false;
                }
            }();
            const bool is_invalid_source = !source.valid() || !State::identifier(source.system_name) ||
                !State::identifier(source.event_name) ||
                source.payload.type_id != lux::semantic::typeId(source.payload.canonical_name) ||
                (source.payload.abi_kind != LUX_SCRIPT_VK_STRUCT_REF && !is_supported_scalar);
            if (is_invalid_source)
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_EVENT_SOURCE);
            if (source.payload.abi_kind == LUX_SCRIPT_VK_STRUCT_REF &&
                std::ranges::none_of(config.record_marshallers, [&](const auto& marshaller) noexcept {
                    return marshaller.semantic_type == source.payload.type_id &&
                        marshaller.canonical_name == source.payload.canonical_name &&
                        marshaller.size == source.payload.size && marshaller.alignment == source.payload.alignment;
                }))
            {
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_EVENT_PAYLOAD);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                const auto& candidate = config.events[previous];
                if ((candidate.system_name == source.system_name && candidate.event_name == source.event_name) ||
                    (candidate.system_id == source.system_id && candidate.event_id == source.event_id))
                {
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::DUPLICATE_EVENT_SOURCE);
                }
            }
        }
        try
        {
            auto state = std::make_unique<State>(config);
            if (!state->vm_configured)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::VM_CONFIGURATION_FAILURE
                );
            }
            return LuaScriptBackend{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ELuaScriptBindingBackendError::ALLOCATION_FAILURE);
        }
    }

    LuaScriptBackend::LuaScriptBackend(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {
    }

    LuaScriptBackend::~LuaScriptBackend() = default;
    LuaScriptBackend::LuaScriptBackend(
        LuaScriptBackend&&
    ) noexcept = default;
    LuaScriptBackend& LuaScriptBackend::operator=(
        LuaScriptBackend&&
    ) noexcept = default;

    LuaScriptBackend::operator bool() const noexcept
    {
        return state_ && state_->state && state_->traceback_ref != LUA_NOREF;
    }

    lux::script::lua::LuaRuntimeInfo LuaScriptBackend::runtimeInfo() const noexcept
    {
        return state_ ? state_->runtime_info : lux::script::lua::LuaRuntimeInfo{};
    }

    LuaScriptBackendStats LuaScriptBackend::stats() const noexcept
    {
        if (!state_)
            return {};
        const auto abilities = state_->prepared_abilities.stats();
        const auto events = state_->prepared_events.stats();
        return {
            abilities.active,
            abilities.high_water,
            events.active,
            events.high_water,
            abilities.storage_bytes + events.storage_bytes,
            state_->vm_coroutine_creations,
            state_->execution_depth_high_water,
            state_->vm_coroutine_resumes,
            state_->vm_coroutine_releases,
            state_->vm_allocations.stats
        };
    }

    ScriptBackendDescriptor LuaScriptBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::LUA_SOURCE,
            state_.get(),
            &State::createInstance,
            &State::prepareMethod,
            &State::releaseMethod,
            &State::destroyInstance};
    }

}
