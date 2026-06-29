#pragma once

#include <lux/engine/render/graph/RGForwardDecls.hpp>
#include <lux/engine/render/graph/RGTransientDSTypes.hpp>   // RGDescriptorWrite (neutral), RGTransientDSHandle
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <limits>
#include <type_traits>

#include <lux/cxx/container/SmallVector.hpp>

namespace lux::render
{
    // ========== Pass Kernel Identifiers ==========
    // KernelTypeId / kInvalidKernelId now live in the public, lightweight
    // RGForwardDecls.hpp (included above) so KernelDescriptor.hpp can reference
    // them without pulling this heavy header onto the public install surface.

    // ========== Kernel Configuration Blob ==========

    /// Maximum size of the inline kernel configuration blob.
    /// Config structs must be trivially copyable and fit within this limit.
    inline constexpr size_t kKernelConfigBlobSize = 128;

    /// Type-erased inline storage for kernel-specific configuration.
    /// Each kernel has a corresponding config struct (e.g. MeshCullKernelConfig).
    /// The blob is memcpy'd at graph-build time and interpreted by the compiler/executor.
    struct KernelConfigBlob
    {
        alignas(8) uint8_t data[kKernelConfigBlobSize]{};
        uint32_t size{0};

        template <typename T>
            requires (std::is_trivially_copyable_v<T> && sizeof(T) <= kKernelConfigBlobSize)
        void store(const T& config) noexcept
        {
            std::memcpy(data, &config, sizeof(T));
            size = static_cast<uint32_t>(sizeof(T));
        }

        template <typename T>
            requires (std::is_trivially_copyable_v<T> && sizeof(T) <= kKernelConfigBlobSize)
        [[nodiscard]] const T& as() const noexcept
        {
            return *reinterpret_cast<const T*>(data);
        }
    };

    // ========== Kernel-specific configuration structs ==========

    /// Configuration for the "MeshCull" / "ShadowCull" kernels.
    struct MeshCullKernelConfig
    {
        RGResourceHandle    frustum_ubo_rg{};       ///< RG handle to frustum UBO
        RGResourceHandle    draw_count_rg{};         ///< RG handle to draw-count buffer
        RGResourceHandle    indirect_rg{};           ///< RG handle to indirect-draw buffer
        uint32_t            pass_mask{0};            ///< Required pass mask for filtering
        uint32_t            geometry_mask{0};        ///< Supported geometry kind bitmask
        uint32_t            max_slices{1};           ///< Shadow: number of cascade slices
        uint32_t            draw_list_count{0};      ///< Number of draw-list entries (legacy: gk×buckets; MDC: mdc_count)
        uint32_t            descriptor_layout_version{0}; ///< Cull descriptor/push-constant ABI version
        uint32_t            extension_flags{0};      ///< Extension bits (e.g. HZB/bindless)
        uint32_t            mdc_count{0};            ///< MDC mode: number of unique draw commands
        uint32_t            view_mdc_count{0};       ///< Shadow MDC: view MDC count per bias group
    };

    struct MdcEntry;  // Forward declaration (defined in MdcTable.hpp)

    /// Configuration for the "MeshDraw" / "ShadowDraw" kernels.
    struct MeshDrawKernelConfig
    {
        RGResourceHandle    draw_count_rg{};         ///< RG handle to draw-count buffer
        RGResourceHandle    indirect_rg{};           ///< RG handle to indirect-draw buffer
        uint32_t            geometry_mask{0};        ///< Supported geometry kind bitmask
        uint32_t            mdc_count{0};            ///< MDC draw mode: number of MDC entries (0 = legacy)
        const MdcEntry*     mdc_entries{nullptr};    ///< Non-owning, valid during graph build/compile
        /// Number of material-family pipeline variants.
        /// Skinned variants, when present, occupy [family_count, 2*family_count)
        /// in pipeline_variants. 0 = no skinned variants (legacy: variant index
        /// == bucket_id).
        uint32_t            family_count{0};
    };

    /// Configuration for the "MdcCompact" kernel.
    struct MdcCompactKernelConfig
    {
        RGResourceHandle    draw_count_rg{};         ///< Per-MDC counter buffer
        RGResourceHandle    indirect_rg{};           ///< Output indirect command buffer
        uint32_t            mdc_count{0};            ///< Number of MDCs to compact
    };

    /// Configuration for the "ShadowDraw" kernel (extends MeshDrawKernelConfig with atlas info).
    struct ShadowDrawKernelConfig
    {
        RGResourceHandle    draw_count_rg{};         ///< RG handle to per-MDC count buffer
        RGResourceHandle    indirect_rg{};           ///< RG handle to shadow MDC indirect buffer
        uint32_t            geometry_mask{0};        ///< Supported geometry kind bitmask
        uint32_t            atlas_resolution{4096};  ///< Shadow atlas page resolution
        uint32_t            view_mdc_count{0};       ///< MDC count per bias group
        uint32_t            bias_group_count{0};     ///< Active bias-group lanes; the draw
                                                     ///< addresses count/indirect over
                                                     ///< [0, bias_group_count*view_mdc_count).
                                                     ///< Must match the buffer sizing.
        // Skinned-shadow variant select: pipeline_variants[0] is the static
        // depth pipeline; [family_count] is the skinned (_vp) variant. The kernel
        // picks per view-MDC by geometry kind. 0 = no skinned variant (legacy).
        const MdcEntry*     mdc_entries{nullptr};    ///< per-view-MDC geometry kind; non-owning, build-time
        uint32_t            family_count{0};         ///< skinned variant index in pipeline_variants (0 = none)
    };

    /// Configuration for the "ClearCounters" kernel.
    struct ClearCountersKernelConfig
    {
        static constexpr uint32_t kMaxBuffers = 8;
        RGResourceHandle    buffers[kMaxBuffers]{};  ///< RG handles to draw-count buffers to clear
        uint32_t            buffer_count{0};         ///< Number of buffers to clear
    };

    /// Configuration for the "FullscreenQuad" / "TonemapPass" / "DeferredLighting" etc. kernels.
    struct FullscreenKernelConfig
    {
        RGResourceHandle    input_texture_rg{};      ///< Primary input texture
        RGResourceHandle    output_texture_rg{};     ///< Primary output texture
    };

    /// Helper: construct a KernelConfigBlob from a typed config struct.
    template <typename T> requires (std::is_trivially_copyable_v<T> && sizeof(T) <= kKernelConfigBlobSize)
    [[nodiscard]] inline KernelConfigBlob makeKernelConfig(const T& config) noexcept
    {
        KernelConfigBlob blob{};
        blob.store(config);
        return blob;
    }
    // RGResourceHandle is defined in RGForwardDecls.hpp

    // ========== Pass Type ==========
    // ERGPassType moved to the public RGEnums.hpp (SDK P1); RGEnums.hpp is already
    // included above (line 4), so existing in-TU users keep resolving.

    // ========== Pass Resource References ==========

    struct RGPassTextureRef
    {
        RGResourceHandle            resource;
        lux::common::ETextureRole   role;
        ERGResourceUsage            usage;
        RGTextureSubresourceRange   range;
        uint32_t                    input_attachment_index{ std::numeric_limits<uint32_t>::max() };
    };

    struct RGPassBufferRef
    {
        RGResourceHandle            resource;
        ERGResourceUsage            usage;
        ERGBufferRole               role;
        uint64_t                    offset{ 0 };
        uint64_t                    size{ 0 };
    };

    // ========== Per-pass descriptor set binding ==========

    // ========== Transient Descriptor Set types ==========

    // RGDescriptorWrite is now the NEUTRAL public POD in RGTransientDSTypes.hpp
    // (included above): EDescriptorType / EImageLayout instead of Vk* enums.

    /// Per-graph description of a transient descriptor set.
    /// One DS is allocated per-view per-frame-in-flight at record-context creation.
    struct RGTransientDSDescription
    {
        std::string                      name;
        VkDescriptorSetLayout            layout = VK_NULL_HANDLE;
        std::vector<RGDescriptorWrite>   writes;
    };

    // RGTransientDSHandle is now in the public RGTransientDSTypes.hpp (included above).

    // ========== Per-pass descriptor set binding ==========

    /// Type-erased function pointer for late-bound DS resolution.
    /// Receives the opaque resource pointer and the FIF slot index;
    /// returns the VkDescriptorSet to bind for this frame.
    using DSResolverFn = VkDescriptorSet (*)(const void* resource, uint32_t frame_slot);

    enum class EDSBindMode : uint8_t
    {
        IMMUTABLE,
        PER_FIF,
        VERSIONED,
    };

    enum class EDSBindingSource : uint8_t
    {
        Immutable,
        Scene,
        Transient,
        Resource,
    };

    struct DescriptorProvider
    {
        const void* resource = nullptr;
        DSResolverFn resolver = nullptr;
    };

    /// Declares a per-slot descriptor set for a single pass.
    ///
    /// Source + bind-mode semantics:
    ///   - source=Immutable: bind immutable_set.
    ///   - source=Scene:     bind frame scene_ds.
    ///   - source=Transient: bind transient DS from record context.
    ///   - source=Resource:  resolve via provider(resolver, resource).
    ///
    /// Bind mode controls compiler-side dedup:
    ///   - Immutable: allow cross-pass skip if identity unchanged.
    ///   - PerFIF/Versioned: force per-pass rebinding.
    struct PassDSBinding
    {
        uint32_t          slot{0};                      ///< Vulkan descriptor set index (0-based)
        EDSBindingSource  source{EDSBindingSource::Immutable};
        EDSBindMode       mode{EDSBindMode::IMMUTABLE};
        VkDescriptorSet   immutable_set{VK_NULL_HANDLE};
        DescriptorProvider provider{};
        uint32_t          transient_ds_index{0};        ///< Index into RGGraphDescription::transient_descriptor_sets

        /// For source=Resource: declares which RG logical resource this DS reads
        /// through its resolver, so dependency analysis / ordering / dead-pass
        /// elimination see the external consumption (the physical bind still goes
        /// via provider.resolver). Invalid (default) = not declared → the pass
        /// relies on markSideEffect / triggers a validation warning. (M3 layer 2)
        RGResourceHandle          declared_consume{};
        ERGResourceType           consume_type{ERGResourceType::BUFFER};
        lux::common::ETextureRole consume_tex_role{lux::common::ETextureRole::SAMPLED};
    };

    // ========== Pass & Graph Description ==========

    struct RGPassDescription
    {
        std::string                         name;
        uint64_t                            name_hash{ 0 };
        ERGPassType                         type{ ERGPassType::GRAPHICS };
        EGeometryType                       geometry_type{ EGeometryType::MESH };

        std::vector<RGPassTextureRef>       textures;
        std::vector<RGPassBufferRef>        buffers;

        phase_mask_t                        phase_mask{ 0 };
        /// Feature-provided recording lambda.  Invoked by the recorder
        /// after pass-level pipeline + descriptor sets are bound.
        /// Used when kernel_id == kInvalidKernelId (no kernel registered).
        PassRecordFn                        recorder;

        /// Lightweight kernel recording callback.  Invoked by the fast-path
        /// executor after common state setup (pipeline, DS, viewport, shared
        /// push constants).  Handles only feature-specific work: custom push
        /// constants, VBO binding, runtime draw/dispatch parameters.
        /// When set, the executor calls this instead of its built-in kernel
        /// command (DrawDirect, Dispatch, etc.).
        PassRecordFn                        kernel_fn;

        /// Kernel identifier assigned by KernelRegistry at static-init time.
        /// kInvalidKernelId (0) means recorder lambda path.
        KernelTypeId                        kernel_id{ kInvalidKernelId };

        /// Kernel-specific POD configuration, interpreted according to kernel_id.
        KernelConfigBlob                    kernel_config{};

        GraphicsPipelineHandle              pipeline_template;
        lux::cxx::SmallVector<GraphicsPipelineHandle, 4> additional_pipelines;
        /// Optional per-variant shader feature masks.
        /// Indexing matches pipeline variants: 0 = setPipeline(), 1..N = addPipeline().
        lux::cxx::SmallVector<uint32_t, 4> pipeline_variant_features;

        ComputePipelineHandle               compute_pipeline_handle{};  ///< For COMPUTE passes: registered compute pipeline handle.

        lux::cxx::SmallVector<std::string, 4>            after_passes;   ///< Explicit ordering: this pass must execute after the named passes.
        ERenderStage                                    stage{ ERenderStage::Default };  ///< Painter-order tie-break for write-after-write between data-independent passes (see ERenderStage). Default = Opaque ⇒ unannotated passes keep declaration-order tie-break.

        PassConditionFn                     condition{};
        bool                                enabled   = true;
        /// When true, all render items in this pass use the pass-level pipeline
        /// (pipeline_template) instead of their per-item pipeline handles.
        /// Use this for passes like DepthPrepass where per-material fragment shaders
        /// should not be compiled against the pass's render pass (e.g., depth-only).
        bool                                force_pass_pipeline = false;

        /// When true, the framework does NOT auto-set viewport/scissor before
        /// invoking the recorder.  The feature is responsible for calling
        /// vkCmdSetViewport / vkCmdSetScissor itself (e.g. ShadowMapFeature).
        bool                                manual_viewport = false;

        /// When true, this pass is NEVER removed by dead-pass elimination, even
        /// if none of its outputs feed an exported resource. Use for passes that
        /// write a FEATURE-OWNED resource consumed OUTSIDE the RenderGraph — e.g.
        /// HzbBuild writes the HZB pyramid, which the cull pass samples through a
        /// feature-managed descriptor set (bindResourceDS), not an RG .read(), so
        /// the graph cannot see the consumer and would otherwise prune the build.
        bool                                has_side_effect = false;

        /// Per-slot descriptor set bindings declared via RGPassBuilder::bindImmutableDS() / bindSceneDS().
        lux::cxx::SmallVector<PassDSBinding, 8> ds_bindings;

        /// Returns true when this pass uses a compile-time kernel rather than a recorder lambda.
        [[nodiscard]] bool hasKernel() const noexcept { return kernel_id != kInvalidKernelId; }
    };

    struct RGGraphDescription
    {
        std::vector<RGResourceDescription>          resources;
        std::vector<RGPassDescription>              passes;
        std::vector<RGTransientDSDescription>       transient_descriptor_sets;
    };

} // namespace lux::render
