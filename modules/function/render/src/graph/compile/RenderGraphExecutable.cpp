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

    std::uint32_t GraphCompileDiagnostics::passSlot(GraphPassId source) const noexcept
    {
        const auto found = std::ranges::find(source_passes, source);
        if (found == source_passes.end()) return kGraphInvalidIndex;
        return static_cast<std::uint32_t>(found - source_passes.begin());
    }

    NativeProgramBindings::NativeProgramBindings(DeviceSession& device, std::size_t slots) noexcept
        : owner_(&device), slots_(slots), streams_(slots)
    {
    }

    NativeProgramBindings::NativeProgramBindings(NativeProgramBindings&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), slots_(std::move(other.slots_)),
          samplers_(std::move(other.samplers_)), streams_(std::move(other.streams_))
    {
    }

    NativeProgramBindings::~NativeProgramBindings() noexcept
    {
        if (owner_) owner_->programs().releaseBindings(*this);
    }

    const nri::Device* NativeProgramBindings::device() const noexcept
    {
        return owner_ ? &owner_->device() : nullptr;
    }

    PreparedRenderGraph::PreparedRenderGraph(DeviceSession& owner, RenderGraphExecutable plan) noexcept
        : executable(std::move(plan)), programs(owner, executable.passes().size())
    {
    }

    PreparedRenderGraph::PreparedRenderGraph(PreparedRenderGraph&&) noexcept = default;

    PreparedRenderGraph::~PreparedRenderGraph() noexcept = default;

    bool RenderGraphExecutable::compatible(const TargetDesc& target) const noexcept
    {
        if (target.outputContract() != output_description_)
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
