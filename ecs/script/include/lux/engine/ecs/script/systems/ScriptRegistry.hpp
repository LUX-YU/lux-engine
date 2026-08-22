#pragma once
// ============================================================================
//  ScriptRegistry.hpp — process-global C++ behavior registry (lux::ecs).
//
//  Owns only the process-global C++ behavior name→ops table that
//  LUX_REGISTER_SCRIPT populates by SELF-REGISTRATION (delivery-agnostic: a game
//  DLL registers on load; a statically-linked module registers at startup — same
//  table).
//
//  ADR v2 — the C++ ops are emitted by a TEMPLATE at registration, so the
//  compiler (not the meta generator) does the two jobs the ADR wanted:
//    · MEMBER DETECTION: requires-expressions determine which exact noexcept
//      lifecycle members T implements. Only those get event-table entries, so C++ joins
//      the "only dispatch to implementers" subscription-index contract.
//    · DIRECT SHIMS: the ABI thunk calls `T::onUpdate` by qualified name.
//      ScriptBehavior has no vtable and the concrete type is known here.
//  Instances live in a per-type chunked pool (stable addresses — locality
//  without the relocation hazards of a compacting arena).
//
//  scriptRegistry() is a function-local-static singleton, so static-init order
//  across translation units / DLLs is safe.
// ============================================================================

#include "ScriptBehavior.hpp"

#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <concepts>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    class World;

    /// The per-type C++ behavior operations the registration template emits.
    /// `construct` allocates from the type's pool and placement-news the
    /// behavior (returned as ScriptBehavior* in void*); `destroy` runs the
    /// destructor and returns the slot. Event shims are null for lifecycle
    /// methods the type does NOT override.
    struct CppBehaviorOps
    {
        void* (*construct)()      = nullptr;
        void  (*destroy)(void*)   = nullptr;
        void  (*bind_context)(void*, EntityHandle, World&) = nullptr;
        lux_script_invoke_fn on_create  = nullptr;
        lux_script_invoke_fn on_update  = nullptr;
        lux_script_invoke_fn on_destroy = nullptr;
    };

    template<class T>
    struct ScriptBehaviorAccess
    {
        static void bind(T& behavior, EntityHandle self, World& world) noexcept
        {
            auto& base = static_cast<ScriptBehavior&>(behavior);
            base.self_ = self;
            base.world_ = &world;
        }
    };

    namespace detail
    {
        /// Per-type chunked instance pool: stable addresses (chunks never
        /// move — no memmove relocation hazards for user members), allocation
        /// front-to-back for creation-order locality. Main-thread only (the
        /// ScriptSystem creates/destroys instances on the main thread).
        /// Function-local-static per instantiating module — registration and
        /// the ops both come from the module that owns T, so construct and
        /// destroy always meet the same pool.
        template<class T>
        class BehaviorPool
        {
        public:
            static BehaviorPool& instance()
            {
                static BehaviorPool pool;
                return pool;
            }

            void* acquire()
            {
                if (free_.empty()) grow();
                void* p = free_.back();
                free_.pop_back();
                return p;
            }
            void release(void* p) { free_.push_back(p); }

        private:
            static constexpr std::size_t kChunkSlots = 64;
            struct Chunk
            {
                alignas(T) std::byte storage[sizeof(T) * kChunkSlots];
            };

            void grow()
            {
                auto& chunk = chunks_.emplace_back(std::make_unique<Chunk>());
                // Push REVERSED so acquisition walks the chunk front-to-back
                // (instances created together sit together).
                for (std::size_t i = kChunkSlots; i-- > 0;)
                    free_.push_back(chunk->storage + i * sizeof(T));
            }

            std::vector<std::unique_ptr<Chunk>> chunks_;
            std::vector<void*>                  free_;
        };

        template<class T>
        concept HasOnCreate = requires(T& value)
        {
            { value.onCreate() } noexcept -> std::same_as<void>;
        };

        template<class T>
        concept HasOnUpdate = requires(T& value, float dt)
        {
            { value.onUpdate(dt) } noexcept -> std::same_as<void>;
        };

        template<class T>
        concept HasOnDestroy = requires(T& value)
        {
            { value.onDestroy() } noexcept -> std::same_as<void>;
        };

        template<class T>
        CppBehaviorOps makeCppBehaviorOps()
        {
            static_assert(std::is_base_of_v<ScriptBehavior, T>,
                          "a registered script must derive from ScriptBehavior");
            static_assert(std::is_default_constructible_v<T>,
                          "a registered script must be default-constructible");
            static_assert(std::is_nothrow_destructible_v<T>,
                          "a registered script must be nothrow-destructible");
            static_assert(!std::is_polymorphic_v<T>,
                          "a registered script must not contain a vptr");

            CppBehaviorOps ops{};
            ops.construct = []() -> void*
            {
                void* slot = BehaviorPool<T>::instance().acquire();
                return new (slot) T();
            };
            ops.destroy = [](void* state)
            {
                T* t = static_cast<T*>(state);
                t->~T();
                BehaviorPool<T>::instance().release(t);
            };
            ops.bind_context = [](void* state, EntityHandle self, World& world)
            {
                ScriptBehaviorAccess<T>::bind(
                    *static_cast<T*>(state),
                    self,
                    world
                );
            };
            if constexpr (HasOnCreate<T>)
                ops.on_create = [](lux_script_call_frame* frame) noexcept -> int
                {
                    static_cast<T*>(frame->user_context)->T::onCreate();
                    return 0;
                };
            if constexpr (HasOnUpdate<T>)
                ops.on_update = [](lux_script_call_frame* frame) noexcept -> int
                {
                    const auto& slot = frame->args[0];
                    static_cast<T*>(frame->user_context)->T::onUpdate(
                        *static_cast<const float*>(slot.data)
                    );
                    return 0;
                };
            if constexpr (HasOnDestroy<T>)
                ops.on_destroy = [](lux_script_call_frame* frame) noexcept -> int
                {
                    static_cast<T*>(frame->user_context)->T::onDestroy();
                    return 0;
                };
            return ops;
        }
    } // namespace detail

    class LUX_FUNCTION_PUBLIC ScriptRegistry
    {
    public:
        ScriptRegistry() = default;
        // Non-copyable process-global registration table.
        ScriptRegistry(const ScriptRegistry&)            = delete;
        ScriptRegistry& operator=(const ScriptRegistry&) = delete;

        /// Register a C++ ScriptBehavior by name with its template-emitted ops
        /// (called by LUX_REGISTER_SCRIPT / the typed helper below).
        void registerCppScript(std::string_view name, CppBehaviorOps ops);

        /// The typed door: emits pool + devirtualized shims for T at the call
        /// site (ADR v2) and registers them under @p name.
        template<class T>
        void registerCppScript(std::string_view name)
        {
            registerCppScript(name, detail::makeCppBehaviorOps<T>());
        }

        /// C++ behavior creation. Lua/Native backends are playback-session
        /// owners and never enter this process-global registry.
        [[nodiscard]] ScriptInstance
            createCppInstanceFromAsset(
                lux::ecs::EntityHandle entity,
                World& world,
                const lux::rdesc::Script& desc) const;

        [[nodiscard]] bool hasCppScript(std::string_view name) const;

        /// A-5 C++ manifest: every registered C++ behavior name, sorted. Views
        /// point into the registry's keys — registration happens at startup,
        /// so editor consumers may treat the list as session-stable.
        [[nodiscard]] std::vector<std::string_view> cppScriptNames() const;

    private:
        std::unordered_map<std::string, CppBehaviorOps> cpp_scripts_;
    };

    /// Process-wide registry (function-local static — safe static-init order).
    [[nodiscard]] LUX_FUNCTION_PUBLIC ScriptRegistry& scriptRegistry();
} // namespace lux::ecs

// ── Self-registration macro (game module: DLL load-time / static startup) ──
#define LUX_SCRIPT_CONCAT_(a, b) a##b
#define LUX_SCRIPT_CONCAT(a, b)  LUX_SCRIPT_CONCAT_(a, b)
#define LUX_REGISTER_SCRIPT(name, T)                                            \
    static bool LUX_SCRIPT_CONCAT(_lux_script_reg_, __COUNTER__) =              \
        (::lux::ecs::scriptRegistry().registerCppScript<T>((name)), true)
