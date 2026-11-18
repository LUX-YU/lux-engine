#pragma once

#include <lux/editor/ui/render/Resource.hpp>
#include <algorithm>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/extensions/graph/InstanceData.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>

namespace lux::editor::ui
{
    using namespace lux::render;

    struct UiDrawParameters final
    {
        std::array<float, 2> scale, translation;
        std::uint32_t source_index = 0;

        // Qualify these CPU field offsets against each target; reflected trailing padding is allowed.
        [[nodiscard]] static bool matchesLayout(const rdesc::ShaderBlock& layout) noexcept
        {
            if (layout.size < sizeof(UiDrawParameters) || layout.fields.size() != 3)
            {
                return false;
            }
            const std::array<std::string_view, 3> names{"scale", "translation", "source_index"};
            const std::array<std::uint32_t, 3> offsets{
                offsetof(UiDrawParameters, scale), offsetof(UiDrawParameters, translation),
                offsetof(UiDrawParameters, source_index)
            };
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                const auto field = std::ranges::find(layout.fields, names[i], &rdesc::ShaderMember::path);
                if (field == layout.fields.end())
                {
                    return false;
                }
                const bool is_invalid_field =
                    field->offset != offsets[i] || field->columns != 1 || field->rows != (i == 2 ? 1u : 2u) ||
                    field->array_stride != 0 ||
                    field->scalar != (i == 2 ? rdesc::EDataScalar::UINT : rdesc::EDataScalar::FLOAT);
                if (is_invalid_field)
                {
                    return false;
                }
            }
            return true;
        }
    };

    struct UiSnapshot final
    {
        std::vector<std::byte> vertices, indices;
        std::vector<IndexedDraw> draws;
        std::vector<SampledImageReference> images;
        std::vector<TargetId> targets;
        UiDrawParameters parameters{};
        float width = 0, height = 0;
    };

    struct UiState final
    {
        using Descriptor = UiResource;
        UiSnapshot snapshot, candidate;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> image_tokens;
        ResourceRegistry* resources = nullptr;
        static RenderResult<std::unique_ptr<UiState>> create(ResourceCreateContext&, ResourceInstanceId) noexcept;
        static ResourceRegistration factory() noexcept;
        static void submit(UiState&, const SubmitDrawData::Request&, SubmitDrawData::Response&) noexcept;
    };

    struct UiPreparation final
    {
        GraphResourceId source;
        GraphPassId pass;
    };

    class UiInstance final : public render::detail::InstanceData
    {
    public:
        UiInstance(UiState& state, InstanceAccess& instance, GraphPassId pass) noexcept
            : state_(state), instance_(instance), pass_(pass)
        {
        }

        static RenderResult<std::unique_ptr<render::detail::InstanceData>>
        create(render::detail::DeviceSession&, ResourceRegistry&, InstanceAccess&, std::span<const ResourceInstanceId>, const void*) noexcept;
        RenderResult<bool>
        prepare(const render::detail::InstanceInputs&, render::detail::ResourceUploader&, render::detail::Preparation&) noexcept
            override;

        std::span<const TargetId> sampledTargets() const noexcept override
        {
            return state_.snapshot.targets;
        }

    private:
        UiState& state_;
        InstanceAccess& instance_;
        GraphPassId pass_;
    };
} // namespace lux::editor::ui
