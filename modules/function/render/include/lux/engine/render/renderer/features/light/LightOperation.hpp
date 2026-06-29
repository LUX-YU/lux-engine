#pragma once
// ============================================================================
//  LightOperation.hpp — LightFeature factory + feature-scoped light CRUD ops.
//
//  Scene lights (LightResources storage + create/update/destroy/batch) are a
//  FEATURE domain, not a core protocol op. The commands are dispatched by
//  dynamically-allocated TypeIds (registered via the feature's register_ops_fn
//  — the grid pattern) and sent by a feature-scoped LightProxy. The core
//  RenderProtocol.hpp no longer names light. NOTE: LightFeature owns the light
//  DATA; DeferredLighting/Forward/Shadow are its CONSUMERS (find<LightResources>).
//  (See .internal/render-architecture-decoupling-design-2026-06-19.md 契约 C2+C5.)
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>      // TypeId, opcodes, CommandTraits
#include <lux/engine/render/renderer/features/FeatureOps.hpp>  // EOpKind / FeatureOpIds / reply_type_id_of_v
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/core/RenderResourceHandle.hpp>  // RLightHandle
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <Eigen/Core>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <variant>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Feature-scoped operation name constants (allocated dynamically at
    //  register_ops_fn time; the client reads the ids from the registration reply)
    // =========================================================================
    struct CreateLightPayload
    {
        RenderSceneId scene_id{};   // M2: light is created in THIS scene
        uint8_t light_type{0}; // 0=Dir, 1=Point, 2=Spot, 3=Area
        float position[3]{};
        float direction[3]{0.f, -1.f, 0.f};
        float color[3]{1.f, 1.f, 1.f};
        float intensity{1.f};
        float range{10.f};
        float attenuation_constant{1.f};
        float attenuation_linear{0.09f};
        float attenuation_quadratic{0.032f};
        float inner_cone_angle{0.5236f};
        float outer_cone_angle{0.7854f};
        uint32_t flags{0};
        uint32_t shadow_map_size{1024};
        float shadow_bias{0.005f};
        float shadow_normal_bias{0.01f};
        uint32_t cascade_count{4};
        float cascade_splits[8]{0.1f, 0.25f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float area_size[2]{1.f, 1.f};
    };
    static_assert(std::is_trivially_copyable_v<CreateLightPayload>);

    struct UpdateLightPayload
    {
        RenderSceneId scene_id{};   // M2: which scene owns `handle`
        RLightHandle handle{};
        uint8_t light_type{0};
        float position[3]{};
        float direction[3]{0.f, -1.f, 0.f};
        float color[3]{1.f, 1.f, 1.f};
        float intensity{1.f};
        float range{10.f};
        float attenuation_constant{1.f};
        float attenuation_linear{0.09f};
        float attenuation_quadratic{0.032f};
        float inner_cone_angle{0.5236f};
        float outer_cone_angle{0.7854f};
        uint32_t flags{0};
        uint32_t shadow_map_size{1024};
        float shadow_bias{0.005f};
        float shadow_normal_bias{0.01f};
        uint32_t cascade_count{4};
        float cascade_splits[8]{0.1f, 0.25f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float area_size[2]{1.f, 1.f};
    };
    static_assert(std::is_trivially_copyable_v<UpdateLightPayload>);

    // M2: dedicated struct (not DestroyResourcePayload<RLightHandle>) because a
    // light handle is only meaningful paired with its owning scene — per-scene
    // LightResources free-lists are independent, so the same {index,gen} can be
    // valid in two scenes.
    struct DestroyLightPayload
    {
        RenderSceneId scene_id{};
        RLightHandle  handle{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyLightPayload>);

    // The light-field tail (light_type .. area_size) is byte-identical in
    // CreateLightPayload and UpdateLightPayload; scene_id (and, for Update, the
    // handle) precede it. Copies between the two MUST be bounded to this tail —
    // NEVER sizeof(CreateLightPayload), which would overrun past scene_id.
    static_assert(std::is_standard_layout_v<CreateLightPayload>);
    static_assert(std::is_standard_layout_v<UpdateLightPayload>);
    inline constexpr std::size_t kLightPayloadTailBytes =
        sizeof(CreateLightPayload) - offsetof(CreateLightPayload, light_type);
    static_assert(
        kLightPayloadTailBytes ==
            sizeof(UpdateLightPayload) - offsetof(UpdateLightPayload, light_type),
        "Create/Update light payload shared tail must have identical layout");

    // =========================================================================
    //  LightDescriptor ↔ flat payload conversion
    // =========================================================================

    inline CreateLightPayload toLightPayload(RenderSceneId scene_id,
                                             const LightDescriptor &desc)
    {
        CreateLightPayload p{};
        p.scene_id = scene_id;

        std::visit(
            [&](auto &&arg)
            {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, DirectionalLightDesc>)
                {
                    p.light_type = 0;
                    Eigen::Map<Eigen::Vector3f>(p.direction) = arg.direction;
                    Eigen::Map<Eigen::Vector3f>(p.color)     = arg.color;
                    p.intensity        = arg.intensity;
                    p.flags            = arg.flags;
                    p.shadow_map_size  = arg.shadow_map_size;
                    p.shadow_bias      = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                    p.cascade_count    = arg.cascade_count;
                    std::copy(arg.cascade_splits.begin(), arg.cascade_splits.end(), p.cascade_splits);
                }
                else if constexpr (std::is_same_v<T, PointLightDesc>)
                {
                    p.light_type = 1;
                    Eigen::Map<Eigen::Vector3f>(p.position)  = arg.position;
                    Eigen::Map<Eigen::Vector3f>(p.color)     = arg.color;
                    p.intensity             = arg.intensity;
                    p.range                 = arg.range;
                    p.attenuation_constant  = arg.attenuation_constant;
                    p.attenuation_linear    = arg.attenuation_linear;
                    p.attenuation_quadratic = arg.attenuation_quadratic;
                    p.flags                 = arg.flags;
                    p.shadow_map_size       = arg.shadow_map_size;
                    p.shadow_bias           = arg.shadow_bias;
                    p.shadow_normal_bias    = arg.shadow_normal_bias;
                }
                else if constexpr (std::is_same_v<T, SpotLightDesc>)
                {
                    p.light_type = 2;
                    Eigen::Map<Eigen::Vector3f>(p.position)  = arg.position;
                    Eigen::Map<Eigen::Vector3f>(p.direction) = arg.direction;
                    Eigen::Map<Eigen::Vector3f>(p.color)     = arg.color;
                    p.intensity             = arg.intensity;
                    p.range                 = arg.range;
                    p.attenuation_constant  = arg.attenuation_constant;
                    p.attenuation_linear    = arg.attenuation_linear;
                    p.attenuation_quadratic = arg.attenuation_quadratic;
                    p.inner_cone_angle      = arg.inner_cone_angle;
                    p.outer_cone_angle      = arg.outer_cone_angle;
                    p.flags                 = arg.flags;
                    p.shadow_map_size       = arg.shadow_map_size;
                    p.shadow_bias           = arg.shadow_bias;
                    p.shadow_normal_bias    = arg.shadow_normal_bias;
                }
                else if constexpr (std::is_same_v<T, AreaLightDesc>)
                {
                    p.light_type = 3;
                    Eigen::Map<Eigen::Vector3f>(p.color)     = arg.color;
                    p.intensity        = arg.intensity;
                    p.area_size[0]     = arg.size.x();
                    p.area_size[1]     = arg.size.y();
                    p.flags            = arg.flags;
                    p.shadow_map_size  = arg.shadow_map_size;
                    p.shadow_bias      = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                } 
            }, 
            desc
        );

        return p;
    }

    inline LightDescriptor fromLightPayload(const CreateLightPayload &p)
    {
        switch (p.light_type)
        {
        case 0:
        {
            DirectionalLightDesc d{};
            d.direction = Eigen::Map<const Eigen::Vector3f>(p.direction);
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            d.cascade_count = p.cascade_count;
            std::copy_n(p.cascade_splits, 8, d.cascade_splits.begin());
            return d;
        }
        case 1:
        {
            PointLightDesc d{};
            d.position = Eigen::Map<const Eigen::Vector3f>(p.position);
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.range = p.range;
            d.attenuation_constant = p.attenuation_constant;
            d.attenuation_linear = p.attenuation_linear;
            d.attenuation_quadratic = p.attenuation_quadratic;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        case 2:
        {
            SpotLightDesc d{};
            d.position = Eigen::Map<const Eigen::Vector3f>(p.position);
            d.direction = Eigen::Map<const Eigen::Vector3f>(p.direction);
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.range = p.range;
            d.attenuation_constant = p.attenuation_constant;
            d.attenuation_linear = p.attenuation_linear;
            d.attenuation_quadratic = p.attenuation_quadratic;
            d.inner_cone_angle = p.inner_cone_angle;
            d.outer_cone_angle = p.outer_cone_angle;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        case 3:
        {
            AreaLightDesc d{};
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.size = Eigen::Map<const Eigen::Vector2f>(p.area_size);
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        default:
            return DirectionalLightDesc{};
        }
    }

    inline UpdateLightPayload toUpdateLightPayload(RenderSceneId scene_id,
                                                   RLightHandle handle,
                                                   const LightDescriptor &desc)
    {
        auto cp = toLightPayload(scene_id, desc);
        UpdateLightPayload up{};
        up.scene_id = scene_id;
        up.handle   = handle;
        // Copy ONLY the shared light-field tail (light_type .. area_size).
        // NOT sizeof(CreateLightPayload) — that would overrun past scene_id.
        std::memcpy(&up.light_type, &cp.light_type, kLightPayloadTailBytes);
        return up;
    }

    inline LightDescriptor fromUpdateLightPayload(const UpdateLightPayload &p)
    {
        CreateLightPayload cp{};
        // Copy ONLY the shared light-field tail (see toUpdateLightPayload).
        std::memcpy(&cp.light_type, &p.light_type, kLightPayloadTailBytes);
        return fromLightPayload(cp);
    }

    // =========================================================================
    //  Reply payload + CommandTraits (moved out of core RenderProtocol.hpp —
    //  createLight is the one light op that REPLIES, with the new RLightHandle).
    //  Delivery is by request_id; reply_type_id is a fixed reply-domain tag.
    // =========================================================================
    struct LightCreatedReply
    {
        RLightHandle handle{};
        uint32_t     status{0};
    };
    static_assert(std::is_trivially_copyable_v<LightCreatedReply>);

    template <>
    struct CommandTraits<CreateLightPayload>
    {
        using Reply = LightCreatedReply;
        static constexpr bool has_reply = true;
        // Q3: id derived from the Reply TYPE (same shape → same id on both ends) —
        // no hand-picked number, no cross-feature collision.
        static constexpr TypeId reply_type_id = reply_type_id_of_v<LightCreatedReply>;
    };

    // =========================================================================
    //  Typed op declarations — the single source the client Ids and the server
    //  registrar both derive from. Handlers live in LightOperationHandlers.cpp.
    //  update/destroy are fire-and-forget but ride the ResourceOp lifecycle ring,
    //  hence Stream + an explicit opcode override.
    // =========================================================================
    struct CreateLightOp
    {
        using Payload = CreateLightPayload;
        static constexpr EOpKind kind = EOpKind::Resource;   // replies with RLightHandle
        static constexpr const char* name = "CreateLight";
    };
    struct UpdateLightOp
    {
        using Payload = UpdateLightPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "UpdateLight";
    };
    struct DestroyLightOp
    {
        using Payload = DestroyLightPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "DestroyLight";
    };
    struct LightBatchOp
    {
        using Payload = UpdateLightPayload;
        static constexpr EOpKind kind = EOpKind::Bulk;
        static constexpr const char* name = "LightBatch";
    };

    /// Operation IDs returned to the client after RegisterFeatureType. A real
    /// (forward-declarable) struct rather than a `using` alias, because gameplay
    /// headers forward-declare it to stay render-light; otherwise it simply IS a
    /// FeatureOpIds (look ops up by op type via id<Op>()).
    struct LightOperationIds
        : FeatureOpIds<CreateLightOp, UpdateLightOp, DestroyLightOp, LightBatchOp>
    {
        using Ids = FeatureOpIds<CreateLightOp, UpdateLightOp, DestroyLightOp, LightBatchOp>;
        LightOperationIds() = default;
        LightOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static LightOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return LightOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// LightFeature factory (create + register_ops_fn allocating the 4 ops above).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kLightFeatureFactory;

    // =========================================================================
    //  Client-side proxy — LightProxy (sends light CRUD via the dynamic ids)
    // =========================================================================
    class LUX_FUNCTION_PUBLIC LightProxy
    {
    public:
        LightProxy(RenderSession& session, LightOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Create a light from a descriptor; replies with its RLightHandle.
        [[nodiscard]] RenderRequest<LightCreatedReply>
        createLight(RenderSceneId scene_id, const LightDescriptor& desc);

        /// Update an existing light in place (fire-and-forget).
        void updateLight(RenderSceneId scene_id, RLightHandle handle, const LightDescriptor& desc);

        /// Batched update — one command carrying N pre-built UpdateLightPayloads.
        void updateLights(std::span<const UpdateLightPayload> entries);

        /// Destroy a light (fire-and-forget).
        void destroyLight(RenderSceneId scene_id, RLightHandle handle);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*    session_;
        LightOperationIds ops_;
    };

} // namespace lux::render
