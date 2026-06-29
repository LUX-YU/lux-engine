#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>

#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/renderer/features/shadow/EVSMShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/PCFShadowTechnique.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowSliceMath.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace lux::render
{
namespace
{
    constexpr float kEpsilon = 1e-5f;
    constexpr float kMinCascadeDepthSpan = 1e-3f;
    constexpr float kCascadeDepthPaddingFactor = 0.25f;
    constexpr float kCascadeDepthPaddingMin = 5.0f;
    constexpr float kCascadeXYPaddingFactor = 0.03f;
    constexpr float kCascadeStabilizeGuardTexels = 2.0f;
    constexpr uint32_t kMinShadowTileResolution = 256u;
    // How far each directional cascade's ortho box is pulled back TOWARD the light
    // beyond the camera-frustum segment. The segment-fit near plane otherwise clips
    // off tall casters standing between the light and the segment — their occluder
    // depth never reaches the cascade, so a receiver near the camera samples "lit"
    // and the NEAR part of a contact shadow drops out as you approach it. This pull-
    // back captures casters up to ~this height (metres). D32_SFLOAT shadow depth
    // absorbs the extra range; far_z stays tight so the depth-bias scale is stable.
    // Tunable: raise it for taller shadow-casters (watch for peter-panning).
    constexpr float kDirectionalCasterNearPullback = 40.0f;

#if !defined(LUX_SHADOW_DEBUG_MULTI_VIEW)
#if !defined(NDEBUG)
#define LUX_SHADOW_DEBUG_MULTI_VIEW 1
#else
#define LUX_SHADOW_DEBUG_MULTI_VIEW 0
#endif
#endif

#if LUX_SHADOW_DEBUG_MULTI_VIEW
    [[nodiscard]] uint64_t fnv1a64(std::span<const std::byte> bytes)
    {
        uint64_t h = 1469598103934665603ull;
        for (std::byte b : bytes)
        {
            h ^= static_cast<uint8_t>(b);
            h *= 1099511628211ull;
        }
        return h;
    }

    [[nodiscard]] uint64_t hashLightVp(const Eigen::Matrix4f& m)
    {
        const auto* p = reinterpret_cast<const std::byte*>(m.data());
        return fnv1a64({p, sizeof(float) * 16});
    }

    [[nodiscard]] uint64_t summarizeSlices(std::span<const ShadowSliceGPU> slices)
    {
        if (slices.empty())
            return 0ull;
        const uint64_t first = hashLightVp(slices.front().light_vp);
        const uint64_t last = hashLightVp(slices.back().light_vp);
        return first ^ ((last << 1) | (last >> 63)) ^ static_cast<uint64_t>(slices.size());
    }
#endif

    [[nodiscard]] bool tryExtractPerspectiveNearFar(
        const Eigen::Matrix4f& proj, float& out_near, float& out_far)
    {
        const float a = proj(2, 2);
        const float b = proj(2, 3);
        const float c = proj(3, 2);

        // Perspective projection requires w = c * z (typically c = ±1) and valid depth slope.
        if (std::abs(a) < kEpsilon || std::abs(c) < kEpsilon || std::abs(a - c) < kEpsilon)
            return false;

        // Vulkan NDC depth range is [0, 1]:
        //   near_z = -b / a
        //   far_z  = -b / (a - c)
        // Camera-space z may be negative depending on convention; use absolute distance.
        float n = std::abs(-b / a);
        float f = std::abs(-b / (a - c));
        if (!std::isfinite(n) || !std::isfinite(f))
            return false;
        if (f < n)
            std::swap(f, n);
        if (n <= 0.0f || f - n < kMinCascadeDepthSpan)
            return false;

        out_near = n;
        out_far = f;
        return true;
    }

    [[nodiscard]] Eigen::Vector3f unprojectNdc(
        const Eigen::Matrix4f& inv_view_proj, float x, float y, float z)
    {
        Eigen::Vector4f clip(x, y, z, 1.0f);
        Eigen::Vector4f world = inv_view_proj * clip;
        const float w = world.w();
        if (std::abs(w) < kEpsilon)
            return world.head<3>();
        return world.head<3>() / w;
    }

    [[nodiscard]] std::array<Eigen::Vector3f, 8> buildFrustumCorners(
        const Eigen::Matrix4f& inv_view_proj)
    {
        static const std::array<Eigen::Vector2f, 4> kNdcXY = {
            Eigen::Vector2f{-1.0f, -1.0f},
            Eigen::Vector2f{ 1.0f, -1.0f},
            Eigen::Vector2f{ 1.0f,  1.0f},
            Eigen::Vector2f{-1.0f,  1.0f},
        };

        std::array<Eigen::Vector3f, 8> corners{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            // Camera projection contract is Vulkan ZO: near z = 0, far z = 1 in NDC.
            corners[i]     = unprojectNdc(inv_view_proj, kNdcXY[i].x(), kNdcXY[i].y(), 0.0f);
            corners[i + 4] = unprojectNdc(inv_view_proj, kNdcXY[i].x(), kNdcXY[i].y(), 1.0f);
        }
        return corners;
    }

    [[nodiscard]] Eigen::Matrix4f makeDirectionalLightView(const Eigen::Vector3f& light_dir)
    {
        Eigen::Vector3f forward = light_dir.normalized();
        Eigen::Vector3f up = (std::abs(forward.y()) > 0.99f)
            ? Eigen::Vector3f(1.0f, 0.0f, 0.0f)
            : Eigen::Vector3f(0.0f, 1.0f, 0.0f);
        Eigen::Vector3f right = forward.cross(up).normalized();
        up = right.cross(forward).normalized();

        Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
        view.block<1, 3>(0, 0) = right.transpose();
        view.block<1, 3>(1, 0) = up.transpose();
        view.block<1, 3>(2, 0) = forward.transpose();
        return view;
    }

    [[nodiscard]] Eigen::Matrix4f makeOrtho(
        float left, float right, float bottom, float top, float near_z, float far_z)
    {
        Eigen::Matrix4f proj = Eigen::Matrix4f::Zero();
        proj(0, 0) = 2.0f / (right - left);
        proj(1, 1) = -2.0f / (top - bottom); // Vulkan Y-down NDC
        proj(2, 2) = 1.0f / (far_z - near_z);
        proj(3, 3) = 1.0f;
        proj(0, 3) = -(right + left) / (right - left);
        proj(1, 3) = (top + bottom) / (top - bottom);
        proj(2, 3) = -near_z / (far_z - near_z);
        return proj;
    }

    [[nodiscard]] uint32_t roundUpPow2(uint32_t v)
    {
        if (v <= 1u)
            return 1u;
        --v;
        v |= v >> 1u;
        v |= v >> 2u;
        v |= v >> 4u;
        v |= v >> 8u;
        v |= v >> 16u;
        return v + 1u;
    }

    // ShadowTileAllocation + assignAtlasTileMetadata + makePerspectiveLightSlice
    // live in ShadowSliceMath.hpp now (pure, unit-tested shadow projection math).

    class ShadowAtlasPacker
    {
    public:
        ShadowAtlasPacker(uint32_t page_resolution, uint32_t page_count)
            : page_resolution_(std::max(page_resolution, 1u))
            , pages_(page_count)
        {
        }

        [[nodiscard]] uint32_t pageResolution() const noexcept
        {
            return page_resolution_;
        }

        [[nodiscard]] bool allocate(uint32_t requested_resolution, ShadowTileAllocation& out)
        {
            uint32_t tile_res = std::max(requested_resolution, 1u);
            tile_res = std::min(roundUpPow2(tile_res), page_resolution_);

            for (uint32_t layer = 0; layer < static_cast<uint32_t>(pages_.size()); ++layer)
            {
                auto& page = pages_[layer];
                uint32_t x = page.cursor_x;
                uint32_t y = page.cursor_y;
                uint32_t row_h = page.row_h;

                if (x + tile_res > page_resolution_)
                {
                    x = 0;
                    y += row_h;
                    row_h = 0;
                }
                if (y + tile_res > page_resolution_)
                    continue;

                page.cursor_x = x + tile_res;
                page.cursor_y = y;
                page.row_h = std::max(row_h, tile_res);

                out.layer = layer;
                out.x = x;
                out.y = y;
                out.resolution = tile_res;
                out.uv_scale = Eigen::Vector2f(
                    static_cast<float>(tile_res) / static_cast<float>(page_resolution_),
                    static_cast<float>(tile_res) / static_cast<float>(page_resolution_));
                out.uv_bias = Eigen::Vector2f(
                    static_cast<float>(x) / static_cast<float>(page_resolution_),
                    static_cast<float>(y) / static_cast<float>(page_resolution_));
                return true;
            }
            return false;
        }

    private:
        struct PageState
        {
            uint32_t cursor_x{0};
            uint32_t cursor_y{0};
            uint32_t row_h{0};
        };

        uint32_t page_resolution_{1};
        std::vector<PageState> pages_{};
    };

    [[nodiscard]] uint32_t normalizeShadowTileResolution(
        uint32_t requested_resolution,
        uint32_t page_resolution)
    {
        const uint32_t req = std::max(requested_resolution, 1u);
        const uint32_t page = std::max(page_resolution, 1u);
        return std::min(roundUpPow2(req), page);
    }

    [[nodiscard]] uint32_t chooseResolutionForRequiredTiles(
        uint32_t requested_resolution,
        uint32_t page_resolution,
        uint32_t page_count,
        uint32_t required_tiles)
    {
        const uint32_t page_res = std::max(page_resolution, 1u);
        const uint32_t pages = std::max(page_count, 1u);
        const uint32_t need_tiles = std::max(required_tiles, 1u);

        uint32_t resolution = normalizeShadowTileResolution(requested_resolution, page_res);
        while (true)
        {
            const uint32_t tiles_per_axis = std::max(page_res / std::max(resolution, 1u), 1u);
            const uint64_t capacity = static_cast<uint64_t>(tiles_per_axis)
                                    * static_cast<uint64_t>(tiles_per_axis)
                                    * static_cast<uint64_t>(pages);
            if (capacity >= need_tiles)
                return resolution;
            if (resolution <= 1u)
                return 1u;
            resolution >>= 1u;
        }
    }

    [[nodiscard]] bool allocateTileWithFallback(
        ShadowAtlasPacker& atlas_packer,
        uint32_t requested_resolution,
        ShadowTileAllocation& out_tile)
    {
        const uint32_t page_resolution = atlas_packer.pageResolution();
        const uint32_t min_resolution = normalizeShadowTileResolution(
            kMinShadowTileResolution,
            page_resolution);

        uint32_t resolution = normalizeShadowTileResolution(
            requested_resolution,
            page_resolution);
        while (true)
        {
            if (atlas_packer.allocate(resolution, out_tile))
                return true;
            if (resolution <= min_resolution)
                break;
            resolution = std::max(min_resolution, resolution >> 1u);
        }
        return false;
    }

    [[nodiscard]] bool allocateCubeWithFallback(
        ShadowAtlasPacker& atlas_packer,
        uint32_t requested_resolution,
        std::array<ShadowTileAllocation, 6>& out_tiles)
    {
        const uint32_t page_resolution = atlas_packer.pageResolution();
        const uint32_t min_resolution = normalizeShadowTileResolution(
            kMinShadowTileResolution,
            page_resolution);

        uint32_t resolution = normalizeShadowTileResolution(
            requested_resolution,
            page_resolution);
        while (true)
        {
            ShadowAtlasPacker test_packer = atlas_packer;
            bool allocated = true;
            for (uint32_t face = 0; face < 6; ++face)
            {
                if (!test_packer.allocate(resolution, out_tiles[face]))
                {
                    allocated = false;
                    break;
                }
            }
            if (allocated)
            {
                atlas_packer = std::move(test_packer);
                return true;
            }
            if (resolution <= min_resolution)
                break;
            resolution = std::max(min_resolution, resolution >> 1u);
        }
        return false;
    }

    template<typename LightType>
    [[nodiscard]] float scoreShadowCandidate(
        const LightType& light,
        const Eigen::Vector3f& position,
        const Eigen::Vector3f& camera_position,
        float non_directional_shadow_max_distance)
    {
        const float range = std::max(light.range, 0.0f);
        const float intensity = std::max(light.intensity, 0.0f);
        if (range <= 0.0f || intensity <= 0.0f)
            return 0.0f;

        const float dist = (camera_position - position).norm();
        float visibility = 1.0f;
        if (std::isfinite(non_directional_shadow_max_distance)
            && non_directional_shadow_max_distance > 0.0f)
        {
            if (dist > non_directional_shadow_max_distance)
                return 0.0f;
            visibility = std::clamp(
                1.0f - dist / std::max(non_directional_shadow_max_distance, kEpsilon),
                0.0f,
                1.0f);
        }
        return intensity * range * visibility;
    }

    void cmdUpdateBufferChunked(
        VkCommandBuffer cmd,
        VkBuffer buffer,
        VkDeviceSize dst_offset,
        const void* src_data,
        VkDeviceSize size_bytes)
    {
        if (cmd == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || src_data == nullptr || size_bytes == 0)
            return;

        constexpr VkDeviceSize kMaxChunkBytes = 65536u;
        const auto* src = static_cast<const std::byte*>(src_data);
        VkDeviceSize remaining = size_bytes;
        VkDeviceSize offset = dst_offset;
        while (remaining > 0)
        {
            VkDeviceSize chunk = std::min(remaining, kMaxChunkBytes);
            // vkCmdUpdateBuffer requires size and offset be multiples of 4.
            chunk &= ~VkDeviceSize(3u);
            if (chunk == 0)
                break;
            vkCmdUpdateBuffer(cmd, buffer, offset, chunk, src);
            src += chunk;
            offset += chunk;
            remaining -= chunk;
        }
    }
} // namespace

ShadowMapFeature::ShadowMapFeature(Config cfg)
    : RenderFeature(RenderFeature::Config{.name = "ShadowMap"})
    , cfg_(std::move(cfg))
{
    // Register all known shadow techniques up-front. Each technique is a
    // lightweight stub at C2 (id + lightingFragVariant only); resource /
    // pass-side polymorphism is wired in progressively from C3 onward.
    techniques_[static_cast<uint32_t>(EShadowTechnique::PCF)]  = std::make_unique<PCFShadowTechnique>();
    techniques_[static_cast<uint32_t>(EShadowTechnique::EVSM)] = std::make_unique<EVSMShadowTechnique>();
    active_technique_ = cfg_.shadow_config.default_technique;

    // Seed the editor-facing param mirror from the bring-up config so the
    // settings panel shows the real current quality, not the struct defaults.
    params_.atlas_page_resolution = cfg_.shadow_config.atlas_page_resolution;
    params_.atlas_page_count      = cfg_.shadow_config.atlas_page_count;
    params_.max_shadow_slices     = cfg_.shadow_config.max_shadow_slices;
    params_.non_directional_shadow_max_distance =
        cfg_.shadow_config.non_directional_shadow_max_distance;
    params_.enable_directional_csm = cfg_.shadow_config.enable_directional_csm;
}

ShadowMapFeature::~ShadowMapFeature()
{
    if (shadow_res_ && shadow_res_->isInitialized())
        shadow_res_->shutdown();
    // Each technique's destructor (via unique_ptr reset) calls its own
    // destroyResources() — EVSM atlas + sampler get freed there.
    for (auto& t : techniques_)
        if (t) t->destroyResources();
}

void ShadowMapFeature::initAndAttachTo(RenderScene& scene)
{
    if (initialized_)
        return;

    auto& ctx = renderContext();
    device_ = ctx.device();

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    shadow_ds_layout_id_ = ctx.descriptorService().registerLayout({
        .bindings = bindings,
        .flags = 0,
        .debug_name = "ShadowFeatureSet"
    });

    // M1 (Plan A): ShadowResources is per-scene now — lazily emplaced into this
    // scene's registry (only scenes with a shadow feature pay the atlas cost).
    // Registered AFTER the scene's LightResources (ctor) so reverse-order
    // shutdown tears Shadow down before Light (Shadow holds a raw Light*).
    auto& sreg = scene.sceneRegistry();
    shadow_res_ = sreg.find<ShadowResources>();
    if (!shadow_res_)
        shadow_res_ = sreg.emplace<ShadowResources>().get();
    if (!shadow_res_)
        return;

    // Publish the current technique (abstract pointer) into the shared resource so
    // MeshShadowFeature drives its caster + post passes polymorphically instead of
    // discovering this feature via dynamic_cast.
    shadow_res_->setCurrentTechnique(techniques_[static_cast<uint32_t>(active_technique_)].get());

    if (!shadow_res_->isInitialized())
    {
        ShadowResources::InitInfo init{};
        init.device = ctx.device();
        init.allocator = ctx.vmaAllocator();
        init.atlas_page_resolution = cfg_.shadow_config.atlas_page_resolution;
        init.atlas_page_count = cfg_.shadow_config.atlas_page_count;
        init.max_shadow_slices = cfg_.shadow_config.max_shadow_slices;
        init.frames_in_flight = ctx.framesInFlight();
        init.ds_layout_id = shadow_ds_layout_id_;
        init.descriptor_svc = &ctx.descriptorService();
        init.arena = &scene.descriptorArena();
        init.light_resources = scene.sceneRegistry().find<LightResources>();
        shadow_res_->init(init);
    }

    // EVSM resource allocation (C3b). Allocated ONLY when the scene's default
    // shadow technique is EVSM — a PCF-only scene must not pay the ~1 GB
    // RGBA16F moment+scratch atlas pair (H13). The technique is chosen at config
    // time; every EVSM consumer gates on activeTechnique()==EVSM /
    // resources().isInitialized(), so PCF-only scenes never touch these
    // resources. (A future runtime PCF->EVSM switch would allocate then, in
    // setActiveTechnique.)
    // No RTTI: the slot is indexed by enum and id() is the type discriminator → guard on
    // id() then static_cast.
    auto* evsm_base = techniques_[static_cast<uint32_t>(EShadowTechnique::EVSM)].get();
    EVSMShadowTechnique* evsm =
        (cfg_.shadow_config.default_technique == EShadowTechnique::EVSM
         && evsm_base && evsm_base->id() == EShadowTechnique::EVSM)
        ? static_cast<EVSMShadowTechnique*>(evsm_base) : nullptr;
    if (evsm)
    {
        EVSMShadowTechnique::InitInfo evsm_info{};
        evsm_info.device                = ctx.device();
        evsm_info.allocator             = ctx.vmaAllocator();
        evsm_info.atlas_page_resolution = cfg_.shadow_config.atlas_page_resolution;
        // EVSM atlas MUST share PCF atlas_page_count: slice metadata
        // (atlas_layer in ShadowSliceGPU) is computed once by the shared
        // ShadowAtlasPacker. If EVSM has fewer pages, slices that landed
        // on PCF's pages 4..N would write to non-existent EVSM layers →
        // silently dropped by the rasterizer → moments stay zero →
        // Chebyshev → 0 → those lights look fully shadowed.
        // `evsm_atlas_page_count` is kept as a knob but treated as a
        // **minimum**; if the PCF page count is larger, EVSM grows to match.
        evsm_info.atlas_page_count      = std::max(
            cfg_.shadow_config.atlas_page_count,
            cfg_.shadow_config.evsm_atlas_page_count);
        evsm_info.frames_in_flight      = ctx.framesInFlight();
        evsm_info.pos_exponent          = cfg_.shadow_config.evsm_pos_exponent;
        evsm_info.neg_exponent          = cfg_.shadow_config.evsm_neg_exponent;
        evsm_info.bleed_reduction       = cfg_.shadow_config.evsm_bleed_reduction;
        evsm->ensureResources(evsm_info);

        // Write EVSM bindings 9 (blurred atlas) and 10 (config UBO) into the
        // per-FIF light descriptor sets. These bindings are PARTIALLY_BOUND
        // in the light DS layout (see GeneralDescriptorSetLayout.cpp), so PCF
        // mode running with bindings 9/10 written is benign — the PCF SPIR-V
        // simply doesn't reference them. EVSM mode requires them written;
        // doing the write up-front keeps runtime technique switching cheap.
        if (evsm->resources().isInitialized())
        {
            auto* light_res = scene.sceneRegistry().find<LightResources>();
            if (light_res)
            {
                writeEVSMBindings(*light_res, evsm->resources());
            }
            // EVSM owns its blur pipelines now — build them via the technique
            // (moved out of this feature; see EVSMShadowTechnique::ensureBlurPipelines).
            evsm->ensureBlurPipelines(ctx);
        }
    }

    initialized_ = true;
}

void ShadowMapFeature::writeEVSMBindings(LightResources& light_res,
                                          EVSMShadowResources& evsm_res)
{
    auto light_ds_list = light_res.allDescriptorSets();
    const uint32_t fif = evsm_res.framesInFlight();

    VkDescriptorImageInfo atlas_info{};
    atlas_info.sampler     = evsm_res.sampler();
    atlas_info.imageView   = evsm_res.blurredView();
    atlas_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (uint32_t fi = 0; fi < static_cast<uint32_t>(light_ds_list.size()); ++fi)
    {
        VkDescriptorSet ds = light_ds_list[fi];
        if (ds == VK_NULL_HANDLE) continue;

        VkDescriptorBufferInfo ubo_info{};
        ubo_info.buffer = evsm_res.configUBO(fi % fif);
        ubo_info.offset = 0;
        ubo_info.range  = sizeof(EVSMShadowResources::ConfigGPU);

        std::array<VkWriteDescriptorSet, 2> w{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = ds;
        w[0].dstBinding      = static_cast<uint32_t>(ELightSetBindings::SHADOW_ATLAS_EVSM);
        w[0].descriptorCount = 1;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo      = &atlas_info;

        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = ds;
        w[1].dstBinding      = static_cast<uint32_t>(ELightSetBindings::SHADOW_EVSM_CONFIG);
        w[1].descriptorCount = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo     = &ubo_info;

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(w.size()),
                               w.data(), 0, nullptr);
    }
}

uint64_t ShadowMapFeature::computeLightConfigHash(LightResources* light_res) const
{
    // FNV-1a 64-bit hash of shadow-relevant light parameters.
    // Only fields that affect shadow slice computation are included.
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    auto mix_val = [&](auto v) { mix(&v, sizeof(v)); };

    light_res->forEachLight<DirectionalLightGPU>([&](uint32_t, const DirectionalLightGPU& dl) {
        mix_val(dl.direction.x); mix_val(dl.direction.y); mix_val(dl.direction.z);
        mix_val(dl.flags); mix_val(dl.shadow_map_size); mix_val(dl.shadow_bias);
        mix_val(dl.shadow_normal_bias); mix_val(dl.cascade_count);
        mix(dl.cascade_splits, sizeof(dl.cascade_splits));
    });
    light_res->forEachLight<PointLightGPU>([&](uint32_t, const PointLightGPU& pl) {
        mix_val(pl.position.x); mix_val(pl.position.y); mix_val(pl.position.z);
        mix_val(pl.range); mix_val(pl.flags); mix_val(pl.shadow_map_size);
        mix_val(pl.shadow_bias); mix_val(pl.shadow_normal_bias);
    });
    light_res->forEachLight<SpotLightGPU>([&](uint32_t, const SpotLightGPU& sl) {
        mix_val(sl.position.x); mix_val(sl.position.y); mix_val(sl.position.z);
        mix_val(sl.direction.x); mix_val(sl.direction.y); mix_val(sl.direction.z);
        mix_val(sl.range); mix_val(sl.outer_cone_angle); mix_val(sl.flags);
        mix_val(sl.shadow_map_size); mix_val(sl.shadow_bias); mix_val(sl.shadow_normal_bias);
    });

    // Include light counts so additions/removals are detected.
    mix_val(light_res->lightCount<DirectionalLightGPU>());
    mix_val(light_res->lightCount<PointLightGPU>());
    mix_val(light_res->lightCount<SpotLightGPU>());

    return h;
}

void ShadowMapFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    if (!initialized_ || !shadow_res_)
        return;

    // Swap: move current → prev, then clear current for this frame.
    std::swap(per_view_shadow_, prev_view_shadow_);
    per_view_shadow_.clear();

    auto* light_res = renderScene().sceneRegistry().find<LightResources>();
    if (!light_res)
        return;

    const uint64_t light_hash = computeLightConfigHash(light_res);

    // The source of truth for render-thread consumers + MeshShadowFeature (later
    // this same frame) is the ShadowResources per-view cache (setCachedData);
    // per_view_shadow_ is just the CPU rotation buffer. So populate the cache on a
    // REBUILD only — on a fingerprint HIT the cache already holds identical data
    // from the last rebuild. This drops the two full per-view copies/frame at rest:
    // the HIT deep-copy (now a move from prev) and the unconditional re-snapshot
    // (now skipped). (P-4 / [M13])
    const uint32_t scene_key = renderScene().sceneGlobalSlot().index;
    auto* cam = renderScene().sceneRegistry().find<ViewCameraResource>();
    renderScene().forEachActiveView([&](View& view) {
        const ViewFrameData* cam_fd = cam ? cam->find(view.handle.index) : nullptr;
        ViewFrameData vfd = cam_fd ? *cam_fd : ViewFrameData{};
        PerViewShadowFingerprint new_fp;
        new_fp.view_proj = vfd.camera_view.view_proj;
        new_fp.camera_pos = vfd.camera_transform.position;
        new_fp.light_config_hash = light_hash;
        new_fp.shadow_config_serial = shadow_config_serial_;

        const auto* prev_fp = per_view_fingerprint_.find(view.handle.index);
        if (prev_fp != nullptr
            && prev_fp->light_config_hash == new_fp.light_config_hash
            && prev_fp->shadow_config_serial == new_fp.shadow_config_serial
            && prev_fp->view_proj.isApprox(new_fp.view_proj, 1e-5f)
            && (prev_fp->camera_pos - new_fp.camera_pos).squaredNorm() < 1e-8f)
        {
            // Fingerprint match — reuse last frame's slices by MOVE (prev_view_shadow_
            // is swapped+cleared next frame, so moving-out is safe) and skip the cache
            // re-snapshot (it already holds identical data). (P-4)
            auto* prev_state = prev_view_shadow_.find(view.handle.index);
            if (prev_state != nullptr) {
                per_view_shadow_.set(view.handle.index, std::move(*prev_state));
                return;
            }
        }

        // Fingerprint mismatch or no previous data — full rebuild + cache refresh.
        // setCachedData must read state BEFORE it is moved into per_view_shadow_.
        PerViewShadowState state{};
        buildSlicesForView(view, light_res, state);
        shadow_res_->setCachedData(
            scene_key, view.handle.index, state.slices,
            state.spot_shadow_slice_index, state.point_shadow_base_slice, state.config);
        per_view_shadow_.set(view.handle.index, std::move(state));
        per_view_fingerprint_.set(view.handle.index, new_fp);
    });
}

void ShadowMapFeature::addPasses(RGBuilder& builder)
{
    if (!initialized_ || !shadow_res_)
        return;

    RGTextureDescription shadow_tex_desc{};
    shadow_tex_desc.width        = shadow_res_->atlasResolution();
    shadow_tex_desc.height       = shadow_res_->atlasResolution();
    shadow_tex_desc.array_layers = shadow_res_->atlasPageCount();
    shadow_tex_desc.format       = lux::common::ETextureFormat::D32_SFLOAT;
    shadow_tex_desc.usage        = ERGTextureUsageBits::DEPTH_STENCIL | ERGTextureUsageBits::SAMPLED;
    shadow_tex_desc.dimension    = lux::common::ETextureDimension::TEX_2D_ARRAY;

    // Import shadow atlas (single image, shared across FIF — depth writes are idempotent)
    RGImportedResourceInfo import_info{};
    import_info.image_getter = [this](VkImage* out_images, uint32_t capacity) -> uint32_t {
        if (out_images == nullptr || capacity == 0 || shadow_res_ == nullptr)
            return 0u;
        out_images[0] = shadow_res_->atlasImage();
        return 1u;
    };
    import_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    import_info.final_layout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    auto shadow_atlas = builder.importTexture(cfg_.shadow_atlas, shadow_tex_desc, import_info);

    // ── EVSM atlas imports (C3c) ──────────────────────────────────────────
    // The 3 RGBA16F atlases live alongside the PCF D32 atlas and follow the
    // same atlas-tile coordinate layout (slice metadata is shared). They are
    // imported here so downstream EVSM caster + blur passes (added when the
    // active technique is EVSM) can reference them. PCF mode never reads
    // these RG handles; the imports are inert in that case.
    auto* evsm_base2 = techniques_[static_cast<uint32_t>(EShadowTechnique::EVSM)].get();
    auto* evsm_tech = (evsm_base2 && evsm_base2->id() == EShadowTechnique::EVSM)
        ? static_cast<EVSMShadowTechnique*>(evsm_base2) : nullptr;
    if (evsm_tech && evsm_tech->resources().isInitialized())
    {
        const auto& evsm_res = evsm_tech->resources();
        RGTextureDescription evsm_tex_desc{};
        evsm_tex_desc.width        = evsm_res.pageResolution();
        evsm_tex_desc.height       = evsm_res.pageResolution();
        evsm_tex_desc.array_layers = evsm_res.pageCount();
        evsm_tex_desc.format       = lux::common::ETextureFormat::RGBA16_SFLOAT;
        evsm_tex_desc.usage  = ERGTextureUsageBits::COLOR_ATTACHMENT | ERGTextureUsageBits::SAMPLED;
        evsm_tex_desc.usage |= ERGTextureUsageBits::STORAGE;
        evsm_tex_desc.dimension    = lux::common::ETextureDimension::TEX_2D_ARRAY;

        auto make_evsm_import = [&](VkImage image)
        {
            RGImportedResourceInfo info{};
            info.image_getter = [image](VkImage* out, uint32_t cap) -> uint32_t {
                if (!out || cap == 0) return 0u;
                out[0] = image;
                return 1u;
            };
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            info.final_layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };
        builder.importTexture("evsm_moment_atlas",  evsm_tex_desc, make_evsm_import(evsm_res.momentImage()));
        builder.importTexture("evsm_scratch_atlas", evsm_tex_desc, make_evsm_import(evsm_res.scratchImage()));
        // No separate "evsm_blurred_atlas": the separable blur ping-pongs back
        // into the moment image (blur_v writes evsm_moment_atlas), so lighting
        // samples evsm_moment_atlas as the final blurred result. (3 atlases → 2.)
    }

    // Import per-FIF shadow slice SSBO
    RGBufferDescription slice_buf_desc{};
    slice_buf_desc.size = sizeof(ShadowSliceGPU) * shadow_res_->maxSlices();
    slice_buf_desc.stride = sizeof(ShadowSliceGPU);
    slice_buf_desc.element_count = shadow_res_->maxSlices();
    slice_buf_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
    slice_buf_desc.memory_usage = ERGMemoryUsage::CPU_TO_GPU;

    RGImportedBufferInfo slice_import{};
    slice_import.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
        if (out_buffers == nullptr || capacity == 0)
            return 0u;
        const uint32_t count = std::min(shadow_res_->framesInFlight(), capacity);
        for (uint32_t fi = 0; fi < count; ++fi)
            out_buffers[fi] = shadow_res_->sliceBuffer(fi);
        return count;
    };
    slice_import.initial_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    slice_import.initial_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    slice_import.final_stage    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    slice_import.final_access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    auto shadow_slices = builder.importBuffer("shadow_slices", slice_buf_desc, slice_import);

    // Import per-FIF shadow config UBO
    RGBufferDescription config_buf_desc{};
    config_buf_desc.size = sizeof(ShadowConfigGPU);
    config_buf_desc.stride = sizeof(ShadowConfigGPU);
    config_buf_desc.element_count = 1;
    config_buf_desc.usage = ERGBufferUsageBits::UNIFORM | ERGBufferUsageBits::TRANSFER_DST;
    config_buf_desc.memory_usage = ERGMemoryUsage::CPU_TO_GPU;

    RGImportedBufferInfo config_import{};
    config_import.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
        if (out_buffers == nullptr || capacity == 0)
            return 0u;
        const uint32_t count = std::min(shadow_res_->framesInFlight(), capacity);
        for (uint32_t fi = 0; fi < count; ++fi)
            out_buffers[fi] = shadow_res_->configBuffer(fi);
        return count;
    };
    config_import.initial_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    config_import.initial_access = VK_ACCESS_2_UNIFORM_READ_BIT;
    config_import.final_stage    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    config_import.final_access   = VK_ACCESS_2_UNIFORM_READ_BIT;
    auto shadow_config = builder.importBuffer("shadow_config", config_buf_desc, config_import);

    // Import per-FIF spot shadow mapping buffer (slot -> slice index)
    RGBufferDescription spot_map_desc{};
    spot_map_desc.size = static_cast<VkDeviceSize>(sizeof(int32_t)) * shadow_res_->shadowMapCapacity();
    spot_map_desc.stride = sizeof(int32_t);
    spot_map_desc.element_count = shadow_res_->shadowMapCapacity();
    spot_map_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
    spot_map_desc.memory_usage = ERGMemoryUsage::CPU_TO_GPU;

    RGImportedBufferInfo spot_map_import{};
    spot_map_import.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
        if (out_buffers == nullptr || capacity == 0)
            return 0u;
        const uint32_t count = std::min(shadow_res_->framesInFlight(), capacity);
        for (uint32_t fi = 0; fi < count; ++fi)
            out_buffers[fi] = shadow_res_->spotMapBuffer(fi);
        return count;
    };
    spot_map_import.initial_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    spot_map_import.initial_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    spot_map_import.final_stage    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    spot_map_import.final_access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    auto spot_shadow_map = builder.importBuffer("shadow_spot_map", spot_map_desc, spot_map_import);

    // Import per-FIF point shadow mapping buffer (slot -> base cube-face slice index)
    RGBufferDescription point_map_desc{};
    point_map_desc.size = static_cast<VkDeviceSize>(sizeof(int32_t)) * shadow_res_->shadowMapCapacity();
    point_map_desc.stride = sizeof(int32_t);
    point_map_desc.element_count = shadow_res_->shadowMapCapacity();
    point_map_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
    point_map_desc.memory_usage = ERGMemoryUsage::CPU_TO_GPU;

    RGImportedBufferInfo point_map_import{};
    point_map_import.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
        if (out_buffers == nullptr || capacity == 0)
            return 0u;
        const uint32_t count = std::min(shadow_res_->framesInFlight(), capacity);
        for (uint32_t fi = 0; fi < count; ++fi)
            out_buffers[fi] = shadow_res_->pointMapBuffer(fi);
        return count;
    };
    point_map_import.initial_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    point_map_import.initial_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    point_map_import.final_stage    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    point_map_import.final_access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    auto point_shadow_map = builder.importBuffer("shadow_point_map", point_map_desc, point_map_import);

    // Upload per-view shadow slice/config data before any pass reads shadow buffers.
    builder.addPass("ShadowViewUpload", ERGPassType::TRANSFER)
        .write(shadow_slices, ERGBufferRole::STORAGE)
        .write(shadow_config, ERGBufferRole::STORAGE)
        .write(spot_shadow_map, ERGBufferRole::STORAGE)
        .write(point_shadow_map, ERGBufferRole::STORAGE)
        .setKernelFn([this, shadow_slices, shadow_config, spot_shadow_map, point_shadow_map]
            (const PassRecordContext& pctx) {
            if (!shadow_res_)
                return;

            const uint32_t view_handle = (pctx.view != nullptr) ? pctx.view->handle.index : 0u;
            const uint32_t scene_key   = renderScene().sceneGlobalSlot().index;

            // IMPORTANT: read from ShadowResources cache, NOT per_view_shadow_.
            //
            // Why: per_view_shadow_ is rotated/cleared at the top of every
            // ShadowMapFeature::onFrameBegin (std::swap + clear), and this
            // kernel callback can fire on the render thread *after* the next
            // frame's onFrameBegin has already started — which means
            // resolveViewState() may return an in-transit, partially-cleared
            // state with slices.size() == 0. Empty state → empty SSBO update →
            // shadow shader sees no slices → black-out (verified by stderr
            // instrumentation on 2026-06-05).
            //
            // The fix: ShadowMapFeature::onFrameBegin eagerly calls
            // shadow_res_->setCachedData() with the freshly-built slices at
            // the same synchronous point where it builds them — that's the
            // authoritative write. This kernel reads back from cache for the
            // SSBO upload and trusts that snapshot. No second setCachedData
            // here, because the kernel-time state can be the racy zero-slice
            // copy and would corrupt the good cache.
            // Hold the snapshot for the whole SSBO-update read below: this runs on
            // the render thread and may overlap the next frame's setCachedData on
            // the frame thread; the shared_ptr pins cache->slices against realloc.
            auto cache = shadow_res_->findViewCache(scene_key, view_handle);
            if (!cache)
                return;

            // Bump frame stamp on the existing cache entry so any downstream
            // consumer that tracks frame_id sees this frame's id (the eager
            // setCachedData in onFrameBegin doesn't have frame info).
            shadow_res_->stampCacheFrame(scene_key, view_handle,
                                         pctx.frame.frame_id,
                                         pctx.frame.frame_index);

            VkBuffer slice_buf = pctx.resolveBufferHandle(shadow_slices);
            VkBuffer config_buf = pctx.resolveBufferHandle(shadow_config);
            VkBuffer spot_map_buf = pctx.resolveBufferHandle(spot_shadow_map);
            VkBuffer point_map_buf = pctx.resolveBufferHandle(point_shadow_map);
            if (slice_buf == VK_NULL_HANDLE
                || config_buf == VK_NULL_HANDLE
                || spot_map_buf == VK_NULL_HANDLE
                || point_map_buf == VK_NULL_HANDLE)
                return;

            std::array<VkBufferMemoryBarrier2, 4> pre_barriers{};
            pre_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            pre_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            pre_barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            pre_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre_barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[0].buffer = slice_buf;
            pre_barriers[0].offset = 0;
            pre_barriers[0].size = VK_WHOLE_SIZE;

            pre_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            pre_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            pre_barriers[1].srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
            pre_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre_barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[1].buffer = config_buf;
            pre_barriers[1].offset = 0;
            pre_barriers[1].size = VK_WHOLE_SIZE;

            pre_barriers[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            pre_barriers[2].srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            pre_barriers[2].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            pre_barriers[2].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre_barriers[2].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre_barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[2].buffer = spot_map_buf;
            pre_barriers[2].offset = 0;
            pre_barriers[2].size = VK_WHOLE_SIZE;

            pre_barriers[3].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            pre_barriers[3].srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                         | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            pre_barriers[3].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            pre_barriers[3].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre_barriers[3].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre_barriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre_barriers[3].buffer = point_map_buf;
            pre_barriers[3].offset = 0;
            pre_barriers[3].size = VK_WHOLE_SIZE;

            VkDependencyInfo pre_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            pre_dep.bufferMemoryBarrierCount = static_cast<uint32_t>(pre_barriers.size());
            pre_dep.pBufferMemoryBarriers = pre_barriers.data();
            vkCmdPipelineBarrier2(pctx.cmd, &pre_dep);

            // SSBO upload sources from the cache (set safely in onFrameBegin) — see
            // big comment above for why state->slices can't be trusted here.
            const VkDeviceSize slices_size = static_cast<VkDeviceSize>(cache->slices.size())
                                           * sizeof(ShadowSliceGPU);
            if (slices_size > 0)
            {
                cmdUpdateBufferChunked(pctx.cmd, slice_buf, 0, cache->slices.data(), slices_size);
            }
            cmdUpdateBufferChunked(pctx.cmd, config_buf, 0, &cache->config, sizeof(ShadowConfigGPU));

            const VkDeviceSize map_capacity_bytes =
                static_cast<VkDeviceSize>(shadow_res_->shadowMapCapacity()) * sizeof(int32_t);
            const VkDeviceSize spot_map_size =
                static_cast<VkDeviceSize>(cache->spot_shadow_slice_index.size()) * sizeof(int32_t);
            if (spot_map_size > 0)
            {
                const VkDeviceSize clamped_size = std::min(spot_map_size, map_capacity_bytes);
                cmdUpdateBufferChunked(
                    pctx.cmd,
                    spot_map_buf,
                    0,
                    cache->spot_shadow_slice_index.data(),
                    clamped_size);
            }

            const VkDeviceSize point_map_size =
                static_cast<VkDeviceSize>(cache->point_shadow_base_slice.size()) * sizeof(int32_t);
            if (point_map_size > 0)
            {
                const VkDeviceSize clamped_size = std::min(point_map_size, map_capacity_bytes);
                cmdUpdateBufferChunked(
                    pctx.cmd,
                    point_map_buf,
                    0,
                    cache->point_shadow_base_slice.data(),
                    clamped_size);
            }

            std::array<VkBufferMemoryBarrier2, 4> post_barriers{};
            post_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            post_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post_barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            post_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            post_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[0].buffer = slice_buf;
            post_barriers[0].offset = 0;
            post_barriers[0].size = VK_WHOLE_SIZE;

            post_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            post_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post_barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            post_barriers[1].dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
            post_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[1].buffer = config_buf;
            post_barriers[1].offset = 0;
            post_barriers[1].size = VK_WHOLE_SIZE;

            post_barriers[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            post_barriers[2].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post_barriers[2].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post_barriers[2].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            post_barriers[2].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            post_barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[2].buffer = spot_map_buf;
            post_barriers[2].offset = 0;
            post_barriers[2].size = VK_WHOLE_SIZE;

            post_barriers[3].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            post_barriers[3].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post_barriers[3].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post_barriers[3].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            post_barriers[3].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            post_barriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post_barriers[3].buffer = point_map_buf;
            post_barriers[3].offset = 0;
            post_barriers[3].size = VK_WHOLE_SIZE;

            VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            post_dep.bufferMemoryBarrierCount = static_cast<uint32_t>(post_barriers.size());
            post_dep.pBufferMemoryBarriers = post_barriers.data();
            vkCmdPipelineBarrier2(pctx.cmd, &post_dep);
        })
        .setKernel("ShadowViewUpload");
}

bool ShadowMapFeature::updateQuality(
    uint32_t atlas_page_resolution,
    uint32_t atlas_page_count,
    uint32_t max_shadow_slices,
    float non_directional_shadow_max_distance)
{
    if (!initialized_ || !shadow_res_)
        return false;

    const uint32_t new_page_resolution =
        (atlas_page_resolution > 0) ? atlas_page_resolution : cfg_.shadow_config.atlas_page_resolution;
    const uint32_t new_page_count =
        (atlas_page_count > 0) ? atlas_page_count : cfg_.shadow_config.atlas_page_count;
    const uint32_t new_slice_budget =
        (max_shadow_slices > 0) ? max_shadow_slices : cfg_.shadow_config.max_shadow_slices;
    float new_non_directional_max_distance = cfg_.shadow_config.non_directional_shadow_max_distance;
    if (std::isfinite(non_directional_shadow_max_distance))
    {
        if (non_directional_shadow_max_distance >= 0.0f)
            new_non_directional_max_distance = non_directional_shadow_max_distance;
    }

    bool needs_rebuild = false;
    if (new_page_resolution != cfg_.shadow_config.atlas_page_resolution)
        needs_rebuild = true;
    if (new_page_count != cfg_.shadow_config.atlas_page_count)
        needs_rebuild = true;
    if (new_slice_budget != cfg_.shadow_config.max_shadow_slices)
        needs_rebuild = true;

    cfg_.shadow_config.non_directional_shadow_max_distance = new_non_directional_max_distance;
    cfg_.shadow_config.atlas_page_resolution = new_page_resolution;
    cfg_.shadow_config.atlas_page_count = new_page_count;
    cfg_.shadow_config.max_shadow_slices = new_slice_budget;

    if (!needs_rebuild)
        return false;

    ++shadow_config_serial_;

    // Requires GPU idle — the caller (operation handler in tick()) guarantees this
    // because the dispatch happens before rendering.
    (void)shadow_res_->tryRebuild(
        new_page_resolution,
        new_page_count,
        new_slice_budget);
    return true;
}

void ShadowMapFeature::setDirectionalCsmEnabled(bool enabled)
{
    cfg_.shadow_config.enable_directional_csm = enabled ? 1u : 0u;
    ++shadow_config_serial_;
}

RenderFeature::EParamApply ShadowMapFeature::applyParams(const void* src, std::size_t size)
{
    if (src == nullptr || size != sizeof(ShadowQualityParams))
        return EParamApply::UNSUPPORTED;

    ShadowQualityParams next{};
    std::memcpy(&next, src, sizeof(ShadowQualityParams));
    params_ = next;   // refresh the editor-facing mirror

    // CSM toggle is a hot per-frame read (no rebuild); the atlas knobs may rebuild
    // GPU resources. updateQuality returns true iff it actually rebuilt the atlas.
    setDirectionalCsmEnabled(next.enable_directional_csm != 0u);
    const bool atlas_rebuilt = updateQuality(
        next.atlas_page_resolution,
        next.atlas_page_count,
        next.max_shadow_slices,
        next.non_directional_shadow_max_distance);

    // Strongest verdict wins: a rebuilt atlas needs a graph recompile (the shared
    // handler calls RenderScene::invalidateGraph on NEEDS_RECOMPILE), matching the
    // old handleUpdateShadowQuality path.
    return atlas_rebuilt ? EParamApply::NEEDS_RECOMPILE : EParamApply::HOT;
}

void ShadowMapFeature::setActiveTechnique(EShadowTechnique technique) noexcept
{
    if (technique == active_technique_)
        return;
    const auto idx = static_cast<uint32_t>(technique);
    if (idx >= techniques_.size() || !techniques_[idx])
        return;   // unknown — leave current active in place
    active_technique_ = technique;
    if (shadow_res_)
        shadow_res_->setCurrentTechnique(techniques_[idx].get());   // keep the shared SSOT in sync
    ++shadow_config_serial_;   // signal lighting feature(s) to rebuild pipelines
}

IShadowTechnique& ShadowMapFeature::currentTechnique() noexcept
{
    return *techniques_[static_cast<uint32_t>(active_technique_)];
}

const IShadowTechnique& ShadowMapFeature::currentTechnique() const noexcept
{
    return *techniques_[static_cast<uint32_t>(active_technique_)];
}

const ShadowMapFeature::PerViewShadowState* ShadowMapFeature::resolveViewState(uint32_t view_handle) const
{
    return per_view_shadow_.find(view_handle);
}

void ShadowMapFeature::buildSlicesForView(
    const View& view, LightResources* light_res, PerViewShadowState& out_state)
{
    out_state.slices.clear();
    out_state.spot_shadow_slice_index.clear();
    out_state.point_shadow_base_slice.clear();
    out_state.config = {};

    if (!light_res)
        return;

    const uint32_t max_slices = cfg_.shadow_config.max_shadow_slices;
    if (max_slices == 0)
        return;

    ShadowAtlasPacker atlas_packer(
        cfg_.shadow_config.atlas_page_resolution,
        cfg_.shadow_config.atlas_page_count);

    out_state.slices.reserve(max_slices);
    out_state.config.dir_light_offset = 0;
    out_state.config.total_slices = 0;
    out_state.config.dir_caster_slot = 0;   // set to the real caster slot below (C-6)

    // Per-view 3D camera state now lives in ViewCameraResource (found-or-default
    // preserves the old behavior of reading a zero cached_frame_data when absent).
    auto* cam = renderScene().sceneRegistry().find<ViewCameraResource>();
    const ViewFrameData* cam_fd = cam ? cam->find(view.handle.index) : nullptr;
    const ViewFrameData vfd = cam_fd ? *cam_fd : ViewFrameData{};

    const uint32_t map_capacity = (shadow_res_ != nullptr)
        ? shadow_res_->shadowMapCapacity()
        : std::numeric_limits<uint32_t>::max();
    const uint32_t spot_count = std::min(light_res->lightCount<SpotLightGPU>(), map_capacity);
    const uint32_t point_count = std::min(light_res->lightCount<PointLightGPU>(), map_capacity);
    out_state.spot_shadow_slice_index.assign(spot_count, -1);
    out_state.point_shadow_base_slice.assign(point_count, -1);

    bool dir_found = false;
    light_res->forEachLight<DirectionalLightGPU>([&](uint32_t slot, const DirectionalLightGPU& dl) {
        if (dir_found || out_state.slices.size() >= max_slices)
            return;
        if ((dl.flags & LF_CAST_SHADOW) == 0)
            return;

        const uint32_t requested_cascades = std::clamp(dl.cascade_count, 1u, 4u);
        const bool directional_csm_enabled = (cfg_.shadow_config.enable_directional_csm != 0u);
        const uint32_t cascades = directional_csm_enabled ? requested_cascades : 1u;
        const uint32_t reserved_non_directional_tiles =
            (point_count > 0 ? 6u : 0u) + (spot_count > 0 ? 1u : 0u);
        const uint32_t directional_tile_request = chooseResolutionForRequiredTiles(
            std::max(dl.shadow_map_size, 1u),
            cfg_.shadow_config.atlas_page_resolution,
            cfg_.shadow_config.atlas_page_count,
            cascades + reserved_non_directional_tiles);
        Eigen::Vector3f light_dir(dl.direction.x, dl.direction.y, dl.direction.z);
        if (light_dir.norm() < kEpsilon)
            return;

        const auto& camera = vfd.camera_view;
        auto full_frustum_corners = buildFrustumCorners(camera.inv_view_proj);

        float camera_near = 0.1f;
        float camera_far = 0.0f;
        if (!tryExtractPerspectiveNearFar(camera.proj, camera_near, camera_far))
        {
            float max_split = dl.cascade_splits[requested_cascades - 1];
            if (max_split <= 1.0001f)
                max_split = 100.0f;
            camera_far = std::max(max_split, camera_near + 50.0f);
        }

        // In single-slice directional mode, prefer the first cascade distance
        // as shadow range to keep texel density reasonable (less blurry).
        float single_slice_far = camera_far;
        if (!directional_csm_enabled)
        {
            // FIRST split (index 0), not the last — reading cascade_splits[N-1]
            // made single_slice_far converge to ~camera_far, fitting the single
            // ortho slice to the whole frustum and making it ~15x blurrier than
            // the intended first-cascade range. (C-5)
            const float split0 = dl.cascade_splits[0];
            if (std::isfinite(split0) && split0 > 1.0001f)
            {
                single_slice_far = std::clamp(
                    split0,
                    camera_near + kMinCascadeDepthSpan,
                    camera_far
                );
            }
        }

        // CSM split interpretation (single-slice mode uses absolute distances).
        const bool normalized_splits = directional_csm_enabled
            ? (dl.cascade_splits[cascades - 1] <= 1.0001f)
            : false;
        const float depth_span = std::max(camera_far - camera_near, kMinCascadeDepthSpan);
        Eigen::Matrix4f light_view = makeDirectionalLightView(light_dir);

        out_state.config.dir_split_is_normalized = normalized_splits ? 1.0f : 0.0f;
        out_state.config.dir_split_near = camera_near;
        out_state.config.dir_split_far = camera_far;

        uint32_t generated = 0;
        for (uint32_t c = 0; c < cascades && out_state.slices.size() < max_slices; ++c)
        {
            const bool is_last_cascade = (c + 1u == cascades);
            const float split_prev_cfg = (c == 0)
                ? (normalized_splits ? 0.0f : camera_near)
                : dl.cascade_splits[c - 1];
            // CSM mode: ensure the final cascade reaches camera_far.
            // Single-slice mode: clamp to single_slice_far for better texel density.
            const float split_curr_cfg = is_last_cascade
                ? (normalized_splits ? 1.0f : (directional_csm_enabled ? camera_far : single_slice_far))
                : dl.cascade_splits[c];

            float segment_near = 0.0f;
            float segment_far = 0.0f;
            if (normalized_splits)
            {
                const float t0 = std::clamp(split_prev_cfg, 0.0f, 1.0f);
                const float t1 = std::clamp(split_curr_cfg, 0.0f, 1.0f);
                segment_near = camera_near + depth_span * t0;
                segment_far  = camera_near + depth_span * t1;
            }
            else
            {
                segment_near = std::clamp(split_prev_cfg, camera_near, camera_far);
                segment_far  = std::clamp(split_curr_cfg, segment_near + kMinCascadeDepthSpan, camera_far);
            }

            if (segment_far - segment_near < kMinCascadeDepthSpan)
                continue;

            ShadowTileAllocation tile{};
            if (!allocateTileWithFallback(
                    atlas_packer,
                    directional_tile_request,
                    tile))
                continue;

            const uint32_t map_resolution = std::max(tile.resolution, 1u);
            const float t_near = std::clamp((segment_near - camera_near) / depth_span, 0.0f, 1.0f);
            const float t_far  = std::clamp((segment_far  - camera_near) / depth_span, 0.0f, 1.0f);

            std::array<Eigen::Vector3f, 8> segment_corners{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                const Eigen::Vector3f ray = full_frustum_corners[i + 4] - full_frustum_corners[i];
                segment_corners[i]     = full_frustum_corners[i] + ray * t_near;
                segment_corners[i + 4] = full_frustum_corners[i] + ray * t_far;
            }

            Eigen::Vector3f min_ls(
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max());
            Eigen::Vector3f max_ls(
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest());

            for (const auto& corner_ws : segment_corners)
            {
                const Eigen::Vector3f corner_ls = (light_view * Eigen::Vector4f(
                    corner_ws.x(), corner_ws.y(), corner_ws.z(), 1.0f)).head<3>();
                min_ls = min_ls.cwiseMin(corner_ls);
                max_ls = max_ls.cwiseMax(corner_ls);
            }

            Eigen::Vector3f extent = max_ls - min_ls;
            if (extent.x() < kMinCascadeDepthSpan || extent.y() < kMinCascadeDepthSpan)
                continue;

            float units_per_texel_x = 0.0f;
            float units_per_texel_y = 0.0f;
            if (map_resolution > 0)
            {
                units_per_texel_x = extent.x() / static_cast<float>(map_resolution);
                units_per_texel_y = extent.y() / static_cast<float>(map_resolution);
            }

            // Guard-band XY bounds so camera motion / stabilization rounding does not
            // create tiny uncovered holes near cascade borders.
            const float xy_pad_x = std::max(
                extent.x() * kCascadeXYPaddingFactor,
                units_per_texel_x * kCascadeStabilizeGuardTexels);
            const float xy_pad_y = std::max(
                extent.y() * kCascadeXYPaddingFactor,
                units_per_texel_y * kCascadeStabilizeGuardTexels);
            min_ls.x() -= xy_pad_x;
            max_ls.x() += xy_pad_x;
            min_ls.y() -= xy_pad_y;
            max_ls.y() += xy_pad_y;

            extent = max_ls - min_ls;
            Eigen::Vector3f center = (min_ls + max_ls) * 0.5f;
            if (map_resolution > 0)
            {
                units_per_texel_x = extent.x() / static_cast<float>(map_resolution);
                units_per_texel_y = extent.y() / static_cast<float>(map_resolution);
                if (units_per_texel_x > kEpsilon)
                    center.x() = std::floor(center.x() / units_per_texel_x + 0.5f) * units_per_texel_x;
                if (units_per_texel_y > kEpsilon)
                    center.y() = std::floor(center.y() / units_per_texel_y + 0.5f) * units_per_texel_y;
            }

            const float half_x = extent.x() * 0.5f + units_per_texel_x * kCascadeStabilizeGuardTexels;
            const float half_y = extent.y() * 0.5f + units_per_texel_y * kCascadeStabilizeGuardTexels;
            min_ls.x() = center.x() - half_x;
            max_ls.x() = center.x() + half_x;
            min_ls.y() = center.y() - half_y;
            max_ls.y() = center.y() + half_y;

            const float z_span = std::max(max_ls.z() - min_ls.z(), kMinCascadeDepthSpan);
            const float z_pad = std::max(kCascadeDepthPaddingMin, z_span * kCascadeDepthPaddingFactor);
            // Pull the NEAR plane far toward the light (only the near side) so tall
            // casters standing between the light and this cascade segment still write
            // occluder depth — otherwise the near part of a contact shadow vanishes as
            // the camera approaches. far_z stays tight (max_ls.z() + z_pad) to keep
            // depth-bias scale stable. (light-space +Z points away from the light.)
            const float near_z = min_ls.z() - std::max(z_pad, kDirectionalCasterNearPullback);
            const float far_z = max_ls.z() + z_pad;
            if (far_z - near_z < kMinCascadeDepthSpan)
                continue;

            ShadowSliceGPU slice{};
            slice.light_vp = makeOrtho(
                min_ls.x(), max_ls.x(),
                min_ls.y(), max_ls.y(),
                near_z, far_z) * light_view;
            // `slice.bias` / `slice.slope_bias` feed directly into
            // `vkCmdSetDepthBias(constantFactor, _, slopeFactor)` —
            // dimensionless multipliers, NOT world-space depths. The
            // rasterizer formula is `depth + slope·max(|∂z/∂x|, |∂z/∂y|)
            // + constant·depth_eps`. For 24-bit unorm depth `depth_eps`
            // is ~6e-8, and a typical floor's per-pixel slope is ~7e-5
            // in normalized depth, so `constantFactor` needs to be in
            // the single-digit-to-low-hundred range and `slopeFactor`
            // in the 1–4 range to push acne-prone receivers cleanly off
            // their own recorded depth. The previous mapping
            // (`shadow_bias / depth_range`) interpreted the parameter
            // as a normalized depth offset and produced ~1e-12 actual
            // bias — effectively zero, which is why back-face culling
            // surfaced full moiré once front-cull stopped masking it.
            //
            // We retain `dl.shadow_bias` as the user-facing knob (default
            // 0.005 in the component) and scale internally to the
            // rasterizer's expected magnitudes. The shader's old
            // `current_depth - s.bias` subtraction is now redundant
            // (the rasterizer-written depth is already biased) and has
            // been removed in `shadow_common.glsl`.
            //
            // The slope-scale multiplier needs **strictly more headroom**
            // than the worst-case per-tap correction from the receiver-
            // plane bias used inside `sampleShadowPCF`. That correction
            // can reach ~2× the per-texel depth slope at a corner tap
            // (`x=+1, y=+1`); for the slope-scale rasterizer offset to
            // not tie with it (and produce texel-grid moiré through FP
            // jitter), the slope factor needs to be comfortably above
            // 2×. 4× is the standard safety margin and is what UE / Unity
            // ship by default for CSM.
            constexpr float kBiasConstantScale = 256.0f;  // 0.005 → 1.28
            constexpr float kBiasSlopeScale    = 800.0f;  // 0.005 → 4.0
            slice.bias       = dl.shadow_bias * kBiasConstantScale;
            slice.slope_bias = std::max(dl.shadow_bias * kBiasSlopeScale, 1e-5f);
            // PCF normal-offset bias (depth-format-independent): the shader
            // offsets the receiver along its normal by N texels of world space.
            // For an ortho cascade the world texel size is constant, so we hand
            // it to the shader here (perspective slices derive it from the
            // light-axis distance instead — see shadow_pcf.glsl).
            slice.normal_bias = std::max(units_per_texel_x, units_per_texel_y);
            // Orthographic cascade: NDC z is already linear, so EVSM warps it
            // directly (no linearization needed).
            slice.shadow_near = near_z;
            slice.shadow_far = far_z;
            slice.depth_is_perspective = 0u;
            assignAtlasTileMetadata(slice, tile);

            out_state.slices.push_back(slice);
            ++generated;
        }

        out_state.config.dir_cascade_count = generated;
        dir_found = (generated > 0);
        if (dir_found)
            out_state.config.dir_caster_slot = slot;   // shader samples this slot's cascades (C-6)
    });

    const Eigen::Vector3f camera_pos = vfd.camera_transform.position;
    const float non_directional_shadow_max_distance =
        cfg_.shadow_config.non_directional_shadow_max_distance;

    struct SpotCandidate
    {
        uint32_t slot{0};
        SpotLightGPU light{};
        float score{0.0f};
    };
    struct PointCandidate
    {
        uint32_t slot{0};
        PointLightGPU light{};
        float score{0.0f};
    };

    std::vector<SpotCandidate> spot_candidates;
    std::vector<PointCandidate> point_candidates;
    spot_candidates.reserve(spot_count);
    point_candidates.reserve(point_count);

    light_res->forEachLight<SpotLightGPU>([&](uint32_t slot, const SpotLightGPU& sl) {
        if (slot >= map_capacity)
            return;
        if ((sl.flags & LF_CAST_SHADOW) == 0u)
            return;
        Eigen::Vector3f pos(sl.position.x, sl.position.y, sl.position.z);
        const float score = scoreShadowCandidate(
            sl,
            pos,
            camera_pos,
            non_directional_shadow_max_distance);
        if (score <= 0.0f)
            return;
        spot_candidates.push_back({slot, sl, score});
    });
    light_res->forEachLight<PointLightGPU>([&](uint32_t slot, const PointLightGPU& pl) {
        if (slot >= map_capacity)
            return;
        if ((pl.flags & LF_CAST_SHADOW) == 0u)
            return;
        Eigen::Vector3f pos(pl.position.x, pl.position.y, pl.position.z);
        const float score = scoreShadowCandidate(
            pl,
            pos,
            camera_pos,
            non_directional_shadow_max_distance);
        if (score <= 0.0f)
            return;
        point_candidates.push_back({slot, pl, score});
    });

    auto score_desc = [](const auto& a, const auto& b) { return a.score > b.score; };
    std::sort(spot_candidates.begin(), spot_candidates.end(), score_desc);
    std::sort(point_candidates.begin(), point_candidates.end(), score_desc);

    // Top-K caster budget across spot/point lights.
    enum class ECasterType : uint8_t { Spot, Point };
    struct CasterChoice
    {
        ECasterType type{ECasterType::Spot};
        uint32_t index{0};
        float score{0.0f};
    };
    std::vector<CasterChoice> caster_choices;
    caster_choices.reserve(spot_candidates.size() + point_candidates.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(spot_candidates.size()); ++i)
        caster_choices.push_back({ECasterType::Spot, i, spot_candidates[i].score});
    for (uint32_t i = 0; i < static_cast<uint32_t>(point_candidates.size()); ++i)
        caster_choices.push_back({ECasterType::Point, i, point_candidates[i].score});
    std::sort(caster_choices.begin(), caster_choices.end(), score_desc);

    out_state.config.spot_light_offset = 0u;
    out_state.config.point_light_offset = 0u;
    uint32_t shadow_spot_slices = 0;
    uint32_t shadow_point_slices = 0;

    static constexpr struct { float dx, dy, dz, ux, uy, uz; } kCubeFaces[6] = {
        { 1, 0, 0,  0,-1, 0}, // +X
        {-1, 0, 0,  0,-1, 0}, // -X
        { 0, 1, 0,  0, 0, 1}, // +Y
        { 0,-1, 0,  0, 0,-1}, // -Y
        { 0, 0, 1,  0,-1, 0}, // +Z
        { 0, 0,-1,  0,-1, 0}, // -Z
    };

    for (const CasterChoice& choice : caster_choices)
    {
        if (out_state.slices.size() >= max_slices)
            break;
        if (choice.type == ECasterType::Spot)
        {
            const SpotLightGPU& sl = spot_candidates[choice.index].light;
            Eigen::Vector3f pos(sl.position.x, sl.position.y, sl.position.z);
            Eigen::Vector3f dir(sl.direction.x, sl.direction.y, sl.direction.z);
            const float dir_len = dir.norm();
            if (dir_len < kEpsilon)
                continue;
            dir /= dir_len;

            const float fov = std::max(sl.outer_cone_angle * 2.0f, 0.1f);
            const float near_z = 0.1f;
            const float far_z = std::max(sl.range, near_z + 0.1f);
            const float tf = std::tan(fov * 0.5f);

            ShadowTileAllocation tile{};
            if (!allocateTileWithFallback(
                    atlas_packer,
                    std::max(sl.shadow_map_size, 1u),
                    tile))
                continue;

            const Eigen::Vector3f up_seed = (std::abs(dir.y()) > 0.99f)
                                            ? Eigen::Vector3f(1.f, 0.f, 0.f)
                                            : Eigen::Vector3f(0.f, 1.f, 0.f);
            const ShadowSliceGPU slice = makePerspectiveLightSlice(
                pos, dir, up_seed, near_z, far_z,
                /*proj_xy_scale=*/ 1.f / tf, sl.shadow_bias, tile);

            const uint32_t slice_index = static_cast<uint32_t>(out_state.slices.size());
            out_state.slices.push_back(slice);
            out_state.spot_shadow_slice_index[spot_candidates[choice.index].slot] =
                static_cast<int32_t>(slice_index);
            ++shadow_spot_slices;
            continue;
        }

        if (out_state.slices.size() + 6 > max_slices)
            continue;

        const PointLightGPU& pl = point_candidates[choice.index].light;
        Eigen::Vector3f pos(pl.position.x, pl.position.y, pl.position.z);
        const float near_z = 0.1f;
        const float far_z = std::max(pl.range, near_z + 0.1f);

        std::array<ShadowTileAllocation, 6> tiles{};
        if (!allocateCubeWithFallback(
                atlas_packer,
                std::max(pl.shadow_map_size, 1u),
                tiles))
            continue;

        // Widen the 90° cube-face FOV by a few texels of overlap so that
        // PCF taps at face edges still have valid depth data rendered.
        const uint32_t cube_tile_res = std::max(tiles[0].resolution, 1u);
        const float overlap = 1.0f + 2.0f / static_cast<float>(cube_tile_res);

        const float point_proj_scale = 1.f / overlap;
        const uint32_t base_slice_index = static_cast<uint32_t>(out_state.slices.size());

        for (int face = 0; face < 6; ++face)
        {
            const Eigen::Vector3f dir(kCubeFaces[face].dx, kCubeFaces[face].dy, kCubeFaces[face].dz);
            const Eigen::Vector3f up_seed(kCubeFaces[face].ux, kCubeFaces[face].uy, kCubeFaces[face].uz);
            out_state.slices.push_back(makePerspectiveLightSlice(
                pos, dir, up_seed, near_z, far_z, point_proj_scale,
                pl.shadow_bias, tiles[face]));
        }
        out_state.point_shadow_base_slice[point_candidates[choice.index].slot] =
            static_cast<int32_t>(base_slice_index);
        shadow_point_slices += 6;
    }

    out_state.config.point_light_count = shadow_point_slices;
    out_state.config.spot_light_count = shadow_spot_slices;
    out_state.config.total_slices = static_cast<uint32_t>(out_state.slices.size());
}

} // namespace lux::render
