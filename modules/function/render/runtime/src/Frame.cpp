#include "RuntimeBackend.hpp"
#include <algorithm>
#include <cstring>

namespace lux::render::detail
{
    namespace
    {
        bool drawable(const RuntimeBackendState& backend, const RuntimeViewState& view) noexcept
        {
            const auto* target = backend.targets.find(view.target);
            return view.active && target && target->width != 0 && target->height != 0;
        }
    } // namespace

    bool runtimeHasOutput(const RuntimeBackendState& backend) noexcept
    {
        if (std::any_of(
                backend.views.begin(),
                backend.views.end(),
                [&](const auto& view) { return drawable(backend, view); }
            ))
        {
            return true;
        }
        for (const auto& target : backend.targets)
        {
            if (!target.presentation || target.width == 0 || target.height == 0)
            {
                continue;
            }
            for (const auto& layer : target.layers)
            {
                const auto* view = backend.views.find(layer.view);
                if (view && view->active && view->latest)
                {
                    return true;
                }
            }
        }
        return false;
    }

    RenderResult<void> executeRuntimeFrame(
        RuntimeBackendState& backend,
        std::uint64_t serial,
        std::uint64_t time_ns
    ) noexcept
    {
        const auto frame_slot = static_cast<std::uint32_t>(serial % backend.capacity.frames_in_flight);
        // Admission precedes any mutation/submission for this frame. Capacity is
        // bounded by actual invocation storage, not by the number of Views.
        std::array<std::uint64_t, 3> budget{};
        for (const auto& view : backend.views)
        {
            if (!drawable(backend, view))
            {
                continue;
            }
            auto& invocations = backend.variants[view.variant]->invocations;
            auto complete = invocations.slotReady(view.identity.index * backend.capacity.frames_in_flight + frame_slot);
            if (!complete)
            {
                return cxx::unexpected(complete.error());
            }
            if (!*complete)
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
            const auto needed = invocations.submissionBudget();
            for (unsigned q = 0; q < 3; ++q)
            {
                budget[q] += needed[q];
            }
        }
        for (const auto& view : backend.views)
        {
            if (!view.active || (!view.latest && !drawable(backend, view)))
            {
                continue;
            }
            for (const auto& target : backend.targets)
            {
                if (!target.presentation || target.width == 0 || target.height == 0)
                {
                    continue;
                }
                const bool output = std::any_of(
                    target.layers.begin(),
                    target.layers.end(),
                    [&](const auto& layer) { return layer.view == view.identity; }
                );
                if (!output)
                {
                    continue;
                }
                // One release, its graphics join, target copy and retention join.
                budget[0] += 4;
                ++budget[1];
                ++budget[2];
            }
        }
        for (auto* queue : backend.queues)
        {
            std::uint64_t needed{};
            for (unsigned q = 0; q < 3; ++q)
            {
                if (backend.queues[q] == queue)
                {
                    needed += budget[q];
                }
            }
            if (needed > queue->capacity())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            if (needed > queue->capacity() - (queue->submitted() - queue->completed()))
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
        }
        for (auto& view : backend.views)
        {
            if (!drawable(backend, view))
            {
                continue;
            }
            const auto* scene = backend.scenes.find(view.scene);
            if (!scene)
            {
                std::terminate();
            }
            for (const auto& field : scene->passes)
            {
                if (field.path != view.path)
                {
                    continue;
                }
                auto& destination = view.values.passes[field.pass.value() - 1];
                // Both shapes were fixed by the same validated PassSchema.
                std::copy(field.value.scalars.begin(), field.value.scalars.end(), destination.scalars.begin());
                std::copy(field.value.fields.begin(), field.value.fields.end(), destination.fields.begin());
                destination.enabled = field.value.enabled;
            }
            view.values.scene_revision = scene->revision;
            auto& variant = *backend.variants[view.variant];
            for (const auto& field : backend.paths[view.path.value() - 1].frame_fields)
            {
                const auto& scalar =
                    variant.code.logical().identity().passes[field.pass.value() - 1].scalar_fields[field.scalar_field];
                auto* bytes = view.values.passes[field.pass.value() - 1].scalars.data() + scalar.offset;
                if (field.value == ERuntimeFrameValue::HISTORY_EPOCH)
                {
                    if (view.values.history_epoch > UINT32_MAX)
                    {
                        return cxx::unexpected(RenderError{vulkan::kCapacity});
                    }
                    const auto epoch = static_cast<std::uint32_t>(view.values.history_epoch);
                    std::memcpy(bytes, &epoch, sizeof(epoch));
                }
                else
                {
                    const float value = field.value == ERuntimeFrameValue::CAMERA_FLOAT
                                            ? view.values.camera[field.camera_element]
                                            : static_cast<float>(time_ns * 1e-9);
                    std::memcpy(bytes, &value, sizeof(value));
                }
            }
            const auto slot = view.identity.index * backend.capacity.frames_in_flight + frame_slot;
            auto& native_inputs = variant.targets[slot].imports;
            for (const auto& target_backing : variant.targets[slot].images)
            {
                if (target_backing->state_count == 0)
                {
                    continue;
                }
                auto input = std::find_if(
                    native_inputs.begin(),
                    native_inputs.end(),
                    [&](const auto& value) { return value.resource == target_backing->resource; }
                );
                if (input == native_inputs.end())
                {
                    std::terminate();
                }
                input->ranges = std::span{target_backing->states}.first(target_backing->state_count);
                input->host_ready = false;
            }
            for (std::size_t i = 0; i < view.native_imports.size(); ++i)
            {
                if (view.import_assets[i] != UINT32_MAX)
                {
                    continue;
                }
                view.native_imports[i] = native_inputs[i];
                view.logical_imports[i].backing = native_inputs[i].identity;
            }
            for (std::size_t i = 0; i < view.native_imports.size(); ++i)
            {
                if (view.import_assets[i] == UINT32_MAX)
                {
                    continue;
                }
                const auto& asset = backend.assets[view.import_assets[i]];
                if (asset.state_count != 0)
                {
                    view.native_imports[i].ranges = std::span{asset.states}.first(asset.state_count);
                    view.native_imports[i].host_ready = false;
                }
            }
            auto bindings = FrameGraphBindings::create(
                variant.code.logical(),
                {serial, time_ns, frame_slot},
                view.logical_imports,
                view.values
            );
            if (!bindings)
            {
                return cxx::unexpected(bindings.error());
            }
            std::optional<vulkan::GraphSubmission> donor;
            for (const auto& previous : backend.views)
            {
                const bool compatible = previous.identity != view.identity && previous.scene == view.scene &&
                                        previous.variant == view.variant && previous.latest_frame == serial &&
                                        previous.latest.has_value();
                if (!compatible)
                {
                    continue;
                }
                auto previous_bindings = FrameGraphBindings::create(
                    variant.code.logical(),
                    {serial, time_ns, frame_slot},
                    previous.logical_imports,
                    previous.values
                );
                if (!previous_bindings)
                {
                    return cxx::unexpected(previous_bindings.error());
                }
                bool share = false;
                for (auto pass : variant.code.logical().executionOrder())
                {
                    if (!variant.code.logical().sceneShareEligible()[pass.value() - 1])
                    {
                        continue;
                    }
                    share = mayShareSceneInvocation(pass, *previous_bindings, *bindings);
                    if (!share)
                    {
                        break;
                    }
                }
                if (share)
                {
                    donor = previous.latest;
                    break;
                }
            }
            auto submitted = variant.invocations.submit(slot, *bindings, view.native_imports, variant.trace, donor);
            if (!submitted)
            {
                return cxx::unexpected(submitted.error());
            }
            view.receipts[frame_slot] = *submitted;
            view.latest = *submitted;
            view.latest_frame = serial;
            for (auto& target_backing : variant.targets[slot].images)
            {
                auto states =
                    variant.invocations.resourceStates(*submitted, target_backing->resource, target_backing->states);
                if (!states)
                {
                    return cxx::unexpected(states.error());
                }
                target_backing->state_count = *states;
            }
            for (std::size_t i = 0; i < view.native_imports.size(); ++i)
            {
                if (view.import_assets[i] == UINT32_MAX)
                {
                    continue;
                }
                auto& asset = backend.assets[view.import_assets[i]];
                auto states =
                    variant.invocations.resourceStates(*submitted, view.native_imports[i].resource, asset.states);
                if (!states)
                {
                    return cxx::unexpected(states.error());
                }
                asset.state_count = *states;
            }
            ++backend.progress.native_invocations;
            for (std::uint32_t i = 0; i < submitted->trace_count; ++i)
            {
                backend.progress.reused_scene_passes += variant.trace[i].operation == vulkan::ENativeTrace::REUSE_SCENE;
            }
        }
        // One output phase per Target after every Scene/View invocation. Layers
        // are explicit opaque output placement, in stable order; shader blending
        // remains a declared Graph pass rather than hidden Driver shader work.
        for (auto& target : backend.targets)
        {
            if (!target.presentation || target.width == 0 || target.height == 0)
            {
                continue;
            }
            backend.output_views.clear();
            backend.output_layers.clear();
            for (const auto& declared : target.layers)
            {
                auto* view = backend.views.find(declared.view);
                if (!view)
                {
                    std::terminate();
                }
                if (!view->active || !view->latest)
                {
                    continue;
                }
                const auto output = backend.paths[view->path.value() - 1].output;
                if (!output)
                {
                    std::terminate();
                }
                auto& variant = *backend.variants[view->variant];
                const auto& images = variant.targets[view->latest->slot].images;
                const auto backing = std::find_if(
                    images.begin(),
                    images.end(),
                    [&](const auto& value) { return value->resource == *output; }
                );
                if (backing == images.end() || (*backing)->state_count != 1)
                {
                    return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                }
                const auto rectangle =
                    declared.placement.rectangle.value_or(RuntimeTargetRect{0, 0, target.width, target.height});
                backend.output_views.push_back(view);
                backend.output_layers.push_back(
                    {&variant.invocations,
                     *view->latest,
                     *output,
                     {{static_cast<std::int32_t>(rectangle.x), static_cast<std::int32_t>(rectangle.y)},
                      {rectangle.width, rectangle.height}},
                     false,
                     nullptr,
                     (*backing)->states[0]}
                );
            }
            if (backend.output_layers.empty())
            {
                continue;
            }
            auto presented = target.presentation->present(backend.output_layers, frame_slot);
            for (std::size_t i = 0; i < backend.output_layers.size(); ++i)
            {
                const auto& layer = backend.output_layers[i];
                if (!layer.copied)
                {
                    continue;
                }
                auto& view = *backend.output_views[i];
                view.latest = layer.receipt;
                view.receipts[layer.receipt.slot % backend.capacity.frames_in_flight] = layer.receipt;
                auto& targets = backend.variants[view.variant]->targets[layer.receipt.slot];
                for (auto& backing : targets.images)
                {
                    if (backing->resource != layer.output)
                    {
                        continue;
                    }
                    for (std::uint32_t range = 0; range < backing->state_count; ++range)
                    {
                        backing->states[range].state = {
                            VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            backend.queues[0]->nativeQueue().family
                        };
                        backing->states[range].ready = layer.receipt.completion;
                    }
                }
            }
            if (!presented)
            {
                const auto error = presented.error();
                const bool unavailable_target =
                    error.type == vulkan::kNativeFailure &&
                    (error.args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DATE_KHR) ||
                     error.args[0] == static_cast<std::uint32_t>(VK_ERROR_SURFACE_LOST_KHR));
                if (!unavailable_target)
                {
                    return cxx::unexpected(error);
                }
                target.notice = error;
                target.width = target.height = 0;
            }
        }
        ++backend.progress.render_frames;
        return {};
    }

    RenderResult<void> applyRuntimePublication(
        RuntimeBackendState& backend,
        RuntimeScenePublication request,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (request.owner != backend.owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        auto* scene = backend.scenes.find({request.index, request.generation});
        if (!scene || request.revision <= scene->revision)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        auto field = std::find_if(
            scene->passes.begin(),
            scene->passes.end(),
            [&](const auto& value)
            { return value.path == RuntimePathId{request.path} && value.pass == GraphPassId{request.pass}; }
        );
        const bool invalid_field = field == scene->passes.end() || field->value.scalars.size() != bytes.size();
        if (invalid_field)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        std::copy(bytes.begin(), bytes.end(), field->value.scalars.begin());
        scene->revision = request.revision;
        ++backend.progress.publications;
        return {};
    }

    RenderResult<void> applyRuntimePublication(
        RuntimeBackendState& backend,
        RuntimeViewPublication request,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (request.owner != backend.owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        auto* view = backend.views.find({request.index, request.generation});
        const bool invalid_view = !view || request.pass == 0 || request.pass > view->values.passes.size();
        if (invalid_view)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& pass = backend.variants[view->variant]->code.logical().identity().passes[request.pass - 1];
        auto& value = view->values.passes[request.pass - 1];
        const bool invalid_value = pass.scope == EExecutionScope::SCENE || value.scalars.size() != bytes.size() ||
                                   request.revision <= view->publication_revision;
        if (invalid_value)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        std::copy(bytes.begin(), bytes.end(), value.scalars.begin());
        view->values.camera = request.camera;
        view->publication_revision = request.revision;
        ++backend.progress.publications;
        return {};
    }

    RenderResult<void> applyRuntimeControl(RuntimeBackendState& backend, RuntimeControl request) noexcept
    {
        if (request.owner != backend.owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        if (request.operation == ERuntimeControl::RESIZE_TARGET)
        {
            const RenderTargetId key{request.index, request.generation};
            auto* target = backend.targets.find(key);
            if (!target)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            // Cold transaction: create every required candidate first. Failure
            // leaves all live View/Target identities and bindings unchanged.
            std::vector<std::pair<RuntimeViewState*, std::uint32_t>> replacements;
            std::vector<std::uint32_t> pins;
            if (request.first != 0 && request.second != 0)
            {
                for (const auto& layer : target->layers)
                {
                    if (!layer.placement.rectangle)
                    {
                        continue;
                    }
                    const auto rectangle = *layer.placement.rectangle;
                    const bool invalid_rectangle = rectangle.x > request.first || rectangle.y > request.second ||
                                                   rectangle.width > request.first - rectangle.x ||
                                                   rectangle.height > request.second - rectangle.y;
                    if (invalid_rectangle)
                    {
                        return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                    }
                }
                replacements.reserve(backend.views.size());
                pins.reserve(backend.views.size());
                for (auto& view : backend.views)
                {
                    if (view.target != key)
                    {
                        continue;
                    }
                    auto candidate = findRuntimeVariant(backend, view.path, {request.first, request.second}, pins);
                    if (!candidate)
                    {
                        return cxx::unexpected(candidate.error());
                    }
                    replacements.emplace_back(&view, *candidate);
                    pins.push_back(*candidate);
                }
            }
            if (target->presentation && request.first != 0 && request.second != 0)
            {
                auto changed = target->presentation->resize({request.first, request.second});
                if (!changed)
                {
                    if (!target->presentation->available())
                    {
                        target->width = target->height = 0;
                    }
                    return changed;
                }
            }
            target->width = request.first;
            target->height = request.second;
            ++target->epoch;
            for (auto [view, variant] : replacements)
            {
                view->variant = variant;
                view->values.history_epoch = target->epoch;
                for (auto& input : view->logical_imports)
                {
                    input.history_epoch = target->epoch;
                }
                std::fill(view->receipts.begin(), view->receipts.end(), std::nullopt);
                view->latest.reset();
                view->latest_frame = 0;
            }
            return {};
        }
        if (request.operation == ERuntimeControl::REMOVE_SCENE)
        {
            const RenderSceneId key{request.index, request.generation};
            if (!backend.scenes.find(key))
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const bool referenced = std::any_of(
                backend.views.begin(),
                backend.views.end(),
                [&](const auto& view) { return view.scene == key; }
            );
            if (referenced)
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
            backend.scenes.erase(key);
            return {};
        }
        if (request.operation == ERuntimeControl::REMOVE_TARGET)
        {
            const RenderTargetId key{request.index, request.generation};
            if (!backend.targets.find(key))
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const bool referenced = std::any_of(
                backend.views.begin(),
                backend.views.end(),
                [&](const auto& view) { return view.target == key; }
            );
            if (referenced)
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
            backend.targets.erase(key);
            return {};
        }
        const RenderViewHandle key{request.index, request.generation};
        auto* view = backend.views.find(key);
        if (!view)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        if (request.operation == ERuntimeControl::SET_VIEW_ACTIVE)
        {
            view->active = request.first != 0;
            return {};
        }
        if (request.operation == ERuntimeControl::REMOVE_VIEW)
        {
            // Native scratch remains owned by its bounded invocation pool until
            // actual completion; deleting semantic View state cannot free it.
            for (auto& target : backend.targets)
            {
                std::erase_if(target.layers, [&](const auto& layer) { return layer.view == key; });
            }
            backend.views.erase(key);
            return {};
        }
        if (request.operation == ERuntimeControl::SET_VIEW_PATH)
        {
            const RuntimePathId path{request.first};
            const auto* target = backend.targets.find(view->target);
            if (!target)
            {
                std::terminate();
            }
            auto candidate = findRuntimeVariant(backend, path, {target->width, target->height});
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            const auto& source = backend.paths[path.value() - 1];
            auto values = source.initial_values;
            values.scene = view->values.scene;
            values.view = view->values.view;
            values.target = view->values.target;
            values.history_epoch = view->values.history_epoch + 1;
            values.camera = view->values.camera;
            const auto& native_inputs = backend.variants[*candidate]->targets[0].imports;
            auto new_native_inputs = native_inputs;
            // A path switch preserves a View's temporal backing only for the
            // same stable resource semantic and compatible declared shape.
            const auto& old_graph = backend.variants[view->variant]->code.logical().identity();
            const auto& new_graph = backend.variants[*candidate]->code.logical().identity();
            for (auto& input : new_native_inputs)
            {
                const auto& declaration = new_graph.resources[input.resource.value() - 1];
                if (!declaration.import_contract || !declaration.import_contract->temporal_history)
                {
                    continue;
                }
                const auto old = std::find_if(
                    view->native_imports.begin(),
                    view->native_imports.end(),
                    [&](const auto& prior)
                    {
                        const auto& previous = old_graph.resources[prior.resource.value() - 1];
                        return previous.semantic == declaration.semantic &&
                               previous.description == declaration.description &&
                               previous.import_contract == declaration.import_contract;
                    }
                );
                if (old == view->native_imports.end())
                {
                    return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                }
                const auto identity = input.resource;
                input = *old;
                input.resource = identity;
            }
            std::vector<GraphImportBinding> imports;
            std::vector<std::uint32_t> assets;
            for (const auto& input : new_native_inputs)
            {
                imports.push_back({input.resource, input.identity, 0, 1, values.history_epoch});
                assets.push_back(runtimeAssetIndex(backend, input));
            }
            auto valid = FrameGraphBindings::create(backend.variants[*candidate]->code.logical(), {}, imports, values);
            if (!valid)
            {
                return cxx::unexpected(valid.error());
            }
            view->values = std::move(values);
            view->logical_imports = std::move(imports);
            view->native_imports = std::move(new_native_inputs);
            view->import_assets = std::move(assets);
            view->path = path;
            view->variant = *candidate;
            std::fill(view->receipts.begin(), view->receipts.end(), std::nullopt);
            view->latest.reset();
            view->latest_frame = 0;
            return {};
        }
        return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
    }
} // namespace lux::render::detail
