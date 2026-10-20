#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RecordGraph.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>
#include <numeric>

namespace lux::render::detail
{
    ExecutionContext::ExecutionContext(
        DeviceSession& session,
        ResourceRegistry& resources,
        std::uint32_t frame_count,
        const CompiledGraph& code,
        OffscreenTarget& output
    ) noexcept
        : session_(&session), code_(&code), target_(output.description()),
          storage_indices_(code.resources().size(), kGraphInvalidIndex), storages_(resources)
    {
        storage_uses_.reserve(code.resources().size());
        context_data_.reserve(code.frame_contributions_.size());
        prepared_data_.reserve(code.frame_contributions_.size());
        context_values_.resize(code.descriptor_plan_->context_parameters.size);
        parameter_copies_.resize(code.passes().size());
        for (const auto& pass : code.passes())
        {
            pass_parameters_.push_back(code.descriptor_plan_->programs[pass.layout_program].initial_parameters);
        }
        frames_.resize(frame_count);
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            frame.bindings.resize(code.resources().size());
            frame.changed_resources.reserve(code.resources().size());
            frame.unavailable_buffers = std::ranges::count_if(
                code.resources(),
                [](const auto& resource)
                {
                    return resource.used && std::holds_alternative<GraphBufferDesc>(resource.description) &&
                           (resource.storage || resource.context_data != kGraphInvalidIndex);
                }
            );
            frame.images.reserve(std::ranges::count_if(
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
        disconnectStorageBindings();
        // Physical cleanup is owned by the Session; it never calls back into this Context.
        session_->deferRelease(
            [frames = std::move(frames_), api = &session_->nriCore()]() mutable noexcept
            {
                for (auto& frame : frames)
                {
                    frame.release(*api);
                }
            }
        );
    }

    void ExecutionContext::disconnectStorageBindings() noexcept
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

    RenderResult<void> ExecutionContext::bindStorage(
        std::uint32_t resource,
        StorageId id,
        const StorageBindingOps& ops,
        void* native,
        const void* argument
    ) noexcept
    {
        storage_indices_[resource] = static_cast<std::uint32_t>(storage_uses_.size());
        auto& use = storage_uses_.emplace_back(this, id, resource);
        use.subscription.receiver = &use;
        use.subscription.changed = [](void* value, std::uint32_t slot, const StorageResource& resource) noexcept
        {
            auto& link = *static_cast<ExecutionContext::StorageUse*>(value);
            link.context->setResource(link.resource, slot, resource);
        };
        use.ops = &ops;
        use.native = native;
        std::array<StorageResource, 3> slices;
        auto resolved = ops.resolve(
            use.native,
            code_->resources()[resource],
            argument,
            use.subscription,
            std::span(slices).first(frames_.size())
        );
        if (!resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            setResource(resource, slot, slices[slot]);
        }
        return {};
    }

    RenderResult<void> ExecutionContext::initialize(
        OffscreenTarget& output,
        FrameExecution& execution,
        ResourceRegistry& resources
    ) noexcept
    {
        auto& session = *session_;
        const auto& code = *code_;
        const auto frame_count = frames_.size();
        const auto& target = output.description();
        const auto max_extent = session.description().dimensions.texture2DMaxDim;
        const bool invalid = &output.session() != &session || code.device() != &session.device() ||
                             !code.compatible(target) || target.width == 0 || target.height == 0 ||
                             target.width > max_extent || target.height > max_extent;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::vector<StorageId> resolved_storages(code.resources().size());
        for (const auto& use : storage_uses_)
        {
            resolved_storages[use.resource] = use.storage_id;
        }
        for (const auto& contribution : code.frame_contributions_)
        {
            auto value =
                contribution
                    .create(session, execution, resources, resolved_storages, contribution.configuration.data());
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            context_data_.push_back(std::move(*value));
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            auto& frame = frames_[slot];
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                const auto& plan = code_->resources()[i];
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
                    if (slot == 0)
                    {
                        if (auto valid = session.validateTexture(description); !valid)
                        {
                            return cxx::unexpected(valid.error());
                        }
                    }
                    const auto created =
                        session.nriCore()
                            .CreatePlacedTexture(session.device(), NriDeviceHeap, description, binding.image);
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
                        const auto status = session.nriCore().CreateTextureView(view, binding.view);
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
                    const auto created =
                        session.nriCore()
                            .CreatePlacedBuffer(session.device(), NriDeviceHeap, description, binding.buffer);
                    if (created != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(created));
                    }
                    binding.buffer_size = size;
                    frame.buffers.push_back(binding.buffer);
                }
            }
            for (const auto& recipe : code_->passes())
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
                        const auto usage = session.nriCore().GetTextureDesc(*native.texture).usage;
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
                        native.before.access = bufferAccess(session.nriCore().GetBufferDesc(*native.buffer).usage);
                    }
                    pass.buffers.push_back(native);
                }
                if (recipe.recipe == ERecipe::COPY_IMAGE_BUFFER)
                {
                    const auto& image = session.nriCore().GetTextureDesc(*frame.bindings[recipe.source].image);
                    const auto pixel =
                        imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.source].description).format);
                    const auto row = std::uint32_t(image.width) * pixel.bytes;
                    const auto& alignment = session.description().memoryAlignment;
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
                            session.nriCore()
                                .CreatePlacedBuffer(session.device(), NriDeviceHeap, temporary, pass.copy_scratch);
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
        if (auto ready = prepareDescriptors(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        return {};
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
                                               frames_[0].bindings[mapping.resource].parameters,
                                               frames_[0].bindings[mapping.resource].parameter_layout
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
        for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(frames_.size()); ++slot)
        {
            auto& frame = frames_[slot];
            if (!layout.private_bindings.empty() && draws)
            {
                nri::DescriptorPoolDesc pool{};
                pool.descriptorSetMaxNum = static_cast<std::uint32_t>(draws);
                pool.storageStructuredBufferMaxNum = static_cast<std::uint32_t>(descriptors);
                const auto created = session_->nriCore().CreateDescriptorPool(*device(), pool, frame.descriptor_pool);
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
                    const auto created = session_->nriCore().AllocateDescriptorSets(
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
        return frames_[slot].bindings[resource].buffer;
    }

    void ExecutionContext::setResource(
        std::uint32_t resource,
        std::uint32_t slot,
        const StorageResource& value
    ) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = frames_[current];
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
        auto& frame = frames_[slot];
        const auto& layout = *code_->descriptor_plan_;
        const auto& api = session_->nriCore();
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

    RenderResult<bool> ExecutionContext::prepareFrame(std::uint32_t slot, ResourceUploader* uploads) noexcept
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
                setResource(code_->frame_contributions_[index].resource.index, slot, *prepared->changed_resource);
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
        auto& frame = frames_[slot];
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

} // namespace lux::render::detail
