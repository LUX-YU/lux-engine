#pragma once
/**
 * @file FifOwned.hpp
 * @brief Move-only RAII owner of a Vulkan handle whose ONLY destruction path is
 *        the frames-in-flight gated DeferredDestroyQueue.
 *
 * Architectural constraint (root-cause class C1, "in-flight resource lifetime"):
 * the engine already has the correct primitive — DeferredDestroyQueue tags a
 * retired handle with the current frame serial and destroys it only after the
 * GPU has finished the frames that could still reference it. The recurring bug
 * class is that calling the queue is a *convention*: call sites reach past it to
 * an inline vkDestroy / vmaDestroy call while frames N-1/N-2 may still use the
 * object (texture removeTexture, runtime removeFeature samplers, etc.). FifOwned makes
 * the safe path the ONLY representable path:
 *
 *   - There is no inline-destroy method and no way to extract the handle as an
 *     owning value (only a non-owning get() for descriptor writes / binding).
 *   - The handle is always retired through the queue on reset / destruction /
 *     move-assignment, so "destroy a still-in-flight object" cannot be written.
 *   - A feature/resource that holds a FifOwned member retires its GPU object
 *     correctly by simply being destroyed — no onDetach discipline required.
 *
 * Lifetime contract: the DeferredDestroyQueue must outlive every FifOwned that
 * retires into it. This holds by construction in lux-engine — RenderContext owns
 * the queue and is destroyed AFTER all scenes/features (RenderServer destroys
 * renderer_ before render_ctx_, see RenderServer shutdown), and ~RenderContext
 * flushAll()s the queue, so handles retired during feature teardown are
 * destroyed by that final flush. Retire is render-thread-only (the queue is not
 * thread-safe), so FifOwned is for render-thread-owned handles; objects created
 * on upload worker threads must keep using the explicit completion path.
 */

#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>

#include <vulkan/vulkan.h>

namespace lux::render
{
    // Trait: how to retire a single (allocation-less) Vulkan handle of type H.
    template <class H> struct FifRetireTraits;

    template <> struct FifRetireTraits<VkSampler>
    {
        static void retire(DeferredDestroyQueue& q, VkSampler h)
        {
            q.retireSampler(h);
        }
    };
    template <> struct FifRetireTraits<VkImageView>
    {
        static void retire(DeferredDestroyQueue& q, VkImageView h)
        {
            q.retireImageView(h);
        }
    };

    /**
     * @brief Move-only owner of a single Vulkan handle, retired through the
     *        DeferredDestroyQueue and never destroyed inline.
     *
     * For handles that also carry a VmaAllocation (VkBuffer / VkImage) use the
     * forthcoming FifOwnedAllocated<H> sibling — those are migrated in a later
     * increment (GpuBufferBase / BindlessCombinedSet image+view).
     */
    template <class H> class FifOwned
    {
    public:
        FifOwned() = default;

        /// Take ownership of @p handle; it will be retired through @p queue.
        /// @p queue must be non-null whenever @p handle is non-null and must
        /// outlive this FifOwned (see file-level lifetime contract).
        FifOwned(DeferredDestroyQueue* queue, H handle) noexcept : queue_(queue), handle_(handle)
        {
        }

        FifOwned(const FifOwned&) = delete;
        FifOwned& operator=(const FifOwned&) = delete;

        FifOwned(FifOwned&& o) noexcept : queue_(o.queue_), handle_(o.handle_)
        {
            o.handle_ = VK_NULL_HANDLE; // source no longer owns; its retire() is a no-op
        }

        FifOwned& operator=(FifOwned&& o) noexcept
        {
            if (this != &o)
            {
                retire(); // retire whatever we currently hold
                queue_ = o.queue_;
                handle_ = o.handle_;
                o.handle_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        ~FifOwned()
        {
            retire();
        }

        /// Non-owning handle for descriptor writes / pipeline binding.
        [[nodiscard]] H get() const noexcept
        {
            return handle_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return handle_ != VK_NULL_HANDLE;
        }

        /// Retire the held handle now (queued behind the current frame fence) and
        /// become empty. Idempotent. Use to release promptly on detach instead of
        /// waiting for destruction.
        void reset() noexcept
        {
            retire();
        }

    private:
        void retire() noexcept
        {
            if (handle_ != VK_NULL_HANDLE && queue_ != nullptr)
                FifRetireTraits<H>::retire(*queue_, handle_);
            handle_ = VK_NULL_HANDLE;
        }

        DeferredDestroyQueue* queue_ = nullptr;
        H handle_ = VK_NULL_HANDLE;
    };

    // Trait: how to retire a (handle + VmaAllocation) pair of type H.
    template <class H> struct FifRetireAllocTraits;

    template <> struct FifRetireAllocTraits<VkBuffer>
    {
        static void retire(DeferredDestroyQueue& q, VkBuffer h, VmaAllocation a)
        {
            q.retireBuffer(h, a);
        }
    };
    template <> struct FifRetireAllocTraits<VkImage>
    {
        static void retire(DeferredDestroyQueue& q, VkImage h, VmaAllocation a)
        {
            q.retireImage(h, a);
        }
    };

    /**
     * @brief Move-only owner of a Vulkan handle that carries a VmaAllocation
     *        (VkBuffer / VkImage), retired through the DeferredDestroyQueue.
     *
     * Same contract as FifOwned, plus it owns the allocation so the (handle,
     * allocation, queue) triple moves as ONE unit — which structurally closes the
     * GpuBuffer moveFrom leak (a move that forgot to carry the queue pointer left
     * a later retire as a silent no-op). The queue may be late-bound (setQueue)
     * before the first handle is adopted, matching GpuBuffer's setDeferredQueue.
     */
    template <class H> class FifOwnedAllocated
    {
    public:
        FifOwnedAllocated() = default;
        FifOwnedAllocated(DeferredDestroyQueue* queue, H handle, VmaAllocation alloc) noexcept
            : queue_(queue), handle_(handle), alloc_(alloc)
        {
        }

        FifOwnedAllocated(const FifOwnedAllocated&) = delete;
        FifOwnedAllocated& operator=(const FifOwnedAllocated&) = delete;

        FifOwnedAllocated(FifOwnedAllocated&& o) noexcept : queue_(o.queue_), handle_(o.handle_), alloc_(o.alloc_)
        {
            o.handle_ = VK_NULL_HANDLE;
            o.alloc_ = VK_NULL_HANDLE;
        }
        FifOwnedAllocated& operator=(FifOwnedAllocated&& o) noexcept
        {
            if (this != &o)
            {
                retire();
                queue_ = o.queue_;
                handle_ = o.handle_;
                alloc_ = o.alloc_;
                o.handle_ = VK_NULL_HANDLE;
                o.alloc_ = VK_NULL_HANDLE;
            }
            return *this;
        }
        ~FifOwnedAllocated()
        {
            retire();
        }

        /// Late-bind the destroy queue. Must be set before the first adopt().
        void setQueue(DeferredDestroyQueue* q) noexcept
        {
            queue_ = q;
        }

        /// Retire the currently-held handle (if any) and take ownership of a new
        /// one. Used by the resize/migration path after the new buffer is created
        /// and the old one's data has been copied out.
        void adopt(H handle, VmaAllocation alloc) noexcept
        {
            retire();
            handle_ = handle;
            alloc_ = alloc;
        }

        [[nodiscard]] H get() const noexcept
        {
            return handle_;
        }
        [[nodiscard]] VmaAllocation alloc() const noexcept
        {
            return alloc_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return handle_ != VK_NULL_HANDLE;
        }

        /// Retire the held handle now (queued behind the current frame fence).
        void reset() noexcept
        {
            retire();
        }

    private:
        void retire() noexcept
        {
            if (handle_ != VK_NULL_HANDLE && queue_ != nullptr)
                FifRetireAllocTraits<H>::retire(*queue_, handle_, alloc_);
            handle_ = VK_NULL_HANDLE;
            alloc_ = VK_NULL_HANDLE;
        }

        DeferredDestroyQueue* queue_ = nullptr;
        H handle_ = VK_NULL_HANDLE;
        VmaAllocation alloc_ = VK_NULL_HANDLE;
    };

} // namespace lux::render
