#include <lux/engine/render/runtime/JobCpuPreparation.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/RasterFramePreparation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace lux::render::detail
{
    RenderResult<void>
    RasterFramePreparation::resizeLayers(GraphResourceId resource, std::uint32_t layers) noexcept
    {
        const auto selected =
            std::ranges::find(frame_.layered_images, resource, &FrameResources::LayeredImage::resource);
        const bool invalid_layers = layers == 0 || layers > session_.description().dimensions.textureLayerMaxNum;
        if (selected == frame_.layered_images.end() || invalid_layers)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (selected->views.size() == layers)
        {
            return {};
        }
        const auto& core = session_.nriCore();
        auto& binding = frame_.bindings[resource.index];
        const auto& compiled = graph_.resources()[resource.index];
        if (layers > 1)
        {
            for (const auto [pass, index] : compiled.descriptor_uses)
            {
                const auto& program = graph_.bindingPlan().pass(pass);
                if (!program.bindings[program.uses[index].field].image_arrayed)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {resource.index, layers}});
            }
        }
        auto description = *binding.image_description;
        const bool replace_image = description.layerNum != layers;
        description.layerNum = static_cast<nri::Dim_t>(layers);
        nri::Texture* image = binding.image;
        if (replace_image)
        {
            const auto created = core.CreatePlacedTexture(session_.device(), NriDeviceHeap, description, image);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
        }

        struct View final
        {
            ImageViewBinding use;
            nri::Descriptor* descriptor;
        };

        std::vector<View> replacements;
        std::vector<nri::Descriptor*> layer_views;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& view : replacements)
                {
                    core.DestroyDescriptor(view.descriptor);
                }
                for (auto* view : layer_views)
                {
                    core.DestroyDescriptor(view);
                }
                if (replace_image)
                {
                    core.DestroyTexture(image);
                }
            }
        );
        const auto add_view = [&](ImageViewBinding use, nri::TextureView kind, std::uint32_t count) -> RenderResult<void>
        {
            nri::TextureViewDesc view{
                image,
                kind,
                description.format,
                static_cast<nri::Dim_t>(use.range.first_mip),
                static_cast<nri::Dim_t>(use.range.mip_count),
                static_cast<nri::Dim_t>(use.range.first_layer),
                static_cast<nri::Dim_t>(count)
            };
            view.planes = static_cast<nri::PlaneBits>(use.range.aspects);
            nri::Descriptor* descriptor = nullptr;
            const auto created = core.CreateTextureView(view, descriptor);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            replacements.push_back({use, descriptor});
            return {};
        };
        if (replace_image)
        {
            for (const auto& use : compiled.image_views)
            {
                const auto kind = use.range.aspects == 1 ? nri::TextureView::COLOR_ATTACHMENT
                                                         : nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
                if (auto created = add_view(use, kind, layers); !created)
                {
                    return cxx::unexpected(created.error());
                }
            }
            for (const auto [pass, index] : compiled.descriptor_uses)
            {
                const auto& program = graph_.bindingPlan().pass(pass);
                const auto& use = program.uses[index];
                const bool arrayed = program.bindings[use.field].image_arrayed;
                const auto kind = use.kind == EDescriptorKind::STORAGE_IMAGE
                    ? (arrayed ? nri::TextureView::STORAGE_TEXTURE_ARRAY : nri::TextureView::STORAGE_TEXTURE)
                    : (arrayed ? nri::TextureView::TEXTURE_ARRAY : nri::TextureView::TEXTURE);
                if (auto created = add_view(
                        {pass, index, EImageViewTarget::DESCRIPTOR, std::get<GraphImageRange>(use.range)},
                        kind,
                        layers
                    );
                    !created)
                {
                    return cxx::unexpected(created.error());
                }
            }
        }
        const auto aspects = rdesc::textureAspectMask(std::get<GraphImageDesc>(compiled.description).format);
        for (std::uint32_t layer = 0; layer < layers; ++layer)
        {
            const nri::TextureViewDesc view{
                image,
                aspects == 1 ? nri::TextureView::COLOR_ATTACHMENT : nri::TextureView::DEPTH_STENCIL_ATTACHMENT,
                description.format,
                0,
                1,
                static_cast<nri::Dim_t>(layer),
                1
            };
            nri::Descriptor* descriptor = nullptr;
            const auto created = core.CreateTextureView(view, descriptor);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            layer_views.push_back(descriptor);
        }
        // The acquired FIF slot has no GPU reader. Adopt all views before releasing the old image.
        frame_.invalidateResource(graph_.recordingDependencies(), resource.index);
        for (auto& replacement : replacements)
        {
            auto& pass = frame_.passes[replacement.use.pass];
            nri::Descriptor** destination = nullptr;
            switch (replacement.use.target)
            {
            case EImageViewTarget::DESCRIPTOR:
                destination = &pass.private_views[replacement.use.index];
                break;
            case EImageViewTarget::COLOR:
                destination = &pass.colors[replacement.use.index].descriptor;
                break;
            case EImageViewTarget::DEPTH:
                destination = &pass.depth.descriptor;
                break;
            case EImageViewTarget::COLOR_RESOLVE:
                destination = &pass.colors[replacement.use.index].resolveDst;
                break;
            case EImageViewTarget::DEPTH_RESOLVE:
                destination = &pass.depth.resolveDst;
                break;
            case EImageViewTarget::CLEAR:
                destination = &pass.clear_views[replacement.use.index];
                break;
            }
            auto* previous = std::exchange(*destination, replacement.descriptor);
            if (replacement.use.target == EImageViewTarget::DESCRIPTOR)
            {
                const auto& use =
                    graph_.bindingPlan().pass(pass.compiled_pass->binding_slot).uses[replacement.use.index];
                const nri::UpdateDescriptorRangeDesc
                    update{pass.private_descriptors, use.binding, use.element, destination, 1};
                core.UpdateDescriptorRanges(&update, 1);
                pass.history_views[replacement.use.index] = nullptr;
                const auto history = graph_.resources()[use.resource].history;
                if (history != kGraphInvalidIndex) frame_.pending_history_views.mark(history);
            }
            else
            {
                *std::ranges::find(frame_.native.views, previous) = replacement.descriptor;
            }
            core.DestroyDescriptor(previous);
        }
        for (auto* previous : selected->views)
        {
            std::erase(frame_.native.views, previous);
            core.DestroyDescriptor(previous);
        }
        selected->views = std::move(layer_views);
        frame_.native.views.insert(frame_.native.views.end(), selected->views.begin(), selected->views.end());
        if (replace_image)
        {
            std::vector<nri::Descriptor*> stream_views;
            frame_.native.collectRasterViews(nullptr, binding.image, stream_views);
            for (auto* view : stream_views) core.DestroyDescriptor(view);
            *std::ranges::find(frame_.native.images, binding.image) = image;
            auto* old_image = binding.image;
            binding.select(nullptr, image, core);
            core.DestroyTexture(old_image);
            // Runtime-layer uses are whole-resource. Each compiled transition spans all nominal layers.
            for (const auto [pass, index] : compiled.barrier_uses)
            {
                auto& barrier = frame_.passes[pass].images[index];
                barrier.texture = binding.image;
                barrier.layerNum = static_cast<nri::Dim_t>(layers);
            }
            for (const auto index : compiled.final_barrier_uses)
            {
                frame_.final_images[index].texture = binding.image;
                frame_.final_images[index].layerNum = static_cast<nri::Dim_t>(layers);
            }
        }
        rollback.release();
        return {};
    }

    RenderResult<void> RasterFramePreparation::prepareSections() noexcept
    {
        for (auto& image : frame_.layered_images) image.requested_layers = 0;

        for (const auto p : graph_.recordingDependencies().producer_passes)
        {
            const auto* program = graph_.programs().rasterStream(p);
            if (!program) continue;
            const auto& stream = draws_[program->list];
            if (!stream.written) return cxx::unexpected(RenderError{kFrameContract, {p}});
            const auto layer_count = static_cast<std::uint32_t>(stream.sections.size());
            const auto limit = session_.description().dimensions.textureLayerMaxNum;
            if (layer_count > limit) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, layer_count}});

            const auto matchesLayers = [&](const ResourceAccess& access) noexcept
            {
                const auto resource = access.version.resource;
                const auto& desc = std::get<GraphImageDesc>(graph_.resources()[resource.index].description);
                const auto& range = std::get<GraphImageRange>(access.range);
                if (!desc.runtime_layers) return !layer_count || range.layer_count == layer_count;
                if (!layer_count) return false;
                const auto image = std::ranges::find(
                    frame_.layered_images, resource, &FrameResources::LayeredImage::resource
                );
                if (image == frame_.layered_images.end()) return false;
                if (image->requested_layers && image->requested_layers != layer_count) return false;
                image->requested_layers = layer_count;
                return true;
            };
            const auto validAttachment = [&](const RasterAttachment& attachment) noexcept
            {
                return matchesLayers(attachment.access) && (!attachment.resolve || matchesLayers(*attachment.resolve));
            };
            const auto& scope = *graph_.recordingProgram().rasterScope(*frame_.passes[p].compiled_pass);
            if (scope.section_depth_bias && stream.sections.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p}});
            const bool compatible = std::ranges::all_of(scope.colors, validAttachment) &&
                (!scope.depth || validAttachment(*scope.depth));
            if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, layer_count}});
            if (!layer_count) continue;

            auto& seen = frame_.stream_layers_seen;
            seen.assign(layer_count, 0);
            std::uint32_t next_draw = 0;
            for (const auto& section : stream.sections)
            {
                const bool covers_next_draws =
                    section.draws.first == next_draw && section.draws.fits(stream.draws.size());
                const bool new_layer = section.layer < layer_count && !seen[section.layer];
                if (!covers_next_draws || !new_layer)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p, section.layer}});
                if (section.viewport)
                {
                    if (auto valid = section.viewport->validate(); !valid) return valid;
                }
                const bool invalid_bias = section.depth_bias &&
                    (!std::isfinite(section.depth_bias->constant) || !std::isfinite(section.depth_bias->slope));
                const bool wrong_bias_contract = section.depth_bias.has_value() != scope.section_depth_bias;
                if (invalid_bias || wrong_bias_contract)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, section.layer}});
                next_draw += section.draws.count;
                seen[section.layer] = 1;
            }
            if (next_draw != stream.draws.size())
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, next_draw}});
        }

        // Every stream has agreed on the shape before any native attachment is replaced.
        for (const auto& image : frame_.layered_images)
        {
            if (!image.requested_layers) continue;
            if (auto selection = resizeLayers(image.resource, image.requested_layers); !selection)
                return cxx::unexpected(selection.error());
        }
        return {};
    }

    RenderResult<void>
    RasterFramePreparation::materializeSections(std::uint32_t p, const RasterDrawInput& stream) noexcept
    {
        auto& candidate = frame_.stream_scratch;
        candidate.has_explicit_sections = !stream.sections.empty();
        candidate.sections.clear();
        candidate.colors.clear();
        if (!candidate.has_explicit_sections) return {};

        const auto& pass = frame_.passes[p];
        const auto& scope = *graph_.recordingProgram().rasterScope(*pass.compiled_pass);
        const auto& first = scope.colors.empty() ? *scope.depth : scope.colors.front();
        const auto& first_range = std::get<GraphImageRange>(first.access.range);
        const auto& core = session_.nriCore();
        const auto* image = frame_.bindings[first.access.version.resource.index].image;
        if (!image) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
        const auto& info = *frame_.bindings[first.access.version.resource.index].image_description;
        const auto width = std::max(1u, std::uint32_t(info.width) >> first_range.first_mip);
        const auto height = std::max(1u, std::uint32_t(info.height) >> first_range.first_mip);
        const auto& default_viewport = scope.viewport ? *scope.viewport : frame_.viewport;

        const auto sliceView = [&](const ResourceAccess& access, bool depth, std::uint32_t layer,
                                   nri::Descriptor*& view) noexcept -> RenderResult<void>
        {
            if (!view) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
            const auto resource = access.version.resource;
            const auto& desc = std::get<GraphImageDesc>(graph_.resources()[resource.index].description);
            if (desc.runtime_layers)
            {
                const auto image = std::ranges::find(
                    frame_.layered_images, resource, &FrameResources::LayeredImage::resource
                );
                if (image == frame_.layered_images.end() || layer >= image->views.size())
                    return cxx::unexpected(RenderError{kFrameContract, {p, layer}});
                view = image->views[layer];
                return {};
            }
            auto range = std::get<GraphImageRange>(access.range);
            if (range.layer_count == 1) return {};
            range.first_layer += layer;
            range.layer_count = 1;
            auto* texture = frame_.bindings[access.version.resource.index].image;
            if (!texture) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
            auto& views = frame_.native.raster_attachment_views;
            const auto found = std::ranges::find_if(views, [&](const auto& entry) noexcept
            {
                return entry.image == texture && entry.range == range && entry.depth == depth;
            });
            if (found != views.end())
            {
                view = found->view;
                return {};
            }
            const auto& native = *frame_.bindings[resource.index].image_description;
            const auto kind = depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT;
            const nri::TextureViewDesc description{
                texture, kind, native.format, static_cast<nri::Dim_t>(range.first_mip), 1,
                static_cast<nri::Dim_t>(range.first_layer), 1
            };
            nri::Descriptor* selected = nullptr;
            const auto status = core.CreateTextureView(description, selected);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            views.push_back({texture, range, depth, selected});
            view = selected;
            return {};
        };
        const auto sliceAttachment = [&](const RasterAttachment& attachment, bool depth, std::uint32_t layer,
                                         nri::AttachmentDesc& native) noexcept -> RenderResult<void>
        {
            auto sliced = sliceView(attachment.access, depth, layer, native.descriptor);
            if (!sliced) return sliced;
            if (attachment.resolve) return sliceView(*attachment.resolve, depth, layer, native.resolveDst);
            return {};
        };

        for (const auto& selected : stream.sections)
        {
            RasterDrawList::Section section;
            section.first_color = static_cast<std::uint32_t>(candidate.colors.size());
            section.color_count = static_cast<std::uint32_t>(scope.colors.size());
            section.first_draw = selected.draws.first;
            section.draw_count = selected.draws.count;
            const auto& viewport = selected.viewport ? *selected.viewport : default_viewport;
            if (auto valid = viewport.validate(); !valid) return valid;
            section.viewport = {
                viewport.x * width, viewport.y * height, viewport.width * width, viewport.height * height,
                viewport.min_depth, viewport.max_depth, true
            };
            const auto& v = section.viewport;
            const auto max_origin = std::numeric_limits<std::int16_t>::max();
            if (v.x > max_origin || v.y > max_origin)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, selected.layer}});
            const nri::Rect scissor{
                static_cast<std::int16_t>(v.x), static_cast<std::int16_t>(v.y),
                static_cast<nri::Dim_t>(v.width), static_cast<nri::Dim_t>(v.height)
            };
            for (auto& packet : std::span{candidate.packets}.subspan(section.first_draw, section.draw_count))
                packet.scissor = scissor;
            if (selected.depth_bias)
            {
                section.depth_bias = nri::DepthBiasDesc{selected.depth_bias->constant, 0, selected.depth_bias->slope};
            }
            for (std::size_t c = 0; c < scope.colors.size(); ++c)
            {
                auto native = pass.colors[c];
                auto sliced = sliceAttachment(scope.colors[c], false, selected.layer, native);
                if (!sliced) return sliced;
                candidate.colors.push_back(native);
            }
            if (scope.depth)
            {
                section.depth = pass.depth;
                auto sliced = sliceAttachment(*scope.depth, true, selected.layer, section.depth);
                if (!sliced) return sliced;
            }
            candidate.sections.push_back(section);
        }
        return {};
    }

} // namespace lux::render::detail
