#pragma once

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/resources/view/Parameters.hpp>

namespace lux::render::detail
{
    class ViewParameterStorage final
    {
    public:
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        [[nodiscard]] RenderResult<void> update(std::span<const ViewGpuParameters>) noexcept;
        [[nodiscard]] RenderResult<std::uint64_t> capture(ViewGpuParameters& output) const noexcept
        {
            output = parameters_;
            return revision_;
        }

    private:
        ViewGpuParameters parameters_;
        std::uint64_t revision_ = 1;
    };

} // namespace lux::render::detail
