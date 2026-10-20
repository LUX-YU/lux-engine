#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RecordGraph.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>
#include <numeric>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct ExecutionContext::FrameResources final
    {
        void release(const nri::CoreInterface& api) noexcept
        {
            api.DestroyDescriptorPool(descriptor_pool);
            for (auto& binding : bindings)
            {
                api.DestroyDescriptor(binding.buffer_view);
            }
            for (auto* view : views)
            {
                api.DestroyDescriptor(view);
            }
            for (auto* image : images)
            {
                api.DestroyTexture(image);
            }
            for (auto* buffer : buffers)
            {
                api.DestroyBuffer(buffer);
            }
        }

        std::vector<Binding> bindings;
        std::vector<Pass> passes;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;
        std::vector<nri::Buffer*> buffers;
        std::vector<nri::Texture*> images;
        std::vector<nri::Descriptor*> views;
        nri::DescriptorPool* descriptor_pool = nullptr;
    };

    ExecutionContext::ExecutionContext(
        DeviceSession& session,
        const CompiledGraph& code,
        OffscreenTarget& output
    ) noexcept
        : session_(&session), code_(&code), target_(output.description()),
          storage_indices_(code.resources().size(), kGraphInvalidIndex), storages_(session.resources())
    {
        storage_uses_.reserve(code.resources().size());
        context_data_.reserve(code.contexts_.size());
        prepared_data_.reserve(code.contexts_.size());
        context_values_.resize(code.descriptor_plan_->context_parameters.size);
        parameter_copies_.resize(code.passes().size());
        for (const auto& pass : code.passes())
        {
            pass_parameters_.push_back(code.descriptor_plan_->programs[pass.layout_program].initial_parameters);
        }
        for (std::uint32_t slot = 0; slot < session.framesInFlight(); ++slot)
        {
            auto& frame = frames_[slot] = std::make_unique<FrameResources>();
            frame->bindings.resize(code.resources().size());
            frame->changed_resources.reserve(code.resources().size());
            frame->unavailable_buffers = std::ranges::count_if(
                code.resources(),
                [](const auto& resource)
                {
                    return resource.used && std::holds_alternative<GraphBufferDesc>(resource.description) &&
                           (resource.storage || resource.context_data != kGraphInvalidIndex);
                }
            );
            frame->images.reserve(std::ranges::count_if(
                code.resources(),
                [](const auto& resource)
                {
                    return resource.used && std::holds_alternative<GraphImageDesc>(resource.description) &&
                           !resource.storage;
                }
            ));
        }
    }

    ExecutionContext::~ExecutionContext() noexcept
    {
        withdraw();
        // Physical cleanup is owned by the Session; it never calls back into this Context.
        session_->deferRelease(
            [frames = std::move(frames_), api = &session_->api()]() mutable noexcept
            {
                for (auto& frame : frames)
                {
                    if (frame)
                    {
                        frame->release(*api);
                    }
                }
            }
        );
    }

    void ExecutionContext::withdraw() noexcept
    {
        // End future-use relationships before the Attachment's actual Storage owners are destroyed.
        // Submitted physical resources remain in the unique retirement facility.
        context_data_.clear();
        for (auto& use : storage_uses_)
        {
            use.subscription.reset();
        }
        storage_uses_.clear();
        storages_.clear();
    }

    RenderResult<std::unique_ptr<ExecutionContext>> DeviceSession::prepare(
        const CompiledGraph& code,
        OffscreenTarget& output,
        StorageSet* scene,
        StorageSet* view,
        std::span<const StorageArgument> arguments
    ) noexcept
    {
        const auto& target = output.description();
        const auto max_extent = description().dimensions.texture2DMaxDim;
        const bool invalid = &output.session() != this || code.device() != &device() || !code.compatible(target) ||
                             target.width == 0 || target.height == 0 || target.width > max_extent ||
                             target.height > max_extent;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const bool is_foreign_registry =
            (scene && &scene->registry() != &resources_) || (view && &view->registry() != &resources_);
        if (is_foreign_registry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        std::vector<const StorageArgument*> resolved_arguments(code.resources().size());
        for (const auto& argument : arguments)
        {
            const auto plan = std::ranges::find(code.resources(), argument.resource_key, &CompiledResource::key);
            if (plan == code.resources().end())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            if (!plan->used)
            {
                continue;
            }
            if (!plan->storage)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            const auto index = static_cast<std::size_t>(plan - code.resources().begin());
            const auto* previous = resolved_arguments[index];
            if (previous && *previous != argument)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            resolved_arguments[index] = &argument;
        }
        std::vector<std::pair<StorageSet*, StorageId>> created_storages;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& [set, id] : created_storages)
                {
                    (void)set->remove(id);
                }
            }
        );
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(*this, code, output));
        const auto prepareGroup = [&](std::span<const std::uint32_t> indices, StorageSet* set) -> RenderResult<void>
        {
            for (auto i : indices)
            {
                const auto& plan = context->code_->resources()[i];
                if (!plan.used)
                {
                    continue;
                }
                if (!set)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                const auto* factory = resources_.factory(plan.storage->type.view());
                const auto key = plan.storage->key.empty() ? plan.key : plan.storage->key;
                const auto* argument = resolved_arguments[i];
                const auto type = argument ? argument->typeInfo() : CppType{};
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument ? argument->data() : nullptr;
                const auto storage_type = resources_.typeId(plan.storage->type.view());
                const auto previous = set->find(storage_type, key);
                auto instance = set->getOrCreate(storage_type, key, plan.storage->config);
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                const auto id = *instance;
                if (previous.isNull() && set != &context->storages_)
                {
                    created_storages.emplace_back(set, id);
                }
                context->storage_indices_[i] = static_cast<std::uint32_t>(context->storage_uses_.size());
                auto& use = context->storage_uses_.emplace_back(context.get(), id, i);
                use.subscription.receiver = &use;
                use.subscription.changed = [](void* value, std::uint32_t slot, const StorageResource& resource) noexcept
                {
                    auto& link = *static_cast<ExecutionContext::StorageUse*>(value);
                    link.context->setResource(link.resource, slot, resource);
                };
                const auto& ops = *static_cast<const StorageBindingOps*>(factory->binding_ops);
                use.ops = &ops;
                use.native = resources_.native(id);
                std::array<StorageResource, 3> slices;
                auto resolved =
                    ops.resolve(use.native, plan, value, use.subscription, std::span(slices).first(frame_count_));
                if (!resolved)
                {
                    return cxx::unexpected(resolved.error());
                }
                for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
                {
                    context->setResource(i, slot, slices[slot]);
                }
            }
            return {};
        };
        const auto& groups = context->code_->storageGroups();
        if (auto r = prepareGroup(groups.global, &global_storages_); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.scene, scene); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.view, view); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.attachment, &context->storages_); !r)
        {
            return cxx::unexpected(r.error());
        }
        std::vector<StorageId> resolved_storages(code.resources().size());
        for (const auto& use : context->storage_uses_)
        {
            resolved_storages[use.resource] = use.storage_id;
        }
        for (const auto& contribution : code.contexts_)
        {
            auto value = contribution.create(*this, resolved_storages, contribution.configuration.data());
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            context->context_data_.push_back(std::move(*value));
        }
        for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
        {
            auto& frame = *context->frames_[slot];
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                const auto& plan = context->code_->resources()[i];
                if ((!plan.used) || plan.storage || plan.context_data != kGraphInvalidIndex)
                {
                    continue;
                }
                if (i == code.output())
                {
                    const auto frame = output.frame(slot);
                    binding.image = frame.image;
                    binding.view = frame.view;
                    continue;
                }
                if (const auto* image = std::get_if<GraphImageDesc>(&plan.description))
                {
                    const auto width = plan.target_sized ? target.width : image->width;
                    const auto height = plan.target_sized ? target.height : image->height;
                    if (width > max_extent || height > max_extent)
                    {
                        return cxx::unexpected(RenderError{kInvalidTargetExtent});
                    }
                    const auto format = imageFormat(image->format);
                    const bool color = (plan.image_usage & nri::TextureUsageBits::COLOR_ATTACHMENT) != 0;
                    const bool depth = (plan.image_usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0;
                    const nri::TextureDesc description{
                        nri::TextureType::TEXTURE_2D,
                        plan.image_usage,
                        format.native,
                        static_cast<nri::Dim_t>(width),
                        static_cast<nri::Dim_t>(height),
                        1,
                        1,
                        1,
                        1,
                        color || depth ? nri::SharingMode::EXCLUSIVE : nri::SharingMode::CONCURRENT
                    };
                    const auto created = api().CreatePlacedTexture(device(), NriDeviceHeap, description, binding.image);
                    if (created != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(created));
                    }
                    frame.images.push_back(binding.image);
                    if (color || depth)
                    {
                        const nri::TextureViewDesc view{
                            binding.image,
                            depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                            format.native,
                            0,
                            1,
                            0,
                            1
                        };
                        const auto status = api().CreateTextureView(view, binding.view);
                        if (status != nri::Result::SUCCESS)
                        {
                            return cxx::unexpected(nativeError(status));
                        }
                        frame.views.push_back(binding.view);
                    }
                }
                else
                {
                    const auto size = std::get<GraphBufferDesc>(plan.description).size;
                    const nri::BufferDesc description{size, 4, plan.buffer_usage};
                    const auto created = api().CreatePlacedBuffer(device(), NriDeviceHeap, description, binding.buffer);
                    if (created != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(created));
                    }
                    binding.buffer_size = size;
                    frame.buffers.push_back(binding.buffer);
                }
            }
            for (const auto& recipe : context->code_->passes())
            {
                ExecutionContext::Pass pass{&recipe};
                for (const auto& barrier : recipe.images)
                {
                    auto native = barrier.native;
                    native.texture = frame.bindings[barrier.resource].image;
                    if (native.before.layout == nri::Layout::UNDEFINED)
                    {
                        // The slot's previous execution is complete; order its reusable physical writes
                        // before discarding content. No prior layout or business identity is inferred here.
                        const auto usage = api().GetTextureDesc(*native.texture).usage;
                        native.before.stages = nri::StageBits::ALL;
                        native.before.access = nri::AccessBits::COPY_DESTINATION;
                        if ((usage & nri::TextureUsageBits::COLOR_ATTACHMENT) != 0)
                        {
                            native.before.access |= nri::AccessBits::COLOR_ATTACHMENT;
                        }
                        if ((usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0)
                        {
                            native.before.access |= nri::AccessBits::DEPTH_STENCIL_ATTACHMENT;
                        }
                    }
                    pass.images.push_back(native);
                }
                for (const auto& barrier : recipe.buffers)
                {
                    auto native = barrier.native;
                    native.buffer = frame.bindings[barrier.resource].buffer;
                    if (native.buffer && native.before.stages == nri::StageBits::ALL &&
                        native.before.access == nri::AccessBits::NONE)
                    {
                        native.before.access = bufferAccess(api().GetBufferDesc(*native.buffer).usage);
                    }
                    pass.buffers.push_back(native);
                }
                if (recipe.recipe == ERecipe::COPY_IMAGE_BUFFER)
                {
                    const auto& image = api().GetTextureDesc(*frame.bindings[recipe.source].image);
                    const auto pixel =
                        imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.source].description).format);
                    const auto row = std::uint32_t(image.width) * pixel.bytes;
                    const auto& alignment = description().memoryAlignment;
                    const auto pitch = (row + alignment.uploadBufferTextureRow - 1) / alignment.uploadBufferTextureRow *
                                       alignment.uploadBufferTextureRow;
                    const auto slice_unit = std::lcm(pitch, alignment.uploadBufferTextureSlice);
                    const auto slice = (std::uint64_t(pitch) * image.height + slice_unit - 1) / slice_unit * slice_unit;
                    if (slice > UINT32_MAX)
                    {
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    }
                    pass.copy_row_pitch = pitch;
                    pass.copy_slice_pitch = static_cast<std::uint32_t>(slice);
                    if (pitch != row ||
                        frame.bindings[recipe.destination].buffer_offset % alignment.uploadBufferTextureSlice != 0)
                    {
                        const nri::BufferDesc temporary{slice, 0, nri::BufferUsageBits::NONE};
                        const auto created =
                            api().CreatePlacedBuffer(device(), NriDeviceHeap, temporary, pass.copy_scratch);
                        if (created != nri::Result::SUCCESS)
                        {
                            return cxx::unexpected(nativeError(created));
                        }
                        frame.buffers.push_back(pass.copy_scratch);
                    }
                }
                frame.passes.push_back(std::move(pass));
            }
        }
        if (auto ready = context->prepareDescriptors(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        rollback.release();
        return context;
    }

    RenderResult<void> ExecutionContext::prepareDescriptors() noexcept
    {
        const auto& layout = *code_->descriptor_plan_;
        // Actual Storage parameter offsets are qualified once at Attach. Physical slices
        // share that type/layout; only their values and GPU addresses differ at recording.
        for (std::uint32_t p = 0; p < code_->passes().size(); ++p)
        {
            const auto& recipe = code_->passes()[p];
            if (recipe.recipe != ERecipe::DRAW && recipe.recipe != ERecipe::DISPATCH)
            {
                continue;
            }
            for (const auto& mapping : layout.programs[recipe.layout_program].parameters)
            {
                const auto& source = mapping.resource == kGraphInvalidIndex
                                         ? StorageResource{nullptr, 0, 0, context_values_, &layout.context_parameters}
                                         : StorageResource{
                                               nullptr,
                                               0,
                                               0,
                                               frames_[0]->bindings[mapping.resource].parameters,
                                               frames_[0]->bindings[mapping.resource].parameter_layout
                                           };
                if (!source.parameter_layout)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto& fields = source.parameter_layout->fields;
                const auto field = std::ranges::find(fields, mapping.source_field, &rdesc::ShaderMember::path);
                if (field == fields.end())
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto& destination = mapping.destination;
                const bool mismatch = field->scalar != destination.scalar || field->rows != destination.rows ||
                                      field->columns != destination.columns ||
                                      field->matrix_stride != destination.matrix_stride ||
                                      field->array_stride != destination.array_stride;
                const auto bytes = std::uint64_t(field->rows) * 4 +
                                   (field->columns ? std::uint64_t(field->columns - 1) * field->matrix_stride : 0);
                const bool invalid = !bytes || bytes > UINT32_MAX ||
                                     std::uint64_t(field->offset) + bytes > source.parameters.size() ||
                                     std::uint64_t(destination.offset) + bytes > pass_parameters_[p].size();
                if (mismatch || invalid)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                parameter_copies_[p].push_back(
                    {mapping.resource, field->offset, destination.offset, std::uint32_t(bytes)}
                );
            }
        }
        DescriptorArray* shared = nullptr;
        if (!layout.shared_bindings.empty())
        {
            auto table = session_->sampledImageArray();
            if (!table)
            {
                return cxx::unexpected(table.error());
            }
            shared = *table;
        }
        const auto draws = std::ranges::count_if(
            code_->passes(),
            [](const auto& recipe) { return recipe.recipe == ERecipe::DRAW || recipe.recipe == ERecipe::DISPATCH; }
        );
        const auto descriptors = std::uint64_t(draws) * layout.private_bindings.size();
        if (std::uint64_t(draws) > UINT32_MAX || descriptors > UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        for (std::uint32_t slot = 0; slot < session_->framesInFlight(); ++slot)
        {
            auto& frame = *frames_[slot];
            if (!layout.private_bindings.empty() && draws)
            {
                nri::DescriptorPoolDesc pool{};
                pool.descriptorSetMaxNum = static_cast<std::uint32_t>(draws);
                pool.storageStructuredBufferMaxNum = static_cast<std::uint32_t>(descriptors);
                const auto created = session_->api().CreateDescriptorPool(*device(), pool, frame.descriptor_pool);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
            }
            for (auto& pass : frame.passes)
            {
                if (pass.recipe->recipe != ERecipe::DRAW && pass.recipe->recipe != ERecipe::DISPATCH)
                {
                    continue;
                }
                pass.descriptors.resize(
                    unsigned(!layout.shared_bindings.empty()) + unsigned(!layout.private_bindings.empty())
                );
                if (shared)
                {
                    pass.descriptors[layout.shared_set] = shared->descriptors();
                }
                if (frame.descriptor_pool)
                {
                    const auto created = session_->api().AllocateDescriptorSets(
                        *frame.descriptor_pool,
                        *code_->layout(),
                        layout.private_set,
                        &pass.private_descriptors,
                        1,
                        0
                    );
                    if (created != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(created));
                    }
                    pass.descriptors[layout.private_set] = pass.private_descriptors;
                }
            }
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(i);
                }
            }
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        return {};
    }

    StorageId ExecutionContext::storageId(std::uint32_t resource) const noexcept
    {
        const auto index = storage_indices_[resource];
        return index == kGraphInvalidIndex ? StorageId{} : storage_uses_[index].storage_id;
    }

    nri::Buffer* ExecutionContext::nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot]->bindings[resource].buffer;
    }

    void ExecutionContext::setResource(
        std::uint32_t resource,
        std::uint32_t slot,
        const StorageResource& value
    ) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? session_->framesInFlight() : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = *frames_[current];
            auto& binding = frame.bindings[resource];
            if (std::holds_alternative<GraphBufferDesc>(code_->resources()[resource].description))
            {
                if (!binding.buffer && value.buffer)
                {
                    --frame.unavailable_buffers;
                }
                else if (binding.buffer && !value.buffer)
                {
                    ++frame.unavailable_buffers;
                }
            }
            binding.buffer = value.buffer;
            binding.buffer_offset = value.offset;
            binding.buffer_size = value.size;
            binding.parameters = value.parameters;
            binding.parameter_layout = value.parameter_layout;
            if (!binding.dirty)
            {
                binding.dirty = true;
                frame.changed_resources.push_back(resource);
            }
        }
    }

    RenderResult<void> ExecutionContext::rebind(std::uint32_t slot) noexcept
    {
        auto& frame = *frames_[slot];
        const auto& layout = *code_->descriptor_plan_;
        const auto& api = session_->api();
        for (auto resource : frame.changed_resources)
        {
            auto& source = frame.bindings[resource];
            if (!source.dirty)
            {
                continue;
            }
            if (!source.buffer)
            {
                source.dirty = false;
                continue;
            }
            const auto& uses = code_->resources()[resource];
            for (const auto [p, index] : uses.barrier_uses)
            {
                auto& barrier = frame.passes[p].buffers[index];
                const auto& recipe = frame.passes[p].recipe->buffers[index].native;
                barrier.buffer = source.buffer;
                if (recipe.before.stages == nri::StageBits::ALL && recipe.before.access == nri::AccessBits::NONE)
                {
                    barrier.before.access = bufferAccess(api.GetBufferDesc(*source.buffer).usage);
                }
            }
            if (!uses.descriptor_uses.empty())
            {
                const nri::BufferViewDesc description{
                    source.buffer,
                    nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                    source.buffer_offset,
                    source.buffer_size
                };
                nri::Descriptor* candidate = nullptr;
                const auto created = api.CreateBufferView(description, candidate);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
                // Only the selected reusable FIF slice is changed. Other slices keep their descriptors.
                api.DestroyDescriptor(std::exchange(source.buffer_view, candidate));
                for (const auto [p, index] : uses.descriptor_uses)
                {
                    auto& pass = frame.passes[p];
                    const auto& use = layout.programs[pass.recipe->layout_program].uses[index];
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, 0, &source.buffer_view, 1};
                    api.UpdateDescriptorRanges(&update, 1);
                }
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> ExecutionContext::setContextParameters(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != context_values_.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, context_values_.begin());
        return {};
    }

    RenderResult<void> ExecutionContext::setParameters(GraphPassId id, std::span<const std::byte> bytes) noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex || bytes.size() != pass_parameters_[index].size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, pass_parameters_[index].begin());
        return {};
    }

    std::span<const std::byte> ExecutionContext::parameters(GraphPassId id) const noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex ? std::span<const std::byte>{} : pass_parameters_[index];
    }

    RenderResult<bool> ExecutionContext::prepareFrame(std::uint32_t slot, UploadContext* uploads) noexcept
    {
        prepared_data_.clear();
        if (!context_data_.empty() && !uploads)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        bool ready = true;
        for (std::uint32_t index = 0; index < context_data_.size(); ++index)
        {
            auto& data = context_data_[index];
            auto prepared = data.prepare(data.value.get(), slot, *uploads);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (prepared->changed_resource)
            {
                setResource(code_->contexts_[index].resource.index, slot, *prepared->changed_resource);
            }
            ready &= prepared->state != FeatureFrameWork::EPreparation::WAITING;
            if (prepared->state == FeatureFrameWork::EPreparation::RECORD)
            {
                prepared_data_.push_back(index);
            }
        }
        if (!ready)
        {
            return false;
        }
        auto& frame = *frames_[slot];
        if (frame.unavailable_buffers)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (!frame.changed_resources.empty())
        {
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        return true;
    }

    void recordGraphPreparation(ExecutionContext& context, nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        for (auto index : context.prepared_data_)
        {
            auto& data = context.context_data_[index];
            data.record(data.value.get(), command, slot);
        }
        context.prepared_data_.clear();
    }

    void recordGraph(ExecutionContext& context, nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        const auto& api = context.session_->api();
        const auto& code = *context.code_;
        const auto& frame = *context.frames_[slot];
        const auto& bindings = frame.bindings;
        nri::BindPoint active_layout = nri::BindPoint::INHERIT;
        for (std::size_t p = 0; p < frame.passes.size(); ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& recipe = *pass.recipe;
            auto& parameters = context.pass_parameters_[p];
            for (const auto& copy : context.parameter_copies_[p])
            {
                const auto* source = copy.resource == kGraphInvalidIndex ? context.context_values_.data()
                                                                         : bindings[copy.resource].parameters.data();
                std::memcpy(parameters.data() + copy.destination_offset, source + copy.source_offset, copy.bytes);
            }
            if (!pass.images.empty() || !pass.buffers.empty())
            {
                const nri::BarrierDesc barriers{
                    nullptr,
                    0,
                    pass.buffers.data(),
                    static_cast<std::uint32_t>(pass.buffers.size()),
                    pass.images.data(),
                    static_cast<std::uint32_t>(pass.images.size())
                };
                api.CmdBarrier(command, barriers);
            }
            const auto& destination = bindings[recipe.destination];
            if (recipe.recipe == ERecipe::DRAW || recipe.recipe == ERecipe::DISPATCH)
            {
                const auto point = recipe.recipe == ERecipe::DRAW ? nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                if (point != active_layout)
                {
                    api.CmdSetPipelineLayout(command, point, *code.layout());
                    active_layout = point;
                }
                if (recipe.recipe == ERecipe::DISPATCH || recipe.bind_pipeline)
                {
                    api.CmdSetPipeline(command, *recipe.pipeline);
                }
                for (std::uint32_t set = 0; set < pass.descriptors.size(); ++set)
                {
                    api.CmdSetDescriptorSet(command, {set, pass.descriptors[set], point});
                }
                if (!parameters.empty())
                {
                    api.CmdSetRootConstants(
                        command,
                        {0, parameters.data(), static_cast<std::uint32_t>(parameters.size()), 0, point}
                    );
                }
            }
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
            {
                if (recipe.begin_rendering)
                {
                    nri::AttachmentDesc color{};
                    if (recipe.color != kGraphInvalidIndex)
                    {
                        color.descriptor = bindings[recipe.color].view;
                    }
                    color.loadOp = recipe.load;
                    color.clearValue.color.f =
                        {recipe.clear_color[0], recipe.clear_color[1], recipe.clear_color[2], recipe.clear_color[3]};
                    nri::RenderingDesc rendering{};
                    rendering.colors = &color;
                    rendering.colorNum = recipe.color != kGraphInvalidIndex;
                    if (recipe.depth != kGraphInvalidIndex)
                    {
                        rendering.depth.descriptor = bindings[recipe.depth].view;
                        rendering.depth.loadOp = recipe.depth_load;
                        rendering.depth.clearValue.depthStencil.depth = recipe.clear_depth;
                    }
                    api.CmdBeginRendering(command, rendering);
                    const auto& image = api.GetTextureDesc(*destination.image);
                    const auto& area = context.viewport_;
                    // Preserve the existing Vulkan clip/viewport convention. NRI's default flips Y.
                    const nri::Viewport viewport{
                        area.x * image.width,
                        area.y * image.height,
                        area.width * image.width,
                        area.height * image.height,
                        area.min_depth,
                        area.max_depth,
                        true
                    };
                    const nri::Rect scissor{0, 0, image.width, image.height};
                    api.CmdSetViewports(command, &viewport, 1);
                    api.CmdSetScissors(command, &scissor, 1);
                }
                for (const auto& source : recipe.vertices)
                {
                    const auto& binding = bindings[source.resource];
                    const nri::VertexBufferDesc buffer{
                        binding.buffer,
                        binding.buffer_offset + source.offset,
                        source.stride
                    };
                    api.CmdSetVertexBuffers(command, source.binding, &buffer, 1);
                }
                if (recipe.draw != EDrawRecipe::VERTICES)
                {
                    const auto& index = bindings[recipe.index_resource];
                    api.CmdSetIndexBuffer(
                        command,
                        *index.buffer,
                        index.buffer_offset + recipe.index_offset,
                        recipe.index_type
                    );
                }
                if (recipe.draw == EDrawRecipe::VERTICES)
                {
                    api.CmdDraw(command, {recipe.count, recipe.instances, recipe.first, recipe.first_instance});
                }
                else if (recipe.draw == EDrawRecipe::INDEXED)
                {
                    api.CmdDrawIndexed(
                        command,
                        {recipe.count, recipe.instances, recipe.first, recipe.vertex_offset, recipe.first_instance}
                    );
                }
                else
                {
                    const auto& indirect = bindings[recipe.indirect_resource];
                    api.CmdDrawIndexedIndirect(
                        command,
                        *indirect.buffer,
                        indirect.buffer_offset + recipe.indirect_offset,
                        recipe.count,
                        recipe.indirect_stride,
                        nullptr,
                        0
                    );
                }
                if (recipe.end_rendering)
                {
                    api.CmdEndRendering(command);
                }
                break;
            }
            case ERecipe::DISPATCH:
                api.CmdDispatch(command, {recipe.dispatch[0], recipe.dispatch[1], recipe.dispatch[2]});
                break;
            case ERecipe::CLEAR_IMAGE:
            {
                nri::RenderingDesc rendering{};
                nri::AttachmentDesc attachment{};
                attachment.descriptor = destination.view;
                attachment.loadOp = nri::LoadOp::CLEAR;
                const auto format =
                    imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.destination].description).format);
                if (format.aspects == nri::PlaneBits::DEPTH)
                {
                    attachment.clearValue.depthStencil.depth = recipe.clear_depth;
                    rendering.depth = attachment;
                }
                else
                {
                    attachment.clearValue.color.f =
                        {recipe.clear_color[0], recipe.clear_color[1], recipe.clear_color[2], recipe.clear_color[3]};
                    rendering.colors = &attachment;
                    rendering.colorNum = 1;
                }
                api.CmdBeginRendering(command, rendering);
                api.CmdEndRendering(command);
                break;
            }
            case ERecipe::FILL_BUFFER:
                api.CmdZeroBuffer(
                    command,
                    *destination.buffer,
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code.resources()[recipe.destination].description).size
                );
                break;
            case ERecipe::COPY_IMAGE_BUFFER:
            {
                const auto* texture = bindings[recipe.source].image;
                const auto& image = api.GetTextureDesc(*texture);
                const auto format =
                    imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.source].description).format);
                const nri::TextureRegionDesc region{0, 0, 0, image.width, image.height, 1, 0, 0, format.aspects};
                const nri::TextureDataLayoutDesc layout{
                    pass.copy_scratch ? 0 : destination.buffer_offset,
                    pass.copy_row_pitch,
                    pass.copy_slice_pitch
                };
                if (pass.copy_scratch)
                {
                    nri::BufferBarrierDesc scratch{
                        pass.copy_scratch,
                        {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY},
                        {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
                    };
                    const nri::BarrierDesc barrier{nullptr, 0, &scratch, 1};
                    api.CmdBarrier(command, barrier);
                    api.CmdReadbackTextureToBuffer(command, *pass.copy_scratch, layout, *texture, region);
                    std::swap(scratch.before, scratch.after);
                    api.CmdBarrier(command, barrier);
                    const auto row_bytes = std::uint64_t(image.width) * format.bytes;
                    for (std::uint32_t row = 0; row < image.height; ++row)
                    {
                        api.CmdCopyBuffer(
                            command,
                            *destination.buffer,
                            destination.buffer_offset + row * row_bytes,
                            *pass.copy_scratch,
                            std::uint64_t(row) * pass.copy_row_pitch,
                            row_bytes
                        );
                    }
                }
                else
                {
                    api.CmdReadbackTextureToBuffer(command, *destination.buffer, layout, *texture, region);
                }
                break;
            }
            case ERecipe::COPY_BUFFER:
            {
                const auto& source = bindings[recipe.source];
                api.CmdCopyBuffer(
                    command,
                    *destination.buffer,
                    destination.buffer_offset,
                    *source.buffer,
                    source.buffer_offset,
                    std::get<GraphBufferDesc>(code.resources()[recipe.source].description).size
                );
                break;
            }
            }
        }
    }

    void ExecutionContext::submitted(std::uint32_t slot) noexcept
    {
        progress_.last_submitted_slot = slot;
        ++progress_.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
    }
} // namespace lux::render::detail
