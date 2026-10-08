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
 *   auto candidate = MeshResources::create(info);
 *   if (!candidate) return unexpected(candidate.error());
 *   auto mesh = reg.insert(std::move(*candidate)); // complete owner, then publication
 *   auto* same = reg.find<MeshResources>();
 *
 * Resources are discovered by type via find<T>() — no index bookkeeping
 * needed by the caller.  For the rare multi-instance case, emplace()
 * returns a ResourceHandle<T> that provides direct O(1) access.
 */

#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/compile_time/type_info.hpp> // type_hash (replaces typeid/type_index — no RTTI)
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp> // must<T>() terminates in EVERY config
#include <lux/engine/render/core/vk_fwd.hpp>
// VkDescriptorSet only — no full <vulkan/vulkan.h> (public-surface friendly)

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render
{
    namespace detail
    {
        // (曾有一个 UploadPhaseTrait 用来在 emplace<T> 里探测 T::kUploadPhase 并自动
        //  登记。自动登记取消后它没有用户了 —— 现在阶段由**安装点**在
        //  addBeginFrameHook 时显式传入。资源仍可声明 kUploadPhase 表达"我需要哪个
        //  阶段"这一**能力**,安装点读它传进来,见 SceneResources。)

        template <typename T, typename = void> struct THasGetDescriptorSet : std::false_type
        {};
        template <typename T>
        struct THasGetDescriptorSet<T, std::void_t<decltype(std::declval<const T&>().getDescriptorSet())>>
            : std::true_type
        {};
    } // namespace detail

    // Forward declaration so ResourceHandle can reference ResourceRegistry.
    class ResourceRegistry;

    // ─────────────────────────────────────────────────────────────────────
    //  ResourceHandle<T> — strong-typed O(1) accessor returned by emplace()
    // ─────────────────────────────────────────────────────────────────────

    template <typename T> class TResourceHandle
    {
    public:
        TResourceHandle() = default;

        [[nodiscard]] T* get() const noexcept;
        [[nodiscard]] T* operator->() const noexcept
        {
            return get();
        }
        [[nodiscard]] T& operator*() const noexcept
        {
            return *get();
        }
        [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept;
        [[nodiscard]] uint32_t index() const noexcept
        {
            return idx_;
        }
        explicit operator bool() const noexcept
        {
            return registry_ != nullptr;
        }

    private:
        friend class ResourceRegistry;
        TResourceHandle(ResourceRegistry* reg, uint32_t idx) : registry_(reg), idx_(idx) {}

        ResourceRegistry* registry_{nullptr};
        uint32_t idx_{UINT32_MAX};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  ResourceRegistry
    // ─────────────────────────────────────────────────────────────────────

    class ResourceRegistry
    {
    public:
        ResourceRegistry() = default;
        ~ResourceRegistry()
        {
            // Dense keys remain in insertion order: registry entries are never removed individually.
            // Reverse iterate so dependencies registered earlier outlive their consumers.
            auto ks = slots_.keys();
            for (auto it = ks.rbegin(); it != ks.rend(); ++it)
            {
                auto& s = slots_.at(*it);
                s.ptr.reset();
            }
            slots_.clear();
            type_map_.clear();
            for (auto& list : begin_frame_hooks_)
            {
                list.clear();
            }
            view_destroyed_hooks_.clear();
        }

        ResourceRegistry(const ResourceRegistry&) = delete;
        ResourceRegistry& operator=(const ResourceRegistry&) = delete;
        ResourceRegistry(ResourceRegistry&&) = delete;
        ResourceRegistry& operator=(ResourceRegistry&&) = delete;

        // ── Registration ────────────────────────────────────────────────

        /// Construct a resource of type T in-place and register it.
        /// First registration of a given type is discoverable via find<T>().
        /// @return A ResourceHandle<T> for direct O(1) access.
        template <typename T, typename... Args>
            requires std::constructible_from<T, Args...>
        TResourceHandle<T> emplace(Args&&... args)
        {
            return insert(std::make_unique<T>(std::forward<Args>(args)...));
        }

        /// Publish a complete resource from its fallible factory. A null owner is a contract violation.
        template <typename T> TResourceHandle<T> insert(std::unique_ptr<T> resource) noexcept
        {
            if (!resource)
            {
                renderFatal("ResourceRegistry::insert(): missing resource owner");
            }
            return TResourceHandle<T>{this, publishOwned<T>(resource.release())};
        }

        /// Construct once and retain the first complete instance of T.
        /// Fallible factories publish their complete result through insert() instead.
        /// On a hit, constructor arguments are not applied to the existing instance.
        template <typename T, typename... Args>
            requires std::constructible_from<T, Args...>
        T& ensure(Args&&... args)
        {
            if (T* existing = find<T>())
            {
                return *existing;
            }
            return *emplace<T>(std::forward<Args>(args)...).get();
        }

        // ── Type-based discovery (singleton fast path) ──────────────────

        /// A resource whose presence is a structural invariant, not a runtime question.
        ///
        /// Use for: the global singletons the render-server factory constructs unconditionally
        /// before any scene or feature exists (ShaderResources / TextureResources /
        /// VertexLayoutRegistry), and resources the caller itself just
        /// emplace<>/ensure<>d. The registry has no erase and type_map_ keeps the FIRST
        /// instance forever, so once registered a resource cannot disappear while the
        /// registry lives.
        ///
        /// For the lazily-built globals (MeshResources / MaterialResources via
        /// ensureGlobal*) absent IS a legal state, so find<T>() + a real null-check is
        /// the correct shape — EXCEPT right after a successful ensureGlobal*() in the
        /// same function, which is just the "caller itself ensured it" case above.
        ///
        /// Why this exists: find<T>() returning T* gave callers no way to express "this
        /// one is guaranteed", so ~26 sites grew `if (!x) return;` guards on conditions
        /// that cannot occur — turning an impossible state into a silent no-op, which is
        /// the failure mode the highlight-regression post-mortem warned about
        /// (the implementation §5.5 lesson 2).
        ///
        /// Terminates through renderFatal, NOT assert: the editor and the player
        /// both ship RelWithDebInfo, which carries NDEBUG — an assert here would
        /// compile away in exactly the configuration that runs on real hardware,
        /// leaving all ~70 call sites dereferencing a null pointer instead. The
        /// defaulted @p where makes the abort name the CALLER, not this header.
        template <typename T> T& must(const std::source_location& where = std::source_location::current()) noexcept
        {
            T* p = find<T>();
            if (!p)
                renderFatal("ResourceRegistry::must<T>(): required resource not registered", where);
            return *p;
        }

        template <typename T>
        const T& must(const std::source_location& where = std::source_location::current()) const noexcept
        {
            const T* p = find<T>();
            if (!p)
                renderFatal("ResourceRegistry::must<T>(): required resource not registered", where);
            return *p;
        }

        /// Find the first registered instance of type T.
        /// @return Pointer to T, or nullptr if no T has been registered.
        /// Prefer must<T>() when absence is impossible — see its comment.
        template <typename T> T* find() noexcept
        {
            auto it = type_map_.find(lux::cxx::type_hash<T>());
            if (it == type_map_.end())
                return nullptr;
            return static_cast<T*>(slots_.at(it->second).ptr.get());
        }

        template <typename T> const T* find() const noexcept
        {
            auto it = type_map_.find(lux::cxx::type_hash<T>());
            if (it == type_map_.end())
                return nullptr;
            return static_cast<const T*>(slots_.at(it->second).ptr.get());
        }

        /// Get the descriptor set of the first registered instance of type T.
        template <typename T> [[nodiscard]] VkDescriptorSet descriptorSetOf() const
        {
            auto it = type_map_.find(lux::cxx::type_hash<T>());
            if (it == type_map_.end())
                return VkDescriptorSet{};
            const auto& s = slots_.at(it->second);
            return s.ds_getter ? s.ds_getter(s.ptr.get()) : VkDescriptorSet{};
        }

        // ── Index-based access (for ResourceHandle internals) ───────────

        /// Retrieve a registered resource by index. O(1) SparseSet lookup.
        /// @return Pointer to T, or nullptr if index is invalid.
        template <typename T> T* getAs(uint32_t index) noexcept
        {
            if (!slots_.contains(index))
                return nullptr;
            return static_cast<T*>(slots_.at(index).ptr.get());
        }

        template <typename T> const T* getAs(uint32_t index) const noexcept
        {
            if (!slots_.contains(index))
                return nullptr;
            return static_cast<const T*>(slots_.at(index).ptr.get());
        }

        // ── Descriptor set access ───────────────────────────────────────

        /// Get the VkDescriptorSet from a registered resource by index.
        [[nodiscard]] VkDescriptorSet getDescriptorSet(uint32_t index) const
        {
            if (!slots_.contains(index))
                return VkDescriptorSet{};
            const auto& s = slots_.at(index);
            return s.ds_getter ? s.ds_getter(s.ptr.get()) : VkDescriptorSet{};
        }

        // ── Frame service registration / dispatch ───────────────────────────

        using BeginFrameHook = std::function<void(const FrameStamp&)>;
        using ViewDestroyedHook = std::function<void(uint32_t scene_key, uint32_t view_id)>;

        /// 登记一条每帧维护回调。由**安装该资源的上层**调用,写在它的"首次安装"
        /// 守卫内 —— 这既让"每帧驱动谁"变成一个看得见的决定,也保证恰好一次
        /// (ensure<> 安装的资源可能有多个安装者)。
        void addBeginFrameHook(EUploadPhase phase, BeginFrameHook fn)
        {
            if (!fn)
                return;
            begin_frame_hooks_[static_cast<size_t>(phase)].push_back(std::move(fn));
        }

        [[nodiscard]] std::span<const BeginFrameHook> beginFrameHooks(EUploadPhase phase) const noexcept
        {
            const auto& list = begin_frame_hooks_[static_cast<size_t>(phase)];
            return {list.data(), list.size()};
        }

        /// 登记一条"视图销毁"回调(逐出该视图的缓存等)。
        void addViewDestroyedHook(ViewDestroyedHook fn)
        {
            if (!fn)
                return;
            view_destroyed_hooks_.push_back(std::move(fn));
        }

        // (曾有一个单参重载 notifySceneViewDestroyed(view_id),转发成 scene_key=0。
        //  零调用者 —— 唯一的通知点 RenderScene::removeView 一直传的是真实
        //  scene_key。随虚接口一并删除。)

        void notifySceneViewDestroyed(uint32_t scene_key, uint32_t view_id)
        {
            for (const auto& fn : view_destroyed_hooks_)
                fn(scene_key, view_id);
        }

        // ── Lifecycle ───────────────────────────────────────────────────

        [[nodiscard]] size_t size() const noexcept
        {
            return slots_.size();
        }

    private:
        using ErasedPtr = std::unique_ptr<void, void (*)(void*)>;

        struct Slot
        {
            ErasedPtr ptr{nullptr, +[](void*) {}};
            VkDescriptorSet (*ds_getter)(const void*){nullptr};
        };

        /// Take ownership of an already-constructed T and make it discoverable.
        /// The single publication point for direct construction and fallible factory owners.
        /// @return the slot index.
        template <typename T> uint32_t publishOwned(T* raw)
        {
            Slot s;
            s.ptr = ErasedPtr(raw, [](void* p) { delete static_cast<T*>(p); });
            if constexpr (detail::THasGetDescriptorSet<T>::value)
            {
                s.ds_getter = [](const void* p) -> VkDescriptorSet {
                    return static_cast<const T*>(p)->getDescriptorSet();
                };
            }
            const auto idx = static_cast<uint32_t>(slots_.emplace(std::move(s)));

            // Register the first instance of each type for find<T>() discovery.
            type_map_.try_emplace(lux::cxx::type_hash<T>(), idx);

            // 不再靠 is_base_of 自动登记每帧回调 —— 那是隐式的:新加一个继承了帧
            // 接口的资源就静默多出一个每帧调用。现在由**安装点**显式登记
            // (addBeginFrameHook / addViewDestroyedHook),要不要每帧驱动是装配的
            // 决定,不是数据类型的属性。

            return idx;
        }

        lux::cxx::AutoSparseSet<Slot> slots_;
        std::unordered_map<std::uint64_t, uint32_t> type_map_; // key = lux::cxx::type_hash<T>()
        std::array<std::vector<BeginFrameHook>, static_cast<size_t>(EUploadPhase::COUNT)> begin_frame_hooks_{};
        std::vector<ViewDestroyedHook> view_destroyed_hooks_{};
    };

    // ── ResourceHandle<T> out-of-line definitions ───────────────────────

    template <typename T> T* TResourceHandle<T>::get() const noexcept
    {
        return registry_ ? registry_->getAs<T>(idx_) : nullptr;
    }

    template <typename T> VkDescriptorSet TResourceHandle<T>::descriptorSet() const noexcept
    {
        return registry_ ? registry_->getDescriptorSet(idx_) : VkDescriptorSet{};
    }

    // GlobalResourceRegistry / SceneResourceRegistry 已删除。
    //(此处两个名字曾被一次机械改名同时覆盖成 "ResourceRegistry",让整句话变成
    // 幸存类型名重复两遍、什么也没说;按 git show 4536b47 的原文补回。)
    //
    // 它们曾是两个**空壳子类**(各自只有 `using Base::Base;`,零行为差异),存在的
    // 唯一作用是在类型系统里给作用域起个名。但一份资源是全局的还是场景的,取决于
    // **谁持有这个注册表** —— RenderContext 持有的那个就是全局的,RenderScene 持有
    // 的那个就是场景的。作用域是**组装**的属性,不是注册表类型的属性;让数据层的
    // 类型去声明它,就是把上层语义倒灌进下层。
    //
    // 作用域现由访问器名表达,一眼可辨且不可能说谎:
    //     RenderContext::globalRegistry()  → 全局
    //     RenderScene::resources()     → 场景
    // 两者返回的都是同一个 ResourceRegistry。

} // namespace lux::render
