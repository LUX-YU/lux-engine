#pragma once
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlot.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/render/resources/mesh/MeshSectionTable.hpp>
#include <lux/engine/render/gpu/memory/PagedGpuStream.hpp>
#include <lux/engine/render/resources/mesh/SparseInstanceStream.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/visibility.h>

// Transfer subsystem forward declaration
namespace lux::render { class TransferScheduler; class SceneDescriptorArena; }

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    /// Render-thread-only classification bit. Public mesh operations must mask
    /// it from authored flags; RenderCluster owns the only writer that sets it.
    inline constexpr std::uint32_t kInstanceInternalFlagClusterOwned =
        1u << 31u;

    // =========================================================================
    //  GPU-compatible struct definitions (std430 layout)
    // =========================================================================
    /// Transform streams use the public wire/GPU contract directly.  Keeping a
    /// second Vulkan-only representation here would make every operation handler
    /// an ABI conversion point and is exactly how the old absolute mat4 path
    /// survived previous migrations.
    using InstanceTransform = RenderSpatialTransform3D;
    using InstanceTransformPrev = RenderSpatialTransform3D;
    static_assert(sizeof(InstanceTransform) == 64);

    /// Cull metadata — read by compute cull shader. Carries one MDC per LOD
    /// level: the cull shader picks the LOD from screen coverage and appends the
    /// instance to lod_mdc[lod]. lod_count==0 marks an unregistered slot.
    struct alignas(16) InstanceCullMeta
    {
        float bsphere[4];      // xyz=normalized page-local center, w=radius (<0 → tombstone)
        std::int32_t bsphere_page[4]; // center page relative to RenderScene origin
        uint32_t bucket_id;
        uint32_t lod_count;    ///< valid lod_mdc[] entries (0 → unregistered)
        uint32_t lod_mdc[4];   ///< MDC index per LOD level (len must be >= kMaxMeshLod)
        uint32_t _pad[2];      // 64 bytes total
    };
    static_assert(sizeof(InstanceCullMeta) == 64);

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
        // detect "no bindless source" cleanly.
        //(旧注释在此承诺"由 writeRenderable() 配合 RenderableComponent 填充"
        // —— 这两个符号全仓从未存在;同段说的"legacy 顶点属性路径仍可用"也已
        // 过时:属性路径的 vert 在 P3.2 R6 随 _vp 化一起删除。现在填这个字段的
        // 是各条 mesh 上传路径本身。)
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
        // World/HLOD visibility transition. Written only on transition
        // edges; shaders advance coverage from SceneGlobalGpuData::time_sec.
        float transition_start_time = 0.0f;
        float transition_duration = 0.0f;
        uint32_t transition_seed = 0u;
        uint32_t transition_flags = 0u;
        /// Per-instance RGBA8 tint. Static World instances populate this from
        /// EditableWorldInstance; ordinary mesh instances default to white.
        uint32_t rgba8 = 0xffffffffu;
        uint32_t _property_pad[3]{};
    };
    static_assert(sizeof(InstanceProperty) == 80,
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
    {
    public:
        struct ResourceBinding final
        {
            MeshHandle mesh{};
            MaterialHandle material{};
        };

        struct InitInfo
        {
            DeviceContext *device_context{nullptr};
            DescriptorService *descriptor_svc{nullptr};   // layouts (global)
            SceneDescriptorArena *arena{nullptr};         // set allocation (per-scene)
            uint32_t initial_capacity{4096};
            uint32_t max_capacity{65536};
            double coordinate_page_size{1024.0};
            bool sparse_bda{false};
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
        [[nodiscard]] bool bindResources(
            RenderObjectHandle object,
            ResourceBinding binding);
        /// Replaces an existing live binding without allocating a new map node.
        /// The caller must already own references for `binding`; on success it
        /// receives the previous binding and is responsible for releasing it.
        [[nodiscard]] std::optional<ResourceBinding> replaceResources(
            RenderObjectHandle object,
            ResourceBinding binding) noexcept;
        [[nodiscard]] std::optional<ResourceBinding> resourceBinding(
            RenderObjectHandle object) const noexcept;
        [[nodiscard]] std::optional<ResourceBinding> takeResources(
            RenderObjectHandle object) noexcept;
        [[nodiscard]] std::vector<ResourceBinding> takeAllResources();
        [[nodiscard]] bool beginFadeRetirement(
            RenderObjectHandle object,
            float scene_time,
            float duration_seconds,
            std::uint32_t transition_seed) noexcept;
        [[nodiscard]] std::vector<RenderObjectHandle>
        collectExpiredFadeRetirements(float scene_time);
        void cancelFadeRetirement(RenderObjectHandle object) noexcept;
        [[nodiscard]] std::uint32_t fadeRetirementCount() const noexcept
        {
            return static_cast<std::uint32_t>(fade_retirements_.size());
        }
        [[nodiscard]] std::uint32_t resourceBindingCount() const noexcept
        {
            return static_cast<std::uint32_t>(resource_bindings_.size());
        }
        [[nodiscard]] std::uint32_t transparentHardCutCount() const noexcept
        {
            return transparent_hard_cut_count_;
        }

        //(这里曾有 reserveSlot(index) —— "外部代码自己拥有槽位身份(如同步命令)"
        // 的入口。全仓零调用方,连同它唯一的下层 InstanceSlotRegistry::reserveSlot
        // 一并删除。真要重新引入,该跟着那条同步命令一起设计。)

        // ─── Per-stream write (marks dirty automatically) ───────────────
        void writeTransform(InstanceSlot slot, const InstanceTransform &xform);
        /// 未接线(运动矢量路径整体未接):prev-transform 流已分配并上传,但没有
        /// 写入者或消费 pass。写点、prevTransformBuffer 与 motion-vector pass
        /// 应由对应的 renderer feature 成套接入。
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
        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept;
        [[nodiscard]] float spatialTileSize() const noexcept
        {
            return coordinate_page_size_;
        }

        void markTransformDirty(InstanceSlot slot);
        void markPrevTransformDirty(InstanceSlot slot);
        void markPropertyDirty(InstanceSlot slot);
        void markCullDirty(InstanceSlot slot);
        void setRenderState(InstanceSlot slot, EGeometryKind geometry_kind, PassMask pass_mask);

        // ─── Instance-flags census(逐位存活计数)────────────────────────
        /// flags 的收口写点:diff 新旧值维护逐位计数(writeProperty 与
        /// free 同样过账)。feature 用 flagBitCount 做"该位当前有无存活
        /// 实例"的整链跳过判定(如 Highlight 的空选中集)。绕过此 API
        /// 直改 propertyAt().flags 会漏账——flags 改动一律走这里。
        void setInstanceFlags(InstanceSlot slot, uint32_t flags);
        [[nodiscard]] uint32_t flagBitCount(uint32_t bit_index) const noexcept
        {
            return bit_index < 32u ? flag_counts_[bit_index] : 0u;
        }

        // ─── Frame lifecycle ────────────────────────────────────────────
        void beginFrame();

        /// New transfer subsystem path — submit copy requests to scheduler.
        /// Replaces recordUploads for barrier-free resource upload.
        void submitTransfers(TransferScheduler& scheduler);

        void onFrameBeginMaintenance(const FrameStamp& stamp)
        {
            // Runs before graph compile; uploadMdcInfo() keys its ring advance on
            // this serial so all cull passes of one compile share a slot.
            mdc_info_current_serial_ = stamp.serial;
            beginFrame();
        }

        // ─── Descriptor set (binding 0 = Transform, binding 1 = Property) ─
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept;

        /// Sets the domain-set dual-write target: per-slice handles plus the
        /// in-domain binding offset.
        ///
        /// A setter is used instead of an InitInfo field because the domain sets
        /// come from the SCENE (RenderScene::domainDescriptorSets()), not from the
        /// device-level wiring InitInfo carries — the owner has both in hand, but
        /// only after it has resolved the scene. It immediately re-writes the
        /// bindings once set, so call order doesn't affect correctness.
        ///
        /// (原注释说"每个 owner 的 init 路径形状不同,所以用 setter 统一" —— 那是
        ///  init 分散在三个 feature 里时的事实。A-4 把 init 收进了唯一的
        ///  StandardMeshStackFeature,此处也就只剩一个调用方。)
        [[nodiscard]] Expected<void> setDomainWriteTarget(std::span<const VkDescriptorSet> sets,
                                  uint32_t binding_offset);
        [[nodiscard]] VkDescriptorSetLayout getDescriptorSetLayout() const noexcept { return descriptorSetLayout(); }

        // ─── Buffer accessors ───────────────────────────────────────────
        /// 未接线:现役绑定走域描述符集(useEngineSet),不再逐个取裸 VkBuffer。
        [[nodiscard]] VkBuffer transformBuffer() const noexcept;
        /// 未接线:见 writePrevTransform —— 运动矢量路径整体未接。
        [[nodiscard]] VkBuffer prevTransformBuffer() const noexcept;
        [[nodiscard]] VkBuffer propertyBuffer() const noexcept;
        [[nodiscard]] VkBuffer cullMetaBuffer() const noexcept;
        /// Dense invocation-index -> stable instance-slot indirection used by all
        /// mesh cull kernels. Its element count is aliveCount(), never slotCount().
        [[nodiscard]] VkBuffer aliveSlotBuffer() const noexcept;
        /// Dense stable-slot stream containing only instances that are not owned
        /// by a static RenderCluster. Candidate expansion consumes this stream so
        /// static instances are not scanned a second time every frame.
        [[nodiscard]] VkBuffer dynamicSlotBuffer() const noexcept;
        [[nodiscard]] VkBuffer meshSectionBuffer() const noexcept;

        [[nodiscard]] uint32_t slotCount() const noexcept;
        [[nodiscard]] uint32_t aliveCount() const noexcept
        {
            return static_cast<uint32_t>(registry_.denseAliveSlots().size());
        }
        [[nodiscard]] uint32_t dynamicCount() const noexcept
        {
            return static_cast<uint32_t>(dense_dynamic_slots_.size());
        }
        [[nodiscard]] std::span<const uint32_t> denseDynamicSlots() const noexcept
        {
            return dense_dynamic_slots_;
        }
        [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] bool usesSparsePageTable() const noexcept
        {
            return sparse_bda_;
        }
        [[nodiscard]] std::uint32_t residentPageCount() const noexcept
        {
            return transform_stream_.pageCount();
        }
        [[nodiscard]] std::uint32_t pageTableLeafCount() const noexcept
        {
            return page_table_.leafCount();
        }
        [[nodiscard]] std::uint64_t descriptorWriteCount() const noexcept
        {
            return descriptor_write_count_;
        }
        [[nodiscard]] VkDeviceSize fieldStorageImportBytes(
            VkDeviceSize flat_stride) const noexcept
        {
            return sparse_bda_
                ? page_table_.rootBufferBytes()
                : static_cast<VkDeviceSize>(capacity_) * flat_stride;
        }
        /// Stable slots are never compacted. This serial remains constant for
        /// the scene lifetime and lets consumers prove ordinary page growth did
        /// not remap an existing handle.
        [[nodiscard]] std::uint64_t slotLayoutSerial() const noexcept
        {
            return slot_layout_serial_;
        }
        [[nodiscard]] std::span<const uint32_t> denseAliveSlots() const noexcept;
        [[nodiscard]] bool needsFullRebuild() const noexcept { return full_rebuild_; }
        void markFullRebuild() noexcept { full_rebuild_ = true; }
        /// @param ibo_segment 网格索引所在的竞技场段号,仅参与 CPU 侧去重键
        ///        (见 MeshSectionTable::registerSection)。
        [[nodiscard]] uint32_t registerMeshSection(const MeshSectionRecord& section,
                                                   uint16_t ibo_segment = 0,
                                                   VkIndexType index_type =
                                                       VK_INDEX_TYPE_UINT32);
        void unregisterMeshSection(uint32_t section_id);
        [[nodiscard]] const MeshSectionRecord& meshSectionAt(
            std::uint32_t section_id) const noexcept
        {
            return mesh_section_table_.at(section_id);
        }

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

        /// Late-bind centralized deferred destroy queue to GPU buffers.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            deferred_queue_ = q;
            page_table_.setDeferredQueue(q);
            transform_stream_.setDeferredQueue(q);
            prev_transform_stream_.setDeferredQueue(q);
            property_stream_.setDeferredQueue(q);
            cull_meta_stream_.setDeferredQueue(q);
            alive_slot_stream_.setDeferredQueue(q);
            dynamic_slot_stream_.setDeferredQueue(q);
            mesh_section_table_.setDeferredQueue(q);
        }

    private:
        static constexpr uint32_t kInvalidObjectId = ~0u;
        [[nodiscard]] bool ensureCapacity(uint32_t required);
        void refreshDescriptorSet();
        void recomputeWorldBsphere(InstanceSlot slot);
        void appendAliveSlot();
        void repairAliveSlotAfterFree(uint32_t dense_position);
        void rebuildAliveSlotStream();
        void addDynamicSlot(std::uint32_t slot_index);
        void removeDynamicSlot(std::uint32_t slot_index);
        void updateDynamicMembership(
            std::uint32_t slot_index,
            std::uint32_t old_flags,
            std::uint32_t new_flags);
        void rebuildDynamicSlotStream();
        // Unregister an instance's N LOD MDCs + their mesh sections (section ids
        // are read back from the MDC entries). Resets cull.lod_count to 0.
        void unregisterInstanceLods(InstanceCullMeta& cull);

        SparseInstancePageTable               page_table_;
        SparseInstanceStream<InstanceTransform> transform_stream_;
        SparseInstanceStream<InstanceTransformPrev> prev_transform_stream_;
        SparseInstanceStream<InstanceProperty> property_stream_;
        SparseInstanceStream<InstanceCullMeta> cull_meta_stream_;
        PagedGpuStream<uint32_t>              alive_slot_stream_;
        PagedGpuStream<uint32_t>              dynamic_slot_stream_;
        MeshSectionTable                      mesh_section_table_;
        MdcTable                              mdc_table_;

        // Per-stream upload-chunk scratch, reused across ticks (cleared at the top
        // of submitTransfers before each collectUploadChunks). submitTransfers runs
        // once per tick on the render thread and the chunks are consumed into the
        // staging copy synchronously within the same call, so a single (non-per-FIF)
        // set is correct. collectUploadChunks only APPENDS — each must be cleared
        // before reuse or stale chunks would re-upload. (P-5)
        std::vector<SparseInstanceStream<InstanceTransform>::UploadChunk> xform_chunks_;
        std::vector<SparseInstanceStream<InstanceTransformPrev>::UploadChunk> prev_xform_chunks_;
        std::vector<SparseInstanceStream<InstanceProperty>::UploadChunk> prop_chunks_;
        std::vector<SparseInstanceStream<InstanceCullMeta>::UploadChunk> cull_chunks_;
        std::vector<PagedGpuStream<uint32_t>::UploadChunk>              alive_slot_chunks_;
        std::vector<PagedGpuStream<uint32_t>::UploadChunk>              dynamic_slot_chunks_;

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
        StableInstanceCpuPages<std::array<float,4>> local_bsphere_;

        /// 值成员而非 unique_ptr:InstanceSlotRegistry 的头就在本文件顶部 include
        /// (无编译防火墙收益),它自己有 init()/shutdown() 管生命周期,而"建了没有"
        /// 这一状态本类已有 initialized_ 表达。之前用 unique_ptr 等于把同一个状态
        /// 记了两份,代价是 20 处 `if (!registry_)` / `registry_ ? ... : 回退` ——
        /// 而它们检测的是"init 了没有",却拿指针空当代理。现在:纯查询路径直接问
        /// 注册表(空态的答案与原回退逐个相同:isAlive→false、generation→0、
        /// resolveSlot/handleForSlot→invalid、slotCount→0、denseAlive*→空 span),
        /// 真正会碰设备/流的路径(allocate / allocateObject / reserve / compact)
        /// 改守 initialized_ —— 守卫说的就是它真正要守的东西。
        InstanceSlotRegistry registry_;
        static constexpr std::uint32_t kInvalidDynamicPosition = ~0u;
        std::vector<std::uint32_t> dense_dynamic_slots_;
        std::vector<std::uint32_t> dynamic_positions_;
        std::unordered_map<std::uint64_t, ResourceBinding>
            resource_bindings_;
        struct FadeRetirement final
        {
            RenderObjectHandle object{};
            float expires_at{0.0f};
        };
        std::vector<FadeRetirement> fade_retirements_;
        std::uint32_t transparent_hard_cut_count_{0u};

        uint32_t slot_count_{0};
        uint32_t capacity_{0};
        uint32_t max_capacity_{0};
        float coordinate_page_size_{1024.0f};
        bool sparse_bda_{false};
        bool full_rebuild_{true};
        std::uint64_t slot_layout_serial_{1u};
        std::uint64_t descriptor_write_count_{0u};
        // ⚠️ 这里曾有一个自己的 `bool initialized_{false};` —— 它**遮蔽**了
        //    GPUResourceBase 的同名 protected 成员(`GPUResourceBase.hpp:74`),
        //    于是基类的 `isInitialized()`(`:55`)读的是一个**从未被写过**的标志,
        //    对本类恒返回 false。全仓当时没有一个调用点读它,所以这个洞一直隐形;
        //    ensure<T>(init_args) 的 void-init 后置检查是第一个读者,它立刻把
        //    "刚 init 成功的 InstanceResources"判成失败(现场:5 个流共 1.9MB
        //    VMA 分配随失败对象销毁而泄漏 —— 因为 ~InstanceResources 也守同一个
        //    恒假的标志)。删掉遮蔽,继承基类那一个。

        /// 逐 flag 位的存活实例计数(setInstanceFlags/writeProperty/free 过账)。
        uint32_t flag_counts_[32]{};
        void accountFlagsDiff(uint32_t old_flags, uint32_t new_flags) noexcept;

        // ── Descriptor ──
        DescriptorLayoutId ds_layout_id_{kInvalidDescriptorLayoutId};

        /// Dual-write target: per-slice domain-set handles plus the
        /// in-domain binding offset.
        DomainWriteTarget            domain_{};

        DeviceContext *device_ctx_{nullptr};
        DescriptorService *descriptor_svc_{nullptr};
        SceneDescriptorArena *arena_{nullptr};
        DeferredDestroyQueue *deferred_queue_{nullptr};
    };

} // namespace lux::render
