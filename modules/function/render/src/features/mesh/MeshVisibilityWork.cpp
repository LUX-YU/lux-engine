#include "MeshCompact.hpp"
#include "MeshFrustumCull.hpp"
#include "MeshFrustumCullPaged.hpp"
#include "MeshReset.hpp"
#include <lux/engine/render/features/mesh/MeshVisibilityWork.hpp>

namespace lux::render
{
    namespace detail
    {
        struct MeshCompactConstants final { std::uint32_t shared_members; };
    }

    template <> struct ConstantBlockTraits<detail::MeshCullConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
        {
            using Block = detail::MeshCullConstants;
            constexpr auto scalar = rdesc::EDataScalar::UINT;
            static constexpr std::array members{
                ConstantMemberLayout{"frustum_culling", offsetof(Block, frustum_culling), scalar},
                ConstantMemberLayout{"shadow_pass", offsetof(Block, shadow_pass), scalar}
            };
            return {sizeof(Block), target == rdesc::EShaderTarget::DXIL ? 16u : 4u, members};
        }
    };

    template <> struct ConstantBlockTraits<detail::MeshCompactConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
        {
            using Block = detail::MeshCompactConstants;
            static constexpr std::array members{
                ConstantMemberLayout{"shared_members", offsetof(Block, shared_members), rdesc::EDataScalar::UINT}
            };
            return {sizeof(Block), target == rdesc::EShaderTarget::DXIL ? 16u : 4u, members};
        }
    };

    RenderResult<detail::MeshVisibilityOutputs> detail::addMeshVisibilityPasses(
        EffectCompileContext& graph, const std::string& pass_prefix, const MeshVisibilityBuffers& input,
        MeshCullConstants culling
    ) noexcept
    {
        GraphProgram cull_variants{kMeshFrustumCull};
        cull_variants.device_address_targets.push_back(kMeshFrustumCullPaged.load(rdesc::EShaderTarget::SPIRV));
        auto cull_program = graph.addProgram(std::move(cull_variants));
        auto compact_program = graph.addProgram(GraphProgram{kMeshCompact});
        if (!cull_program || !compact_program)
        {
            return cxx::unexpected(!cull_program ? cull_program.error() : compact_program.error());
        }
        const auto table = input.groups;
        const auto dispatch = input.dispatch;
        auto visible = input.visible;
        auto reset_program = graph.addProgram(GraphProgram{kMeshReset});
        if (!reset_program)
        {
            return cxx::unexpected(reset_program.error());
        }
        ComputeWork reset;
        reset.interface.program = *reset_program;
        reset.interface.resources = {{"groups", {table}}, {"visible", {visible, {}, EGraphAccess::WRITE}}};
        reset.dispatch = IndirectDispatchArguments{dispatch, 12};
        reset.allow_async_compute = true;
        auto cleared = graph.addComputePass(
            pass_prefix + "reset",
            std::move(reset)
        );
        if (!cleared)
        {
            return cxx::unexpected(cleared.error());
        }
        visible = cleared->written.front();
        ComputeWork cull;
        cull.interface.program = *cull_program;
        cull.interface.resources = {
            {"transforms", {input.transforms}},
            {"alive", {input.alive}},
            {"properties", {input.properties}},
            {"views", {input.views}},
            {"candidates", {input.candidates}},
            {"groups", {table}},
            {"chunks", {input.chunks}},
            {"visible", {visible, {}, EGraphAccess::READ_WRITE}}
        };
        cull.interface.parameters.setInlineConstant(culling);
        cull.dispatch = IndirectDispatchArguments{dispatch, 0};
        cull.allow_async_compute = true;
        auto culled = graph.addComputePass(
            pass_prefix + "frustum",
            std::move(cull)
        );
        if (!culled)
        {
            return cxx::unexpected(culled.error());
        }
        visible = culled->written.front();
        ComputeWork compact;
        compact.interface.program = *compact_program;
        compact.interface.resources = {
            {"groups", {table}},
            {"visible", {visible}},
            {"commands", {input.commands, {}, EGraphAccess::WRITE}}
        };
        compact.interface.parameters.setInlineConstant(detail::MeshCompactConstants{graph.device().indirect_first_instance});
        compact.dispatch = IndirectDispatchArguments{dispatch, 12};
        compact.allow_async_compute = true;
        auto completed = graph.addComputePass(
            pass_prefix + "compact",
            std::move(compact)
        );
        if (!completed)
        {
            return cxx::unexpected(completed.error());
        }
        return MeshVisibilityOutputs{visible, completed->written.front()};
    }
} // namespace lux::render
