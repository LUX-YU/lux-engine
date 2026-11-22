#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceEntry;
    struct EffectDeviceVariant;
    struct GraphReaderTag;
    class ViewParameterStorage;
    struct DeviceVariantTag;
    using DeviceVariantId = cxx::SlotKey<DeviceVariantTag>;
}

namespace lux::render::runtime_detail
{
    struct StorageInput final
    {
        ResourceInstanceId storage;
        std::uint32_t projection;
        GraphResourceId resource;
        cxx::SlotKey<detail::GraphReaderTag> reader{};
    };

    struct JobGraphPlan final
    {
        detail::RenderGraphExecutable executable;
        detail::NativeProgramBindings programs;
        std::vector<GraphResourceId> target_imports;
        std::vector<GraphResourceId> storage_imports;
        detail::PreparationPlan preparation;
        std::vector<detail::LinkedUniformBlock> uniforms{};
    };

    struct JobExecution final
    {
        JobExecution(detail::DeviceEntry&, const detail::EffectDeviceVariant&, TargetId, detail::TargetOutput) noexcept;
        ~JobExecution() noexcept;
        JobExecution(const JobExecution&) = delete;
        JobExecution& operator=(const JobExecution&) = delete;

        const EffectId effect;
        std::vector<ResourceInstanceId> resource_ids;
        std::vector<ResourceInstanceId> gpu_resources;
        DeviceSessionId device;
        detail::DeviceVariantId variant;
        TargetId target;
        const JobGraphPlan* const graph_plan;
        const detail::RenderGraphExecutable& executable;
        detail::RenderGraphInstance instance;
        std::uint32_t pending_uses = 0;
        struct ProgramPreparation final
        {
            std::vector<std::uint64_t> revisions;
            std::optional<stdexec::inplace_stop_source> stop;
            RenderError failure;
        };
        ProgramPreparation programs;

        [[nodiscard]] RenderError programError() const noexcept
        {
            if (programs.failure.type && instance.matchesProgramRevisions(programs.revisions))
                return programs.failure;
            return {};
        }

        [[nodiscard]] bool programsReady() const noexcept
        {
            return !programs.stop && !instance.requiresProducerPrograms();
        }

        FrameLayout input_layout;
        // Qualified once at assembly. These identities also define which exclusive commands
        // conflict with a live Run, including resources used only for GPU bindings.
        [[nodiscard]] std::span<const ResourceInstanceId> retainedResources() const & noexcept
        {
            return resource_ids;
        }
        // Each registered Resource type owns exactly one FrameDataCall.
        struct Input final
        {
            ResourceInstanceId identity;
            ResourceTypeId call_slot;
            struct ColumnRange final
            {
                std::uint32_t first, count;
                bool operator==(const ColumnRange&) const noexcept = default;
            };
            ColumnRange columns;
        };
        std::vector<Input> inputs;
        std::vector<std::uint32_t> input_columns;

        [[nodiscard]] std::span<const std::uint32_t> columnsFor(const Input& input) const & noexcept
        {
            return std::span(input_columns).subspan(input.columns.first, input.columns.count);
        }
        std::span<const std::uint32_t> columnsFor(const Input&) const && = delete;

        detail::TargetReservation storage_start;
        std::vector<StorageInput> storage_inputs;

        struct TargetImage final
        {
            TargetId target;
            GraphResourceId resource;
        };

        std::vector<TargetImage> target_images;
        void selectViewParameters() noexcept;
        const detail::ViewParameterStorage* view_parameters = nullptr;
        std::uint64_t view_revision = 0;
    };
} // namespace lux::render::runtime_detail
