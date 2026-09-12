#include <lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux_lua55_extensions.h>

#include <lux/engine/function/script/lua/Lua.hpp>
#include <lux/engine/function/script/lua/detail/Lua55Operations.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptContractValidation.hpp>
#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>

#include <lua.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
    lux::cxx::expected<LuaPreparedEntryRequirements, ELuaScriptBindingBackendError> describeLuaPreparedRequirements(
        const lux::rdesc::Script& description,
        std::span<const lux::script::lua::ScriptAbilityLuaContribution> contributions) noexcept
    {
        LuaPreparedEntryRequirements result{0U, description.event_requirements.size()};
        for (const auto& requirement : description.api_requirements)
        {
            const lux::script::ScriptAbilityDescription* selected{};
            for (const auto& contribution : contributions)
            {
                const auto* candidate = contribution.description;
                if (candidate == nullptr || candidate->id.name() != requirement.contract.name() ||
                    candidate->id.hash() != requirement.contract.hash()) continue;
                if (selected != nullptr || candidate->schema_hash != requirement.expected_schema_hash)
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_SCRIPT_REQUIREMENT);
                selected = candidate;
            }
            if (selected == nullptr || selected->methods.size() >
                (std::numeric_limits<std::size_t>::max)() - result.ability_methods)
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_SCRIPT_REQUIREMENT);
            result.ability_methods += selected->methods.size();
        }
        return result;
    }

    struct LuaScriptBackend::Impl final
    {
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
        static constexpr std::int32_t kLuaAllocationFailure = -10;

        struct ThreadCreateRequest final
        {
            lua_State* thread{};
            int roots_ref{LUA_NOREF};
            std::size_t root_slot{};
            bool rooted{};
            int function_ref{LUA_NOREF};
            int self_ref{LUA_NOREF};
            const lux_script_value_slot* arguments{};
            std::uint32_t plain_count{};
            bool arguments_valid{true};
        };

        // Only trivial locals may be crossed by a Lua error. The reservation and
        // rollback live outside pcall; thread allocation remains inside the protected
        // boundary.
        static int createThread(lua_State* state)
        {
            auto* request = static_cast<ThreadCreateRequest*>(lua_touserdata(state, 1));
            lua_rawgeti(state, LUA_REGISTRYINDEX, request->roots_ref);
            auto* thread = lua_newthread(state);
            lua_rawseti(state, -2, static_cast<lua_Integer>(request->root_slot + 1U));
            request->thread = thread;
            request->rooted = true;
            if (!lua_checkstack(thread, static_cast<int>(request->plain_count) + 2))
                return luaL_error(state, "Lua invocation stack allocation failed");
            lua_rawgeti(thread, LUA_REGISTRYINDEX, request->function_ref);
            if (request->self_ref != LUA_NOREF) lua_rawgeti(thread, LUA_REGISTRYINDEX, request->self_ref);
            for (std::uint32_t index{}; index < request->plain_count; ++index)
            {
                if (!pushArgument(thread, request->arguments[index], nullptr))
                {
                    request->arguments_valid = false;
                    break;
                }
            }
            return 0;
        }

        [[nodiscard]] int createThreadProtected(ThreadCreateRequest& request) noexcept
        {
            if (!lua_checkstack(main_thread, 2)) return LUA_ERRMEM;
            const auto base = lua_gettop(main_thread);
            lua_pushcfunction(main_thread, &createThread);
            lua_pushlightuserdata(main_thread, &request);
            const auto status = lua_pcall(main_thread, 1, 0, 0);
            lua_settop(main_thread, base);
            return status;
        }

        enum class EPendingOperation : std::uint8_t
        {
            NONE,
            ABILITY,
            EVENT,
        };

        struct HostHandle final
        {
            Impl* owner{};
            ScriptBehavior* host{};
            bool alive{};
        };

        struct PreparedSpan final
        {
            detail::BoundedClassStorage::Allocation block;
            std::uint32_t count{(std::numeric_limits<std::uint32_t>::max)()};
            [[nodiscard]] bool valid() const noexcept
            {
                return count != (std::numeric_limits<std::uint32_t>::max)();
            }
        };

        struct Prototype final
        {
            lux::script::ScriptArtifactContentId content;
            lux::asset::AssetId asset;
            std::size_t instance_refs{};
            bool superseded{};
            std::vector<std::size_t> function_slots;
            int table_ref{LUA_NOREF};
            int environment_ref{LUA_NOREF};
            const void* layout_token{};
            std::vector<std::uint32_t> ability_ordinals;
            std::vector<std::uint32_t> event_ordinals;
            detail::BoundedClassStorage::ClassHandle ability_class;
            detail::BoundedClassStorage::ClassHandle event_class;
        };

        struct Instance final
        {
            ScriptBehavior* behavior{};
            Impl* owner{};
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

        struct PrototypeKey final
        {
            lux::asset::AssetId asset;
            lux::script::ScriptArtifactContentId content;
            [[nodiscard]] bool operator==(const PrototypeKey&) const noexcept = default;
            struct Hash final
            {
                [[nodiscard]] std::size_t operator()(const PrototypeKey& key) const noexcept
                {
                    return std::hash<lux::asset::AssetId>{}(key.asset) ^
                        lux::script::ScriptArtifactContentId::Hash{}(key.content);
                }
            };
        };

        struct FunctionKey final
        {
            const Prototype* prototype{};
            lux::script::ScriptSymbolId symbol{};

            [[nodiscard]] bool operator==(const FunctionKey&) const noexcept = default;
        };

        struct FunctionKeyHash final
        {
            [[nodiscard]] std::size_t operator()(const FunctionKey& key) const noexcept
            {
                const auto asset_hash = std::hash<const Prototype*>{}(key.prototype);
                const auto symbol_hash = std::hash<lux::script::ScriptSymbolId>{}(key.symbol);
                return asset_hash ^ (symbol_hash + 0x9E3779B9U + (asset_hash << 6U) + (asset_hash >> 2U));
            }
        };

        struct SyncArgument final
        {
            bool (*push)(lua_State*, const void*, const lux::script::lua::LuaValueOperation*) noexcept {};
            const lux::script::lua::LuaValueOperation* operation{};
        };

        struct LuaFunctionBinding final
        {
            lux::rdesc::ScriptFunction signature;
            int function_ref{LUA_NOREF};
            std::vector<const lux::script::lua::LuaValueOperation*> argument_operations;
            std::vector<SyncArgument> sync_arguments;
            bool (*read_sync_result)(lua_State*, void*) noexcept {};
            bool scalar_sync{};
            bool sync_prepared{};
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
            LuxLuaTypedWorker entry{};
        };

        struct PreparedAbility final
        {
            void* context{};
            const void* dispatch{};
            const lux::script::ScriptAbilityErasedMethodBinding* method{};
            const lux::script::ScriptAbilityMethodDescription* semantic{};
            PreparedLocalAsyncStart local_async;
        };

        struct PreparedEventSource final
        {
            const lux::script::lua::LuaValueOperation* operation{};
            const lux::script::ScriptEventSourceDescription* source{};
            ScriptEventAdmissionHandle admission;
        };

        template <class Value>
        class PreparedBlockStorage final
        {
            static_assert(std::is_nothrow_default_constructible_v<Value>);
            static_assert(std::is_nothrow_destructible_v<Value>);
        public:
            struct Stats final
            {
                std::size_t active{};
                std::size_t high_water{};
                std::size_t storage_bytes{};
                std::uint64_t acquire_steps{};
                std::uint64_t release_steps{};
            };

            PreparedBlockStorage(std::size_t capacity, std::span<const LuaPreparedBlockClass> classes,
                std::size_t byte_budget) : capacity_(capacity)
            {
                if (capacity == 0U)
                    return;
                if (classes.empty() || classes.size() > 64U || capacity >= (std::numeric_limits<std::uint32_t>::max)())
                {
                    valid_ = false;
                    return;
                }
                std::vector<detail::StorageClassPlan> plans;
                plans.reserve(classes.size());
                std::size_t entries{};
                std::size_t blocks{};
                for (const auto& item : classes)
                {
                    const bool is_invalid_class = item.entries == 0U || item.blocks == 0U ||
                        item.entries > capacity - entries || item.blocks > (capacity - entries) / item.entries ||
                        item.entries > (std::numeric_limits<std::size_t>::max)() / sizeof(Value);
                    if (is_invalid_class)
                    {
                        valid_ = false;
                        return;
                    }
                    const auto block_bytes = item.entries * sizeof(Value);
                    if (item.blocks > (std::numeric_limits<std::size_t>::max)() / block_bytes)
                    {
                        valid_ = false;
                        return;
                    }
                    plans.push_back({block_bytes, alignof(Value), block_bytes * item.blocks, 1U});
                    entries += item.entries * item.blocks;
                    blocks += item.blocks;
                }
                auto created = detail::BoundedClassStorage::create(plans, byte_budget, blocks);
                valid_ = static_cast<bool>(created);
                if (created)
                    storage_ = std::move(*created);
            }

            [[nodiscard]] detail::BoundedClassStorage::ClassHandle select(std::size_t count) noexcept
            {
                const bool is_valid_count = count <= capacity_ &&
                    count <= (std::numeric_limits<std::size_t>::max)() / sizeof(Value);
                return is_valid_count ? storage_.select(count * sizeof(Value), alignof(Value)) :
                    detail::BoundedClassStorage::ClassHandle{};
            }

            [[nodiscard]] PreparedSpan allocate(detail::BoundedClassStorage::ClassHandle layout,
                std::size_t count) noexcept
            {
                if (count == 0U)
                    return {{}, 0U};
                if (count > capacity_ - active_ || count > (std::numeric_limits<std::size_t>::max)() / sizeof(Value))
                    return {};
                const auto allocation = storage_.acquire(layout, count * sizeof(Value));
                if (!allocation)
                    return {};
                auto* values = static_cast<Value*>(allocation->data);
                for (std::size_t index{}; index < count; ++index)
                    std::construct_at(values + index);
                active_ += count;
                high_water_ = (std::max)(high_water_, active_);
                return {*allocation, static_cast<std::uint32_t>(count)};
            }

            void release(PreparedSpan& span) noexcept
            {
                if (span.block)
                {
                    auto* values = static_cast<Value*>(span.block.data);
                    for (std::size_t index{}; index < span.count; ++index)
                        std::destroy_at(values + index);
                    if (!storage_.release(span.block))
                        std::terminate();
                    active_ -= span.count;
                }
                span = {};
            }

            [[nodiscard]] Value* at(const PreparedSpan& span, std::size_t local_slot) noexcept
            {
                return span.valid() && local_slot < span.count ?
                    static_cast<Value*>(span.block.data) + local_slot : nullptr;
            }
            [[nodiscard]] const Value* at(const PreparedSpan& span, std::size_t local_slot) const noexcept
            {
                return const_cast<PreparedBlockStorage*>(this)->at(span, local_slot);
            }
            [[nodiscard]] Stats stats() const noexcept
            {
                const auto stats = storage_.stats();
                return {active_, high_water_, stats.arena_bytes + stats.metadata_bytes,
                    stats.acquire_steps, stats.release_steps};
            }
            [[nodiscard]] bool valid() const noexcept { return valid_; }

        private:
            detail::BoundedClassStorage storage_;
            std::size_t capacity_{};
            std::size_t active_{};
            std::size_t high_water_{};
            bool valid_{true};
        };

        struct LuaContinuation final
        {
            Impl* owner{};
            Instance* instance{};
            PreparedCall* call{};
            lua_State* thread{};
            ScriptAwaitableId waiting_on;
            std::uint32_t pending_ordinal{};
            EPendingOperation pending_operation{EPendingOperation::NONE};
            std::int32_t failure_status{};
            bool active{};
            std::uint64_t generation{};
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
            ExecutionScope(Impl& owner, ExecutionFrame frame) noexcept
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
            Impl* owner_{};
            ExecutionFrame frame_;
            bool active_{};
        };

        Impl(
            LuaScriptBackendConfig config
        )
            : engine([&config] {
                auto vm = config.vm;
                vm.track_allocations |= config.track_vm_allocations;
                return vm;
              }()),
              main_thread(engine.state()),
              instance_capacity(config.instance_capacity),
              prepared_call_capacity(config.prepared_call_capacity),
              continuation_capacity(config.continuation_capacity),
              execution_depth_capacity(config.execution_depth_capacity),
              prepared_abilities(config.prepared_ability_capacity, config.prepared_ability_blocks,
                  config.prepared_ability_storage_bytes),
              prepared_events(config.prepared_event_capacity, config.prepared_event_blocks,
                  config.prepared_event_storage_bytes)
        {
            if (!prepared_abilities.valid() || !prepared_events.valid())
                return;
            if (!lux::script::lua::detail::configureLuaVm(
                    main_thread,
                    runtime_info
                ))
            {
                return;
            }
            if (!lux::script::lua::detail::LuaValueAccess::initialize(main_thread)) return;
            for (const auto& value : config.values)
                if (value.prepare != nullptr && !value.prepare(main_thread)) return;
            for (const auto& ability : config.abilities)
                for (const auto& method : ability.methods)
                {
                    for (const auto& value : method.parameters)
                        if (value.prepare != nullptr && !value.prepare(main_thread)) return;
                    for (const auto& value : method.results)
                        if (value.prepare != nullptr && !value.prepare(main_thread)) return;
                }
            prototypes.reserve(config.instance_capacity);
            latest_prototypes.reserve(config.instance_capacity);
            components.assign(
                config.components.begin(),
                config.components.end()
            );
            component_index.reserve(components.size());
            for (std::size_t index{}; index < components.size(); ++index)
                component_index.emplace(components[index].name, index);
            value_operations.assign(
                config.values.begin(),
                config.values.end()
            );
            value_operation_index.reserve(value_operations.size());
            for (std::size_t index{}; index < value_operations.size(); ++index)
            {
                value_operation_index.emplace(
                    value_operations[index].semantic_type,
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
            function_bindings.resize(prepared_call_capacity);
            free_function_bindings.reserve(prepared_call_capacity);
            for (std::size_t index = prepared_call_capacity; index > 0U; --index)
                free_function_bindings.push_back(index - 1U);
            function_index.reserve(prepared_call_capacity);
            prepared_calls.resize(prepared_call_capacity);
            free_prepared_calls.reserve(prepared_call_capacity);
            for (std::size_t index = prepared_call_capacity; index > 0U; --index)
                free_prepared_calls.push_back(index - 1U);
            continuations.resize(continuation_capacity);
            free_continuations.reserve(continuation_capacity);
            for (std::size_t index = continuation_capacity; index > 0U; --index)
                free_continuations.push_back(index - 1U);
            if (!lua_checkstack(main_thread, 3)) return;
            lua_pushcfunction(main_thread, &Impl::createRoots);
            lua_pushlightuserdata(main_thread, this);
            if (lua_pcall(main_thread, 1, 0, 0) != LUA_OK)
            {
                lua_pop(main_thread, 1);
                return;
            }
            vm_configured = true;
        }

        static int createRoots(lua_State* vm)
        {
            auto* owner = static_cast<Impl*>(lua_touserdata(vm, 1));
            if (owner->continuation_capacity != 0U)
            {
                lua_createtable(vm, static_cast<int>(owner->continuation_capacity), 0);
                for (std::size_t index{}; index < owner->continuation_capacity; ++index)
                {
                    lua_pushboolean(vm, false);
                    lua_rawseti(vm, -2, static_cast<lua_Integer>(index + 1U));
                }
                owner->thread_roots_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            }
            lua_pushcfunction(vm, &Impl::traceback);
            owner->traceback_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            return 0;
        }

        void clearThreadRoot(std::size_t slot) noexcept
        {
            lua_rawgeti(main_thread, LUA_REGISTRYINDEX, thread_roots_ref);
            lua_pushboolean(main_thread, false);
            lua_rawseti(main_thread, -2, static_cast<lua_Integer>(slot + 1U));
            lua_pop(main_thread, 1);
        }

        ~Impl()
        {
            if (!main_thread) return;
            // The root table owns all remaining VM references and is released once.
            if (thread_roots_ref != LUA_NOREF) luaL_unref(main_thread, LUA_REGISTRYINDEX, thread_roots_ref);
            for (const auto& [asset, prototype] : prototypes)
            {
                static_cast<void>(asset);
                if (prototype.table_ref != LUA_NOREF) luaL_unref(main_thread, LUA_REGISTRYINDEX, prototype.table_ref);
                if (prototype.environment_ref != LUA_NOREF)
                    luaL_unref(main_thread, LUA_REGISTRYINDEX, prototype.environment_ref);
            }
            for (const auto& function : function_bindings)
                if (function.function_ref != LUA_NOREF)
                    luaL_unref(main_thread, LUA_REGISTRYINDEX, function.function_ref);
            if (traceback_ref != LUA_NOREF) luaL_unref(main_thread, LUA_REGISTRYINDEX, traceback_ref);
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

        // Only C++ allocation here. The subsequent protected Lua render reads this
        // frozen layout.
        [[nodiscard]] bool prepareArtifactLayout(
            Prototype& prototype, const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            try
            {
                for (const auto& requirement : artifact.description().api_requirements)
                {
                    std::size_t ordinal{};
                    while (ordinal < ability_methods.size())
                    {
                        const auto* ability = ability_methods[ordinal].ability;
                        if (ability->id.hash() == requirement.contract.hash() &&
                            ability->id.name() == requirement.contract.name() &&
                            ability->schema_hash == requirement.expected_schema_hash) break;
                        ++ordinal;
                    }
                    if (ordinal == ability_methods.size()) return false;
                    const auto* ability = ability_methods[ordinal].ability;
                    while (ordinal < ability_methods.size() && ability_methods[ordinal].ability == ability)
                        prototype.ability_ordinals.push_back(static_cast<std::uint32_t>(ordinal++));
                }
                for (const auto& requirement : artifact.description().event_requirements)
                {
                    const auto found = std::ranges::find(event_sources, requirement);
                    if (found == event_sources.end()) return false;
                    prototype.event_ordinals.push_back(static_cast<std::uint32_t>(found - event_sources.begin()));
                }
                prototype.ability_class = prepared_abilities.select(prototype.ability_ordinals.size());
                prototype.event_class = prepared_events.select(prototype.event_ordinals.size());
                return true;
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }
        }

        void appendArtifactAbilities(const Prototype& prototype, int lux_index) noexcept
        {
            std::size_t local_slot{};
            while (local_slot < prototype.ability_ordinals.size())
            {
                const auto* ability = ability_methods[prototype.ability_ordinals[local_slot]].ability;
                lua_createtable(main_thread, 0, static_cast<int>(ability->methods.size()));
                const auto ability_index = lua_gettop(main_thread);
                while (local_slot < prototype.ability_ordinals.size())
                {
                    const auto& entry = ability_methods[prototype.ability_ordinals[local_slot]];
                    if (entry.ability != ability) break;
                    lua_pushlstring(main_thread, entry.method->name.data(), entry.method->name.size());
                    lua_pushlightuserdata(main_thread, this);
                    lua_pushinteger(main_thread, static_cast<lua_Integer>(local_slot));
                    lua_pushlightuserdata(main_thread, this);
                    lua_rawget(main_thread, lux_index);
                    pushPrimitive(entry.entry);
                    lua_rawset(main_thread, ability_index);
                    ++local_slot;
                }
                lua_pushlstring(main_thread, ability->name.data(), ability->name.size());
                lua_pushvalue(main_thread, ability_index);
                lua_rawset(main_thread, lux_index);
                lua_pop(main_thread, 1);
            }
        }

        // All four upvalues are rooted before the C closure is published.
        void pushPrimitive(LuxLuaTypedWorker worker)
        {
            auto* storage = static_cast<LuxLuaTypedWorker*>(lua_newuserdatauv(main_thread, sizeof(worker), 0));
            *storage = worker;
            lua_pushcclosure(main_thread, &luxLuaBoundaryEntry, 4);
        }

        void appendArtifactEvents(
            Prototype& prototype,
            const lux::script::ScriptArtifact& artifact,
            int lux_index
        ) noexcept
        {
            if (artifact.description().event_requirements.empty()) return;
            std::size_t groups{};
            std::string_view previous;
            for (const auto& source : artifact.description().event_requirements)
            {
                if (source.system_name == previous) continue;
                ++groups;
                previous = source.system_name;
            }
            lua_createtable(main_thread, 0, static_cast<int>(groups));
            const auto event_index = lua_gettop(main_thread);
            std::string_view active_system;
            int system_index{};
            std::size_t local_slot{};
            for (const auto& requirement : artifact.description().event_requirements)
            {
                if (active_system != requirement.system_name)
                {
                    if (system_index != 0)
                    {
                        lua_pushlstring(main_thread, active_system.data(), active_system.size());
                        lua_pushvalue(main_thread, system_index);
                        lua_settable(main_thread, event_index);
                        lua_remove(main_thread, system_index);
                    }
                    active_system = requirement.system_name;
                    lua_newtable(main_thread);
                    system_index = lua_gettop(main_thread);
                }
                lua_pushlstring(main_thread, requirement.event_name.data(), requirement.event_name.size());
                lua_pushlightuserdata(main_thread, this);
                lua_pushinteger(main_thread, static_cast<lua_Integer>(local_slot));
                lua_pushlightuserdata(main_thread, this);
                lua_rawget(main_thread, lux_index);
                pushPrimitive(&Impl::invokeEventWait);
                lua_settable(main_thread, system_index);
                ++local_slot;
            }
            if (system_index != 0)
            {
                lua_pushlstring(main_thread, active_system.data(), active_system.size());
                lua_pushvalue(main_thread, system_index);
                lua_settable(main_thread, event_index);
                lua_remove(main_thread, system_index);
            }
            lua_setfield(main_thread, lux_index, "Event");
        }

        void pushArtifactEnvironment(
            Prototype& prototype,
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            lua_createtable(main_thread, 0, 1);
            const auto environment_index = lua_gettop(main_thread);
            lua_createtable(main_thread, 0, 1);
            lua_pushglobaltable(main_thread);
            lua_setfield(main_thread, -2, "__index");
            lua_setmetatable(main_thread, environment_index);
            lua_createtable(main_thread, 0, static_cast<int>(artifact.description().api_requirements.size() + 2U));
            const auto lux_index = lua_gettop(main_thread);
            // A full userdata is retained by the environment and every closure. An old
            // reachable closure therefore prevents reuse of its layout identity,
            // independently of C++ prototype storage.
            lua_pushlightuserdata(main_thread, this);
            prototype.layout_token = lua_newuserdata(main_thread, 1U);
            lua_rawset(main_thread, lux_index);
            appendArtifactAbilities(prototype, lux_index);
            appendArtifactEvents(prototype, artifact, lux_index);
            lua_setfield(main_thread, environment_index, "lux");
        }

        static int traceback(lua_State* state)
        {
            const char* message = lua_tostring(state, 1);
            luaL_traceback(state, state, message ? message : "script error", 1);
            return 1;
        }

        // No owning C++ objects may be constructed in callbacks passed here.
        [[nodiscard]] int runCold(lua_CFunction entry, void* request) noexcept
        {
            if (!lua_checkstack(main_thread, 3)) return LUA_ERRMEM;
            const auto base = lua_gettop(main_thread);
            lua_pushcfunction(main_thread, entry);
            lua_pushlightuserdata(main_thread, request);
            const auto status = lua_pcall(main_thread, 1, 0, 0);
            lua_settop(main_thread, base);
            return status;
        }

        struct PrototypeRequest final
        {
            Impl* owner;
            Prototype* prototype;
            const lux::script::ScriptArtifact* artifact;
            const lux::rdesc::LuaSourceScript* body;
            bool complete{};
        };

        static int loadPrototype(lua_State* vm)
        {
            auto& request = *static_cast<PrototypeRequest*>(lua_touserdata(vm, 1));
            auto& prototype = *request.prototype;
            const auto& artifact = *request.artifact;
            request.owner->pushArtifactEnvironment(prototype, artifact);
            const auto environment_index = lua_gettop(vm);
            if (luaL_loadbufferx(vm, reinterpret_cast<const char*>(artifact.payload().data()),
                    artifact.payload().size(), artifact.description().module_name.c_str(), "t") != LUA_OK)
                return lua_error(vm);
            if (!lux::script::lua::detail::setLuaChunkEnvironment(vm, -1, environment_index)) return 0;
            lua_call(vm, 0, 1);
            if (!lua_istable(vm, -1))
            {
                lua_pop(vm, 1);
                lua_getfield(vm, environment_index, request.body->entry.c_str());
            }
            if (!lua_istable(vm, -1)) return 0;
            prototype.table_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            lua_pushvalue(vm, environment_index);
            prototype.environment_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            request.complete = true;
            return 0;
        }

        void releasePrototype(const Prototype& prototype) noexcept
        {
            if (prototype.table_ref != LUA_NOREF) luaL_unref(main_thread, LUA_REGISTRYINDEX, prototype.table_ref);
            if (prototype.environment_ref != LUA_NOREF)
                luaL_unref(main_thread, LUA_REGISTRYINDEX, prototype.environment_ref);
        }

        void collectPrototype(Prototype& prototype) noexcept
        {
            if (!prototype.superseded || prototype.instance_refs != 0U) return;
            for (const auto index : prototype.function_slots)
            {
                auto& binding = function_bindings[index];
                function_index.erase(FunctionKey{&prototype, binding.signature.symbol_id});
                luaL_unref(main_thread, LUA_REGISTRYINDEX, binding.function_ref);
                binding = {};
                free_function_bindings.push_back(index);
            }
            releasePrototype(prototype);
            prototypes.erase(PrototypeKey{prototype.asset, prototype.content});
        }

        [[nodiscard]] Prototype* prototypeFor(
            const ScriptInstanceCreateContext& context, const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            const auto content = artifact.contentIdentity();
            // Content is immutable; AssetId remains the observable closure publication
            // domain.
            const PrototypeKey key{context.asset, content};
            const auto found = prototypes.find(key);
            if (found != prototypes.end()) return std::addressof(found->second);
            if (content.isNull() || artifact.payload().empty()) return nullptr;
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(std::addressof(artifact.description().body));
            if (!body) return nullptr;
            auto latest = latest_prototypes.find(context.asset);
            Prototype* evicted{};
            if (latest == latest_prototypes.end() && latest_prototypes.size() >= instance_capacity)
            {
                // A new publication domain may replace an idle cache entry. Live
                // instances retain their roots.
                for (const auto& [asset, cached] : latest_prototypes)
                    if (cached->instance_refs == 0U)
                    {
                        evicted = cached;
                        break;
                    }
                if (evicted == nullptr) return nullptr;
            }
            Prototype prototype;
            prototype.content = content;
            prototype.asset = context.asset;
            try
            {
                prototype.function_slots.reserve(artifact.description().exports.size());
            }
            catch (const std::bad_alloc&)
            {
                return nullptr;
            }
            if (!prepareArtifactLayout(prototype, artifact)) return nullptr;
            PrototypeRequest request{this, &prototype, &artifact, body};
            if (runCold(&loadPrototype, &request) != LUA_OK || !request.complete)
            {
                releasePrototype(prototype);
                return nullptr;
            }
            try
            {
                const auto inserted = prototypes.emplace(key, std::move(prototype));
                if (inserted.second)
                {
                    auto* current = std::addressof(inserted.first->second);
                    if (latest == latest_prototypes.end())
                    {
                        try
                        {
                            latest_prototypes.emplace(context.asset, current);
                            if (evicted != nullptr)
                            {
                                latest_prototypes.erase(evicted->asset);
                                evicted->superseded = true;
                                collectPrototype(*evicted);
                            }
                        }
                        catch (const std::bad_alloc&)
                        {
                            releasePrototype(*current);
                            prototypes.erase(inserted.first);
                            return nullptr;
                        }
                    }
                    else
                    {
                        auto* previous = latest->second;
                        latest->second = current;
                        previous->superseded = true;
                        collectPrototype(*previous);
                    }
                    return current;
                }
            }
            catch (const std::bad_alloc&)
            {
            }
            releasePrototype(prototype);
            return nullptr;
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
            auto span = prepared_abilities.allocate(
                instance.prototype->ability_class, instance.prototype->ability_ordinals.size());
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
                    projected.method,
                    capability->local_async.resolve(method->method, capability->context, capability->dispatch)
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
            auto span = prepared_events.allocate(instance.prototype->event_class, requirements.size());
            if (!span.valid())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            instance.prepared_events = span;
            for (std::size_t local_slot{}; local_slot < requirements.size(); ++local_slot)
            {
                const auto& requirement = requirements[local_slot];
                const auto resolved = std::ranges::find_if(context.events, [&](const auto& entry) noexcept {
                    return entry.source != nullptr && *entry.source == requirement;
                });
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
                    eventOperation(event_sources[ordinal].payload.type_id),
                    std::addressof(event_sources[ordinal]), resolved->admission
                };
            }
            return EScriptBackendResult::SUCCESS;
        }

        [[nodiscard]] const lux::script::lua::LuaValueOperation* eventOperation(std::uint64_t type) const noexcept
        {
            const auto found = value_operation_index.find(type);
            return found == value_operation_index.end() ? nullptr : &value_operations[found->second];
        }

        [[nodiscard]] const lux::script::lua::LuaValueOperation* recordOperation(
            const lux::rdesc::ScriptValueType& type
        ) const noexcept
        {
            if (type.pass != lux::semantic::EValuePass::CONST_REF)
                return nullptr;
            const auto found = value_operation_index.find(type.type_id);
            if (found == value_operation_index.end())
                return nullptr;
            const auto& marshaller = value_operations[found->second];
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
            return detail::checkedLuaNumber(value, result);
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

        static int destroySelf(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const bool valid = handle && handle->alive && handle->host &&
                handle->host->captureInvocation().valid();
            lua_pushboolean(state, valid && handle->host->command(EScriptHostCommand::DESTROY_ENTITY));
            return 1;
        }

        static int createSelf(lua_State* vm)
        {
            auto* instance = static_cast<Instance*>(lua_touserdata(vm, 1));
            lua_createtable(vm, 0, instance->entity_scope ? 4 : 0);
            const auto instance_index = lua_gettop(vm);
            lua_rawgeti(vm, LUA_REGISTRYINDEX, instance->prototype->table_ref);
            const auto prototype_index = lua_gettop(vm);
            lua_pushnil(vm);
            while (lua_next(vm, prototype_index) != 0)
            {
                lua_pushvalue(vm, -2);
                lua_pushvalue(vm, -2);
                lua_settable(vm, instance_index);
                lua_pop(vm, 1);
            }
            lua_pop(vm, 1);
            if (instance->entity_scope)
            {
                auto* handle = static_cast<HostHandle*>(
                    lua_newuserdata(vm, sizeof(HostHandle)));
                *handle = HostHandle{
                    instance->owner,
                    instance->behavior,
                    true};
                instance->host_handle = handle;
                const auto handle_index = lua_gettop(vm);
                lua_pushvalue(vm, handle_index);
                lua_pushcclosure(vm, &Impl::hasComponent, 1);
                lua_setfield(vm, instance_index, "has_component");
                lua_pushvalue(vm, handle_index);
                lua_pushcclosure(vm, &Impl::getComponent, 1);
                lua_setfield(vm, instance_index, "get_component");
                lua_pushvalue(vm, handle_index);
                lua_pushcclosure(vm, &Impl::patchComponent, 1);
                lua_setfield(vm, instance_index, "patch_component");
                lua_pushvalue(vm, handle_index);
                lua_pushcclosure(vm, &Impl::destroySelf, 1);
                lua_setfield(vm, instance_index, "destroy");
                lua_pop(vm, 1);
            }
            const auto table_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            instance->table_ref = table_ref;
            return 0;
        }

        struct FunctionRequest final
        {
            int table_ref;
            const char* name;
            int function_ref{LUA_NOREF};
        };

        static int rootFunction(lua_State* vm)
        {
            auto& request = *static_cast<FunctionRequest*>(lua_touserdata(vm, 1));
            lua_rawgeti(vm, LUA_REGISTRYINDEX, request.table_ref);
            lua_getfield(vm, -1, request.name);
            if (lua_isfunction(vm, -1)) request.function_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
            return 0;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<Impl*>(opaque);
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
                context.behavior, std::addressof(self),
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
            instance->behavior = context.behavior;

            if (self.runCold(&createSelf, instance) != LUA_OK)
            {
                // Nothing was published to user code; the failed Lua stack owns any
                // partial userdata.
                self.prepared_events.release(instance->prepared_events);
                self.prepared_abilities.release(instance->prepared_abilities);
                *instance = {};
                self.free_instances.push_back(instance_slot);
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
            ++prototype->instance_refs;
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
            auto& self = *static_cast<Impl*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;

            const auto stack_values = (std::max)(function.args.size(), function.returns.size());
            if (stack_values > static_cast<std::size_t>((std::numeric_limits<int>::max)()) - 8U)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            if (!lua_checkstack(self.main_thread, static_cast<int>(stack_values + 8U)))
                return EScriptBackendResult::ALLOCATION_FAILURE;

            const FunctionKey key{instance->prototype, function.symbol_id};
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
                if (self.free_function_bindings.empty())
                    return EScriptBackendResult::CAPACITY_EXCEEDED;
                std::vector<const lux::script::lua::LuaValueOperation*> argument_operations;
                try
                {
                    argument_operations.reserve(function.args.size());
                    for (const auto& argument : function.args)
                    {
                        const auto* record = self.recordOperation(argument);
                        if (!self.supportedType(argument) && !record)
                            return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                        argument_operations.push_back(record);
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

                FunctionRequest request{instance->prototype->table_ref, function.name.c_str()};
                const auto status = self.runCold(&rootFunction, &request);
                if (status != LUA_OK || request.function_ref == LUA_NOREF)
                    return status == LUA_ERRMEM ? EScriptBackendResult::ALLOCATION_FAILURE :
                        EScriptBackendResult::CONSTRUCTION_FAILURE;
                const auto function_ref = request.function_ref;
                try
                {
                    LuaFunctionBinding binding{function, function_ref, std::move(argument_operations)};
                    const auto binding_index = self.free_function_bindings.back();
                    instance->prototype->function_slots.push_back(binding_index);
                    try
                    {
                        if (!self.function_index.emplace(key, binding_index).second)
                        {
                            instance->prototype->function_slots.pop_back();
                            luaL_unref(self.main_thread, LUA_REGISTRYINDEX, function_ref);
                            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                        }
                    }
                    catch (const std::bad_alloc&)
                    {
                        instance->prototype->function_slots.pop_back();
                        luaL_unref(self.main_thread, LUA_REGISTRYINDEX, function_ref);
                        return EScriptBackendResult::ALLOCATION_FAILURE;
                    }
                    static_assert(std::is_nothrow_move_assignable_v<LuaFunctionBinding>);
                    self.function_bindings[binding_index] = std::move(binding);
                    self.free_function_bindings.pop_back();
                    function_binding = std::addressof(self.function_bindings[binding_index]);
                }
                catch (const std::bad_alloc&)
                {
                    luaL_unref(self.main_thread, LUA_REGISTRYINDEX, function_ref);
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
            const bool protected_arguments = stack_values + 8U > LUA_MINSTACK ||
                std::any_of(function_binding->argument_operations.begin(), function_binding->argument_operations.end(),
                    [](const auto* operation) noexcept { return operation != nullptr; });
            result = {
                std::addressof(call),
                lux::script::BoundScriptCall{
                    protected_arguments ? &invokeConvertedSync : &invokePreparedSync, std::addressof(call)},
                resumable
                    ? BoundScriptStepCall{std::addressof(call), &invokePreparedStep}
                    : BoundScriptStepCall{}
            };
            return EScriptBackendResult::SUCCESS;
        }

        static bool pushArgument(
            lua_State* state,
            const lux_script_value_slot& value,
            const lux::script::lua::LuaValueOperation* record
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

        [[nodiscard]] static bool pushAbilityResult(
            lua_State* state,
            const lux::script::ScriptAbilityValueDescription& description,
            const void* value
        ) noexcept
        {
            return pushComponentValue(state, description.abi_kind, value);
        }

        static LuxLuaBoundaryOutcome abilityFailure(
            lua_State* state,
            LuaContinuation* continuation,
            std::int32_t status,
            const char* message
        ) noexcept
        {
            if (continuation != nullptr)
                continuation->failure_status = status;
            const auto count = lux::script::lua::detail::LuaValueAccess::failure(state, message);
            return {LUX_LUA_BOUNDARY_ERROR, count, status};
        }

        struct EventWaitAdmission final
        {
            ScriptAwaitableId waiting_on;
            std::int32_t failure{};
        };

        [[nodiscard]] static EventWaitAdmission admitEventWait(
            ScriptStepContext& step,
            ScriptEventAdmissionHandle source
        ) noexcept
        {
            const auto waiting = step.event_waits.wait(source);
            if (!waiting)
            {
                return {
                    {},
                    kEventWaitFailure - static_cast<std::int32_t>(waiting.error())
                };
            }
            return {*waiting, 0};
        }

        static LuxLuaBoundaryOutcome invokeEventWait(lua_State* state) noexcept
        {
            auto* self = static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
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
            const auto admission = admitEventWait(*execution->step, prepared->admission);
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
            return {LUX_LUA_BOUNDARY_SUSPEND, 0, 0};
        }

        struct ConvertedSyncRequest final
        {
            PreparedCall& call;
            Instance& instance;
            const LuaFunctionBinding& function;
            lux_script_call_frame& frame;
            const ScriptBehavior* behavior;
            ScriptInvocationValidity qualification;
            bool bound_authority;
            std::int32_t status{kInvalidCall};
        };

        static int executeConvertedSync(lua_State* vm)
        {
            auto& request = *static_cast<ConvertedSyncRequest*>(lua_touserdata(vm, 1));
            auto& frame = request.frame;
            const auto slots = (std::max)(frame.arg_count, frame.return_count);
            const bool invalid_slots = slots > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) - 8U;
            if (invalid_slots || !lua_checkstack(vm, static_cast<int>(slots + 8U)))
                return 0;
            lua_rawgeti(vm, LUA_REGISTRYINDEX, request.function.function_ref);
            const bool entity_scope = request.instance.entity_scope;
            if (entity_scope) lua_rawgeti(vm, LUA_REGISTRYINDEX, request.instance.table_ref);
            for (std::uint32_t i{}; i < frame.arg_count; ++i)
            {
                const auto* operation = i < request.function.argument_operations.size()
                    ? request.function.argument_operations[i] : nullptr;
                if (!pushArgument(vm, frame.args[i], operation))
                {
                    request.status = kMarshalFailure;
                    return 0;
                }
            }
            const bool same_binding = request.call.active && request.instance.active &&
                request.call.instance == &request.instance && request.call.function == &request.function &&
                request.instance.behavior == request.behavior &&
                (request.behavior != nullptr && request.behavior->hasInvocationAuthority()) == request.bound_authority;
            if (!same_binding || (request.bound_authority && !request.qualification.valid())) return 0;
            lua_call(vm, static_cast<int>(frame.arg_count) + entity_scope, static_cast<int>(frame.return_count));
            for (std::uint32_t i{}; i < frame.return_count; ++i)
            {
                if (!readReturn(vm, 2 + static_cast<int>(i), frame.returns[i]))
                {
                    request.status = -5;
                    return 0;
                }
            }
            // Converter-created to-be-closed values and errors stay inside this C boundary.
            lua_settop(vm, 1);
            request.status = 0;
            return 0;
        }

        static int invokeConvertedSync(void* opaque, lux_script_call_frame* frame) noexcept
        {
            if (!opaque || !frame) return kInvalidCall;
            auto& call = *static_cast<PreparedCall*>(opaque);
            const bool invalid_call = !call.active || !call.instance || !call.function;
            if (invalid_call) return kInvalidCall;
            auto& self = *call.instance->owner;
            const auto* behavior = call.instance->behavior;
            const bool bound_authority = behavior != nullptr && behavior->hasInvocationAuthority();
            const auto qualification = bound_authority ? behavior->captureInvocation() : ScriptInvocationValidity{};
            if (bound_authority && !qualification.valid()) return kInvalidCall;
            ExecutionScope execution{self, {self.main_thread, call.instance, nullptr, nullptr, nullptr}};
            if (!execution) return kExecutionDepthCapacity;
            auto* vm = self.main_thread;
            if (!lua_checkstack(vm, 3)) return kLuaFailure;
            const auto base = lua_gettop(vm);
            ConvertedSyncRequest request{call, *call.instance, *call.function, *frame, behavior,
                qualification, bound_authority};
            lua_pushcfunction(vm, &traceback);
            lua_pushcfunction(vm, &executeConvertedSync);
            lua_pushlightuserdata(vm, &request);
            const auto result = lua_pcall(vm, 1, 0, base + 1);
            lua_settop(vm, base);
            return result == LUA_OK ? request.status : kLuaFailure;
        }

        static int invokePreparedSync(void* invocation_context, lux_script_call_frame* frame) noexcept
        {
            if (!frame || !invocation_context)
                return -1;
            auto& call = *static_cast<PreparedCall*>(invocation_context);
            if (!call.active || !call.instance || !call.function)
                return -1;
            auto& self = *call.instance->owner;
            lua_rawgeti(self.main_thread, LUA_REGISTRYINDEX, self.traceback_ref);
            const auto error_index = lua_gettop(self.main_thread);
            lua_rawgeti(self.main_thread, LUA_REGISTRYINDEX, call.function->function_ref);
            std::uint32_t argument_count{};
            if (call.instance->entity_scope)
            {
                lua_rawgeti(
                    self.main_thread,
                    LUA_REGISTRYINDEX,
                    call.instance->table_ref);
                ++argument_count;
            }
            for (std::uint32_t index{}; index < frame->arg_count; ++index)
            {
                const auto* record = index < call.function->argument_operations.size()
                    ? call.function->argument_operations[index]
                    : nullptr;
                if (!pushArgument(self.main_thread, frame->args[index], record))
                {
                    lua_settop(self.main_thread, error_index - 1);
                    return -3;
                }
                ++argument_count;
            }
            ExecutionScope execution{
                self,
                {self.main_thread, call.instance, nullptr, nullptr, nullptr}
            };
            if (!execution)
            {
                lua_settop(self.main_thread, error_index - 1);
                return kExecutionDepthCapacity;
            }
            if (lua_pcall(
                    self.main_thread,
                    static_cast<int>(argument_count),
                    static_cast<int>(frame->return_count),
                    error_index) != LUA_OK)
            {
                lua_settop(self.main_thread, error_index - 1);
                return kLuaFailure;
            }
            for (std::uint32_t index{}; index < frame->return_count; ++index)
            {
                const auto stack_index =
                    error_index + 1 + static_cast<int>(index);
                if (!readReturn(
                        self.main_thread,
                        stack_index,
                        frame->returns[index]))
                {
                    lua_settop(self.main_thread, error_index - 1);
                    return -5;
                }
            }
            lua_settop(self.main_thread, error_index - 1);
            return 0;
        }

        template <class T>
        static bool pushSyncScalar(lua_State* vm, const void* data, const lux::script::lua::LuaValueOperation*) noexcept
        {
            if constexpr (std::is_same_v<T, bool>)
                lua_pushboolean(vm, *static_cast<const T*>(data));
            else
                lua_pushnumber(vm, static_cast<lua_Number>(*static_cast<const T*>(data)));
            return true;
        }

        static bool pushSyncRecord(lua_State* vm, const void* data,
                                   const lux::script::lua::LuaValueOperation* operation) noexcept
        {
            return operation->push(vm, data);
        }

        static SyncArgument syncArgument(const lux::rdesc::ScriptValueType& type,
                                         const lux::script::lua::LuaValueOperation* operation) noexcept
        {
            if (operation)
                return {&pushSyncRecord, operation};
            switch (type.abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                return {&pushSyncScalar<bool>, nullptr};
            case LUX_SCRIPT_VK_INT32:
                return {&pushSyncScalar<std::int32_t>, nullptr};
            case LUX_SCRIPT_VK_UINT32:
                return {&pushSyncScalar<std::uint32_t>, nullptr};
            case LUX_SCRIPT_VK_FLOAT:
                return {&pushSyncScalar<float>, nullptr};
            case LUX_SCRIPT_VK_DOUBLE:
                return {&pushSyncScalar<double>, nullptr};
            default:
                return {};
            }
        }

        template <class T> static bool readSyncResult(lua_State* vm, void* output) noexcept
        {
            T value;
            if constexpr (std::is_same_v<T, bool>)
            {
                if (lua_type(vm, -1) != LUA_TBOOLEAN)
                    return false;
                value = lua_toboolean(vm, -1) != 0;
            }
            else if (!readStrictNumber(vm, -1, value))
                return false;
            std::memcpy(output, &value, sizeof(value));
            return true;
        }

        // Cold-only. The normal prepared function shares this immutable plan across
        // instances.
        static void prepareSyncOperations(LuaFunctionBinding& function)
        {
            if (function.sync_prepared)
                return;
            std::vector<SyncArgument> arguments;
            arguments.reserve(function.signature.args.size());
            bool scalar = true;
            for (std::size_t i{}; i < function.signature.args.size(); ++i)
            {
                const auto* operation = function.argument_operations[i];
                scalar = scalar && operation == nullptr;
                arguments.push_back(syncArgument(function.signature.args[i], operation));
            }
            function.sync_arguments = std::move(arguments);
            function.scalar_sync = scalar;
            function.sync_prepared = true;
            if (function.signature.returns.empty())
                return;
            switch (function.signature.returns[0].abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                function.read_sync_result = &readSyncResult<bool>;
                break;
            case LUX_SCRIPT_VK_INT32:
                function.read_sync_result = &readSyncResult<std::int32_t>;
                break;
            case LUX_SCRIPT_VK_UINT32:
                function.read_sync_result = &readSyncResult<std::uint32_t>;
                break;
            case LUX_SCRIPT_VK_FLOAT:
                function.read_sync_result = &readSyncResult<float>;
                break;
            case LUX_SCRIPT_VK_DOUBLE:
                function.read_sync_result = &readSyncResult<double>;
                break;
            default:
                break;
            }
        }

        struct SyncInvocationRequest final
        {
            PreparedCall* call;
            const void* const* arguments;
            void* output;
            const ScriptInvocationValidity* qualification;
            std::int32_t status{kInvalidCall};
        };

        // Record/custom conversion can allocate or reenter. The protected C callback
        // contains only trivial borrows; each converter retains its own typed
        // construction/cleanup frame.
        template <bool EntityScope, bool HasResult> static int executeSyncStep(lua_State* vm)
        {
            auto& request = *static_cast<SyncInvocationRequest*>(lua_touserdata(vm, 1));
            const auto& call = *request.call;
            const auto& operations = call.function->sync_arguments;
            if (!lua_checkstack(vm, static_cast<int>(operations.size() + 9U)))
            {
                request.status = kLuaFailure;
                return 0;
            }
            lua_rawgeti(vm, LUA_REGISTRYINDEX, call.function->function_ref);
            if constexpr (EntityScope)
                lua_rawgeti(vm, LUA_REGISTRYINDEX, call.instance->table_ref);
            for (std::size_t i{}; i < operations.size(); ++i)
            {
                const auto& operation = operations[i];
                if (!operation.push(vm, request.arguments[i], operation.operation))
                {
                    request.status = -3;
                    return 0;
                }
            }
            if (!request.qualification->valid())
                return 0;
            lua_call(vm, static_cast<int>(operations.size()) + EntityScope, HasResult ? 1 : 0);
            if constexpr (HasResult)
            {
                if (!call.function->read_sync_result(vm, request.output))
                {
                    request.status = kInvalidResult;
                    return 0;
                }
            }
            // Preserve converter-created to-be-closed cleanup before publishing
            // success.
            lua_settop(vm, 1);
            request.status = 0;
            return 0;
        }

        template <bool EntityScope, bool HasResult, bool ScalarArguments, class Scalar = void>

        static int invokeSyncStep(void* opaque, const void* const* arguments, void* output,
                                  const ScriptInvocationValidity& qualification) noexcept
        {
            // The typed bridge already checked the publication, shape and original
            // authority.
            const auto& call = *static_cast<PreparedCall*>(opaque);
            auto& self = *call.instance->owner;
            auto* vm = self.main_thread;
            ExecutionScope execution{self, {vm, call.instance, nullptr, nullptr, nullptr}};
            if (!execution) return kExecutionDepthCapacity;
            const auto& operations = call.function->sync_arguments;
            const auto argument_count = [&]() {
                if constexpr (std::is_same_v<Scalar, std::nullptr_t>) return std::size_t{};
                else if constexpr (!std::is_void_v<Scalar>) return std::size_t{1U};
                else return operations.size();
            }();
            constexpr auto protected_slots = ScalarArguments ? 3U + EntityScope : 3U;
            const auto stack_slots = ScalarArguments ? argument_count + protected_slots : protected_slots;
            if (!lua_checkstack(vm, static_cast<int>(stack_slots)))
                return kLuaFailure;
            const auto base = lua_gettop(vm);
            lua_pushcfunction(vm, &traceback);
            if constexpr (ScalarArguments)
            {
                // These APIs neither allocate nor raise with the established stack
                // allowance. Stack growth above can call the allocator: validate again
                // before running Lua.
                lua_rawgeti(vm, LUA_REGISTRYINDEX, call.function->function_ref);
                if constexpr (EntityScope)
                    lua_rawgeti(vm, LUA_REGISTRYINDEX, call.instance->table_ref);
                if constexpr (std::is_same_v<Scalar, std::nullptr_t>) {}
                else if constexpr (!std::is_void_v<Scalar>) pushSyncScalar<Scalar>(vm, arguments[0], nullptr);
                else
                    for (std::size_t i{}; i < argument_count; ++i)
                        operations[i].push(vm, arguments[i], nullptr);
                if (!qualification.valid())
                {
                    lua_settop(vm, base);
                    return kInvalidCall;
                }
                const auto status =
                    lua_pcall(vm, static_cast<int>(argument_count) + EntityScope, HasResult ? 1 : 0, base + 1);
                bool valid_result = true;
                if constexpr (HasResult)
                    if (status == LUA_OK)
                        valid_result = call.function->read_sync_result(vm, output);
                lua_settop(vm, base);
                if (status != LUA_OK)
                    return kLuaFailure;
                return valid_result ? 0 : kInvalidResult;
            }
            else
            {
                SyncInvocationRequest request{const_cast<PreparedCall*>(&call), arguments, output, &qualification};
                lua_pushcfunction(vm, (&executeSyncStep<EntityScope, HasResult>));
                lua_pushlightuserdata(vm, &request);
                const auto status = lua_pcall(vm, 1, 0, base + 1);
                lua_settop(vm, base);
                return status != LUA_OK ? kLuaFailure : request.status;
            }
            // The bridge checks the original qualification after this scope/cleanup
            // finishes, before exposing any scalar result to the task. Backend errors
            // keep their priority.
        }

        [[nodiscard]] lux::cxx::expected<LuaContinuation*, std::int32_t> acquireContinuation(
            Instance& instance,
            PreparedCall& call,
            const lux_script_call_frame& frame,
            std::uint32_t plain_count
        ) noexcept
        {
            if (free_continuations.empty())
                return lux::cxx::unexpected(kContinuationCapacity);
            const auto slot = free_continuations.back();
            free_continuations.pop_back();
            ++instance.active_continuations;
            struct Reservation final
            {
                Impl& owner;
                Instance& instance;
                std::size_t slot;
                bool committed{};
                ~Reservation()
                {
                    if (committed) return;
                    --instance.active_continuations;
                    owner.free_continuations.push_back(slot);
                }
            } reservation{*this, instance, slot};
            ThreadCreateRequest request;
            request.roots_ref = thread_roots_ref;
            request.root_slot = slot;
            request.function_ref = call.function->function_ref;
            request.self_ref = instance.entity_scope ? instance.table_ref : LUA_NOREF;
            request.arguments = frame.args;
            request.plain_count = plain_count;
            const auto status = createThreadProtected(request);
            const bool missing_root = request.thread == nullptr || !request.rooted;
            if (status != LUA_OK || missing_root || !request.arguments_valid)
            {
                if (request.rooted) clearThreadRoot(slot);
                return lux::cxx::unexpected(!request.arguments_valid ? kMarshalFailure :
                    status == LUA_ERRMEM ? kLuaAllocationFailure : kLuaFailure);
            }
            auto& continuation = continuations[slot];
            const auto generation = continuation.generation + 1U;
            if (generation == 0U) std::terminate();
            continuation = {
                this,
                std::addressof(instance),
                std::addressof(call),
                request.thread,
                {},
                0U,
                EPendingOperation::NONE,
                0,
                true,
                generation
            };
            reservation.committed = true;
            ++vm_coroutine_creations;
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
            owner->clearThreadRoot(slot);
            if (continuation.instance == nullptr || continuation.instance->active_continuations == 0U)
                std::terminate();
            --continuation.instance->active_continuations;
            const auto generation = continuation.generation;
            continuation = {};
            continuation.generation = generation;
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
                    const bool pushed = prepared->operation &&
                        prepared->operation->push(continuation.thread, packet.value->bytes.data());
                    if (!pushed)
                        return false;
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
            const auto* behavior = continuation.instance->behavior;
            const bool bound_authority = behavior != nullptr && behavior->hasInvocationAuthority();
            const auto qualification = bound_authority ? behavior->captureInvocation() : ScriptInvocationValidity{};
            if (bound_authority && !qualification.valid()) return ScriptStepResult::failed(kInvalidCall);
            const auto* original_instance = continuation.instance;
            const auto* original_call = continuation.call;
            auto* original_thread = continuation.thread;
            const auto original_wait = continuation.waiting_on;
            const auto original_generation = continuation.generation;
            const auto base = lua_gettop(original_thread);
            int argument_count{};
            if (!pushResumeValue(continuation, packet, argument_count))
            {
                lua_settop(original_thread, base);
                return ScriptStepResult::failed(kInvalidResume);
            }
            const bool same_execution = continuation.active && continuation.instance == original_instance &&
                continuation.call == original_call && continuation.thread == original_thread &&
                continuation.waiting_on == original_wait && continuation.generation == original_generation;
            const bool same_binding = same_execution && original_instance->active &&
                original_instance->behavior == behavior &&
                (behavior != nullptr && behavior->hasInvocationAuthority()) == bound_authority;
            if (!same_binding || (bound_authority && !qualification.valid()))
            {
                lua_settop(original_thread, base);
                return ScriptStepResult::failed(kInvalidCall);
            }
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
            const auto* behavior = call.instance->behavior;
            const bool bound_authority = behavior != nullptr && behavior->hasInvocationAuthority();
            const auto qualification = bound_authority ? behavior->captureInvocation() : ScriptInvocationValidity{};
            if (bound_authority && !qualification.valid()) return ScriptStepResult::failed(kInvalidCall);
            std::uint32_t plain_count{};
            while (plain_count < frame.arg_count &&
                (plain_count >= call.function->argument_operations.size() ||
                    call.function->argument_operations[plain_count] == nullptr)) ++plain_count;
            const auto acquired = self.acquireContinuation(*call.instance, call, frame, plain_count);
            if (!acquired) return ScriptStepResult::failed(acquired.error());
            auto* continuation = *acquired;

            std::uint32_t argument_count = plain_count + (call.instance->entity_scope ? 1U : 0U);
            for (std::uint32_t index = plain_count; index < frame.arg_count; ++index)
            {
                const auto* record = index < call.function->argument_operations.size()
                    ? call.function->argument_operations[index]
                    : nullptr;
                if (!pushArgument(continuation->thread, frame.args[index], record))
                {
                    destroyLuaContinuation(*continuation);
                    return ScriptStepResult::failed(kMarshalFailure);
                }
                ++argument_count;
            }
            // Allocation, GC and record conversion can call native code. A bound but
            // revoked invocation is never standalone; retain the original lifetime
            // category and epoch.
            const bool same_binding = call.instance->behavior == behavior &&
                (behavior != nullptr && behavior->hasInvocationAuthority()) == bound_authority;
            const bool still_qualified = same_binding && call.instance->active &&
                (!bound_authority || qualification.valid());
            if (!still_qualified)
            {
                destroyLuaContinuation(*continuation);
                return ScriptStepResult::failed(kInvalidCall);
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
            auto& self = *static_cast<Impl*>(opaque);
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
            auto& self = *static_cast<Impl*>(opaque);
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
                luaL_unref(self.main_thread, LUA_REGISTRYINDEX, instance->table_ref);
            const auto instance_slot = static_cast<std::size_t>(
                instance - self.instances.data()
            );
            self.prepared_abilities.release(instance->prepared_abilities);
            self.prepared_events.release(instance->prepared_events);
            auto* prototype = instance->prototype;
            *instance = {};
            self.free_instances.push_back(instance_slot);
            --prototype->instance_refs;
            self.collectPrototype(*prototype);
        }

        lux::script::lua::ScriptEngine engine;
        lua_State* main_thread{};
        lux::script::lua::LuaRuntimeInfo runtime_info;
        bool vm_configured{};
#if defined(LUX_LUA55_LEAF_YIELD_REVISION)
        static constexpr bool leaf_yield_available = true;
#else
        static constexpr bool leaf_yield_available = false;
#endif
        int traceback_ref{LUA_NOREF};
        int thread_roots_ref{LUA_NOREF};
        std::size_t instance_capacity{};
        std::size_t prepared_call_capacity{};
        std::size_t continuation_capacity{};
        std::size_t execution_depth_capacity{};
        using PrototypeMap = std::unordered_map<PrototypeKey, Prototype, PrototypeKey::Hash>;
        PrototypeMap prototypes;
        std::unordered_map<lux::asset::AssetId, Prototype*> latest_prototypes;
        std::vector<LuaComponentBinding> components;
        std::unordered_map<std::string_view, std::size_t> component_index;
        std::vector<lux::script::lua::LuaValueOperation> value_operations;
        std::unordered_map<std::uint64_t, std::size_t>
            value_operation_index;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
        std::vector<LuaFunctionBinding> function_bindings;
        std::vector<std::size_t> free_function_bindings;
        std::unordered_map<FunctionKey, std::size_t, FunctionKeyHash> function_index;
        std::vector<PreparedCall> prepared_calls;
        std::vector<std::size_t> free_prepared_calls;
        std::vector<AbilityMethod> ability_methods;
        PreparedBlockStorage<PreparedAbility> prepared_abilities;
        std::vector<lux::script::ScriptEventSourceDescription> event_sources;
        PreparedBlockStorage<PreparedEventSource> prepared_events;
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
        auto* owner = static_cast<LuaScriptBackend::Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
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
        result.context = prepared->context;
        result.dispatch = prepared->dispatch;
        result.step = owner->active_execution->step;
        result.local_async = prepared->local_async;
        result.local_slot = static_cast<std::uint32_t>(local_slot);
        result.argument_count = lua_gettop(state);
        result.execution = owner->active_execution;
        result.behavior = instance->behavior;
        result.has_core_authority = result.behavior && result.behavior->hasInvocationAuthority();
        if (result.has_core_authority)
        {
            result.validity = result.behavior->captureInvocation();
            if (!result.validity.valid()) return false;
        }
        else result.validity = {};
        return true;
    }

    bool detail::LuaAbilityProjectionAccess::revalidate(
        lua_State* state, const LuaPreparedAbilityAccess& original
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
        if (owner == nullptr || owner->active_execution != original.execution)
            return false;
        const auto* frame = owner->active_execution;
        if (frame == nullptr || frame->thread != state || frame->instance == nullptr)
            return false;
        const auto* instance = frame->instance;
        const bool same_projection = lua_tointeger(state, lua_upvalueindex(2)) == original.local_slot &&
            instance->prototype != nullptr &&
            lua_touserdata(state, lua_upvalueindex(3)) == instance->prototype->layout_token;
        if (!same_projection || instance->behavior != original.behavior)
            return false;
        // The still-active execution frame pins its immutable prepared layout and
        // provider association. Reuse the ORIGINAL authority capture, including
        // lifecycle privilege and retirement epoch.
        const bool has_authority = original.behavior && original.behavior->hasInvocationAuthority();
        return has_authority == original.has_core_authority &&
            (!has_authority || original.validity.valid());
    }

    LuxLuaBoundaryOutcome detail::LuaAbilityProjectionAccess::fail(
        lua_State* state,
        std::int32_t status,
        const char* message
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* continuation = owner != nullptr && owner->active_execution != nullptr
            ? owner->active_execution->continuation
            : nullptr;
        return LuaScriptBackend::Impl::abilityFailure(state, continuation, status, message);
    }

    LuxLuaBoundaryOutcome detail::LuaAbilityProjectionAccess::suspend(
        lua_State* state,
        ScriptStepResult result,
        std::uint32_t local_slot
    ) noexcept
    {
        auto* owner = static_cast<LuaScriptBackend::Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* execution = owner != nullptr ? owner->active_execution : nullptr;
        if (execution == nullptr || execution->thread != state || execution->continuation == nullptr ||
            result.state != EScriptStepState::SUSPENDED || !result.valid())
        {
            const auto status = result.error.valid() ? result.error.status : -1;
            return LuaScriptBackend::Impl::abilityFailure(
                state,
                execution != nullptr ? execution->continuation : nullptr,
                status,
                "async Script Ability admission failed"
            );
        }
        execution->continuation->waiting_on = result.waiting_on;
        execution->continuation->pending_ordinal = local_slot;
        execution->continuation->pending_operation = LuaScriptBackend::Impl::EPendingOperation::ABILITY;
        return {LUX_LUA_BOUNDARY_SUSPEND, 0, 0};
    }

    LuxLuaBoundaryOutcome detail::LuaAbilityProjectionAccess::succeed(lua_State* state, int results) noexcept
    {
        static_cast<void>(state);
        return {LUX_LUA_BOUNDARY_RETURN, results, 0};
    }

    bool detail::LuaAbilityProjectionAccess::read(lua_State* state, int index, bool& value) noexcept
    {
        if (!lua_isboolean(state, index))
            return false;
        value = lua_toboolean(state, index) != 0;
        return true;
    }

    bool detail::LuaAbilityProjectionAccess::number(lua_State* state, int index, double& value) noexcept
    {
        if (lua_type(state, index) != LUA_TNUMBER) return false;
        value = lua_tonumber(state, index);
        return true;
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
            config.prepared_call_capacity == 0U ||
            config.execution_depth_capacity == 0U || config.ability_catalog_method_capacity == 0U ||
            config.event_catalog_capacity == 0U ||
            config.ability_catalog_method_capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()) - 2U ||
            config.event_catalog_capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()) - 2U;
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
        for (std::size_t index{}; index < config.values.size(); ++index)
        {
            const auto& marshaller = config.values[index];
            const bool power_of_two_alignment = marshaller.alignment != 0U &&
                (marshaller.alignment & (marshaller.alignment - 1U)) == 0U;
            const bool valid_identity = marshaller.semantic_type != 0U &&
                !marshaller.canonical_name.empty() &&
                marshaller.semantic_type == lux::semantic::typeId(
                    marshaller.canonical_name
                );
            if (!valid_identity || marshaller.size == 0U ||
                !power_of_two_alignment || !marshaller.push || !marshaller.writable ||
                marshaller.frame_bytes > 65536 || marshaller.representation == 0 || marshaller.policy == 0)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION
                );
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                const auto& candidate = config.values[previous];
                if (candidate.semantic_type == marshaller.semantic_type ||
                    candidate.canonical_name == marshaller.canonical_name)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            DUPLICATE_VALUE_OPERATION
                    );
                }
            }
        }
        const auto consistent = [](const auto& left, const auto& right) noexcept {
            if (left.policy != right.policy) return false;
            if (left.semantic_type != right.semantic_type) return true;
            return left.canonical_name == right.canonical_name && left.size == right.size &&
                left.alignment == right.alignment && left.representation == right.representation &&
                left.readable == right.readable && left.writable == right.writable;
        };
        const auto each_operation = [&](auto&& visit) noexcept {
            for (const auto& value : config.values) if (!visit(value)) return false;
            for (const auto& ability : config.abilities)
                for (const auto& method : ability.methods)
                {
                    for (const auto& value : method.parameters) if (!visit(value)) return false;
                    for (const auto& value : method.results) if (!visit(value)) return false;
                }
            return true;
        };
        if (!each_operation([&](const auto& left) noexcept {
            return each_operation([&](const auto& right) noexcept { return consistent(left, right); });
        })) return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION);
        if (config.continuation_capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        std::size_t ability_method_count{};
        for (std::size_t ability_index{}; ability_index < config.abilities.size(); ++ability_index)
        {
            const auto& contribution = config.abilities[ability_index];
            if (!contribution.valid() || contribution.description->methods.empty() ||
                !Impl::identifier(contribution.description->name) ||
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
                if (!Impl::identifier(method.name) || method.parameters.size() > Impl::kMaxAbilityArguments ||
                    method.results.size() > Impl::kMaxAbilityResults ||
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
                const bool wrong_signature = projection.parameters.size() != method.parameters.size() ||
                    projection.results.size() != method.results.size();
                if (wrong_signature)
                    return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                std::size_t frame_bytes = sizeof(std::optional<lux::script::lua::LuaValueFailure>);
                for (std::size_t i{}; i < method.parameters.size(); ++i)
                {
                    const auto& value = method.parameters[i].value;
                    const auto& operation = projection.parameters[i];
                    const bool mismatch = !operation.readable || operation.semantic_type != value.type_id ||
                        operation.canonical_name != value.canonical_name || operation.size != value.size ||
                        operation.alignment != value.alignment || operation.frame_bytes > 65536 - frame_bytes;
                    const bool invalid_async = method.kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION &&
                        (!Impl::supportedType(value) || !operation.native_scalar);
                    if (mismatch || invalid_async)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                    frame_bytes += operation.frame_bytes;
                    if (operation.alignment > (65536 - frame_bytes) / 2)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                    frame_bytes += 2 * operation.alignment; // Conservative inter-slot and final tuple padding.
                }
                for (std::size_t i{}; i < method.results.size(); ++i)
                {
                    const auto& value = method.results[i];
                    const auto& operation = projection.results[i];
                    const bool mismatch = !operation.writable || operation.semantic_type != value.type_id ||
                        operation.canonical_name != value.canonical_name || operation.size != value.size ||
                        operation.alignment != value.alignment || operation.frame_bytes > 65536 - frame_bytes;
                    const bool invalid_async = method.kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION &&
                        (!Impl::supportedType(value) || !operation.native_scalar ||
                            value.pass != lux::semantic::EValuePass::VALUE ||
                         value.lifetime != lux::script::EScriptAbilityValueLifetime::AWAITABLE);
                    if (mismatch || invalid_async)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                    frame_bytes += operation.frame_bytes;
                    if (operation.alignment > (65536 - frame_bytes) / 2)
                        return lux::cxx::unexpected(ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
                    frame_bytes += 2 * operation.alignment; // Conservative inter-slot and final tuple padding.
                }
            }
            const bool has_method_count_overflow = ability_method_count >
                std::numeric_limits<std::size_t>::max() - contribution.description->methods.size();
            if (has_method_count_overflow)
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
            ability_method_count += contribution.description->methods.size();
        }
        if (ability_method_count > config.ability_catalog_method_capacity)
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        if (config.events.size() > config.event_catalog_capacity)
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
            const bool is_invalid_source = !source.valid() || !Impl::identifier(source.system_name) ||
                !Impl::identifier(source.event_name) ||
                source.payload.type_id != lux::semantic::typeId(source.payload.canonical_name) ||
                (source.payload.abi_kind != LUX_SCRIPT_VK_STRUCT_REF && !is_supported_scalar);
            if (is_invalid_source)
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_EVENT_SOURCE);
            if (source.payload.abi_kind == LUX_SCRIPT_VK_STRUCT_REF &&
                std::ranges::none_of(config.values, [&](const auto& marshaller) noexcept {
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
            auto state = std::make_unique<Impl>(config);
            if (!state->prepared_abilities.valid() || !state->prepared_events.valid())
                return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
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
        std::unique_ptr<Impl> state
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
        return state_ && state_->main_thread && state_->traceback_ref != LUA_NOREF;
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
        unsigned long long fast{}, fallback{};
        const bool collected = luxlua_vmleafstats(state_->engine.state(), &fast, &fallback) != 0;
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
            state_->engine.allocationStats(),
            abilities.acquire_steps + events.acquire_steps,
            abilities.release_steps + events.release_steps,
            state_->prototypes.size(),
            Impl::leaf_yield_available, collected, fast, fallback
        };
    }

    EScriptBackendResult LuaScriptBackend::prepareSyncStep(ScriptBackendInstance instance,
                                                           const lux::rdesc::ScriptFunction& function,
                                                           const ScriptSyncStepShape& shape,
                                                           ScriptBackendPreparedMethod& method,
                                                           PreparedScriptSyncStep& result) noexcept
    {
        const bool invalid_shape = function.args.size() > 64U || function.returns.size() > 1U ||
                                   !detail::syncStepShapeMatches(shape, function);
        if (!state_ || invalid_shape)
            return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
        ScriptBackendPreparedMethod prepared;
        const auto status = Impl::prepareMethod(state_.get(), instance, function, prepared);
        if (status != EScriptBackendResult::SUCCESS) return status;
        if (prepared.resumable)
        {
            Impl::releaseMethod(state_.get(), instance, prepared);
            return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
        }
        const auto& call = *static_cast<Impl::PreparedCall*>(prepared.token);
        try
        {
            Impl::prepareSyncOperations(*const_cast<Impl::LuaFunctionBinding*>(call.function));
        }
        catch (const std::bad_alloc&)
        {
            Impl::releaseMethod(state_.get(), instance, prepared);
            return EScriptBackendResult::ALLOCATION_FAILURE;
        }
        decltype(PreparedScriptSyncStep::invoke) invoke{};
        const auto select = [&]<bool EntityScope, bool HasResult>()
        {
            invoke = call.function->scalar_sync ? &Impl::invokeSyncStep<EntityScope, HasResult, true>
                                                : &Impl::invokeSyncStep<EntityScope, HasResult, false>;
            // The common zero/one scalar shapes need neither an argument loop nor a per-value indirect push.
            // Custom operations always retain the protected converter path, including enum custom representations.
            if (!call.function->scalar_sync) return;
            if (function.args.empty())
                invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, std::nullptr_t>;
            else if (function.args.size() == 1U)
            {
                switch (function.args[0].abi_kind)
                {
                case LUX_SCRIPT_VK_BOOL: invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, bool>; break;
                case LUX_SCRIPT_VK_INT32:
                    invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, std::int32_t>; break;
                case LUX_SCRIPT_VK_UINT32:
                    invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, std::uint32_t>; break;
                case LUX_SCRIPT_VK_FLOAT: invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, float>; break;
                case LUX_SCRIPT_VK_DOUBLE: invoke = &Impl::invokeSyncStep<EntityScope, HasResult, true, double>; break;
                default: break;
                }
            }
        };
        if (call.instance->entity_scope)
        {
            if (function.returns.empty())
                select.template operator()<true, false>();
            else
                select.template operator()<true, true>();
        }
        else
        {
            if (function.returns.empty())
                select.template operator()<false, false>();
            else
                select.template operator()<false, true>();
        }
        method = prepared;
        result = {&function, &shape, prepared.token, invoke};
        return EScriptBackendResult::SUCCESS;
    }

    ScriptBackendDescriptor LuaScriptBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::LUA_SOURCE,
            state_.get(),
            &Impl::createInstance,
            &Impl::prepareMethod,
            &Impl::releaseMethod,
            &Impl::destroyInstance};
    }

} // namespace lux::simulation::script
