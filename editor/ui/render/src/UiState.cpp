#include <algorithm>
#include <cmath>
#include <cstring>
#include <lux/editor/ui/render/UiState.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>

namespace lux::editor::ui
{
    RenderResult<ImguiFrameData> DrawCapture::capture(
        const ImDrawData& data, std::span<const ImguiImage> images
    ) noexcept
    {
        const bool invalid_lists = data.CmdListsCount < 0 || data.CmdListsCount != data.CmdLists.Size ||
            (data.CmdListsCount != 0 && !data.CmdLists.Data);
        if (!data.Valid || invalid_lists) return cxx::unexpected(RenderError{kFrameContract});
        lists_.clear();
        lists_.reserve(static_cast<std::size_t>(data.CmdListsCount));
        const auto valid_array = [](const auto& array) noexcept
        {
            return array.Size >= 0 && (array.Size == 0 || array.Data);
        };
        for (const auto* list : data.CmdLists)
        {
            const bool valid_list = list && valid_array(list->VtxBuffer) && valid_array(list->IdxBuffer) &&
                valid_array(list->CmdBuffer);
            if (!valid_list) return cxx::unexpected(RenderError{kFrameContract});
            lists_.push_back({
                {list->VtxBuffer.Data, static_cast<std::size_t>(list->VtxBuffer.Size)},
                {list->IdxBuffer.Data, static_cast<std::size_t>(list->IdxBuffer.Size)},
                {list->CmdBuffer.Data, static_cast<std::size_t>(list->CmdBuffer.Size)}
            });
        }
        return ImguiFrameData{
            lists_,
            {data.DisplayPos.x, data.DisplayPos.y},
            {data.DisplaySize.x, data.DisplaySize.y},
            {data.FramebufferScale.x, data.FramebufferScale.y},
            images
        };
    }

    RenderResult<std::unique_ptr<UiState>> UiState::create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        auto result = std::make_unique<UiState>();
        result->resources = &context.resources;
        return result;
    }

    void
    UiState::submit(UiState& state, const SubmitDrawData::Request& request, SubmitDrawData::Response& response) noexcept
    {
        response = {};
        auto& next = state.candidate;
        next.vertices.clear();
        next.indices.clear();
        next.draws.clear();
        next.images.clear();
        next.targets.clear();
        next.parameters = {};
        next.width = next.height = 0;
        const auto& input = request.data;
        const auto fail = [&]() noexcept { response.error = {kFrameContract}; };
        const auto publish = [&]() noexcept
        {
            const bool has_changed_outputs = state.snapshot.targets != next.targets;
            std::swap(state.snapshot, next);
            if (has_changed_outputs)
            {
                state.resources->outputUsesChanged();
            }
        };
        const bool is_empty = std::ranges::all_of(
            input.lists,
            [](const auto& list) { return list.vertices.empty() && list.indices.empty() && list.commands.empty(); }
        );
        if (is_empty)
        {
            publish();
            return;
        }
        const auto positive = [](float v) { return std::isfinite(v) && v > 0; };
        const bool is_valid_extent = positive(input.display_size[0]) && positive(input.display_size[1]) &&
                                     positive(input.framebuffer_scale[0]) && positive(input.framebuffer_scale[1]);
        const bool is_valid_position =
            std::isfinite(input.display_position[0]) && std::isfinite(input.display_position[1]);
        if (!is_valid_extent || !is_valid_position)
        {
            fail();
            return;
        }
        next.width = input.display_size[0] * input.framebuffer_scale[0];
        next.height = input.display_size[1] * input.framebuffer_scale[1];
        if (!positive(next.width) || !positive(next.height) || next.width > INT16_MAX || next.height > INT16_MAX)
        {
            fail();
            return;
        }
        next.parameters = {
            {2 / input.display_size[0], 2 / input.display_size[1]},
            {-1 - 2 * input.display_position[0] / input.display_size[0],
             -1 - 2 * input.display_position[1] / input.display_size[1]}
        };
        if (input.images.size() > UINT32_MAX)
        {
            fail();
            return;
        }
        auto& tokens = state.image_tokens;
        tokens.clear();
        tokens.reserve(input.images.size());
        for (std::size_t i = 0; i < input.images.size(); ++i)
        {
            const auto& image = input.images[i];
            if (image.texture.isNull() == image.target.isNull())
            {
                fail();
                return;
            }
            tokens.emplace_back(image.token, static_cast<std::uint32_t>(i));
            next.images.push_back({image.texture, image.target});
        }
        std::ranges::sort(tokens);
        const bool has_duplicate_token =
            std::ranges::adjacent_find(tokens, [](const auto& a, const auto& b) { return a.first == b.first; }) !=
            tokens.end();
        if (has_duplicate_token)
        {
            fail();
            return;
        }
        for (const auto& chunk : input.lists)
        {
            const auto vertex_offset = next.vertices.size() / sizeof(ImDrawVert);
            const auto index_offset = next.indices.size() / sizeof(ImDrawIdx);
            const bool is_excessive_size = chunk.vertices.size_bytes() > UINT32_MAX - next.vertices.size() ||
                                           chunk.indices.size_bytes() > UINT32_MAX - next.indices.size() - 3;
            if (is_excessive_size)
            {
                fail();
                return;
            }
            const auto vertices = std::as_bytes(chunk.vertices), indices = std::as_bytes(chunk.indices);
            next.vertices.insert(next.vertices.end(), vertices.begin(), vertices.end());
            next.indices.insert(next.indices.end(), indices.begin(), indices.end());
            next.indices.resize((next.indices.size() + 3) & ~std::size_t{3});
            for (const auto& command : chunk.commands)
            {
                const bool unsupported_callback = command.UserCallback &&
                    command.UserCallback != ImDrawCallback_ResetRenderState;
                const bool has_callback_payload = command.UserCallbackData || command.UserCallbackDataSize;
                if (unsupported_callback || has_callback_payload)
                {
                    fail();
                    return;
                }
                if (command.UserCallback) continue; // Packets already set the complete draw state.
                const bool is_invalid_indices = command.IdxOffset > chunk.indices.size() ||
                                                command.ElemCount > chunk.indices.size() - command.IdxOffset;
                const bool is_invalid_vertices = command.VtxOffset > chunk.vertices.size();
                const std::array clip{command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w};
                const bool is_invalid_clip = !std::ranges::all_of(clip, [](float v) { return std::isfinite(v); });
                if (is_invalid_indices || is_invalid_vertices || is_invalid_clip)
                {
                    fail();
                    return;
                }
                const auto available_vertices = chunk.vertices.size() - command.VtxOffset;
                const auto draw_indices = chunk.indices.subspan(command.IdxOffset, command.ElemCount);
                const bool references_missing_vertex = std::ranges::any_of(
                    draw_indices, [available_vertices](auto index) noexcept { return index >= available_vertices; }
                );
                if (references_missing_vertex)
                {
                    fail();
                    return;
                }
                const auto token = static_cast<std::uint64_t>(command.GetTexID());
                const auto image = std::ranges::lower_bound(tokens, token, {}, [](const auto& item) { return item.first; });
                if (image == tokens.end() || image->first != token)
                {
                    fail();
                    return;
                }
                const auto left =
                    std::clamp((clip[0] - input.display_position[0]) * input.framebuffer_scale[0], 0.F, next.width);
                const auto top =
                    std::clamp((clip[1] - input.display_position[1]) * input.framebuffer_scale[1], 0.F, next.height);
                const auto right =
                    std::clamp((clip[2] - input.display_position[0]) * input.framebuffer_scale[0], 0.F, next.width);
                const auto bottom =
                    std::clamp((clip[3] - input.display_position[1]) * input.framebuffer_scale[1], 0.F, next.height);
                if (right <= left || bottom <= top || command.ElemCount == 0)
                {
                    continue;
                }
                const auto target = next.images[image->second].target;
                if (!target.isNull() && std::ranges::find(next.targets, target) == next.targets.end())
                    next.targets.push_back(target);
                next.draws.push_back(
                    {{command.ElemCount, 1, static_cast<std::uint32_t>(index_offset + command.IdxOffset),
                      static_cast<std::int32_t>(vertex_offset + command.VtxOffset), 0},
                     {static_cast<std::int16_t>(left), static_cast<std::int16_t>(top),
                      static_cast<nri::Dim_t>(right - left), static_cast<nri::Dim_t>(bottom - top)},
                     image->second}
                );
            }
        }
        publish();
    }

    ResourceRegistration UiState::factory() noexcept
    {
        static const std::array commands{CommandBinding::declare<SubmitDrawData, &UiState::submit>()};
        static const render::detail::StorageBindingOps binding{
            [](void*, const VGraphResourceDesc&, const void*, render::detail::StorageSubscription&,
               std::span<render::detail::ResourceInput>) noexcept -> RenderResult<void> { return {}; }
        };
        return makeResourceRegistration<UiResource, UiState, &UiState::create>(nullptr, &binding, commands);
    }

    RenderResult<std::unique_ptr<render::detail::InstanceData>> UiInstance::create(
        render::detail::DeviceSession&, ResourceRegistry& resources, InstanceAccess& instance,
        std::span<const ResourceInstanceId> resolved, const void* value
    ) noexcept
    {
        const auto& recipe = *static_cast<const UiPreparation*>(value);
        if (recipe.source.index >= resolved.size())
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto state = resources.borrow<UiState>(resolved[recipe.source.index]);
        if (!state)
        {
            return cxx::unexpected(state.error());
        }
        auto stream = instance.indexedDrawStream(recipe.pass);
        if (!stream)
        {
            return cxx::unexpected(stream.error());
        }
        return std::unique_ptr<render::detail::InstanceData>(new UiInstance(**state, instance, *stream));
    }

    RenderResult<bool> UiInstance::
        prepare(const render::detail::InstanceInputs& input, render::detail::ResourceUploader&, render::detail::Preparation&) noexcept
    {
        const auto& value = state_.snapshot;
        const auto& extent = instance_.target();
        if (value.width > extent.width || value.height > extent.height)
        {
            return cxx::unexpected(RenderError{kFrameContract, {extent.width, extent.height, 1}});
        }
        const IndexedDrawData data{value.vertices,   value.indices, std::as_bytes(std::span{&value.parameters, 1}),
                                   value.draws,      value.images,  sizeof(ImDrawVert),
                                   sizeof(ImDrawIdx)};
        auto prepared = instance_.setIndexedDrawData(stream_, input.slot, data);
        if (!prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        return true;
    }
} // namespace lux::editor::ui
