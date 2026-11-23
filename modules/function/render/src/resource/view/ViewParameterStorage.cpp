#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/ViewParameterCommands.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>

#include <lux/engine/render/graph/RasterViewport.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <algorithm>
#include <cmath>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<void> validate(const ViewGpuParameters& value) noexcept
        {
            const auto& v = value.viewport;
            if (auto valid = RasterViewport{v[0], v[1], v[2], v[3]}.validate(); !valid) return valid;
            const auto finite = [](const auto& values) noexcept
            {
                return std::ranges::all_of(values, [](float item) noexcept { return std::isfinite(item); });
            };
            const auto& p = value.position_near;
            const auto& d = value.direction_far;
            const auto direction_length_squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const bool invalid_values = !finite(value.view) || !finite(value.projection) ||
                !finite(value.inverse_view_projection) || !finite(value.background) || !finite(p) || !finite(d);
            const bool invalid_planes = p[3] <= 0 || d[3] <= p[3];
            const bool invalid_direction = std::abs(direction_length_squared - 1.F) > 0.001F;
            if (const bool invalid_view = invalid_values || invalid_planes || invalid_direction; invalid_view)
                return cxx::unexpected(RenderError{kFrameContract});
            return {};
        }

        RenderResult<void>
        updateViewParameters(ViewParameterStorage& storage, std::span<const ViewGpuParameters> values) noexcept
        {
            return storage.update(values);
        }

        void updateViewParametersCommand(
            ViewParameterStorage& storage, const ViewParameterCommands::Update::Request& input, AckResponse& reply
        ) noexcept
        {
            const auto result = updateViewParameters(storage, input.values);
            reply.error = result ? RenderError{} : result.error();
        }

        RenderResult<std::unique_ptr<ViewParameterStorage>>
        createViewParameters(ResourceCreateContext&, ResourceInstanceId) noexcept
        {
            return std::make_unique<ViewParameterStorage>();
        }
    } // namespace

    RenderResult<void> ViewParameterStorage::update(std::span<const ViewGpuParameters> values) noexcept
    {
        for (const auto& value : values)
        {
            if (auto valid = validate(value); !valid) return valid;
        }
        if (values.empty() || values.back() == parameters_) return {};
        if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        parameters_ = values.back();
        ++revision_;
        return {};
    }

    std::span<const CommandBinding> viewParameterStorageCommands() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<ViewParameterCommands::Update, &updateViewParametersCommand>()
        };
        return commands;
    }

    ResourceRegistration ViewParameterStorage::factory() noexcept
    {
        static constexpr auto snapshot =
            StorageSnapshotOps::make<ViewParameterStorage, ViewGpuParameters, &ViewParameterStorage::selectParameters>();
        static constexpr StorageBindingOps binding{.cpu_snapshot = &snapshot};
        static const auto frame_call =
            FrameDataCall::declare<ViewParameterResource, ViewParameterStorage, &updateViewParameters>();
        return makeResourceRegistration<ViewParameterResource, ViewParameterStorage, createViewParameters>(
            &binding,
            frame_call
        );
    }
} // namespace lux::render::detail
