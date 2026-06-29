#pragma once

#include <cstdint>
#include <cassert>
#include <limits>

#include <lux/engine/render/core/Geometry.hpp>
#include <lux/engine/common/ImageEnums.hpp>

namespace lux::render
{
    // ========== Basic resource types ==========

    enum class ERGResourceType
    {
        BUFFER,
        TEXTURE
    };

    enum class ERGResourceUsage
    {
        READ,
        WRITE,
        READ_WRITE
    };

    // ========== Pass Type ==========
    // (Moved here from sinclude RGPassTypes.hpp so RGBuilder::addPass(name, type)
    //  is nameable from the install tree — SDK P1.)

    enum class ERGPassType
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER,
        ASYNC_COMPUTE,
        ASYNC_TRANSFER,
    };

    // ========== Render Phase ==========

    using render_phase_id = uint8_t;
    using phase_mask_t    = uint64_t;

    static_assert(sizeof(phase_mask_t) * 8 >= 64, "phase_mask_t must support at least 64 phases");

    inline constexpr phase_mask_t phaseBit(render_phase_id id) noexcept
    {
        assert(id < sizeof(phase_mask_t) * 8 && "phase_id exceeds phase_mask_t capacity");
        return (id < sizeof(phase_mask_t) * 8) ? (phase_mask_t(1ull) << id) : phase_mask_t(0);
    }

    enum class ECoreRenderPhase : render_phase_id
    {
        Depth        = 0,
        GBuffer      = 1,
        ForwardOpaque = 2,
        ForwardTrans = 3,
        Shadow       = 4,
        UI           = 5,
        PostProcess  = 6,
        PointCloud   = 7,  ///< point-cloud items (P2-B: centralised phase constant)
        OctreeDebug  = 8,  ///< octree wireframe overlay
        Gizmo        = 9,  ///< gizmo overlays (line list, tri overlay)
    };

    // ========== Render-graph pass painter-order sort key ==========
    /// Painter-order tie-break for render-graph passes. It ONLY orders passes that
    /// write the SAME resource with NO data dependency between them (write-after-
    /// write) — the graph still derives all real ordering from read/write data
    /// dependencies and explicit after(). Without it, such ties fell back to feature
    /// REGISTRATION order, which silently made e.g. the Grid overlay disappear behind
    /// the Skybox when features were registered in the "wrong" order. A pass that does
    /// not set a stage defaults to Opaque, so unannotated passes keep their previous
    /// declaration-order tie-break (behaviour-preserving). NOTE: distinct from
    /// ECoreRenderPhase above (a cull/extraction classification whose numeric values
    /// are NOT a draw order). Gaps of 1000 leave room to insert stages later.
    enum class ERenderStage : uint16_t
    {
        Early       = 1000,   ///< shadow atlas / pre-scene compute
        Geometry    = 2000,   ///< depth prepass / GBuffer
        Opaque      = 3000,   ///< deferred lighting / forward opaque
        Sky         = 4000,   ///< skybox — after opaque, before transparent
        Transparent = 5000,   ///< blended geometry
        PostProcess = 6000,   ///< bloom / tonemap / HDR→LDR scene composite
        Overlay     = 7000,   ///< grid / gizmo / debug — composited ON TOP of the
                              ///< post-processed image. MUST outrank PostProcess: the
                              ///< overlays write the same SceneColor that tonemap writes,
                              ///< so a lower stage lets tonemap overwrite them and the
                              ///< grid/AABB vanish (editor registers them last for this).
        Present     = 8000,
        Default     = Opaque, ///< unset == Opaque ⇒ ties fall back to declaration order
    };

    // ========== Texture enums (canonical definitions in lux::gapi::Image.hpp) ==========

    enum class ERGTextureUsageBits : uint32_t
    {
        NONE             = 0,
        COLOR_ATTACHMENT = 1u << 0,
        DEPTH_STENCIL    = 1u << 1,
        SAMPLED          = 1u << 2,
        STORAGE          = 1u << 3,
        TRANSFER_SRC     = 1u << 4,
        TRANSFER_DST     = 1u << 5,
        INPUT_ATTACHMENT = 1u << 6
    };
    using ERGTextureUsageFlags = uint32_t;

    inline ERGTextureUsageFlags operator|(ERGTextureUsageBits a, ERGTextureUsageBits b)
    {
        return static_cast<ERGTextureUsageFlags>(a) | static_cast<ERGTextureUsageFlags>(b);
    }

    inline ERGTextureUsageFlags& operator|=(ERGTextureUsageFlags& a, ERGTextureUsageBits b)
    {
        a |= static_cast<ERGTextureUsageFlags>(b);
        return a;
    }

    // ========== Buffer enums ==========

    enum class ERGBufferRole
    {
        UNDEFINED = 0,
        VERTEX,
        INDEX,
        CONSTANT,
        STORAGE,
        INDIRECT
    };

    enum class ERGMemoryUsage
    {
        GPU_ONLY,
        CPU_TO_GPU,
        GPU_TO_CPU,
        CPU_ONLY
    };

    enum class ERGBufferUsageBits : uint32_t
    {
        NONE         = 0,
        VERTEX       = 1 << 0,
        INDEX        = 1 << 1,
        UNIFORM      = 1 << 2,
        STORAGE      = 1 << 3,
        INDIRECT     = 1 << 4,
        TRANSFER_SRC = 1 << 5,
        TRANSFER_DST = 1 << 6,
        ACCEL_STRUCT  = 1 << 7,
        SHADER_BINDING = 1 << 8
    };
    using ERGBufferUsageFlags = uint32_t;

    inline ERGBufferUsageFlags operator|(ERGBufferUsageBits a, ERGBufferUsageBits b)
    {
        return static_cast<ERGBufferUsageFlags>(a) | static_cast<ERGBufferUsageFlags>(b);
    }

    inline ERGBufferUsageFlags& operator|=(ERGBufferUsageFlags& a, ERGBufferUsageBits b)
    {
        a |= static_cast<ERGBufferUsageFlags>(b);
        return a;
    }

    inline ERGBufferUsageFlags operator&(ERGBufferUsageFlags a, ERGBufferUsageBits b)
    {
        return a & static_cast<ERGBufferUsageFlags>(b);
    }

    // ========== Resource lifecycle / sizing ==========

    enum class ERGResourceLifetime
    {
        PERSISTENT,
        TRANSIENT,
        IMPORTED,
        FORWARD_REFERENCE,  ///< Placeholder resolved at compile time to the actual TRANSIENT/IMPORTED resource with the same name
        EXTERNAL,           ///< Feature owns the physical storage and updates it OUTSIDE the graph
                            ///< (e.g. scene-level Light/Material SSBOs). RG records ONLY the logical
                            ///< identity so a pass can declare consumption via bindResourceDS(...,
                            ///< declared_consume) and dependency/dead-pass analysis sees it. RG never
                            ///< allocates it, fetches its handle, or emits a barrier for it.
        PING_PONG           ///< N physical copies rotated by frame (double-buffer / history). The
                            ///< CURRENT handle (ring_phase 0) writes this frame's copy and owns the
                            ///< allocation; PREVIOUS/history handles (ring_phase>0) read an earlier
                            ///< frame's copy of the same resource and allocate nothing. RG picks the
                            ///< copy per frame. For HZB / TAA / temporal cross-frame resources. (M1)
    };

    enum class ERGSizeMode
    {
        ABSOLUTE_MODE,
        RELATIVE_MODE
    };

    // ========== Reference (forward-ref) dependency semantics ==========

    /// What happens when a referenceTexture/Buffer/PingPong names a producer that
    /// does not exist at compile time. Makes the cross-feature data dependency
    /// explicit and its failure mode chosen rather than silent. (M2)
    enum class ERGReference : uint8_t
    {
        Optional,   ///< no producer → reader pruned by dead-pass (current behaviour); consumer degrades
        Required,   ///< no producer → compile FAILS FAST with a named error (no silent VUID downstream)
    };

    // ========== Queue type ==========

    /// @brief Queue type assignment for a compiled pass
    enum class ERGQueueType : uint8_t
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER,
    };

    // ========== Update Group ==========

    enum class RGUpdateGroup : uint32_t
    {
        GROUP_NONE      = 0,
        GROUP_SWAPCHAIN = 1 << 0,
        GROUP_GUI       = 1 << 1,
        GROUP_VIDEO     = 1 << 2,
        GROUP_COMPUTE   = 1 << 3,
        GROUP_ALL       = 0xFFFFFFFF
    };

    inline uint32_t operator|(RGUpdateGroup a, RGUpdateGroup b)
    {
        return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
    }

    inline uint32_t operator&(uint32_t a, RGUpdateGroup b)
    {
        return a & static_cast<uint32_t>(b);
    }

} // namespace lux::render
