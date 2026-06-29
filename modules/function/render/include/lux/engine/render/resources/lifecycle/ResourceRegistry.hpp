#pragma once
/**
 * @file ResourceRegistry.hpp
 * @brief Dynamic GPU resource registry backed by AutoSparseSet.
 *
 * Replaces the old tuple-based GPUResourceRegistry with a runtime registry
 * that stores type-erased resource slots in a dense SparseSet.
 *
 * Usage:
 *   ResourceRegistry reg;
 *   reg.emplace<MeshResources>();              // returns ResourceHandle<MeshResources>
 *   reg.find<MeshResources>()->init(...);      // type-based singleton lookup
 *   auto ds = reg.descriptorSetOf<MeshResources>();
 *   reg.shutdown();
 *
 * Resources are discovered by type via find<T>() — no index bookkeeping
 * needed by the caller.  For the rare multi-instance case, emplace()
 * returns a ResourceHandle<T> that provides direct O(1) access.
 */

#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/compile_time/type_info.hpp>   // type_hash (replaces typeid/type_index — no RTTI)
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/core/vk_fwd.hpp>  // VkDescriptorSet only — no full <vulkan/vulkan.h> (public-surface friendly)

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render
{
namespace detail
{
    template<typename T, typename = void>
    struct GlobalUploadPhaseTrait
    {
        static constexpr EUploadPhase value = EUploadPhase::Upload;
    };

    template<typename T>
    struct GlobalUploadPhaseTrait<T, std::void_t<decltype(T::kGlobalUploadPhase)>>
    {
        static constexpr EUploadPhase value = T::kGlobalUploadPhase;
    };

    template<typename T, typename = void>
    struct SceneUploadPhaseTrait
    {
        static constexpr EUploadPhase value = EUploadPhase::Upload;
    };

    template<typename T>
    struct SceneUploadPhaseTrait<T, std::void_t<decltype(T::kSceneUploadPhase)>>
    {
        static constexpr EUploadPhase value = T::kSceneUploadPhase;
    };
    template<typename T, typename = void>
    struct HasGetDescriptorSet : std::false_type {};
    template<typename T>
    struct HasGetDescriptorSet<T, std::void_t<decltype(std::declval<const T&>().getDescriptorSet())>>
        : std::true_type {};

    template<typename T, typename = void>
    struct HasShutdown : std::false_type {};
    template<typename T>
    struct HasShutdown<T, std::void_t<decltype(std::declval<T&>().shutdown())>>
        : std::true_type {};
} // namespace detail

// Forward declaration so ResourceHandle can reference ResourceRegistry.
class ResourceRegistryBase;

// ─────────────────────────────────────────────────────────────────────
//  ResourceHandle<T> — strong-typed O(1) accessor returned by emplace()
// ─────────────────────────────────────────────────────────────────────

template<typename T>
class ResourceHandle
{
public:
    ResourceHandle() = default;

    [[nodiscard]] T*              get()           const noexcept;
    [[nodiscard]] T*              operator->()    const noexcept { return get(); }
    [[nodiscard]] T&              operator*()     const noexcept { return *get(); }
    [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept;
    [[nodiscard]] uint32_t        index()         const noexcept { return idx_; }
    explicit operator bool()                      const noexcept { return registry_ != nullptr; }

private:
    friend class ResourceRegistryBase;
    ResourceHandle(ResourceRegistryBase* reg, uint32_t idx) : registry_(reg), idx_(idx) {}

    ResourceRegistryBase* registry_{nullptr};
    uint32_t          idx_{UINT32_MAX};
};

// ─────────────────────────────────────────────────────────────────────
//  ResourceRegistry
// ─────────────────────────────────────────────────────────────────────

class ResourceRegistryBase
{
public:
    ResourceRegistryBase()  = default;
    ~ResourceRegistryBase() { shutdown(); }

    ResourceRegistryBase(const ResourceRegistryBase&)            = delete;
    ResourceRegistryBase& operator=(const ResourceRegistryBase&) = delete;
    ResourceRegistryBase(ResourceRegistryBase&&)                 = delete;
    ResourceRegistryBase& operator=(ResourceRegistryBase&&)      = delete;

    // ── Registration ────────────────────────────────────────────────

    /// Construct a resource of type T in-place and register it.
    /// First registration of a given type is discoverable via find<T>().
    /// @return A ResourceHandle<T> for direct O(1) access.
    template<typename T, typename... Args>
    ResourceHandle<T> emplace(Args&&... args)
    {
        auto* raw = new T(std::forward<Args>(args)...);
        Slot s;
        s.ptr         = ErasedPtr(raw, [](void* p) { delete static_cast<T*>(p); });
        if constexpr (detail::HasGetDescriptorSet<T>::value)
        {
            s.ds_getter   = [](const void* p) -> VkDescriptorSet {
                return static_cast<const T*>(p)->getDescriptorSet();
            };
        }
        if constexpr (detail::HasShutdown<T>::value)
        {
            s.shutdown_fn = [](void* p) { static_cast<T*>(p)->shutdown(); };
        }
        auto idx = static_cast<uint32_t>(slots_.emplace(std::move(s)));

        // Register the first instance of each type for find<T>() discovery.
        auto key = lux::cxx::type_hash<T>();
        type_map_.try_emplace(key, idx);

        if constexpr (std::is_base_of_v<IGlobalFrameService, T>)
        {
            registerGlobalFrameService(
                static_cast<IGlobalFrameService*>(raw),
                detail::GlobalUploadPhaseTrait<T>::value);
        }
        if constexpr (std::is_base_of_v<ISceneFrameService, T>)
        {
            registerSceneFrameService(
                static_cast<ISceneFrameService*>(raw),
                detail::SceneUploadPhaseTrait<T>::value);
        }

        return ResourceHandle<T>{this, idx};
    }

    /// Idempotent get-or-create. Returns the existing first instance of T if one
    /// is already registered; otherwise constructs and registers a new T(args...).
    /// Lets every feature that needs a shared scene-local resource simply declare
    /// that need in initAndAttachTo — whoever attaches first builds it, the rest
    /// receive the same instance — instead of special-casing a single owner.
    /// Returns a borrowed pointer owned by the registry (never null).
    template<typename T, typename... Args>
    T* ensure(Args&&... args)
    {
        if (T* existing = find<T>())
            return existing;
        return emplace<T>(std::forward<Args>(args)...).get();
    }

    // ── Type-based discovery (singleton fast path) ──────────────────

    /// Find the first registered instance of type T.
    /// @return Pointer to T, or nullptr if no T has been registered.
    template<typename T>
    T* find() noexcept
    {
        auto it = type_map_.find(lux::cxx::type_hash<T>());
        if (it == type_map_.end()) return nullptr;
        return static_cast<T*>(slots_.at(it->second).ptr.get());
    }

    template<typename T>
    const T* find() const noexcept
    {
        auto it = type_map_.find(lux::cxx::type_hash<T>());
        if (it == type_map_.end()) return nullptr;
        return static_cast<const T*>(slots_.at(it->second).ptr.get());
    }

    /// Get the descriptor set of the first registered instance of type T.
    template<typename T>
    [[nodiscard]] VkDescriptorSet descriptorSetOf() const
    {
        auto it = type_map_.find(lux::cxx::type_hash<T>());
        if (it == type_map_.end()) return VkDescriptorSet{};
        const auto& s = slots_.at(it->second);
        return s.ds_getter ? s.ds_getter(s.ptr.get()) : VkDescriptorSet{};
    }

    // ── Index-based access (for ResourceHandle internals) ───────────

    /// Retrieve a registered resource by index. O(1) SparseSet lookup.
    /// @return Pointer to T, or nullptr if index is invalid.
    template<typename T>
    T* getAs(uint32_t index) noexcept
    {
        if (!slots_.contains(index)) return nullptr;
        return static_cast<T*>(slots_.at(index).ptr.get());
    }

    template<typename T>
    const T* getAs(uint32_t index) const noexcept
    {
        if (!slots_.contains(index)) return nullptr;
        return static_cast<const T*>(slots_.at(index).ptr.get());
    }

    // ── Descriptor set access ───────────────────────────────────────

    /// Get the VkDescriptorSet from a registered resource by index.
    [[nodiscard]] VkDescriptorSet getDescriptorSet(uint32_t index) const
    {
        if (!slots_.contains(index)) return VkDescriptorSet{};
        const auto& s = slots_.at(index);
        return s.ds_getter ? s.ds_getter(s.ptr.get()) : VkDescriptorSet{};
    }

    // ── Frame service registration / dispatch ───────────────────────────

    void registerGlobalFrameService(IGlobalFrameService* service,
                                    EUploadPhase phase = EUploadPhase::Upload)
    {
        if (!service) return;
        global_frame_services_[static_cast<size_t>(phase)].push_back(service);
    }

    void registerSceneFrameService(ISceneFrameService* service,
                                   EUploadPhase phase = EUploadPhase::Upload)
    {
        if (!service) return;
        scene_frame_services_[static_cast<size_t>(phase)].push_back(service);
    }

    [[nodiscard]] std::span<IGlobalFrameService* const> globalFrameServices(
        EUploadPhase phase) const noexcept
    {
        const auto& list = global_frame_services_[static_cast<size_t>(phase)];
        return std::span<IGlobalFrameService* const>(list.data(), list.size());
    }

    [[nodiscard]] std::span<ISceneFrameService* const> sceneFrameServices(
        EUploadPhase phase) const noexcept
    {
        const auto& list = scene_frame_services_[static_cast<size_t>(phase)];
        return std::span<ISceneFrameService* const>(list.data(), list.size());
    }

    void notifySceneViewDestroyed(uint32_t view_id)
    {
        notifySceneViewDestroyed(0u, view_id);
    }

    void notifySceneViewDestroyed(uint32_t scene_key, uint32_t view_id)
    {
        for (auto phase = 0u; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (auto* svc : global_frame_services_[phase])
            {
                if (svc) svc->onSceneViewDestroyed(scene_key, view_id);
            }
            for (auto* svc : scene_frame_services_[phase])
            {
                if (svc) svc->onSceneViewDestroyed(scene_key, view_id);
            }
        }
    }

    // ── Lifecycle ───────────────────────────────────────────────────

    /// Shutdown all resources in reverse registration order, then free.
    void shutdown()
    {
        // Copy keys — dense array order is insertion order (no erase before shutdown).
        // Reverse iterate for "later registrations shutdown first".
        auto ks = slots_.keys();
        for (auto it = ks.rbegin(); it != ks.rend(); ++it)
        {
            auto& s = slots_.at(*it);
            if (s.ptr)
            {
                if (s.shutdown_fn) s.shutdown_fn(s.ptr.get());
                s.ptr.reset();
            }
        }
        slots_.clear();
        type_map_.clear();
        for (auto& list : global_frame_services_)
            list.clear();
        for (auto& list : scene_frame_services_)
            list.clear();
    }

    [[nodiscard]] size_t size() const noexcept { return slots_.size(); }

private:
    using ErasedPtr = std::unique_ptr<void, void(*)(void*)>;

    struct Slot
    {
        ErasedPtr ptr{nullptr, +[](void*) {}};
        VkDescriptorSet (*ds_getter)(const void*){nullptr};
        void (*shutdown_fn)(void*){nullptr};
    };

    lux::cxx::AutoSparseSet<Slot> slots_;
    std::unordered_map<std::uint64_t, uint32_t> type_map_;   // key = lux::cxx::type_hash<T>()
    std::array<std::vector<IGlobalFrameService*>, static_cast<size_t>(EUploadPhase::Count)>
        global_frame_services_{};
    std::array<std::vector<ISceneFrameService*>, static_cast<size_t>(EUploadPhase::Count)>
        scene_frame_services_{};
};

// ── ResourceHandle<T> out-of-line definitions ───────────────────────

template<typename T>
T* ResourceHandle<T>::get() const noexcept
{
    return registry_ ? registry_->getAs<T>(idx_) : nullptr;
}

template<typename T>
VkDescriptorSet ResourceHandle<T>::descriptorSet() const noexcept
{
    return registry_ ? registry_->getDescriptorSet(idx_) : VkDescriptorSet{};
}

class GlobalResourceRegistry final : public ResourceRegistryBase
{
public:
    using ResourceRegistryBase::ResourceRegistryBase;
};

class SceneResourceRegistry final : public ResourceRegistryBase
{
public:
    using ResourceRegistryBase::ResourceRegistryBase;
};

} // namespace lux::render
