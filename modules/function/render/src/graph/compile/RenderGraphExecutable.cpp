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

    std::span<const DescriptorUse> RenderGraphExecutable::descriptorUses(std::uint32_t binding_slot) const noexcept
    {
        return binding_plan_->pass(binding_slot).uses;
    }

    NativeGraphPrograms::NativeGraphPrograms(DeviceSession& session, std::size_t slots) noexcept
        : session_(&session), slots_(slots), streams_(slots)
    {
    }

    NativeGraphPrograms::NativeGraphPrograms(NativeGraphPrograms&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)), slots_(std::move(other.slots_)),
          streams_(std::move(other.streams_)),
          program_uses_(std::move(other.program_uses_)), layout_uses_(std::move(other.layout_uses_))
    {
    }

    NativeGraphPrograms::~NativeGraphPrograms() noexcept
    {
        if (!session_) return;
        auto& programs = session_->programs();
        if (!layout_uses_.empty())
        {
            session_->deferRelease([layouts = std::move(layout_uses_), &programs]() noexcept
            {
                for (auto* layout : layouts)
                {
                    programs.releaseLayout(layout);
                }
            });
        }
        programs.release(program_uses_);
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
