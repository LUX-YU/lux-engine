#pragma once
// ============================================================================
//  ScriptRegistry.hpp — the script backend + factory registry (lux::ecs).
//
//  Owns the IScriptBackends and, for the C++ path, a name→ops table that
//  LUX_REGISTER_SCRIPT populates by SELF-REGISTRATION (delivery-agnostic: a game
//  DLL registers on load; a statically-linked module registers at startup — same
//  table).
//
//  ADR v2 — the C++ ops are emitted by a TEMPLATE at registration, so the
//  compiler (not the meta generator) does the two jobs the ADR wanted:
//    · OVERRIDE DETECTION: `decltype(&T::onUpdate)` is a pointer-to-member of
//      the class that DECLARES the override — comparing against the base's
//      member-pointer type tells, at compile time, which lifecycle events T
//      actually implements. Only those get event-table entries, so C++ joins
//      the "only dispatch to implementers" subscription-index contract.
//    · DEVIRTUALIZED SHIMS: the shim calls `T::onUpdate` by QUALIFIED name —
//      a direct, inlinable call (T is the most-derived registered type; the
//      factory constructs exactly T, so bypassing the vtable is sound).
//  Instances live in a per-type chunked pool (stable addresses — locality
//  without the relocation hazards of a compacting arena).
//
//  scriptRegistry() is a function-local-static singleton, so static-init order
//  across translation units / DLLs is safe.
// ============================================================================

#include "ScriptBehavior.hpp"

#include <lux/engine/function/visibility.h>

#include <cstddef>
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
        ScriptEventFn on_create   = nullptr;
        ScriptEventFn on_update   = nullptr;
        ScriptEventFn on_destroy  = nullptr;
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

        /// Compile-time "does T override this lifecycle method": the member
        /// pointer's class is the one DECLARING the (most-derived) override.
        template<class T>
        inline constexpr bool overrides_on_create =
            !std::is_same_v<decltype(&T::onCreate), void (ScriptBehavior::*)()>;
        template<class T>
        inline constexpr bool overrides_on_update =
            !std::is_same_v<decltype(&T::onUpdate), void (ScriptBehavior::*)(float)>;
        template<class T>
        inline constexpr bool overrides_on_destroy =
            !std::is_same_v<decltype(&T::onDestroy), void (ScriptBehavior::*)()>;

        template<class T>
        CppBehaviorOps makeCppBehaviorOps()
        {
            static_assert(std::is_base_of_v<ScriptBehavior, T>,
                          "a registered script must derive from ScriptBehavior");
            static_assert(std::is_default_constructible_v<T>,
                          "a registered script must be default-constructible");

            CppBehaviorOps ops{};
            ops.construct = []() -> void*
            {
                void* slot = BehaviorPool<T>::instance().acquire();
                // state convention: void* = ScriptBehavior* (single upcast).
                return static_cast<ScriptBehavior*>(new (slot) T());
            };
            ops.destroy = [](void* state)
            {
                T* t = static_cast<T*>(static_cast<ScriptBehavior*>(state));
                t->~T();
                BehaviorPool<T>::instance().release(t);
            };
            // Qualified calls — direct, inlinable, no vtable hop. Bound ONLY
            // when T actually overrides, so absent events stay absent from
            // the subscription index.
            if constexpr (overrides_on_create<T>)
                ops.on_create = [](void* s, ScriptEventId, void* const*) -> bool
                {
                    static_cast<T*>(static_cast<ScriptBehavior*>(s))->T::onCreate();
                    return true;
                };
            if constexpr (overrides_on_update<T>)
                ops.on_update = [](void* s, ScriptEventId, void* const* a) -> bool
                {
                    static_cast<T*>(static_cast<ScriptBehavior*>(s))
                        ->T::onUpdate(*static_cast<const float*>(a[0]));
                    return true;
                };
            if constexpr (overrides_on_destroy<T>)
                ops.on_destroy = [](void* s, ScriptEventId, void* const*) -> bool
                {
                    static_cast<T*>(static_cast<ScriptBehavior*>(s))->T::onDestroy();
                    return true;
                };
            return ops;
        }
    } // namespace detail

    class LUX_FUNCTION_PUBLIC ScriptRegistry
    {
    public:
        ScriptRegistry() = default;
        // Non-copyable (singleton + a vector<unique_ptr> member). Explicitly deleted
        // so the dllexport (LUX_FUNCTION_PUBLIC) does not try to instantiate an
        // implicit copy of the move-only backend vector.
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

        /// Register a non-Cpp backend (Lua/native/flowforge — later phases).
        void registerBackend(std::unique_ptr<IScriptBackend> backend);

        /// Forward the per-frame context to every backend (input, …). Called by
        /// ScriptSystem before dispatch.
        void beginFrame(const ScriptContext& ctx);

        /// Asset-routed creation — the ONLY creation door (the name path
        /// retired with ScriptComponent.entry). CppBehaviorScript
        /// resolves against the registry's own factory table (the lifecycle
        /// shims are bound here); every other kind is offered to the backends
        /// in registration order — the first that claims it wins. Returns an
        /// EMPTY instance when nothing accepts.
        [[nodiscard]] ScriptInstance
            createInstanceFromAsset(
                lux::ecs::EntityHandle entity,
                World& world,
                                    const lux::rdesc::Script&  desc,
                                    std::span<const std::byte> payload,
                                    std::string_view           cache_key) const;

        [[nodiscard]] bool hasCppScript(std::string_view name) const;

        /// A-5 C++ manifest: every registered C++ behavior name, sorted. Views
        /// point into the registry's keys — registration happens at startup,
        /// so editor consumers may treat the list as session-stable.
        [[nodiscard]] std::vector<std::string_view> cppScriptNames() const;

    private:
        std::unordered_map<std::string, CppBehaviorOps> cpp_scripts_;
        std::vector<std::unique_ptr<IScriptBackend>>    backends_;
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
