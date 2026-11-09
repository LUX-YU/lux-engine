#include <algorithm>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/capabilities/UiInputHandler.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/execution/DrawListResources.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/execution/TargetOutput.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    UiInputHandler* UiInputHandler::create(ResourceRegistry& resources) noexcept
    {
        return new UiInputHandler(resources);
    }

    RenderResult<void> UiInputHandler::draw(FrameDrawTarget target, ImguiFrameData input) noexcept
    {
        const auto* pass = target.instance.compiledPass(target.pass);
        const auto* raster = pass ? std::get_if<RasterCommands>(&pass->work) : nullptr;
        const auto* list = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
        const bool has_layout = list && raster->vertices.size() == 1;
        const bool layout_matches = has_layout && raster->vertices[0].stride == sizeof(ImDrawVert) &&
                                    (list->type == EIndexType::UINT16 ? 2u : 4u) == sizeof(ImDrawIdx) &&
                                    pass->parameter_size == sizeof(UiDrawParameters);
        const auto positive = [](float value) noexcept { return std::isfinite(value) && value > 0; };
        const bool valid_extent = positive(input.display_size[0]) && positive(input.display_size[1]) &&
                                  positive(input.framebuffer_scale[0]) && positive(input.framebuffer_scale[1]);
        const bool valid_position =
            std::isfinite(input.display_position[0]) && std::isfinite(input.display_position[1]);
        if (!layout_matches || !valid_extent || !valid_position)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto width = input.display_size[0] * input.framebuffer_scale[0];
        const auto height = input.display_size[1] * input.framebuffer_scale[1];
        const auto& extent = target.instance.target();
        if (!positive(width) || !positive(height) || width > extent.width || height > extent.height)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        std::size_t vertex_bytes = 0, index_bytes = 0;
        for (const auto& chunk : input.lists)
        {
            const auto vertices = input.frame.borrow(chunk.vertices);
            const auto indices = input.frame.borrow(chunk.indices);
            const auto padded_indices = (indices.size_bytes() + 3u) & ~std::size_t{3};
            if (vertices.size_bytes() > UINT32_MAX - vertex_bytes || padded_indices > UINT32_MAX - index_bytes)
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            vertex_bytes += vertices.size_bytes();
            index_bytes += padded_indices;
        }
        std::uint32_t slot = 0;
        if (!target.execution.tryBeginWrite(slot))
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finished([&]() noexcept { target.execution.finishWrite(slot, progress); });
        auto prepared = target.instance.prepareDrawList(target.pass, slot, vertex_bytes, index_bytes);
        if (!prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        auto& physical = **prepared;
        cxx::scope_exit rollback([&]() noexcept
        {
            target.instance.finishDrawListUploads(slot, cxx::unexpected(RenderError{kFrameContract}));
        });
        physical.upload_writes.clear();
        physical.staged_write = physical.staged_bytes = 0;
        physical.upload_writes.reserve(input.lists.size() * 3);
        physical.upload_tails.resize(input.lists.size());
        auto& selections = physical.image_indices;
        selections.clear();
        selections.reserve(input.images.size());
        physical.draws.reads.clear();
        for (const auto& image : input.images)
        {
            const nri::Texture* texture = nullptr;
            std::uint32_t descriptor = UINT32_MAX;
            if (!image.texture.isNull() && image.target.isNull())
            {
                auto storage =
                    resources_.borrow<SampledTexture2DStorage>(extension_detail::ResourceIdentity::storage(image.texture
                    ));
                const auto* value = storage ? (*storage)->find(image.texture) : nullptr;
                if (!value)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                texture = &value->image();
                descriptor = value->gpuReference().image_index;
            }
            else if (image.texture.isNull() && !image.target.isNull() && target.resolve_target)
            {
                auto output = target.resolve_target(target.target_context, image.target, slot);
                if (!output)
                {
                    return cxx::unexpected(output.error());
                }
                texture = output->image;
                descriptor = output->descriptor;
            }
            else
            {
                return cxx::unexpected(RenderError{kFrameContract});
            }
            selections.emplace_back(image.token, descriptor);
            physical.draws.reads.push_back({texture, GraphImageRange{1, 0, 1, 0, 1}, EGraphUse::SAMPLED, 2});
        }
        std::ranges::sort(selections);
        if (std::ranges::adjacent_find(selections, [](const auto& a, const auto& b) { return a.first == b.first; }) !=
            selections.end())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const UiDrawParameters parameters{
            {2 / input.display_size[0], 2 / input.display_size[1]},
            {-1 - 2 * input.display_position[0] / input.display_size[0],
             -1 - 2 * input.display_position[1] / input.display_size[1]}
        };
        std::memcpy(physical.draws.parameters.data(), &parameters, sizeof(parameters));
        std::size_t vertex_offset = 0, index_offset = 0, chunk_index = 0;
        for (const auto& chunk : input.lists)
        {
            const auto vertices = input.frame.borrow(chunk.vertices);
            const auto indices = input.frame.borrow(chunk.indices);
            const auto commands = input.frame.borrow(chunk.commands);
            if (!vertices.empty())
            {
                physical.upload_writes.push_back({physical.vertices, vertex_offset, std::as_bytes(vertices)});
            }
            const auto bytes = std::as_bytes(indices);
            const auto aligned = bytes.size() & ~std::size_t{3};
            if (aligned)
            {
                physical.upload_writes.push_back({physical.indices, index_offset, bytes.first(aligned)});
            }
            if (aligned != bytes.size())
            {
                auto& tail = physical.upload_tails[chunk_index];
                tail = {};
                std::memcpy(tail.data(), bytes.data() + aligned, bytes.size() - aligned);
                physical.upload_writes.push_back({physical.indices, index_offset + aligned, tail});
            }
            for (const auto& command : commands)
            {
                if (command.UserCallback)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    {
                        return cxx::unexpected(RenderError{kFrameContract});
                    }
                    // Executor rebinds the declared state; arbitrary callbacks never run here.
                    continue;
                }
                const bool invalid_indices =
                    command.IdxOffset > indices.size() || command.ElemCount > indices.size() - command.IdxOffset;
                const bool invalid_vertex = command.VtxOffset > vertices.size();
                const std::array clip{command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w};
                if (invalid_indices || invalid_vertex ||
                    !std::ranges::all_of(clip, [](float v) noexcept { return std::isfinite(v); }))
                {
                    return cxx::unexpected(RenderError{kFrameContract});
                }
                const auto token = static_cast<std::uint64_t>(command.GetTexID());
                const auto selection =
                    std::ranges::lower_bound(selections, token, {}, [](const auto& item) { return item.first; });
                if (selection == selections.end() || selection->first != token)
                {
                    return cxx::unexpected(RenderError{kRuntimeIdentity});
                }
                const auto left =
                    std::clamp((clip[0] - input.display_position[0]) * input.framebuffer_scale[0], 0.F, width);
                const auto top =
                    std::clamp((clip[1] - input.display_position[1]) * input.framebuffer_scale[1], 0.F, height);
                const auto right =
                    std::clamp((clip[2] - input.display_position[0]) * input.framebuffer_scale[0], 0.F, width);
                const auto bottom =
                    std::clamp((clip[3] - input.display_position[1]) * input.framebuffer_scale[1], 0.F, height);
                if (right > left && bottom > top && command.ElemCount != 0)
                {
                    auto& draws = physical.draws;
                    const auto parameter_offset = draws.parameters.size();
                    const auto parameter_size =
                        list->partial_parameters ? sizeof(selection->second) : sizeof(parameters);
                    if (parameter_offset > UINT32_MAX - parameter_size)
                    {
                        return cxx::unexpected(RenderError{kRuntimeCapacity});
                    }
                    draws.parameters.resize(parameter_offset + parameter_size);
                    auto* destination = draws.parameters.data() + parameter_offset;
                    if (!list->partial_parameters)
                    {
                        std::memcpy(destination, &parameters, sizeof(parameters));
                        destination += list->selection_offset;
                    }
                    std::memcpy(destination, &selection->second, sizeof(selection->second));
                    RasterDrawPacket packet;
                    packet.parameter_offset = static_cast<std::uint32_t>(parameter_offset);
                    packet.parameter_size = static_cast<std::uint32_t>(parameter_size);
                    packet.parameter_destination = list->partial_parameters ? list->selection_offset : 0;
                    packet.scissor = {
                        static_cast<std::int16_t>(left),
                        static_cast<std::int16_t>(top),
                        static_cast<nri::Dim_t>(right - left),
                        static_cast<nri::Dim_t>(bottom - top)
                    };
                    packet.draw = nri::DrawIndexedDesc{
                        command.ElemCount,
                        1,
                        static_cast<std::uint32_t>(index_offset / sizeof(ImDrawIdx)) + command.IdxOffset,
                        static_cast<std::int32_t>(vertex_offset / sizeof(ImDrawVert) + command.VtxOffset),
                        0
                    };
                    draws.packets.push_back(packet);
                }
            }
            vertex_offset += vertices.size_bytes();
            index_offset += (bytes.size() + 3u) & ~std::size_t{3};
            ++chunk_index;
        }
        rollback.release();
        return RenderResult<void>{};
    }
} // namespace lux::render::detail
