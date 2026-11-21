#include <algorithm>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    RenderGraphExecutable::RenderGraphExecutable(nri::Device* device, EffectOutputDesc output) noexcept
        : device_(device), output_description_(output)
    {
    }

    RenderGraphExecutable::~RenderGraphExecutable() noexcept = default;

    RenderGraphExecutable::RenderGraphExecutable(RenderGraphExecutable&&) noexcept = default;

    std::span<const DescriptorAccess> RenderGraphExecutable::descriptorUses(std::uint32_t binding_slot) const noexcept
    {
        return binding_plan_->pass(binding_slot).uses;
    }

    NativeProgramBindings::NativeProgramBindings(nri::Device& device, std::size_t slots) noexcept
        : device_(&device), slots_(slots), streams_(slots)
    {
    }

    NativeProgramBindings::NativeProgramBindings(NativeProgramBindings&&) noexcept = default;

    NativeProgramBindings::~NativeProgramBindings() noexcept = default;

    PreparedRenderGraph::PreparedRenderGraph(DeviceSession& owner, RenderGraphExecutable plan) noexcept
        : executable(std::move(plan)), programs(owner.device(), executable.passes().size()), owner_(&owner)
    {
    }

    PreparedRenderGraph::PreparedRenderGraph(PreparedRenderGraph&& other) noexcept
        : executable(std::move(other.executable)), programs(std::move(other.programs)),
          assembly(std::move(other.assembly)), diagnostics(std::move(other.diagnostics)),
          owner_(std::exchange(other.owner_, nullptr))
    {
    }

    PreparedRenderGraph::~PreparedRenderGraph() noexcept
    {
        if (owner_) owner_->programs().releaseBindings(programs);
    }

    NativeProgramBindings PreparedRenderGraph::takePrograms() && noexcept
    {
        owner_ = nullptr;
        return std::move(programs);
    }

    bool RenderGraphExecutable::compatible(const TargetDesc& target) const noexcept
    {
        const bool same_type = target.kind == output_description_.kind && target.format == output_description_.format &&
                               target.samples == output_description_.samples &&
                               target.sampled == output_description_.sampled;
        if (!same_type)
        {
            return false;
        }
        const bool fixed_extent_mismatch = fixed_output_extent_[0] != 0 && (target.width != fixed_output_extent_[0] ||
                                                                            target.height != fixed_output_extent_[1]);
        if (fixed_extent_mismatch)
        {
            return false;
        }
        // Instance creation qualifies resolved extents and copy pitches against actual backing capacities.
        // Resize can retain this immutable plan while preparing a complete candidate before publication.
        return true;
    }

} // namespace lux::render::detail
