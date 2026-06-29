#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/RenderObjectTypes.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>
#include <vector>

namespace lux::render
{
    class RenderFeature;
    struct View;

    enum class EProviderType : uint8_t
    {
        Mesh = 0,
        PointCloud,
        Particle,
        Trajectory,
        Custom,
    };

    enum class EPassDomain : uint8_t
    {
        DepthPrepass = 0,
        Shadow,
        GBuffer,
        DeferredLighting,
        ForwardOpaque,
        ForwardTransparent,
        Emissive,
        Velocity,
        Custom,
    };

    struct DrawPacket
    {
        EProviderType provider_type{EProviderType::Mesh};
        EPassDomain   pass_domain{EPassDomain::GBuffer};
        uint64_t      pipeline_key{0};
        uint64_t      material_key{0};
        uint64_t      geometry_key{0};
        uint32_t      resource_ref0{0};
        uint32_t      resource_ref1{0};
        uint64_t      sort_key{0};
        const uint32_t* instance_span{nullptr};
        uint32_t      instance_count{0};
    };

    namespace draw_packet_keys
    {
        // pipeline_key layout:
        //   [63:56] provider_type
        //   [55:48] pass_domain
        //   [47:0 ] local variant id (material bucket / mode id / etc.)
        [[nodiscard]] constexpr uint64_t makePipelineKey(
            EProviderType provider,
            EPassDomain pass_domain,
            uint64_t local_variant) noexcept
        {
            return (static_cast<uint64_t>(provider) << 56u)
                 | (static_cast<uint64_t>(pass_domain) << 48u)
                 | (local_variant & 0x0000FFFFFFFFFFFFull);
        }

        [[nodiscard]] constexpr uint32_t localVariant(uint64_t pipeline_key) noexcept
        {
            return static_cast<uint32_t>(pipeline_key & 0x00000000FFFFFFFFull);
        }

        // material_key layout:
        //   [63:56] material family
        //   [55:40] material model
        //   [39:0 ] material local index/bucket
        [[nodiscard]] constexpr uint64_t makeMaterialKey(
            uint8_t family_id,
            uint16_t model_id,
            uint64_t local_index) noexcept
        {
            return (static_cast<uint64_t>(family_id) << 56u)
                 | (static_cast<uint64_t>(model_id) << 40u)
                 | (local_index & 0x000000FFFFFFFFFFull);
        }

        // geometry_key layout:
        //   [63:56] provider_type
        //   [55:48] geometry kind (EGeometryKind)
        //   [47:0 ] geometry-local id
        [[nodiscard]] constexpr uint64_t makeGeometryKey(
            EProviderType provider,
            uint64_t local_id) noexcept
        {
            return (static_cast<uint64_t>(provider) << 56u)
                 | (local_id & 0x00FFFFFFFFFFFFFFull);
        }

        [[nodiscard]] constexpr uint64_t makeGeometryKey(
            EProviderType provider,
            EGeometryKind geometry_kind,
            uint64_t local_id) noexcept
        {
            return (static_cast<uint64_t>(provider) << 56u)
                 | (static_cast<uint64_t>(geometry_kind) << 48u)
                 | (local_id & 0x0000FFFFFFFFFFFFull);
        }

        [[nodiscard]] constexpr EGeometryKind geometryKind(uint64_t geometry_key) noexcept
        {
            return static_cast<EGeometryKind>((geometry_key >> 48u) & 0xFFu);
        }

        [[nodiscard]] constexpr uint32_t localGeometryId(uint64_t geometry_key) noexcept
        {
            return static_cast<uint32_t>(geometry_key & 0x00000000FFFFFFFFull);
        }
    } // namespace draw_packet_keys

    struct RenderObjectExtractContext
    {
        const View* view{nullptr};
        ECoreRenderPhase phase{ECoreRenderPhase::GBuffer};
    };

    using RenderObjectProviderTypeId = uint32_t;
    inline constexpr RenderObjectProviderTypeId kInvalidRenderObjectProviderTypeId = 0;

    class LUX_FUNCTION_PUBLIC RenderObjectExtractor
    {
    public:
        struct ProviderVTable
        {
            using ExtractFn = void (*)(RenderFeature&, const RenderObjectExtractContext&, std::vector<DrawPacket>&);
            ExtractFn extract{nullptr};
            phase_mask_t phase_filter{(std::numeric_limits<phase_mask_t>::max)()};
        };

        template <typename T>
        static RenderObjectProviderTypeId featureTypeId()
        {
            static const RenderObjectProviderTypeId kTypeId = nextTypeId();
            return kTypeId;
        }

        template <typename T>
        RenderObjectProviderTypeId ensureTypeRegistered()
        {
            const RenderObjectProviderTypeId type_id = featureTypeId<T>();
            if (type_id >= vtables_.size())
                vtables_.resize(type_id + 1);
            if (vtables_[type_id].extract == nullptr)
            {
                vtables_[type_id].extract = resolveExtractFn<T>();
                vtables_[type_id].phase_filter = resolvePhaseFilter<T>();
            }
            return type_id;
        }

        [[nodiscard]] bool hasExtract(RenderObjectProviderTypeId type_id) const
        {
            return type_id < vtables_.size() && vtables_[type_id].extract != nullptr;
        }

        void registerProvider(RenderFeature* feature, RenderObjectProviderTypeId type_id)
        {
            if (feature == nullptr || type_id == kInvalidRenderObjectProviderTypeId)
                return;

            if (type_id >= vtables_.size() || vtables_[type_id].extract == nullptr)
                return;

            auto exists = std::find_if(providers_.begin(), providers_.end(),
                [feature](const ProviderEntry& e) { return e.feature == feature; });
            if (exists != providers_.end())
                return;

            providers_.push_back(ProviderEntry{
                .type_id = type_id,
                .feature = feature,
                .extract = vtables_[type_id].extract,
                .phase_filter = vtables_[type_id].phase_filter,
            });
            rebuildPhaseBuckets();
        }

        void unregisterProvider(const RenderFeature* feature)
        {
            if (feature == nullptr)
                return;
            auto it = std::remove_if(providers_.begin(), providers_.end(),
                [feature](const ProviderEntry& e) { return e.feature == feature; });
            providers_.erase(it, providers_.end());
            rebuildPhaseBuckets();
        }

        void clearProviders()
        {
            providers_.clear();
            rebuildPhaseBuckets();
        }

        [[nodiscard]] bool hasProvidersForPhase(ECoreRenderPhase phase) const noexcept
        {
            const render_phase_id phase_id = static_cast<render_phase_id>(phase);
            if (phase_id >= kPhaseCount)
                return false;
            return (active_phase_mask_ & phaseBit(phase_id)) != 0;
        }

        void extract(const RenderObjectExtractContext& ctx, std::vector<DrawPacket>& out_packets) const
        {
            const render_phase_id phase_id = static_cast<render_phase_id>(ctx.phase);
            if (phase_id >= kPhaseCount)
                return;
            for (const uint32_t provider_index : providers_by_phase_[phase_id])
            {
                const ProviderEntry& entry = providers_[provider_index];
                if (entry.feature != nullptr && entry.extract != nullptr)
                    entry.extract(*entry.feature, ctx, out_packets);
            }
        }

    private:
        template <typename T>
        static ProviderVTable::ExtractFn resolveExtractFn()
        {
            if constexpr (requires(T& t, const RenderObjectExtractContext& ctx, std::vector<DrawPacket>& out_packets)
                {
                    t.extractRenderObjects(ctx, out_packets);
                })
            {
                return +[](RenderFeature& base,
                           const RenderObjectExtractContext& ctx,
                           std::vector<DrawPacket>& out_packets)
                {
                    static_cast<T&>(base).extractRenderObjects(ctx, out_packets);
                };
            }
            else
            {
                return nullptr;
            }
        }

        static RenderObjectProviderTypeId nextTypeId()
        {
            static std::atomic<RenderObjectProviderTypeId> next{1};
            return next.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename T>
        static constexpr phase_mask_t resolvePhaseFilter()
        {
            if constexpr (requires {
                              { T::kExtractPhaseMask } -> std::convertible_to<phase_mask_t>;
                          })
            {
                return static_cast<phase_mask_t>(T::kExtractPhaseMask);
            }
            else
            {
                return (std::numeric_limits<phase_mask_t>::max)();
            }
        }

        struct ProviderEntry
        {
            RenderObjectProviderTypeId type_id{kInvalidRenderObjectProviderTypeId};
            RenderFeature* feature{nullptr};
            ProviderVTable::ExtractFn extract{nullptr};
            phase_mask_t phase_filter{(std::numeric_limits<phase_mask_t>::max)()};
        };

        static constexpr uint32_t kPhaseCount = sizeof(phase_mask_t) * 8u;

        void rebuildPhaseBuckets()
        {
            for (auto& bucket : providers_by_phase_)
                bucket.clear();
            active_phase_mask_ = 0;

            for (uint32_t i = 0; i < providers_.size(); ++i)
            {
                const ProviderEntry& entry = providers_[i];
                if (entry.feature == nullptr || entry.extract == nullptr)
                    continue;

                phase_mask_t mask = entry.phase_filter;
                while (mask != 0)
                {
                    const uint32_t phase_id = static_cast<uint32_t>(std::countr_zero(mask));
                    if (phase_id < kPhaseCount)
                    {
                        providers_by_phase_[phase_id].push_back(i);
                        active_phase_mask_ |= phaseBit(static_cast<render_phase_id>(phase_id));
                    }
                    mask &= (mask - 1);
                }
            }
        }

        std::vector<ProviderVTable> vtables_;
        std::vector<ProviderEntry> providers_;
        std::array<std::vector<uint32_t>, kPhaseCount> providers_by_phase_{};
        phase_mask_t active_phase_mask_{0};
    };
} // namespace lux::render
