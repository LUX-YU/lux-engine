#pragma once

#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <lux/engine/render/gpu/utils/Slot.hpp>

#include <cstdint>
#include <vector>
#include <limits>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <type_traits>
#include <concepts>
#include <optional>

namespace lux::render
{
    // Out-of-line VMA helper to keep vk_mem_alloc.h out of sinclude headers.
    LUX_FUNCTION_PUBLIC bool createGpuBufferVmaBuffer(
        VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        bool cpu_writable,
        VkBuffer* pBuffer,
        VmaAllocation* pAllocation,
        void** ppMapped
    );

    // Symmetric out-of-line destroy — lets callers that only manage a raw VMA
    // buffer (e.g. SpatialCullGrid's per-FIF mask ring) release it without
    // pulling vk_mem_alloc.h into their translation unit. No-op on a null buffer.
    LUX_FUNCTION_PUBLIC void
    destroyGpuBufferVmaBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);

    LUX_FUNCTION_PUBLIC void flushGpuBufferVmaAllocation(
        VmaAllocator allocator,
        VmaAllocation allocation,
        VkDeviceSize offset,
        VkDeviceSize size
    );

    LUX_FUNCTION_PUBLIC void invalidateGpuBufferVmaAllocation(
        VmaAllocator allocator,
        VmaAllocation allocation,
        VkDeviceSize offset,
        VkDeviceSize size
    );

    // =========================================================================
    //  1. Concepts & Enums
    // =========================================================================
    enum class EGpuBufferType
    {
        CPU_TO_GPU,
        GPU_ONLY
    };

    template <typename T>
    concept GpuBufferElement = std::is_trivially_copyable_v<T> && (sizeof(T) % 4 == 0);

    struct GpuBufferCreateInfo
    {
        DeviceContext* device_context = nullptr;
        DeferredDestroyQueue* deferred_queue = nullptr;
        uint32_t initial_capacity = 0;
        uint32_t slices = 1;
        bool allow_shader_write = false;
        bool clear_on_remove = false;        // Only used when HostMirror=true
        VkBufferUsageFlags buffer_usage = 0; // If 0, defaults to STORAGE_BUFFER_BIT
    };

    // =========================================================================
    //  2. Data Policies (Core of eliminating redundant data members)
    // =========================================================================

    // --- Policy: Slicing ---
    // Only stores the slices_ variable when IsSliced=true
    template <bool IsSliced> struct SlicePolicy;

    template <> struct SlicePolicy<true>
    {
    protected:
        uint32_t slices_ = 1;
        void initSlices(uint32_t n)
        {
            slices_ = std::max(1u, n);
        }

    public:
        [[nodiscard]] uint32_t slices() const
        {
            return slices_;
        }
    };

    template <> struct SlicePolicy<false>
    {
    protected:
        void initSlices(uint32_t)
        {
        }

    public:
        [[nodiscard]] constexpr uint32_t slices() const
        {
            return 1;
        }
    };

    // --- Policy: Mapping ---
    // Only CPU_TO_GPU stores the mapped_ pointer
    template <typename T, EGpuBufferType Type> struct MapPolicy;

    template <typename T> struct MapPolicy<T, EGpuBufferType::CPU_TO_GPU>
    {
    protected:
        void* mapped_ = nullptr;
        T* getMappedPtr() const
        {
            return static_cast<T*>(mapped_);
        }
    };

    template <typename T> struct MapPolicy<T, EGpuBufferType::GPU_ONLY>
    {
    protected:
        // GPU_ONLY mode has no member variables
        T* getMappedPtr() const
        {
            return nullptr;
        } // Only for internal logic unification; upper layers cannot call this
    };

    // --- Policy: Dirty Tracking ---
    // Determines whether dirty tracking is stored and the data structure used (vector vs single struct)
    struct DirtyRange
    {
        VkDeviceSize min;
        VkDeviceSize max;
    };

    template <EGpuBufferType Type, bool IsSliced> struct DirtyPolicy;

    // Case 1: GPU_ONLY -> no dirty tracking needed
    template <bool IsSliced> struct DirtyPolicy<EGpuBufferType::GPU_ONLY, IsSliced>
    {
    protected:
        void resetDirty(uint32_t)
        {
        }
        void markDirty(uint32_t, VkDeviceSize, VkDeviceSize)
        {
        }
        DirtyRange getDirty(uint32_t) const
        {
            return {0, 0};
        }
    };

    // Case 2: CPU_TO_GPU + Sliced -> requires vector
    template <> struct DirtyPolicy<EGpuBufferType::CPU_TO_GPU, true>
    {
    protected:
        std::vector<DirtyRange> dirty_state_;

        void resetDirty(uint32_t slices)
        {
            dirty_state_.assign(slices, {std::numeric_limits<VkDeviceSize>::max(), 0});
        }
        void markDirty(uint32_t slice, VkDeviceSize offset, VkDeviceSize size)
        {
            if (slice < dirty_state_.size())
            {
                auto& d = dirty_state_[slice];
                d.min = std::min(d.min, offset);
                d.max = std::max(d.max, offset + size);
            }
        }
        DirtyRange getDirty(uint32_t slice) const
        {
            return dirty_state_[slice];
        }
        DirtyRange& getDirtyRef(uint32_t slice)
        {
            return dirty_state_[slice];
        }
    };

    // Case 3: CPU_TO_GPU + Single -> only needs a single variable, no vector needed
    template <> struct DirtyPolicy<EGpuBufferType::CPU_TO_GPU, false>
    {
    protected:
        DirtyRange dirty_state_;

        void resetDirty(uint32_t)
        {
            dirty_state_ = {std::numeric_limits<VkDeviceSize>::max(), 0};
        }
        void markDirty(uint32_t, VkDeviceSize offset, VkDeviceSize size)
        {
            auto& d = dirty_state_;
            d.min = std::min(d.min, offset);
            d.max = std::max(d.max, offset + size);
        }
        DirtyRange getDirty(uint32_t) const
        {
            return dirty_state_;
        }
        DirtyRange& getDirtyRef(uint32_t)
        {
            return dirty_state_;
        }
    };

    // --- Policy: Host Mirror ---
    // When true, maintains a CPU-side std::vector<T> + epoch-based dirty tracking.
    // This is the strategy used by the former SlicedSSBO for persistent data with
    // random sparse updates (lights, materials, mesh info).
    template <typename T, bool HasMirror> struct HostMirrorPolicy;

    template <typename T> struct HostMirrorPolicy<T, false>
    {
    protected:
        void initHostMirror(uint32_t, uint32_t, bool)
        {
        }
        void resizeHostMirror(uint32_t, uint32_t)
        {
        }
        void clearHostMirror()
        {
        }
        void moveHostMirror(HostMirrorPolicy&&) noexcept
        {
        }
    };

    template <typename T> struct HostMirrorPolicy<T, true>
    {
    protected:
        std::vector<T> hm_values_;            // Host-side data copy (indexed by slot)
        std::vector<uint64_t> hm_elem_epoch_; // Per-element: epoch when last modified
        uint64_t hm_global_epoch_ = 1;
        std::vector<uint64_t> hm_slice_epoch_;   // Per-slice: synced up to this epoch
        std::vector<uint32_t> hm_dirty_indices_; // Indices modified since last full sync
        std::vector<uint64_t> hm_listed_ticket_; // Per-element dedup token
        std::vector<uint32_t>
            hm_upload_scratch_; // Reused per upload to coalesce dirty runs (no per-call heap alloc). (#H8)
        uint64_t hm_current_ticket_ = 1;
        bool hm_clear_on_remove_ = false;

        void initHostMirror(uint32_t capacity, uint32_t slices, bool clear_on_remove)
        {
            hm_clear_on_remove_ = clear_on_remove;
            hm_values_.resize(capacity);
            hm_elem_epoch_.resize(capacity, 0ull);
            hm_listed_ticket_.resize(capacity, 0ull);
            hm_slice_epoch_.assign(slices, 0ull);
            hm_global_epoch_ = 1;
            hm_current_ticket_ = 1;
            hm_dirty_indices_.clear();
        }

        void resizeHostMirror(uint32_t new_cap, uint32_t slices)
        {
            hm_values_.resize(new_cap);
            hm_elem_epoch_.resize(new_cap, 0ull);
            hm_listed_ticket_.resize(new_cap, 0ull);
            if (hm_slice_epoch_.size() != slices)
                hm_slice_epoch_.assign(slices, 0ull);
            std::fill(hm_slice_epoch_.begin(), hm_slice_epoch_.end(), hm_global_epoch_);
        }

        void clearHostMirror()
        {
            hm_values_.clear();
            hm_elem_epoch_.clear();
            hm_listed_ticket_.clear();
            hm_slice_epoch_.clear();
            hm_dirty_indices_.clear();
        }

        void moveHostMirror(HostMirrorPolicy&& o) noexcept
        {
            hm_values_ = std::move(o.hm_values_);
            hm_elem_epoch_ = std::move(o.hm_elem_epoch_);
            hm_global_epoch_ = o.hm_global_epoch_;
            hm_slice_epoch_ = std::move(o.hm_slice_epoch_);
            hm_dirty_indices_ = std::move(o.hm_dirty_indices_);
            hm_listed_ticket_ = std::move(o.hm_listed_ticket_);
            hm_current_ticket_ = o.hm_current_ticket_;
            hm_clear_on_remove_ = o.hm_clear_on_remove_;
        }

        void hmPushDirty(uint32_t idx)
        {
            if (idx >= hm_listed_ticket_.size())
                return;
            if (hm_listed_ticket_[idx] != hm_current_ticket_)
            {
                hm_dirty_indices_.push_back(idx);
                hm_listed_ticket_[idx] = hm_current_ticket_;
            }
        }

        bool hmAllSlicesSynced(uint64_t epoch) const noexcept
        {
            for (auto e : hm_slice_epoch_)
                if (e != epoch)
                    return false;
            return true;
        }
    };

    // =========================================================================
    //  3. Common Base (shared data for all buffer types)
    // =========================================================================
    class GpuBufferBase
    {
    protected:
        DeviceContext* device_ctx_ = nullptr;
        // (buffer, allocation, deferred-queue) folded into one move-only owner:
        // the triple travels together on move (closing the old moveFrom leak that
        // dropped the queue pointer) and the buffer's ONLY destruction path is the
        // FIF-gated queue — no inline vmaDestroyBuffer. (C1)
        FifOwnedAllocated<VkBuffer> buffer_owned_;

    public:
        /// Late-bind centralized deferred destroy queue (called after RenderContext creation).
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            buffer_owned_.setQueue(q);
        }

    protected:
        uint32_t stride_ = 0;
        uint32_t capacity_ = 0;
        uint32_t count_ = 0;
        uint32_t buffer_gen_ = 1;
        bool allow_shader_write_ = false;
        VkBufferUsageFlags buffer_usage_ = 0;

        // Slot management
        std::vector<uint8_t> alive_;
        std::vector<uint32_t> generations_;
        std::vector<uint32_t> free_;

        /// Move the current buffer+allocation into the deferred destroy queue.
        void retireCurrentBuffer()
        {
            buffer_owned_.reset();
        }

        [[nodiscard]] bool createVmaBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            bool cpu_writable,
            VkBuffer* pBuffer,
            VmaAllocation* pAllocation,
            void** ppMapped
        )
        {
            return createGpuBufferVmaBuffer(
                device_ctx_->vmaAllocator(),
                size,
                usage,
                cpu_writable,
                pBuffer,
                pAllocation,
                ppMapped
            );
        }
    };

    // =========================================================================
    //  4. Unified GpuBuffer Class
    // =========================================================================

    template <GpuBufferElement T, EGpuBufferType Type, bool IsSliced, bool HostMirror = false>
    class GpuBuffer : public GpuBufferBase,
                      public SlicePolicy<IsSliced>,
                      public MapPolicy<T, Type>,
                      public DirtyPolicy<Type, IsSliced>,
                      public HostMirrorPolicy<T, HostMirror>
    {
        // Compile-time policy flags
        static constexpr bool IsCpuWritable = (Type == EGpuBufferType::CPU_TO_GPU);
        static constexpr bool HasMirror = HostMirror;

        static_assert(
            !HasMirror || (IsCpuWritable && IsSliced),
            "HostMirror mode requires CPU_TO_GPU + Sliced (epoch dirty needs host copy and per-slice sync)");

    public:
        GpuBuffer() = default;
        explicit GpuBuffer(const GpuBufferCreateInfo& ci)
        {
            init(ci);
        }
        ~GpuBuffer()
        {
            destroy();
        }

        // Move only
        GpuBuffer(const GpuBuffer&) = delete;
        GpuBuffer& operator=(const GpuBuffer&) = delete;
        GpuBuffer(GpuBuffer&& o) noexcept
        {
            moveFrom(std::move(o));
        }
        GpuBuffer& operator=(GpuBuffer&& o) noexcept
        {
            if (this != &o)
            {
                destroy();
                moveFrom(std::move(o));
            }
            return *this;
        }

        // ---------- Init / Destroy ----------
        void init(const GpuBufferCreateInfo& ci)
        {
            assert(ci.device_context && "DeviceContext null");
            assert(!device_ctx_ && "Init called twice");

            device_ctx_ = ci.device_context;
            // Only override the destroy queue when the create-info actually carries
            // one. A queue may already have been late-bound via setDeferredQueue()
            // BEFORE init() — e.g. InstanceResources binds the scene's
            // DeferredDestroyQueue at scene construction but defers its streams'
            // init() to feature attachment (PagedGpuStream::init leaves
            // ci.deferred_queue null). Unconditionally assigning here would reset
            // queue_ back to null, making the FifOwnedAllocated retire a silent
            // no-op → the buffer leaks at teardown (VUID-vkDestroyDevice-05137).
            if (ci.deferred_queue)
                buffer_owned_.setQueue(ci.deferred_queue);
            allow_shader_write_ = ci.allow_shader_write;
            buffer_usage_ = ci.buffer_usage;
            stride_ = static_cast<uint32_t>(sizeof(T));

            this->initSlices(ci.slices);      // Call SlicePolicy
            this->resetDirty(this->slices()); // Call DirtyPolicy

            if constexpr (HasMirror)
            {
                this->initHostMirror(std::max(1u, ci.initial_capacity), this->slices(), ci.clear_on_remove);
            }

            if (ci.initial_capacity > 0)
            {
                [[maybe_unused]] bool ok = reserve(ci.initial_capacity, false);
                assert(ok && "GpuBuffer: initial reserve failed (VMA allocation)");
            }
        }

        void destroy() noexcept
        {
            // Retire through the FIF queue (the queue outlives this buffer — it is
            // RenderContext's first-declared / last-destroyed member). (C1)
            buffer_owned_.reset();

            if constexpr (IsCpuWritable)
            {
                this->mapped_ = nullptr; // Call MapPolicy
            }

            alive_.clear();
            generations_.clear();
            free_.clear();
            capacity_ = 0;
            count_ = 0;

            this->resetDirty(this->slices());

            if constexpr (HasMirror)
            {
                this->clearHostMirror();
            }
        }

        // ---------- Slot Management ----------

        [[nodiscard]] SlotHandle allocate()
        {
            if (!ensureCapacity())
                return SlotHandle{}; // growth failed -> invalid handle (C-1)
            uint32_t index = 0;
            if (!free_.empty())
            {
                index = free_.back();
                free_.pop_back();
            }
            else
            {
                index = count_++;
            }
            // Track high-water mark: count_ must always be >= (max used index + 1)
            // so that writeDescriptorTight() covers all live slots.
            if (index >= count_)
                count_ = index + 1;
            if (index >= alive_.size())
                alive_.resize(index + 1, 0); // Safety
            alive_[index] = 1;
            return SlotHandle{index, generations_[index]};
        }

        bool free(const SlotHandle& h)
        {
            if (!isAlive(h))
                return false;
            const uint32_t idx = h.index;
            alive_[idx] = 0;
            generations_[idx]++;
            free_.push_back(idx);
            return true;
        }

        [[nodiscard]] bool isAlive(const SlotHandle& h) const noexcept
        {
            return device_ctx_ && h.isValid() && h.index < capacity_ && alive_[h.index] &&
                   generations_[h.index] == h.gen;
        }

        // ---------- CPU Write (Concept Constraint) ----------
        // Constrained via requires: only available for CPU_TO_GPU buffers
        [[nodiscard]] T* mapped() noexcept
            requires IsCpuWritable
        {
            return this->getMappedPtr();
        }

        [[nodiscard]] T* mapped(uint32_t slice, const SlotHandle& h) noexcept
            requires IsCpuWritable
        {
            if (!isAlive(h))
                return nullptr;
            return getSlicePtr(slice) + h.index;
        }

        // Overload for single-slice (non-sliced) buffers
        [[nodiscard]] T* mapped(const SlotHandle& h) noexcept
            requires(IsCpuWritable && !IsSliced)
        {
            return mapped(0, h);
        }

        void write(uint32_t slice, const SlotHandle& h, const T& value)
            requires IsCpuWritable
        {
            T* ptr = mapped(slice, h);
            assert(ptr && "Invalid handle or buffer not mapped");
            if (!ptr)
                return;
            *ptr = value;

            VkDeviceSize offset = getSliceByteOffset(slice) + VkDeviceSize(h.index) * stride_;
            this->markDirty(slice, offset, stride_); // Call DirtyPolicy
        }

        void writeAllSlices(const SlotHandle& h, const T& value)
            requires(IsCpuWritable && IsSliced)
        {
            for (uint32_t s = 0; s < this->slices(); ++s)
                write(s, h, value);
        }

        // Overload for single-slice (non-sliced) buffers
        void write(const SlotHandle& h, const T& value)
            requires(IsCpuWritable && !IsSliced)
        {
            write(0, h, value);
        }

        // ---------- Flush (Concept Constraint) ----------
        // Flush memory without barrier (for batch processing)
        void flush(uint32_t slice = 0)
            requires IsCpuWritable
        {
            if (!buffer_owned_.valid())
                return;

            auto& d = this->getDirtyRef(slice);
            if (d.max <= d.min)
                return;

            VkDeviceSize size = d.max - d.min;
            vmaFlushAllocation(device_ctx_->vmaAllocator(), buffer_owned_.alloc(), d.min, size);

            // Note: Don't reset dirty here, caller should call resetDirty() after barrier
        }

        // Get barrier info without recording (for batch processing)
        std::optional<VkBufferMemoryBarrier2> getBarrier(uint32_t slice = 0) const
            requires IsCpuWritable
        {
            if (!buffer_owned_.valid())
                return std::nullopt;

            auto d = this->getDirty(slice);
            if (d.max <= d.min)
                return std::nullopt;

            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.buffer = buffer_owned_.get();
            barrier.offset = d.min;
            barrier.size = d.max - d.min;

            barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            if (allow_shader_write_)
                barrier.dstAccessMask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

            return barrier;
        }

        // Reset dirty state after barrier has been recorded
        void resetDirtySlice(uint32_t slice = 0)
            requires IsCpuWritable
        {
            auto& d = this->getDirtyRef(slice);
            d.min = std::numeric_limits<VkDeviceSize>::max();
            d.max = 0;
        }

        /**
         * @brief Mark a range of elements as dirty (for external / raw-pointer writes).
         *
         * Call this when data was written through raw mapped pointers (e.g. memcpy)
         * rather than through GpuBuffer::write(), so the flush/barrier path can
         * pick up the correct byte range.
         *
         * @param slice       The slice index.
         * @param first_index First element index within the slice.
         * @param count       Number of elements written.
         */
        void markDirtyRange(uint32_t slice, uint32_t first_index, uint32_t count)
            requires IsCpuWritable
        {
            if (count == 0)
                return;
            VkDeviceSize offset = getSliceByteOffset(slice) + VkDeviceSize(first_index) * stride_;
            VkDeviceSize size = VkDeviceSize(count) * stride_;
            this->markDirty(slice, offset, size);
        }

        // ---------- Staging Upload (GPU_ONLY Only) ----------
        void stageUpload(
            VkCommandBuffer cmd,
            VkBuffer stagingBuf,
            VkDeviceSize stagingOffset,
            uint32_t slice,
            uint32_t dstIndex,
            uint32_t count
        )
            requires(!IsCpuWritable)
        {
            if (count == 0)
                return;
            VkBufferCopy region{};
            region.srcOffset = stagingOffset;
            region.dstOffset = getSliceByteOffset(slice) + VkDeviceSize(dstIndex) * stride_;
            region.size = count * stride_;

            vkCmdCopyBuffer(cmd, stagingBuf, buffer_owned_.get(), 1, &region);
            recordBarrier(cmd, region.dstOffset, region.size);
        }

        // ---------- Capacity ----------
        [[nodiscard]] bool
        reserve(uint32_t min_capacity, bool preserve_data = true, VkCommandBuffer cmd = VK_NULL_HANDLE)
        {
            if (min_capacity <= capacity_)
                return true;

            const uint32_t old_cap = capacity_;
            const uint32_t new_cap = nextGrowCapacity(old_cap, min_capacity);
            VkDeviceSize new_size = VkDeviceSize(new_cap) * stride_ * this->slices();

            // 1. Prepare creation params
            VkBuffer new_buf = VK_NULL_HANDLE;
            VmaAllocation new_alloc = VK_NULL_HANDLE;
            void* new_ptr = nullptr;

            VkBufferUsageFlags usage = buffer_usage_ ? buffer_usage_ : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if constexpr (!IsCpuWritable)
                usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

            if (!this->createVmaBuffer(
                    new_size,
                    usage,
                    IsCpuWritable,
                    &new_buf,
                    &new_alloc,
                    IsCpuWritable ? &new_ptr : nullptr))
                return false;

            // 2. Migration (Only for CPU_TO_GPU)
            if constexpr (IsCpuWritable)
            {
                if (!preserve_data || old_cap == 0)
                {
                    // Zero-initialize all slices so GPU reads stale/uninitialized data on the
                    // very first use (e.g. before uploadDataSlice has visited every slice).
                    if (new_ptr)
                    {
                        std::memset(new_ptr, 0, static_cast<size_t>(new_size));
                        vmaFlushAllocation(device_ctx_->vmaAllocator(), new_alloc, 0, new_size);
                    }
                }
                else if (preserve_data && old_cap > 0)
                {
                    if constexpr (HasMirror)
                    {
                        // Mirror mode: copy from accurate host-side values_ (avoids slow WC read)
                        const uint32_t copy_n = std::min(new_cap, static_cast<uint32_t>(this->hm_values_.size()));
                        for (uint32_t s = 0; s < this->slices(); ++s)
                        {
                            char* base = static_cast<char*>(new_ptr) + VkDeviceSize(s) * new_cap * stride_;
                            for (uint32_t i = 0; i < copy_n; ++i)
                            {
                                std::memcpy(base + VkDeviceSize(i) * stride_, &this->hm_values_[i], sizeof(T));
                            }
                            if (copy_n < new_cap)
                            {
                                std::memset(
                                    base + VkDeviceSize(copy_n) * stride_,
                                    0,
                                    size_t(VkDeviceSize(new_cap - copy_n) * stride_)
                                );
                            }
                        }
                        vmaFlushAllocation(device_ctx_->vmaAllocator(), new_alloc, 0, new_size);
                    }
                    else if (this->mapped_)
                    {
                        // Non-mirror: slow WC read from old mapped memory
                        for (uint32_t s = 0; s < this->slices(); ++s)
                        {
                            auto old_off = VkDeviceSize(s) * old_cap * stride_;
                            auto new_off = VkDeviceSize(s) * new_cap * stride_;
                            std::memcpy(
                                static_cast<char*>(new_ptr) + new_off,
                                static_cast<char*>(this->mapped_) + old_off,
                                old_cap * stride_
                            );
                        }
                        // Flush the migrated bytes like the zero-init and mirror
                        // paths do — resetDirty() below clears the dirty ranges, so
                        // without this the preserved region is never flushed and the
                        // GPU reads stale data on non-coherent host memory. (P1#8)
                        vmaFlushAllocation(device_ctx_->vmaAllocator(), new_alloc, 0, new_size);
                    }
                }
            }
            else
            {
                // GPU_ONLY buffers: use GPU-side copy if a command buffer is provided.
                if (cmd != VK_NULL_HANDLE && old_cap > 0 && buffer_owned_.valid())
                {
                    VkBufferCopy region{};
                    region.srcOffset = 0;
                    region.dstOffset = 0;
                    region.size = VkDeviceSize(old_cap) * stride_ * this->slices();
                    vkCmdCopyBuffer(cmd, buffer_owned_.get(), new_buf, 1, &region);

                    // Barrier: transfer write on new_buf must finish before shader reads
                    VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                    bar.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    bar.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
                    bar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
                    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.buffer = new_buf;
                    bar.offset = 0;
                    bar.size = region.size;

                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dep.bufferMemoryBarrierCount = 1;
                    dep.pBufferMemoryBarriers = &bar;
                    vkCmdPipelineBarrier2(cmd, &dep);
                }
                else
                {
                    // No command buffer: callers must re-upload after reserve().
                    assert(!preserve_data && "GPU_ONLY buffer reserve() without cmd cannot preserve data");
                }
            }

            // 3. Swap — adopt() retires the old buffer (in-flight command buffers
            // may still reference it) and takes ownership of the new one atomically.
            buffer_owned_.adopt(new_buf, new_alloc);
            capacity_ = new_cap;
            buffer_gen_++;

            if constexpr (IsCpuWritable)
                this->mapped_ = new_ptr;

            // 4. Update Slots
            alive_.resize(capacity_, 0);
            generations_.resize(capacity_, 1);
            // Push new slots in reverse order so allocate() (pop_back) yields lowest indices first,
            // keeping live data contiguous and improving GPU cache locality.
            for (uint32_t i = new_cap; i-- > old_cap;)
                free_.push_back(i);

            // 5. Reset Dirty (Only CPU_TO_GPU)
            if constexpr (IsCpuWritable)
            {
                this->resetDirty(this->slices());
            }

            // 6. Resize host mirror (if enabled)
            if constexpr (HasMirror)
            {
                this->resizeHostMirror(new_cap, this->slices());
            }
            return true;
        }

        // ---------- Getters ----------
        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return buffer_owned_.get();
        }
        [[nodiscard]] uint32_t capacity() const noexcept
        {
            return capacity_;
        }
        [[nodiscard]] uint32_t count() const noexcept
        {
            return count_;
        }
        [[nodiscard]] uint32_t bufferGeneration() const noexcept
        {
            return buffer_gen_;
        }
        [[nodiscard]] uint32_t stride() const noexcept
        {
            return stride_;
        }

        /// Device address of the whole buffer or one logical FIF slice. The
        /// caller must opt the buffer into
        /// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT; optional consumers receive
        /// zero when that capability is absent.
        [[nodiscard]] VkDeviceAddress deviceAddress(uint32_t slice = 0u) const noexcept
        {
            const bool is_missing_device = device_ctx_ == nullptr;
            const bool is_missing_buffer = buffer_owned_.get() == VK_NULL_HANDLE;
            const bool has_device_address_usage = (buffer_usage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0u;
            const bool has_valid_buffer = !is_missing_device && !is_missing_buffer && has_device_address_usage;
            const bool is_invalid_slice = has_valid_buffer && slice >= this->slices();
            const bool is_invalid_request = !has_valid_buffer || is_invalid_slice;
            if (is_invalid_request)
            {
                return 0u;
            }
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer_owned_.get();
            const VkDeviceAddress base = vkGetBufferDeviceAddress(device_ctx_->logicalDevice(), &info);
            return base + getSliceByteOffset(slice);
        }

        void writeDescriptor(
            VkDescriptorSet set,
            uint32_t binding,
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        ) const
        {
            if (!buffer_owned_.valid())
                return;
            VkDescriptorBufferInfo info{buffer_owned_.get(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = type;
            w.descriptorCount = 1;
            w.pBufferInfo = &info;
            vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 1, &w, 0, nullptr);
        }

        /// Write a descriptor scoped to a specific per-frame slice.
        /// The descriptor covers exactly one slice's capacity (offset + size),
        /// so the shader's 0-based element indices map to the correct region.
        void writeDescriptorSlice(
            VkDescriptorSet set,
            uint32_t binding,
            uint32_t slice,
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        ) const
        {
            if (!buffer_owned_.valid())
                return;
            const VkDeviceSize offset = getSliceByteOffset(slice);
            const VkDeviceSize size = VkDeviceSize(capacity_) * stride_;
            VkDescriptorBufferInfo info{buffer_owned_.get(), offset, std::max(size, VkDeviceSize(stride_))};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = type;
            w.descriptorCount = 1;
            w.pBufferInfo = &info;
            vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 1, &w, 0, nullptr);
        }

        [[nodiscard]] uint32_t baseIndexForSlice(uint32_t slice) const noexcept
        {
            if (slice >= this->slices())
                return 0;
            return slice * capacity_;
        }

        // =====================================================================
        // Host Mirror API (requires HostMirror=true)
        // Data-aware slot operations with epoch-based dirty tracking.
        // Used by persistent resources (lights, materials, mesh info) that are
        // modified randomly and need selective per-slice GPU sync.
        // =====================================================================

        /// Allocate slot + store value in host mirror, mark epoch-dirty.
        SlotHandle add(const T& v)
            requires HasMirror
        {
            if (!ensureCapacity())
                return SlotHandle{}; // growth failed -> invalid handle (C-1)
            uint32_t index = 0;
            if (!free_.empty())
            {
                index = free_.back();
                free_.pop_back();
            }
            else
            {
                index = count_++;
            }
            // Track high-water mark: count_ must always be >= (max used index + 1)
            // so that writeDescriptorTight() covers all live slots.
            if (index >= count_)
                count_ = index + 1;
            if (index >= alive_.size())
                alive_.resize(index + 1, 0);
            alive_[index] = 1;

            this->hm_values_[index] = v;
            this->hm_elem_epoch_[index] = ++this->hm_global_epoch_;
            this->hmPushDirty(index);
            return SlotHandle{index, generations_[index]};
        }

        /// Remove slot, optionally zeroing GPU data (if clear_on_remove).
        bool remove(const SlotHandle& h)
            requires HasMirror
        {
            if (!isAlive(h))
                return false;
            const uint32_t idx = h.index;

            if (this->hm_clear_on_remove_)
            {
                this->hm_values_[idx] = T{};
                this->hm_elem_epoch_[idx] = ++this->hm_global_epoch_;
                this->hmPushDirty(idx);
            }
            alive_[idx] = 0;
            generations_[idx]++;
            free_.push_back(idx);
            return true;
        }

        /// Modify value in host mirror and mark epoch-dirty.
        bool modify(const SlotHandle& h, const T& nv)
            requires HasMirror
        {
            if (!isAlive(h))
                return false;
            const uint32_t idx = h.index;
            this->hm_values_[idx] = nv;
            this->hm_elem_epoch_[idx] = ++this->hm_global_epoch_;
            this->hmPushDirty(idx);
            return true;
        }

        /// Internal owner maintenance by a validated live raw slot (for
        /// scene-wide transactions such as spatial-origin rebasing).
        bool modifyAt(uint32_t index, const T& value)
            requires HasMirror
        {
            if (!isSlotAlive(index))
                return false;
            this->hm_values_[index] = value;
            this->hm_elem_epoch_[index] = ++this->hm_global_epoch_;
            this->hmPushDirty(index);
            return true;
        }

        /// Get mutable pointer to host value. Call touch() after modifying.
        T* get(const SlotHandle& h)
            requires HasMirror
        {
            return isAlive(h) ? &this->hm_values_[h.index] : nullptr;
        }

        const T* get(const SlotHandle& h) const
            requires HasMirror
        {
            return isAlive(h) ? &this->hm_values_[h.index] : nullptr;
        }

        /// Mark slot as modified without changing data (for after raw get() edits).
        bool touch(const SlotHandle& h)
            requires HasMirror
        {
            if (!isAlive(h))
                return false;
            this->hm_elem_epoch_[h.index] = ++this->hm_global_epoch_;
            this->hmPushDirty(h.index);
            return true;
        }

        /// Upload only changed elements from host mirror to GPU for one slice,
        /// emitting the post-write barrier inline. Collect / coalesce / flush is
        /// shared with uploadDataSliceDeferred() via uploadDataSliceInternal().
        void uploadDataSlice(VkCommandBuffer cmd, uint32_t slice)
            requires HasMirror
        {
            const auto barrier = uploadDataSliceInternal(slice);
            if (barrier)
                recordBarrier(cmd, barrier->offset, barrier->size);
        }

        /// Upload changed elements and RETURN the post-write barrier instead of
        /// emitting it, for external merging (e.g. via
        /// TransferScheduler::submitExtraPostBarrier). Returns a valid barrier
        /// when work was performed, std::nullopt otherwise.
        std::optional<VkBufferMemoryBarrier2> uploadDataSliceDeferred(uint32_t slice)
            requires HasMirror
        {
            return uploadDataSliceInternal(slice);
        }

    private:
        /// Shared core of uploadDataSlice() / uploadDataSliceDeferred(): collect
        /// epoch-dirty elements for @p slice, coalesce into runs, memcpy + flush,
        /// advance the slice epoch, and return the post-write barrier (offset/size
        /// + src/dst masks) when work was done — else std::nullopt. The two public
        /// methods differ ONLY in whether they emit that barrier inline or hand it
        /// back. Uses sorted run-length merging to batch the memcpys.
        std::optional<VkBufferMemoryBarrier2> uploadDataSliceInternal(uint32_t slice)
            requires HasMirror
        {
            assert(slice < this->slices() && "slice out of range");
            if (slice >= this->slices())
                return std::nullopt;

            const uint64_t since = this->hm_slice_epoch_[slice];

            // ── Steady-state fast-exit ──
            // If this slice is already synced to the global epoch, no element can
            // have elem_epoch > since (global is the max), so the collect below
            // would always produce an empty set. Returning here skips the
            // O(capacity) fallback scan that otherwise runs every idle frame for
            // every host-mirrored SlicedSSBO (5 family SSBOs + light SSBOs). The
            // dirty-clear / ticket-bump bookkeeping in the empty branch was already
            // performed by whichever call brought the last slice up to the global
            // epoch, so nothing needed is skipped. (#H8)
            if (since == this->hm_global_epoch_)
                return std::nullopt;

            char* base = static_cast<char*>(this->mapped_);
            const VkDeviceSize slice_base = getSliceByteOffset(slice);

            // ── Collect applicable dirty indices, filtered by epoch ──
            // Reuse a member scratch buffer to avoid a per-call heap allocation.
            std::vector<uint32_t>& filtered = this->hm_upload_scratch_;
            filtered.clear();
            if (!this->hm_dirty_indices_.empty())
            {
                for (uint32_t idx : this->hm_dirty_indices_)
                {
                    if (idx<capacity_&& this->hm_elem_epoch_[idx]> since)
                        filtered.push_back(idx);
                }
            }
            else
            {
                // Fallback: scan all alive slots by epoch
                for (uint32_t idx = 0; idx < capacity_; ++idx)
                {
                    if (alive_[idx] && this->hm_elem_epoch_[idx] > since)
                        filtered.push_back(idx);
                }
            }

            if (filtered.empty())
            {
                this->hm_slice_epoch_[slice] = this->hm_global_epoch_;
                if (this->hmAllSlicesSynced(this->hm_global_epoch_))
                {
                    this->hm_dirty_indices_.clear();
                    this->hm_current_ticket_++;
                }
                return std::nullopt;
            }

            // ── Sort + coalesce into runs ──
            std::sort(filtered.begin(), filtered.end());

            VkDeviceSize min_abs = std::numeric_limits<VkDeviceSize>::max();
            VkDeviceSize max_abs = 0;

            uint32_t i = 0;
            while (i < static_cast<uint32_t>(filtered.size()))
            {
                const uint32_t first = filtered[i];
                uint32_t end = first + 1;
                ++i;
                while (i < static_cast<uint32_t>(filtered.size()) && filtered[i] == end)
                {
                    ++end;
                    ++i;
                }
                const uint32_t count = end - first;
                const VkDeviceSize abs_off = slice_base + VkDeviceSize(first) * stride_;
                const VkDeviceSize bytes = VkDeviceSize(count) * sizeof(T);
                std::memcpy(base + abs_off, &this->hm_values_[first], bytes);
                min_abs = std::min(min_abs, abs_off);
                max_abs = std::max(max_abs, abs_off + bytes);
            }

            std::optional<VkBufferMemoryBarrier2> result;
            if (max_abs > min_abs)
            {
                vmaFlushAllocation(device_ctx_->vmaAllocator(), buffer_owned_.alloc(), min_abs, max_abs - min_abs);

                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                barrier.buffer = buffer_owned_.get();
                barrier.offset = min_abs;
                barrier.size = max_abs - min_abs;
                if constexpr (IsCpuWritable)
                {
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
                }
                else
                {
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                }
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                if (allow_shader_write_)
                    barrier.dstAccessMask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                result = barrier;
            }

            this->hm_slice_epoch_[slice] = this->hm_global_epoch_;
            if (this->hmAllSlicesSynced(this->hm_global_epoch_))
            {
                this->hm_dirty_indices_.clear();
                this->hm_current_ticket_++;
            }
            return result;
        }

    public:
        /// Write tight SSBO descriptor scoped to a specific slice's live elements.
        void writeDescriptorTight(VkDescriptorSet set, uint32_t binding, uint32_t slice = 0) const
            requires HasMirror
        {
            if (!buffer_owned_.valid())
                return;
            const VkDeviceSize offset = getSliceByteOffset(slice);
            const VkDeviceSize size = std::max<VkDeviceSize>(VkDeviceSize(count_) * stride_, stride_);
            VkDescriptorBufferInfo info{buffer_owned_.get(), offset, size};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo = &info;
            vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 1, &w, 0, nullptr);
        }

        /// Clear all slots and zero GPU memory.
        void clearAll()
            requires HasMirror
        {
            for (auto& g : generations_)
                g++;
            std::fill(alive_.begin(), alive_.end(), 0);
            free_.clear();
            for (uint32_t i = 0; i < capacity_; ++i)
                free_.push_back(i);
            count_ = 0;

            std::fill(this->hm_values_.begin(), this->hm_values_.end(), T{});
            if (buffer_owned_.valid())
            {
                auto vma = device_ctx_->vmaAllocator();
                for (uint32_t s = 0; s < this->slices(); ++s)
                {
                    char* p = static_cast<char*>(this->mapped_) + getSliceByteOffset(s);
                    std::memset(p, 0, size_t(VkDeviceSize(capacity_) * stride_));
                }
                vmaFlushAllocation(vma, buffer_owned_.alloc(), 0, VK_WHOLE_SIZE);
            }

            this->hm_global_epoch_++;
            std::fill(this->hm_slice_epoch_.begin(), this->hm_slice_epoch_.end(), 0ull);
            this->hm_dirty_indices_.clear();
            std::fill(this->hm_listed_ticket_.begin(), this->hm_listed_ticket_.end(), 0ull);
            this->hm_current_ticket_++;
        }

        [[nodiscard]] uint64_t globalEpoch() const noexcept
            requires HasMirror
        {
            return this->hm_global_epoch_;
        }

        /// Read-only access to a host mirror value by raw index.
        /// The caller is responsible for ensuring index < count().
        [[nodiscard]] const T& hostValue(uint32_t index) const
            requires HasMirror
        {
            return this->hm_values_[index];
        }

        /// Check whether a raw slot index is alive.
        [[nodiscard]] bool isSlotAlive(uint32_t index) const noexcept
        {
            return index < capacity_ && alive_[index];
        }

    private:
        VkDeviceSize getSliceByteOffset(uint32_t slice) const
        {
            return VkDeviceSize(slice) * VkDeviceSize(capacity_) * stride_;
        }

        T* getSlicePtr(uint32_t slice) const
            requires IsCpuWritable
        {
            return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this->getMappedPtr()) + getSliceByteOffset(slice));
        }

        static uint32_t nextPow2(uint32_t v)
        {
            if (v <= 1)
                return 1;
            --v;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            return ++v;
        }

        /// Unified growth formula: double the old capacity, at least min_required,
        /// rounded up to a 64-element boundary (minimum 64).
        static uint32_t nextGrowCapacity(uint32_t old_cap, uint32_t min_required)
        {
            constexpr uint32_t kAlign = 64;
            const uint32_t raw = std::max(min_required, old_cap * 2);
            const uint32_t aligned = (raw + kAlign - 1) & ~(kAlign - 1);
            return std::max(aligned, kAlign);
        }

        /// @return false if a new slot is needed but the buffer could not grow
        /// (VMA allocation failed under memory pressure). Callers MUST stop and
        /// return an invalid handle — continuing would index past the slot arrays
        /// (generations_/hm_values_/hm_elem_epoch_ are NOT grown on the failure
        /// path), an OOB read/write the release build's missing assert would let
        /// through. (C-1)
        [[nodiscard]] bool ensureCapacity()
        {
            if (!free_.empty())
                return true;
            if (count_ < capacity_)
                return true;
            const bool ok = reserve(nextGrowCapacity(capacity_, capacity_ + 1), true);
            assert(ok && "GpuBuffer: ensureCapacity failed to grow buffer");
            return ok;
        }

        void recordBarrier(VkCommandBuffer cmd, VkDeviceSize offset, VkDeviceSize size) const
        {
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.buffer = buffer_owned_.get();
            barrier.offset = offset;
            barrier.size = size;

            if constexpr (IsCpuWritable)
            {
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            }
            else
            {
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            }

            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            if (allow_shader_write_)
                barrier.dstAccessMask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // Move implementation
        void moveFrom(GpuBuffer&& o) noexcept
        {
            // Move Base. buffer_owned_ carries (buffer, allocation, queue) as ONE
            // unit — this is what closes the old moveFrom leak, where the queue
            // pointer was NOT transferred so a moved-into buffer's later
            // retireCurrentBuffer() silently no-op'd and leaked the old buffer. (C1)
            device_ctx_ = o.device_ctx_;
            buffer_owned_ = std::move(o.buffer_owned_);
            stride_ = o.stride_;
            capacity_ = o.capacity_;
            count_ = o.count_;
            buffer_gen_ = o.buffer_gen_;
            allow_shader_write_ = o.allow_shader_write_;
            buffer_usage_ = o.buffer_usage_;
            alive_ = std::move(o.alive_);
            generations_ = std::move(o.generations_);
            free_ = std::move(o.free_);

            // Move Policies
            if constexpr (IsSliced)
                this->slices_ = o.slices_;
            if constexpr (IsCpuWritable)
            {
                this->mapped_ = o.mapped_;
                if constexpr (IsSliced)
                    this->dirty_state_ = std::move(o.dirty_state_);
                else
                    this->dirty_state_ = o.dirty_state_;
            }
            if constexpr (HasMirror)
            {
                this->moveHostMirror(std::move(o));
            }

            // Nullify source (buffer_owned_ already emptied by its own move)
            o.capacity_ = 0;
            o.count_ = 0;
            if constexpr (IsCpuWritable)
                o.mapped_ = nullptr;
        }
    };

    // =========================================================================
    //  5. Type Aliases (The clean API)
    // =========================================================================

    /// Dynamic SSBO: CPU-writable, sliced for multi-frame (Instance transforms, etc.)
    /// Direct mapped writes, per-slice range dirty tracking.
    template <typename T> using DynamicSSBO = GpuBuffer<T, EGpuBufferType::CPU_TO_GPU, true, false>;

    /// Static buffer: GPU-only, no mapping, no slicing (Mesh VBO/IBO).
    template <typename T> using StaticBuffer = GpuBuffer<T, EGpuBufferType::GPU_ONLY, false, false>;

    /// Single-frame staging: CPU-writable, not sliced.
    template <typename T> using StagingSSBO = GpuBuffer<T, EGpuBufferType::CPU_TO_GPU, false, false>;

    /// SlicedSSBO: CPU-writable, sliced, WITH host mirror + epoch dirty tracking.
    /// This is the unified replacement for the old standalone SlicedSSBO class.
    /// API: add(), remove(), modify(), get(), touch(), uploadDataSlice(), clearAll(), globalEpoch().
    template <typename T> using SlicedSSBO = GpuBuffer<T, EGpuBufferType::CPU_TO_GPU, true, true>;

    // =========================================================================
    //  6. SSBOInitConfig — convenience config that converts to GpuBufferCreateInfo
    // =========================================================================
    struct SSBOInitConfig
    {
        DeviceContext* device_context = nullptr;
        DeferredDestroyQueue* deferred_queue = nullptr;
        uint32_t initial_dense_capacity = 1024;
        uint32_t slices = 1;
        bool allow_shader_write = false;
        bool clear_on_remove = false;

        /// Implicit conversion so callers can pass SSBOInitConfig to init().
        operator GpuBufferCreateInfo() const
        {
            return GpuBufferCreateInfo{
                .device_context = device_context,
                .deferred_queue = deferred_queue,
                .initial_capacity = initial_dense_capacity,
                .slices = slices,
                .allow_shader_write = allow_shader_write,
                .clear_on_remove = clear_on_remove};
        }
    };

} // namespace lux::render
