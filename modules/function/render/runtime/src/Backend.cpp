#include "RuntimeBackend.hpp"
#include <algorithm>
#include <lux/engine/render/vulkan/shader/Format.hpp>

namespace lux::render
{
    namespace detail
    {
#if defined(LUX_RUNTIME_FOOTPRINT)
        void observeRuntimeFootprint(const RuntimeBackendState& backend) noexcept;
#endif

        std::uint32_t runtimeAssetIndex(
            const RuntimeBackendState& backend,
            const vulkan::NativeImportBinding& input
        ) noexcept
        {
            for (std::uint32_t i = 0; i < backend.assets.size(); ++i)
            {
                const bool found = std::visit(
                    [&](const auto& value)
                    {
                        using T = std::remove_cvref_t<decltype(value)>;
                        const auto* borrow = std::get_if<std::reference_wrapper<const T>>(&input.backing);
                        return borrow && &borrow->get() == &value;
                    },
                    backend.assets[i].backing
                );
                if (found)
                {
                    return i;
                }
            }
            return UINT32_MAX;
        }

        RuntimeBackendState::RuntimeBackendState(
            vulkan::VulkanInstance host,
            vulkan::VulkanDevice native,
            vulkan::VulkanAllocator memory,
            std::array<std::unique_ptr<vulkan::SubmissionQueue>, 3> queue_storage,
            std::array<vulkan::SubmissionQueue*, 3> native_queues,
            vulkan::RetirementQueue retired,
            RuntimeCapacity limits,
            std::uint64_t identity
        ) noexcept
            : instance(std::move(host)), device(std::move(native)), allocator(std::move(memory)),
              queue_owners(std::move(queue_storage)), queues(native_queues), retirement(std::move(retired)),
              scenes(limits.scenes), targets(limits.targets), views(limits.views), capacity(limits), owner(identity)
        {
            assets.reserve(limits.assets);
            samplers.reserve(limits.samplers);
            paths.reserve(limits.paths);
            variants.resize(limits.native_variants);
            output_layers.reserve(limits.views);
            output_views.reserve(limits.views);
        }

        RuntimeBackendState::~RuntimeBackendState() noexcept
        {
            // Each invocation owner also covers partial submissions with no final
            // join. Its teardown waits all accepted queue tickets before assets die.
            variants.clear();
        }

        RenderResult<void> drainRuntimeNative(RuntimeBackendState& backend) noexcept
        {
            for (const auto& variant : backend.variants)
            {
                if (!variant)
                {
                    continue;
                }
                auto last = variant->invocations.lastUse();
                if (!last)
                {
                    return cxx::unexpected(last.error());
                }
                if (*last)
                {
                    auto completed = backend.queues[0]->wait(**last, UINT64_MAX);
                    if (!completed)
                    {
                        return cxx::unexpected(completed.error());
                    }
                    if (!*completed)
                    {
                        std::terminate();
                    }
                }
            }
            auto collected = backend.retirement.collect();
            if (!collected)
            {
                return cxx::unexpected(collected.error());
            }
            return {};
        }

        static RenderResult<std::vector<RuntimeNativeVariant::InvocationTargets>> makeInvocationTargets(
            RuntimeBackendState& backend,
            const RuntimePathState& path_state,
            const vulkan::ExecutableGraphPlan& code,
            VkExtent2D extent
        ) noexcept
        {
            std::vector<RuntimeNativeVariant::InvocationTargets> targets(backend.capacity.frames_in_flight);
            for (auto& invocation : targets)
            {
                invocation.images.reserve(code.logical().imports().size());
                invocation.imports.reserve(code.logical().imports().size());
                for (const auto id : code.logical().imports())
                {
                    const auto& resource = code.logical().identity().resources[id.value() - 1];
                    if (!resource.target_semantic.isValid())
                    {
                        const auto input = std::find_if(
                            path_state.imports.begin(),
                            path_state.imports.end(),
                            [&](const auto& value) { return value.resource == id; }
                        );
                        if (input == path_state.imports.end())
                        {
                            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                        }
                        invocation.imports.push_back(*input);
                        continue;
                    }
                    const auto& texture = resource.texture();
                    const auto shape = texture.extent_kind == EExtentKind::TARGET_RELATIVE
                                           ? extent
                                           : VkExtent2D{texture.width, texture.height};
                    auto usage = code.resourceUsage(id);
                    if (!usage)
                    {
                        return cxx::unexpected(usage.error());
                    }
                    vulkan::ImageDescription description;
                    description.extent = shape;
                    description.format = vulkan::nativeTextureFormat(texture.format);
                    description.usage = static_cast<VkImageUsageFlags>(*usage);
                    description.samples = static_cast<VkSampleCountFlagBits>(texture.samples);
                    description.mip_levels = texture.mip_count;
                    description.array_layers = texture.array_layers;
                    auto image = vulkan::Image::create(backend.allocator, description);
                    if (!image)
                    {
                        return cxx::unexpected(image.error());
                    }
                    if (backend.next_backing == UINT64_MAX)
                    {
                        return cxx::unexpected(RenderError{vulkan::kCapacity});
                    }
                    auto owned =
                        std::make_unique<RuntimeNativeVariant::TargetBacking>(RuntimeNativeVariant::TargetBacking{
                            id,
                            GraphBackingId{backend.next_backing++},
                            std::move(*image)
                        });
                    owned->states.resize(backend.capacity.native_state_ranges);
                    const auto requirements = code.importViews(id);
                    owned->views.reserve(requirements.size());
                    owned->view_borrows.reserve(requirements.size());
                    for (const auto& required : requirements)
                    {
                        auto view = vulkan::ImageView::create(owned->image, required.range, required.type);
                        if (!view)
                        {
                            return cxx::unexpected(view.error());
                        }
                        owned->views.push_back(std::move(*view));
                        owned->view_borrows.push_back(std::cref(owned->views.back()));
                    }
                    invocation.imports.push_back(
                        {id, owned->identity, std::cref(owned->image), {}, {}, true, owned->view_borrows}
                    );
                    invocation.images.push_back(std::move(owned));
                }
            }
            return targets;
        }

        RenderResult<std::uint32_t> findRuntimeVariant(
            RuntimeBackendState& backend,
            RuntimePathId path,
            VkExtent2D extent,
            std::span<const std::uint32_t> candidate_pins
        ) noexcept
        {
            if (!path.isValid() || path.value() > backend.paths.size() || extent.width == 0 || extent.height == 0)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            std::optional<std::uint32_t> available;
            for (std::uint32_t i = 0; i < backend.variants.size(); ++i)
            {
                const auto& variant = backend.variants[i];
                if (!variant)
                {
                    available = i;
                    continue;
                }
                if (variant->path == path && variant->extent.width == extent.width &&
                    variant->extent.height == extent.height)
                {
                    ++backend.progress.native_cache_hits;
                    return i;
                }
            }
            if (!available)
            {
                for (std::uint32_t i = 0; i < backend.variants.size(); ++i)
                {
                    const bool referenced = std::any_of(
                        backend.views.begin(),
                        backend.views.end(),
                        [&](const auto& view) { return view.variant == i; }
                    );
                    const bool candidate =
                        std::find(candidate_pins.begin(), candidate_pins.end(), i) != candidate_pins.end();
                    if (referenced || candidate)
                    {
                        continue;
                    }
                    auto last = backend.variants[i]->invocations.lastUse();
                    if (!last)
                    {
                        return cxx::unexpected(last.error());
                    }
                    if (*last)
                    {
                        auto retired = backend.retirement.retire(**last, std::move(backend.variants[i]));
                        if (!retired)
                        {
                            return cxx::unexpected(retired.error());
                        }
                    }
                    else
                    {
                        backend.variants[i].reset();
                    }
                    available = i;
                    break;
                }
            }
            if (!available)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            auto& path_state = backend.paths[path.value() - 1];
            const auto original_extent = path_state.code.targetExtent();
            const bool compatible_extent =
                extent.width == original_extent.width && extent.height == original_extent.height;
            const vulkan::NativeCompileInputs
                inputs{path_state.code.logical(), backend.device, backend.queues, path_state.commands, true, extent};
            RenderResult<vulkan::ExecutableGraphPlan> code =
                compatible_extent ? RenderResult<vulkan::ExecutableGraphPlan>{path_state.code}
                                  : vulkan::specializeVulkanGraph(inputs, path_state.code);
            if (!code)
            {
                return cxx::unexpected(code.error());
            }
            for (const auto id : code->logical().imports())
            {
                auto count = code->resourceStateCount(id);
                if (!count)
                {
                    return cxx::unexpected(count.error());
                }
                if (*count > backend.capacity.native_state_ranges)
                {
                    return cxx::unexpected(RenderError{vulkan::kCapacity, {*count}});
                }
            }
            backend.progress.native_compilations += !compatible_extent;
            std::vector<std::pair<GraphSampler, std::reference_wrapper<const vulkan::Sampler>>> samplers;
            samplers.reserve(backend.samplers.size());
            for (std::uint32_t i = 0; i < backend.samplers.size(); ++i)
            {
                samplers.emplace_back(GraphSampler{i + 1}, std::cref(backend.samplers[i]));
            }
            auto candidate_targets = makeInvocationTargets(backend, path_state, *code, extent);
            if (!candidate_targets)
            {
                return cxx::unexpected(candidate_targets.error());
            }
            auto targets = std::move(*candidate_targets);
            auto invocations = vulkan::NativeGraphInvocations::create(
                *code,
                {backend.device,
                 backend.allocator,
                 targets[0].imports,
                 path_state.initial_values,
                 samplers,
                 backend.capacity.frames_in_flight}
            );
            if (!invocations)
            {
                return cxx::unexpected(invocations.error());
            }
            std::vector<vulkan::NativeTraceEvent> trace(invocations->traceCapacity());
            backend.variants[*available] = std::make_unique<RuntimeNativeVariant>(RuntimeNativeVariant{
                path,
                extent,
                std::move(*code),
                std::move(targets),
                std::move(*invocations),
                std::move(trace)
            });
#if defined(LUX_RUNTIME_FOOTPRINT)
            observeRuntimeFootprint(backend);
#endif
            return *available;
        }

        RenderResult<std::uint32_t> acquireRuntimeSlots(
            RuntimeBackendState& backend,
            std::uint32_t variant_index,
            std::span<const std::pair<std::uint32_t, std::uint32_t>> candidate_slots
        ) noexcept
        {
            auto& variant = *backend.variants[variant_index];
            const auto count = backend.capacity.frames_in_flight;
            for (std::uint32_t first = 0; first < variant.targets.size(); first += count)
            {
                const bool referenced = std::any_of(
                    backend.views.begin(),
                    backend.views.end(),
                    [&](const auto& view) { return view.variant == variant_index && view.invocation_first == first; }
                );
                const bool candidate =
                    std::find(candidate_slots.begin(), candidate_slots.end(), std::pair{variant_index, first}) !=
                    candidate_slots.end();
                if (referenced || candidate)
                {
                    continue;
                }
                bool ready = true;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    auto complete = variant.invocations.slotReady(first + i);
                    if (!complete)
                    {
                        return cxx::unexpected(complete.error());
                    }
                    ready = ready && *complete;
                }
                if (ready)
                {
                    return first;
                }
            }
            if (variant.targets.size() >= backend.capacity.views * count)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            const auto& path = backend.paths[variant.path.value() - 1];
            auto targets = makeInvocationTargets(backend, path, variant.code, variant.extent);
            if (!targets)
            {
                return cxx::unexpected(targets.error());
            }
            std::vector<std::pair<GraphSampler, std::reference_wrapper<const vulkan::Sampler>>> samplers;
            for (std::uint32_t i = 0; i < backend.samplers.size(); ++i)
            {
                samplers.emplace_back(GraphSampler{i + 1}, std::cref(backend.samplers[i]));
            }
            auto first = variant.invocations.append(
                {backend.device, backend.allocator, targets->front().imports, path.initial_values, samplers, count}
            );
            if (!first)
            {
                return cxx::unexpected(first.error());
            }
            // All fallible native construction has succeeded. Moves preserve
            // Image/ImageView addresses and every old GraphSubmission identity.
            for (auto& target : *targets)
            {
                variant.targets.push_back(std::move(target));
            }
#if defined(LUX_RUNTIME_FOOTPRINT)
            observeRuntimeFootprint(backend);
#endif
            return *first;
        }

    } // namespace detail

    VulkanBackend::VulkanBackend(std::unique_ptr<detail::RuntimeBackendState> backing) noexcept
        : backing_(std::move(backing))
    {
    }

    VulkanBackend::~VulkanBackend() noexcept = default;

    RenderResult<std::unique_ptr<VulkanBackend>> VulkanBackend::create(
        const RuntimeNativeOptions& options,
        std::uint64_t owner
    ) noexcept
    {
        const auto& c = options.capacity;
        const bool invalid_capacity = c.scenes == 0 || c.views == 0 || c.targets == 0 || c.paths == 0 ||
                                      c.native_variants == 0 || c.queue_submissions == 0 || c.frames_in_flight < 2 ||
                                      c.frames_in_flight > 3 || c.views > UINT32_MAX / c.frames_in_flight ||
                                      c.native_state_ranges == 0 || owner == 0;
        if (invalid_capacity)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        auto instance_options = options.instance;
        auto device_options = options.device;
        std::vector<const char*> extensions(instance_options.extensions.begin(), instance_options.extensions.end());
        if (options.presentation)
        {
            extensions.insert(
                extensions.end(),
                {VK_KHR_SURFACE_EXTENSION_NAME,
                 "VK_KHR_win32_surface",
                 VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                 VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME}
            );
            instance_options.extensions = extensions;
            device_options.swapchain_maintenance = true;
            device_options.multiple_queues = true;
        }
        auto instance = vulkan::VulkanInstance::create(instance_options);
        if (!instance)
        {
            return cxx::unexpected(instance.error());
        }
        auto device = vulkan::VulkanDevice::create(*instance, device_options);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        auto allocator = vulkan::VulkanAllocator::create(*device);
        if (!allocator)
        {
            return cxx::unexpected(allocator.error());
        }
        std::array<std::unique_ptr<vulkan::SubmissionQueue>, 3> owners;
        std::array<vulkan::SubmissionQueue*, 3> queues{};
        for (unsigned role = 0; role < 3; ++role)
        {
            const auto selected = device->queue(static_cast<vulkan::EQueueRole>(role));
            for (unsigned earlier = 0; earlier < role; ++earlier)
            {
                if (queues[earlier]->nativeQueue().handle == selected.handle)
                {
                    queues[role] = queues[earlier];
                    break;
                }
            }
            if (queues[role])
            {
                continue;
            }
            auto queue =
                vulkan::SubmissionQueue::create(*device, c.queue_submissions, static_cast<vulkan::EQueueRole>(role));
            if (!queue)
            {
                return cxx::unexpected(queue.error());
            }
            owners[role] = std::move(*queue);
            queues[role] = owners[role].get();
        }
        auto retirement = vulkan::RetirementQueue::create(*queues[0], c.native_variants);
        if (!retirement)
        {
            return cxx::unexpected(retirement.error());
        }
        auto state = std::make_unique<detail::RuntimeBackendState>(
            std::move(*instance),
            std::move(*device),
            std::move(*allocator),
            std::move(owners),
            queues,
            std::move(*retirement),
            c,
            owner
        );
        return std::unique_ptr<VulkanBackend>{new VulkanBackend(std::move(state))};
    }

    const vulkan::VulkanDevice& VulkanBackend::device() const noexcept
    {
        return backing_->device;
    }

    const vulkan::VulkanAllocator& VulkanBackend::allocator() const noexcept
    {
        return backing_->allocator;
    }

    RenderResult<std::reference_wrapper<const vulkan::Buffer>> VulkanBackend::retain(vulkan::Buffer buffer) noexcept
    {
        if (buffer.device() != device().native())
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        if (backing_->assets.size() == backing_->capacity.assets)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        auto& value = backing_->assets.emplace_back(detail::RuntimeAssetBacking{
            std::move(buffer),
            std::vector<vulkan::NativeRangeState>(backing_->capacity.native_state_ranges)
        });
        return std::cref(std::get<vulkan::Buffer>(value.backing));
    }

    RenderResult<std::reference_wrapper<const vulkan::Image>> VulkanBackend::retain(vulkan::Image image) noexcept
    {
        if (image.device() != device().native())
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        if (backing_->assets.size() == backing_->capacity.assets)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        auto& value = backing_->assets.emplace_back(detail::RuntimeAssetBacking{
            std::move(image),
            std::vector<vulkan::NativeRangeState>(backing_->capacity.native_state_ranges)
        });
        return std::cref(std::get<vulkan::Image>(value.backing));
    }

    RenderResult<GraphSampler> VulkanBackend::retain(vulkan::Sampler sampler) noexcept
    {
        if (sampler.device() != device().native())
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        if (backing_->samplers.size() == backing_->capacity.samplers)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        if (!backing_->paths.empty())
        {
            return cxx::unexpected(RenderError{vulkan::kBusy});
        }
        backing_->samplers.push_back(std::move(sampler));
        return GraphSampler{static_cast<std::uint32_t>(backing_->samplers.size())};
    }

    RenderResult<RuntimePath> VulkanBackend::addPath(RuntimeTarget target, RuntimePathSource source) noexcept
    {
        if (target.owner != backing_->owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        const auto* output = backing_->targets.find(target.local);
        if (!output)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        if (!backing_->scenes.empty())
        {
            return cxx::unexpected(RenderError{vulkan::kBusy});
        }
        if (backing_->paths.size() == backing_->capacity.paths)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        const auto required_imports = std::count_if(
            source.logical.imports().begin(),
            source.logical.imports().end(),
            [&](auto id) { return !source.logical.identity().resources[id.value() - 1].target_semantic.isValid(); }
        );
        if (source.imports.size() != required_imports)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        for (std::size_t i = 0; i < source.frame_fields.size(); ++i)
        {
            const auto& field = source.frame_fields[i];
            if (!field.pass.isValid() || field.pass.value() > source.logical.identity().passes.size())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto& pass = source.logical.identity().passes[field.pass.value() - 1];
            if (field.scalar_field >= pass.scalar_fields.size())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto& scalar = pass.scalar_fields[field.scalar_field];
            const bool resource_member = std::any_of(
                pass.bindings.begin(),
                pass.bindings.end(),
                [&](const auto& binding)
                { return scalar.path.starts_with(binding.path + ".") || scalar.path.starts_with(binding.path + "["); }
            );
            const bool history = field.value == ERuntimeFrameValue::HISTORY_EPOCH;
            const bool camera = field.value == ERuntimeFrameValue::CAMERA_FLOAT;
            const auto required_input = camera ? 1u : history ? 4u : 2u;
            const bool invalid = resource_member || scalar.size != 4 || scalar.array_count != 1 ||
                                 scalar.kind != (history ? rdesc::EScalarKind::UINT : rdesc::EScalarKind::FLOAT) ||
                                 (pass.invocation_inputs & required_input) == 0 ||
                                 (camera ? field.camera_element >= 16 : field.camera_element != 0) ||
                                 (field.value != ERuntimeFrameValue::CAMERA_FLOAT &&
                                  field.value != ERuntimeFrameValue::RENDER_SECONDS && !history);
            if (invalid)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (source.frame_fields[j].pass == field.pass &&
                    source.frame_fields[j].scalar_field == field.scalar_field)
                {
                    return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                }
            }
        }
        for (auto& input : source.imports)
        {
            const auto index = detail::runtimeAssetIndex(*backing_, input);
            if (index == UINT32_MAX || !input.views.empty())
            {
                return cxx::unexpected(RenderError{vulkan::kWrongOwner});
            }
            auto& asset = backing_->assets[index];
            if (input.ranges.size() > asset.states.size())
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            // No caller-owned range span escapes cold assembly.
            if (!input.ranges.empty())
            {
                if (asset.state_count != 0)
                {
                    return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                }
            }
        }
        std::span<const GraphImportBinding> no_imports;
        auto validated = FrameGraphBindings::create(source.logical, {}, no_imports, source.initial_values);
        // Import readiness is an Invocation fact. Validate complete imported fixtures when views are attached.
        if (source.logical.imports().empty() && !validated)
        {
            return cxx::unexpected(validated.error());
        }
        auto code = vulkan::compileVulkanGraph(
            {source.logical, backing_->device, backing_->queues, source.commands, true, {output->width, output->height}
            },
            std::move(source.programs)
        );
        if (!code)
        {
            return cxx::unexpected(code.error());
        }
        std::optional<GraphResourceId> present_output;
        for (const auto& output : source.logical.identity().outputs)
        {
            if (output.kind != EGraphOutput::PRESENT)
            {
                continue;
            }
            if (present_output)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto& resource = source.logical.identity().resources[output.resource.value() - 1];
            const bool invalid_output = resource.kind() != EGraphResourceKind::IMAGE ||
                                        resource.texture().mip_count != 1 || resource.texture().array_layers != 1 ||
                                        resource.texture().samples != 1 ||
                                        rdesc::textureAspectMask(resource.texture().format) != 1;
            if (invalid_output)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            present_output = output.resource;
        }
        // Only publish borrowed initial-state bytes after all fallible path
        // construction has succeeded. A failed candidate cannot mutate assets.
        for (auto& input : source.imports)
        {
            if (input.ranges.empty())
            {
                continue;
            }
            auto& asset = backing_->assets[detail::runtimeAssetIndex(*backing_, input)];
            std::copy(input.ranges.begin(), input.ranges.end(), asset.states.begin());
            asset.state_count = static_cast<std::uint32_t>(input.ranges.size());
            input.ranges = std::span{asset.states}.first(asset.state_count);
        }
        backing_->paths.push_back(
            {std::move(*code),
             std::move(source.commands),
             std::move(source.initial_values),
             std::move(source.imports),
             present_output,
             std::move(source.frame_fields)}
        );
        ++backing_->progress.native_compilations;
        return RuntimePath{backing_->owner, RuntimePathId{static_cast<std::uint32_t>(backing_->paths.size())}};
    }

    RenderResult<RuntimeScene> VulkanBackend::addScene() noexcept
    {
        auto& state = *backing_;
        if (state.scenes.size() == state.capacity.scenes)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        // All paths are assembled before Scenes. Refine shared native Buffer
        // cells once, transactionally, without changing logical versions or PSOs.
        if (state.scenes.empty())
        {
            std::vector<std::optional<vulkan::ExecutableGraphPlan>> candidates(state.paths.size());
            for (std::size_t p = 0; p < state.paths.size(); ++p)
            {
                const auto& path = state.paths[p];
                std::vector<vulkan::NativeBufferBoundary> extra;
                for (const auto& input : path.imports)
                {
                    const auto& declaration = path.code.logical().identity().resources[input.resource.value() - 1];
                    if (declaration.kind() != EGraphResourceKind::BUFFER)
                    {
                        continue;
                    }
                    const auto asset = detail::runtimeAssetIndex(state, input);
                    const auto boundaries = [](const LogicalGraphPlan& logical, GraphResourceId resource)
                    {
                        std::vector<std::uint64_t> offsets;
                        const auto append = [&](const VGraphRange& value)
                        {
                            if (const auto* range = std::get_if<BufferRange>(&value))
                            {
                                offsets.push_back(range->byte_offset);
                                offsets.push_back(range->byte_offset + range->byte_count);
                            }
                        };
                        for (const auto& pass : logical.identity().passes)
                        {
                            for (const auto& use : pass.uses)
                            {
                                if (use.resource == resource || (use.fallback && use.fallback->resource == resource))
                                {
                                    append(use.range);
                                }
                            }
                        }
                        for (const auto& output : logical.identity().outputs)
                        {
                            if (output.resource == resource)
                            {
                                append(output.range);
                            }
                        }
                        return offsets;
                    };
                    const auto own = boundaries(path.code.logical(), input.resource);
                    for (const auto& other : state.paths)
                    {
                        for (const auto& imported : other.imports)
                        {
                            if (detail::runtimeAssetIndex(state, imported) != asset)
                            {
                                continue;
                            }
                            const auto& shape =
                                other.code.logical().identity().resources[imported.resource.value() - 1];
                            if (shape.description != declaration.description)
                            {
                                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                            }
                            for (const auto offset : boundaries(other.code.logical(), imported.resource))
                            {
                                if (offset != 0 && offset < declaration.buffer().byte_size &&
                                    std::find(own.begin(), own.end(), offset) == own.end())
                                {
                                    extra.push_back({input.resource, offset});
                                }
                            }
                        }
                    }
                }
                if (extra.empty())
                {
                    continue;
                }
                auto refined = vulkan::specializeVulkanGraph(
                    {path.code.logical(),
                     state.device,
                     state.queues,
                     path.commands,
                     true,
                     path.code.targetExtent(),
                     extra},
                    path.code
                );
                if (!refined)
                {
                    return cxx::unexpected(refined.error());
                }
                for (const auto resource : refined->logical().imports())
                {
                    auto count = refined->resourceStateCount(resource);
                    if (!count)
                    {
                        return cxx::unexpected(count.error());
                    }
                    if (*count > state.capacity.native_state_ranges)
                    {
                        return cxx::unexpected(RenderError{vulkan::kCapacity});
                    }
                }
                candidates[p] = std::move(*refined);
            }
            for (std::size_t p = 0; p < candidates.size(); ++p)
            {
                if (candidates[p])
                {
                    state.paths[p].code = std::move(*candidates[p]);
                    ++state.progress.native_compilations;
                }
            }
        }
        detail::RuntimeSceneState scene;
        for (std::uint32_t path = 0; path < state.paths.size(); ++path)
        {
            const auto& source = state.paths[path];
            for (const auto id : source.code.logical().executionOrder())
            {
                if (source.code.logical().identity().passes[id.value() - 1].scope == EExecutionScope::SCENE)
                {
                    scene.passes.push_back({RuntimePathId{path + 1}, id, source.initial_values.passes[id.value() - 1]});
                }
            }
        }
        return RuntimeScene{state.owner, state.scenes.insert(std::move(scene))};
    }

    RenderResult<RuntimeTarget> VulkanBackend::addOffscreenTarget(std::uint32_t width, std::uint32_t height) noexcept
    {
        auto& state = *backing_;
        if (width == 0 || height == 0)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        if (state.targets.size() == state.capacity.targets)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        return RuntimeTarget{state.owner, state.targets.emplace(detail::RuntimeTargetState{width, height})};
    }

    RenderResult<RuntimeTarget> VulkanBackend::addWindowTarget(
        void* instance,
        void* window,
        std::uint32_t width,
        std::uint32_t height
    ) noexcept
    {
        auto& state = *backing_;
        if (state.targets.size() == state.capacity.targets)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        auto presentation = detail::PresentationTarget::create(
            state.device,
            state.allocator,
            *state.queues[0],
            instance,
            window,
            {width, height},
            state.capacity.frames_in_flight
        );
        if (!presentation)
        {
            return cxx::unexpected(presentation.error());
        }
        const auto extent = (*presentation)->extent();
        return RuntimeTarget{
            state.owner,
            state.targets.emplace(detail::RuntimeTargetState{extent.width, extent.height, 1, std::move(*presentation)})
        };
    }

    RenderResult<RuntimeView> VulkanBackend::addView(
        RuntimeScene scene,
        RuntimeTarget target,
        RuntimePath path,
        GraphInvocationData values,
        std::span<const vulkan::NativeImportBinding> history,
        RuntimeOutputPlacement placement
    ) noexcept
    {
        auto& state = *backing_;
        const bool wrong_owner = scene.owner != state.owner || target.owner != state.owner || path.owner != state.owner;
        if (wrong_owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        const auto* output = state.targets.find(target.local);
        const bool invalid = !state.scenes.find(scene.local) || !output || !path.local.isValid() ||
                             path.local.value() > state.paths.size();
        if (invalid)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        if (output->presentation && !state.paths[path.local.value() - 1].output)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        if (placement.rectangle)
        {
            const auto rect = *placement.rectangle;
            const bool invalid_rect = rect.x > output->width || rect.y > output->height || rect.width == 0 ||
                                      rect.height == 0 || rect.width > output->width - rect.x ||
                                      rect.height > output->height - rect.y;
            if (invalid_rect)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
        }
        if (output->presentation)
        {
            const auto& resource = state.paths[path.local.value() - 1]
                                       .code.logical()
                                       .identity()
                                       .resources[state.paths[path.local.value() - 1].output->value() - 1];
            auto supported =
                output->presentation->validateSource(vulkan::nativeTextureFormat(resource.texture().format));
            if (!supported)
            {
                return cxx::unexpected(supported.error());
            }
        }
        if (state.views.size() == state.capacity.views)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        auto variant = detail::findRuntimeVariant(state, path.local, {output->width, output->height});
        if (!variant)
        {
            return cxx::unexpected(variant.error());
        }
        auto imports = state.variants[*variant]->targets[0].imports;
        // History identity belongs to this View; native backing remains in the
        // single Backend owner tree. This is cold assembly, not a DataStorage API.
        const auto& logical = state.paths[path.local.value() - 1].code.logical();
        const auto history_count = std::count_if(
            logical.imports().begin(),
            logical.imports().end(),
            [&](auto id)
            {
                const auto& resource = logical.identity().resources[id.value() - 1];
                return resource.import_contract && resource.import_contract->temporal_history;
            }
        );
        if (history.size() != history_count)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        std::vector<std::uint32_t> history_assets;
        for (const auto& input : history)
        {
            auto found = std::find_if(
                imports.begin(),
                imports.end(),
                [&](const auto& value) { return value.resource == input.resource; }
            );
            if (found == imports.end())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto& declaration = logical.identity().resources[input.resource.value() - 1];
            const auto index = detail::runtimeAssetIndex(state, input);
            const bool invalid_history = declaration.persistent_scope != EPersistentScope::VIEW ||
                                         !declaration.import_contract ||
                                         !declaration.import_contract->temporal_history || index == UINT32_MAX ||
                                         !input.ranges.empty() || !input.views.empty();
            if (invalid_history)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const bool already_bound = std::any_of(
                state.views.begin(),
                state.views.end(),
                [&](const auto& view)
                {
                    return std::find(view.import_assets.begin(), view.import_assets.end(), index) !=
                           view.import_assets.end();
                }
            );
            const bool duplicate =
                std::find(history_assets.begin(), history_assets.end(), index) != history_assets.end();
            if (already_bound || duplicate)
            {
                return cxx::unexpected(RenderError{vulkan::kWrongOwner});
            }
            history_assets.push_back(index);
            *found = input;
        }
        std::vector<std::uint32_t> assets;
        for (const auto& input : imports)
        {
            assets.push_back(detail::runtimeAssetIndex(state, input));
        }
        std::vector<GraphImportBinding> logical_imports;
        for (const auto& input : imports)
        {
            logical_imports.push_back({input.resource, input.identity, 0, 1, output->epoch});
        }
        values.scene = (std::uint64_t{scene.local.gen} << 32) | (std::uint64_t{scene.local.index} + 1);
        values.scene_revision = state.scenes.find(scene.local)->revision;
        values.target = (std::uint64_t{target.local.gen} << 32) | (std::uint64_t{target.local.index} + 1);
        values.history_epoch = output->epoch;
        values.view = 1; // The local key is installed atomically with the completed View below.
        auto validated =
            FrameGraphBindings::create(state.variants[*variant]->code.logical(), {}, logical_imports, values);
        if (!validated)
        {
            return cxx::unexpected(validated.error());
        }
        auto first = detail::acquireRuntimeSlots(state, *variant);
        if (!first)
        {
            return cxx::unexpected(first.error());
        }
        const auto key = state.views.emplace(detail::RuntimeViewState{
            {},
            scene.local,
            target.local,
            path.local,
            *variant,
            *first,
            std::move(values),
            std::move(logical_imports),
            std::move(imports),
            std::move(assets),
            std::vector<std::optional<vulkan::GraphSubmission>>(state.capacity.frames_in_flight)
        });
        auto& view = *state.views.find(key);
        view.identity = key;
        view.values.view = (std::uint64_t{key.gen} << 32) | (std::uint64_t{key.index} + 1);
        auto* target_state = state.targets.find(target.local);
        target_state->layers.push_back({key, placement});
        std::sort(
            target_state->layers.begin(),
            target_state->layers.end(),
            [](const auto& a, const auto& b)
            {
                return a.placement.layer != b.placement.layer ? a.placement.layer < b.placement.layer
                                                              : a.view.index < b.view.index;
            }
        );
        return RuntimeView{state.owner, key};
    }

    RenderResult<void> VulkanBackend::reuseOutput(
        RuntimeView view,
        RuntimeTarget target,
        RuntimeOutputPlacement placement
    ) noexcept
    {
        auto& state = *backing_;
        if (view.owner != state.owner || target.owner != state.owner)
        {
            return cxx::unexpected(RenderError{vulkan::kWrongOwner});
        }
        const auto* source = state.views.find(view.local);
        auto* destination = state.targets.find(target.local);
        if (!source || !destination || !destination->presentation)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& path = state.paths[source->path.value() - 1];
        if (!path.output)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& resource = path.code.logical().identity().resources[path.output->value() - 1];
        auto format = destination->presentation->validateSource(vulkan::nativeTextureFormat(resource.texture().format));
        if (!format)
        {
            return format;
        }
        if (placement.rectangle)
        {
            const auto rect = *placement.rectangle;
            const bool invalid = rect.x > destination->width || rect.y > destination->height || rect.width == 0 ||
                                 rect.height == 0 || rect.width > destination->width - rect.x ||
                                 rect.height > destination->height - rect.y;
            if (invalid)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
        }
        const bool duplicate = std::any_of(
            destination->layers.begin(),
            destination->layers.end(),
            [&](const auto& layer) { return layer.view == view.local; }
        );
        if (duplicate)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        destination->layers.push_back({view.local, placement});
        std::sort(
            destination->layers.begin(),
            destination->layers.end(),
            [](const auto& a, const auto& b)
            {
                return a.placement.layer != b.placement.layer ? a.placement.layer < b.placement.layer
                                                              : a.view.index < b.view.index;
            }
        );
        return {};
    }
} // namespace lux::render
