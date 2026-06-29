#pragma once
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlot.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/render/resources/mesh/MeshSectionTable.hpp>
#include <lux/engine/render/resources/memory/PagedGpuStream.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceCapacityMonitor.hpp>
#include <lux/engine/render/core/RenderObjectTypes.hpp>
#include <lux/engine/function/visibility.h>

// Transfer subsystem forward declaration
namespace lux::render { class TransferScheduler; class SceneDescriptorArena; }

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    // =========================================================================
    //  GPU-compatible struct definitions (std430 layout)
    // =========================================================================
    /// Transform stream — read by vertex shader (column-major mat4).
    struct alignas(16) InstanceTransform
    {
        float world[16]; // 64 bytes
    };
    static_assert(sizeof(InstanceTransform) == 64);

    struct alignas(16) InstanceTransformPrev
    {
        float world_prev[16]; // 64 bytes
    };
    static_assert(sizeof(InstanceTransformPrev) == 64);

    /// Cull metadata — read by compute cull shader. Carries one MDC per LOD
    /// level: the cull shader picks the LOD from screen coverage and appends the
    /// instance to lod_mdc[lod]. lod_count==0 marks an unregistered slot.
    struct alignas(16) InstanceCullMeta
    {
        float bsphere[4];      // xyz=center, w=radius (w<0 → free/tombstone)
        uint32_t bucket_id;
        uint32_t lod_count;    ///< valid lod_mdc[] entries (0 → unregistered)
        uint32_t lod_mdc[4];   ///< MDC index per LOD level (len must be >= kMaxMeshLod)
        uint32_t _pad[2];      // 48 bytes total
    };
    static_assert(sizeof(InstanceCullMeta) == 48);

    /// Per-instance properties — read by vertex/fragment shader.
    ///
    /// vertex_pool_id / vertex_base / vertex_count fields let vertex
    /// shaders resolve any drawable's vertex source uniformly via:
    ///
    ///   Vertex v = vertex_pools[p.vertex_pool_id].data[p.vertex_base + gl_VertexIndex];
    struct alignas(16) InstanceProperty
    {
        uint32_t object_id;
        uint32_t layer_mask;
        uint32_t flags;
        uint32_t material_type; // canonical: (family_id << 12) | shading_model_id
        uint32_t material_index;
        uint32_t transform_index; // normally == slot index (identity mapping)
        uint32_t pass_and_geometry; // low16 pass_mask, next8 geometry_kind
        uint32_t user_meta_index; //                                    32 B mark
        // ---- vertex source (see IVertexSource.hpp) ----
        // Default ~0u so any shader reading this without explicit init can
        // detect "no bindless source" cleanly. writeRenderable() (added
        // alongside RenderableComponent) will populate it; until then,
        // existing write paths use aggregate / memset zero-init and the
        // legacy vertex-attribute path keeps working.
        uint32_t vertex_pool_id = ~0u; // index into VertexPoolRegistry bindless array
        uint32_t vertex_base    = 0;   // first vertex within the pool (honest output base)
        uint32_t vertex_count   = 0;   // vertex count for this draw
        // Bias subtracted from gl_VertexIndex in the _vp path. Equals
        // the draw's vertexOffset (the input mesh's base_vertex). Lets the _vp
        // shader compute local = gl_VertexIndex - input_vertex_offset, so
        // vertex_base stays the true output base instead of the old
        // (out_base - in_base) uint-wraparound encoded on the CPU. 0 for the
        // legacy vertex-attribute path (unused there). 48 B total.
        uint32_t input_vertex_offset = 0;
    };
    static_assert(sizeof(InstanceProperty) == 48,
                  "InstanceProperty layout drift — keep instance.glsl in sync.");

    // =========================================================================
    //  InstanceResources
    // =========================================================================
    /**
     * Three-stream GPU instance storage with stable slot allocation.
     *
     * Streams:
     *   0. **Transform** (64 B)  — mat4 model, read by vertex shader.
     *   1. **CullMeta**  (32 B)  — bsphere + draw params, read by cull compute.
     *   2. **Property**   (32 B)  — material + flags, read by vertex/fragment.
     *
     * Each stream has independent dirty tracking; uploads are incremental
     * (coalesced consecutive-run copy via staging buffer) unless a full
     * rebuild is forced.
     */
    class LUX_FUNCTION_PUBLIC InstanceResources
        : public GPUResourceBase<InstanceResources, EGPUResourceType::Instance>
        , public ISceneFrameService
    {
    public:
        struct InitInfo
        {
            DeviceContext *device_context{nullptr};
            DescriptorService *descriptor_svc{nullptr};   // layouts (global)
            SceneDescriptorArena *arena{nullptr};         // set allocation (per-scene)
            uint32_t initial_capacity{4096};
            uint32_t max_capacity{65536};
        };

        InstanceResources() = default;
        ~InstanceResources();

        InstanceResources(const InstanceResources &) = delete;
        InstanceResources &operator=(const InstanceResources &) = delete;

        void init(const InitInfo &info);
        void shutdown();

        // ─── Slot management ────────────────────────────────────────────
        [[nodiscard]] InstanceSlot          allocate();
        void                                free(InstanceSlot slot);
        [[nodiscard]] bool                  isAlive(InstanceSlot slot) const noexcept;
        [[nodiscard]] RenderObjectHandle    allocateObject();
        void                                freeObject(RenderObjectHandle handle);
        [[nodiscard]] bool                  isAlive(RenderObjectHandle handle) const noexcept;
        [[nodiscard]] uint32_t              generation(InstanceSlot slot) const noexcept;
        [[nodiscard]] InstanceSlot          resolveSlot(RenderObjectHandle handle) const noexcept;
        [[nodiscard]] RenderObjectHandle    handleForSlot(InstanceSlot slot) const noexcept;

        /// Ensure slot @p index is writable (grows capacity if needed,
        /// bumps slot_count_ if needed). Does NOT go through the free-list.
        /// Newly claimed slots are initialized to invalid section/object defaults
        /// to keep mesh-section ref-counting consistent.
        /// Use when external code owns slot identity (e.g. sync commands).
        InstanceSlot reserveSlot(uint32_t index);

        // ─── Per-stream write (marks dirty automatically) ───────────────
        void writeTransform(InstanceSlot slot, const InstanceTransform &xform);
        void writePrevTransform(InstanceSlot slot, const InstanceTransformPrev& xform_prev);
        void writeProperty(InstanceSlot slot, const InstanceProperty &prop);
        /// Writes draw/cull metadata and then refreshes world-space bsphere from
        /// current local bsphere + transform.
        /// If section_id changes, old mesh-section references are released.
        void writeCullMeta(InstanceSlot slot, const InstanceCullMeta &meta);

        /// Store mesh-local bounding sphere and recompute world-space bsphere.
        void setLocalBsphere(InstanceSlot slot, float cx, float cy, float cz, float r);

        // ─── Direct staging access for in-place patching ────────────────
        [[nodiscard]] InstanceTransform &transformAt(InstanceSlot slot) noexcept;
        [[nodiscard]] InstanceTransformPrev &prevTransformAt(InstanceSlot slot) noexcept;
        [[nodiscard]] InstanceProperty &propertyAt(InstanceSlot slot) noexcept;
        [[nodiscard]] const InstanceProperty &propertyAt(InstanceSlot slot) const noexcept;
        [[nodiscard]] InstanceCullMeta &cullMetaAt(InstanceSlot slot) noexcept;
        [[nodiscard]] const InstanceCullMeta &cullMetaAt(InstanceSlot slot) const noexcept;

        void markTransformDirty(InstanceSlot slot);
        void markPrevTransformDirty(InstanceSlot slot);
        void markPropertyDirty(InstanceSlot slot);
        void markCullDirty(InstanceSlot slot);
        void setRenderState(InstanceSlot slot, EGeometryKind geometry_kind, PassMask pass_mask);

        // ─── Frame lifecycle ────────────────────────────────────────────
        void beginFrame();

        /// New transfer subsystem path — submit copy requests to scheduler.
        /// Replaces recordUploads for barrier-free resource upload.
        void submitTransfers(TransferScheduler& scheduler);

        void requestCompaction() noexcept { compaction_requested_ = true; }

        void onBeginFrame(const FrameStamp& stamp) override
        {
            // Runs before graph compile; uploadMdcInfo() keys its ring advance on
            // this serial so all cull passes of one compile share a slot.
            mdc_info_current_serial_ = stamp.serial;
            beginFrame();
        }

        // ─── Descriptor set (binding 0 = Transform, binding 1 = Property) ─
        [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept { return ds_; }
        [[nodiscard]] VkDescriptorSet getDescriptorSet() const noexcept { return ds_; }
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept;
        [[nodiscard]] VkDescriptorSetLayout getDescriptorSetLayout() const noexcept { return descriptorSetLayout(); }

        // ─── Buffer accessors ───────────────────────────────────────────
        [[nodiscard]] VkBuffer transformBuffer() const noexcept;
        [[nodiscard]] VkBuffer prevTransformBuffer() const noexcept;
        [[nodiscard]] VkBuffer propertyBuffer() const noexcept;
        [[nodiscard]] VkBuffer cullMetaBuffer() const noexcept;
        [[nodiscard]] VkBuffer meshSectionBuffer() const noexcept;

        [[nodiscard]] uint32_t slotCount() const noexcept;
        [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] std::span<const uint32_t> denseAliveSlots() const noexcept;
        [[nodiscard]] std::span<const RenderObjectHandle> denseAliveHandles() const noexcept;
        [[nodiscard]] bool needsFullRebuild() const noexcept { return full_rebuild_; }
        void markFullRebuild() noexcept { full_rebuild_ = true; }
        [[nodiscard]] uint32_t registerMeshSection(const MeshSectionRecord& section);
        void unregisterMeshSection(uint32_t section_id);

        // ─── MDC table (Mesh Draw Command dedup) ───────────────────────
        [[nodiscard]] MdcTable& mdcTable() noexcept { return mdc_table_; }
        [[nodiscard]] const MdcTable& mdcTable() const noexcept { return mdc_table_; }

        /// GPU buffer holding the MdcInfo (offsets) for the LATEST compile.
        /// CRITICAL: this is a recompile-indexed ring (see uploadMdcInfo). The
        /// cull pass must capture the slot at COMPILE time via currentMdcInfoSlot()
        /// and read mdcInfoBufferAt(slot) — NOT this accessor — so an in-flight
        /// frame keeps reading the buffer it was recorded with even after a later
        /// recompile rewrites a different ring slot.
        [[nodiscard]] VkBuffer mdcInfoBuffer() const noexcept { return mdc_info_buffers_[mdc_info_current_slot_]; }

        /// Buffer for a specific ring slot (captured at compile time).
        [[nodiscard]] VkBuffer mdcInfoBufferAt(uint32_t slot) const noexcept
        {
            return mdc_info_buffers_[slot % kMdcInfoRingSize];
        }

        /// Ring slot written by the most recent uploadMdcInfo() (= the current compile).
        [[nodiscard]] uint32_t currentMdcInfoSlot() const noexcept { return mdc_info_current_slot_; }

        /// Rebuild MDC offsets and upload them into a fresh ring slot.
        /// Called once per graph compile (idempotent within one frame serial).
        void uploadMdcInfo();

        /// Capacity snapshot for monitoring.
        [[nodiscard]] CapacityStatus capacityStatus() const noexcept
        {
            return { slot_count_, capacity_, max_capacity_ };
        }

        /// Late-bind centralized deferred destroy queue to GPU buffers.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            deferred_queue_ = q;
            transform_stream_.setDeferredQueue(q);
            prev_transform_stream_.setDeferredQueue(q);
            property_stream_.setDeferredQueue(q);
            cull_meta_stream_.setDeferredQueue(q);
            mesh_section_table_.setDeferredQueue(q);
        }

    private:
        static constexpr uint32_t kInvalidObjectId = ~0u;
        [[nodiscard]] bool ensureCapacity(uint32_t required);
        bool shouldCompact() const noexcept;
        void compactSlots();
        void refreshDescriptorSet();
        void recomputeWorldBsphere(InstanceSlot slot);
        // Unregister an instance's N LOD MDCs + their mesh sections (section ids
        // are read back from the MDC entries). Resets cull.lod_count to 0.
        void unregisterInstanceLods(InstanceCullMeta& cull);
        
        PagedGpuStream<InstanceTransform>     transform_stream_;
        PagedGpuStream<InstanceTransformPrev> prev_transform_stream_;
        PagedGpuStream<InstanceProperty>      property_stream_;
        PagedGpuStream<InstanceCullMeta>      cull_meta_stream_;
        MeshSectionTable                      mesh_section_table_;
        MdcTable                              mdc_table_;

        // Per-stream upload-chunk scratch, reused across ticks (cleared at the top
        // of submitTransfers before each collectUploadChunks). submitTransfers runs
        // once per tick on the render thread and the chunks are consumed into the
        // staging copy synchronously within the same call, so a single (non-per-FIF)
        // set is correct. collectUploadChunks only APPENDS — each must be cleared
        // before reuse or stale chunks would re-upload. (P-5)
        std::vector<PagedGpuStream<InstanceTransform>::UploadChunk>     xform_chunks_;
        std::vector<PagedGpuStream<InstanceTransformPrev>::UploadChunk> prev_xform_chunks_;
        std::vector<PagedGpuStream<InstanceProperty>::UploadChunk>      prop_chunks_;
        std::vector<PagedGpuStream<InstanceCullMeta>::UploadChunk>      cull_chunks_;

        // MdcInfo is rebuilt only at graph compile, but in-flight frames N-1/N-2
        // are still reading the previous compile's buffer on their own GPU. A
        // single buffer would be overwritten under them (torn offsets, and an
        // OOB write into the prior frame's smaller visible buffer when capacity
        // grows). So MdcInfo is a recompile-indexed ring: each compile writes a
        // fresh slot and the cull pass captures that slot, leaving prior slots
        // untouched until their frames retire. Ring size = FIF + 1 guarantees a
        // reused slot is at least FIF+1 frames old even under per-frame recompiles.
        static constexpr uint32_t            kMdcInfoRingSize = kMaxFramesInFlight + 1u;
        std::array<VkBuffer, kMdcInfoRingSize>      mdc_info_buffers_{};
        std::array<VmaAllocation, kMdcInfoRingSize> mdc_info_allocs_{};
        std::array<VkDeviceSize, kMdcInfoRingSize>  mdc_info_sizes_{};
        std::array<void*, kMdcInfoRingSize>         mdc_info_mapped_{};
        uint32_t                             mdc_info_ring_cursor_{0};
        uint32_t                             mdc_info_current_slot_{0};
        uint64_t                             mdc_info_current_serial_{0};   ///< set by onBeginFrame
        uint64_t                             mdc_info_last_upload_serial_{~0ull};
        std::vector<std::array<float,4>>      local_bsphere_; ///< mesh-local bsphere for world recomputation

        std::unique_ptr<InstanceSlotRegistry> registry_;

        uint32_t slot_count_{0};
        uint32_t capacity_{0};
        uint32_t max_capacity_{0};
        bool full_rebuild_{true};
        bool compaction_requested_{false};
        bool initialized_{false};            ///< idempotent init() guard

        // ── Descriptor ──
        DescriptorLayoutId ds_layout_id_{kInvalidDescriptorLayoutId};
        VkDescriptorSet ds_{VK_NULL_HANDLE};

        DeviceContext *device_ctx_{nullptr};
        DescriptorService *descriptor_svc_{nullptr};
        SceneDescriptorArena *arena_{nullptr};
        DeferredDestroyQueue *deferred_queue_{nullptr};
    };

} // namespace lux::render
