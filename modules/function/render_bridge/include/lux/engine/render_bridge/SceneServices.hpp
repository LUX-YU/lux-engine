#pragma once
// ============================================================================
//  SceneServices.hpp — the scene-scoped service container (ADR:
//  lux-engine-pack-architecture §4, lux::render_bridge).
//
//  Purpose: concrete runtimes (a pixel-field runtime, a physics world, …)
//  must not leak into public install signatures. A pack's installer either
//  EMPLACES the service it owns or GETS the services it consumes; the install
//  entry point shrinks to (World, SceneServices, Plan) forever, however many
//  runtimes packs invent.
//
//  Deliberate NON-goals (recorded):
//   - NOT a global service locator. The container is passed explicitly during
//     scene ASSEMBLY (pack installers + the app that owns the scene). Systems
//     capture strong typed pointers at wiring time, exactly as before — there
//     is no runtime lookup on hot paths.
//   - No lazy construction, no dependency graph: installers run in registry
//     order (ScenePackRegistry), so "my prerequisite exists" is an ORDER
//     contract, preflight-checked before anything mutates the World.
//
//  Lifetime contract: the container must OUTLIVE the World it was used to
//  assemble (phase closures inside World-owned systems may reference services
//  the container owns). Declare it before the World in the owning scope:
//      SceneServices services;   // destroyed LAST
//      ecs::World    world;      // destroyed first
//  adopt()ed pointers are borrowed — the caller keeps ownership (used for
//  runtimes the app also drives directly, e.g. a stack-owned field runtime).
// ============================================================================

#include <memory>
#include <typeindex>
#include <vector>

namespace lux::render_bridge
{
    class SceneServices
    {
    public:
        SceneServices() = default;
        SceneServices(const SceneServices&)            = delete;
        SceneServices& operator=(const SceneServices&) = delete;

        /// The service of type T, or nullptr when absent. O(n) over a handful
        /// of entries — assembly-time only, never a hot path.
        template <class T>
        [[nodiscard]] T* get() const noexcept
        {
            for (const auto& s : slots_)
                if (s.type == std::type_index(typeid(T)))
                    return static_cast<T*>(s.ptr);
            return nullptr;
        }

        /// Register an OWNED service (destroyed with the container, reverse
        /// registration order). Returns the live reference. One slot per type:
        /// registering a duplicate type replaces the pointer the container
        /// hands out but keeps the old instance alive (assembly bug — assert).
        template <class T>
        T& emplace(std::unique_ptr<T> svc)
        {
            T* raw = svc.get();
            slots_.push_back(Slot{
                std::type_index(typeid(T)), raw,
                std::shared_ptr<void>(svc.release(), [](void* p) { delete static_cast<T*>(p); })});
            return *raw;
        }

        /// Register a BORROWED service (caller-owned; must outlive the scene).
        template <class T>
        void adopt(T* borrowed) noexcept
        {
            slots_.push_back(Slot{std::type_index(typeid(T)), borrowed, nullptr});
        }

        ~SceneServices()
        {
            // Reverse order: later services may reference earlier ones.
            while (!slots_.empty())
                slots_.pop_back();
        }

    private:
        struct Slot
        {
            std::type_index       type;
            void*                 ptr;
            std::shared_ptr<void> owned;   ///< null for adopt()ed slots
        };
        std::vector<Slot> slots_;
    };

} // namespace lux::render_bridge
