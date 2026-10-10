#include <lux/engine/scene/detail/RenderResourcesImpl.hpp>

#include <algorithm>

namespace lux::scene
{
    namespace
    {
        using namespace detail;
        bool terminal(ERenderAssetState state) noexcept
        {
            return state == ERenderAssetState::FAILED || state == ERenderAssetState::CANCELLED ||
                   state == ERenderAssetState::UNREFERENCED;
        }
        template <class Reply, class Handle>
        void acceptReply(
            render::TRenderRequest<Reply>& request,
            Handle& handle,
            Handle Reply::*field,
            AssetState& record
        ) noexcept
        {
            if (!request.valid() || !request.isReady())
                return;
            const auto result = request.tryResult();
            if (result)
            {
                handle = result->get().*field;
                if (result->get().status && !terminal(record.status.state))
                {
                    record.status.backend_status = result->get().status;
                    record.status.failed_dependency = record.key.asset;
                    record.status.state = ERenderAssetState::FAILED;
                }
            }
            else if (!terminal(record.status.state))
            {
                record.status.render_failure = result.error();
                record.status.failed_dependency = record.key.asset;
                record.status.state = ERenderAssetState::FAILED;
            }
            request = {};
        }
        void acceptUploads(AssetState& record) noexcept
        {
            std::visit(
                [&](auto& state) noexcept {
                    using State = std::remove_reference_t<decltype(state)>;
                    if constexpr (std::same_as<State, MeshState>)
                        acceptReply(state.upload, state.handle, &render::MeshUploadedReply::handle, record);
                    else if constexpr (std::same_as<State, TextureState>)
                        acceptReply(state.upload, state.handle, &render::Texture2DCreatedReply::handle, record);
                    else
                    {
                        acceptReply(state.upload, state.handle, &render::MaterialUploadedReply::handle, record);
                        acceptReply(state.forward_upload, state.forward, &render::ShaderCompiledReply::shader, record);
                        acceptReply(state.gbuffer_upload, state.gbuffer, &render::ShaderCompiledReply::shader, record);
                    }
                },
                record.payload
            );
        }
        void uploadFailure(AssetState& record, render::ERenderUploadSubmitError error) noexcept
        {
            const bool backpressured = error == render::ERenderUploadSubmitError::QUEUE_FULL ||
                                       error == render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED;
            if (!backpressured)
            {
                record.status.failure = error;
                record.status.failed_dependency = record.key.asset;
                record.status.state = ERenderAssetState::FAILED;
            }
        }
    } // namespace

    void RenderResources::Impl::accept(Record& owner) noexcept
    {
        auto& record = owner.asset();
        acceptUploads(record);
        if (terminal(record.status.state))
            return;
        std::visit(
            [&](auto& state) noexcept {
                using Result = typename decltype(state.read)::element_type;
                if (!state.read)
                {
                    record.status.state = ERenderAssetState::CANCELLED;
                    return;
                }
                const auto result = state.read->state.load(std::memory_order_acquire);
                if (result == Result::EState::ERROR)
                {
                    record.status.failure = state.read->failure;
                    record.status.failed_dependency = record.key.asset;
                    record.status.state = ERenderAssetState::FAILED;
                }
                else if (result == Result::EState::CANCELLED)
                    record.status.state = ERenderAssetState::CANCELLED;
                else if (state.handle.isValid())
                    record.status.state = ERenderAssetState::READY;
            },
            record.payload
        );
        if (auto* texture = std::get_if<TextureState>(&record.payload);
            texture && record.status.state == ERenderAssetState::READY)
        {
            texture->lookup.key() = texture->handle;
            texture->lookup.mapped() = &owner;
            texture_records.insert(std::move(texture->lookup));
        }
        if (auto* material = std::get_if<MaterialState>(&record.payload))
            for (auto id : material->textures)
                if (const auto* dependency = id.isValid() ? find(id) : nullptr;
                    dependency && !terminal(record.status.state) && terminal(dependency->asset().status.state))
                    record.status = dependency->asset().status;
    }

    void RenderResources::Impl::prepare(Record& owner) noexcept
    {
        auto& record = owner.asset();
        std::visit(
            [&](auto& state) noexcept {
                using State = std::remove_reference_t<decltype(state)>;
                using Result = typename decltype(state.read)::element_type;
                if (record.key.asset.isNull())
                {
                    record.status.state = ERenderAssetState::UNREFERENCED;
                    state.read->state.store(Result::EState::CANCELLED, std::memory_order_release);
                    return;
                }
                if (!state.read->started)
                {
                    const auto admitted = read(record.input, record.key.asset, state.read);
                    if (!admitted)
                    {
                        record.status.state = ERenderAssetState::FAILED;
                        record.status.failure = admitted.error();
                        record.status.failed_dependency = record.key.asset;
                        state.read->state.store(Result::EState::CANCELLED, std::memory_order_release);
                        return;
                    }
                }
                if (state.read->state.load(std::memory_order_acquire) != Result::EState::VALUE || state.upload.valid())
                    return;
                record.status.state = ERenderAssetState::UPLOADING;
                if constexpr (std::same_as<State, MeshState>)
                {
                    auto uploaded = render::uploadMesh(
                        render::MeshStackUploadClient{*runtime.upload(), state.operations},
                        std::shared_ptr<const lux::rdesc::Mesh>{state.read->value, &state.read->value->data()}
                    );
                    if (uploaded)
                        state.upload = std::move(*uploaded);
                    else
                        uploadFailure(record, uploaded.error());
                }
                else if constexpr (std::same_as<State, TextureState>)
                {
                    const auto& image = state.read->value->data();
                    if (image.layers() != 1)
                    {
                        uploadFailure(record, render::ERenderUploadSubmitError::PAYLOAD_INVALID);
                        return;
                    }
                    std::vector<render::OwnedTextureMipLevel> levels;
                    levels.reserve(image.mipCount());
                    for (std::uint32_t mip{}; mip < image.mipCount(); ++mip)
                    {
                        const auto& range = image.mipRange(mip);
                        levels.push_back(
                            {image.pixels().subspan(std::size_t(range.offset), std::size_t(range.size)),
                             range.width,
                             range.height}
                        );
                    }
                    const bool mips = image.mipCount() == 1 && !rdesc::isCompressedFormat(image.pixelFormat()) &&
                                      !rdesc::hasTextureFlag(image.flags(), rdesc::ETextureAssetFlags::NO_MIPS);
                    auto uploaded =
                        runtime.upload()
                            ->tryCreateTexture2DMips(std::move(levels), image.channel(), image.pixelFormat(), mips);
                    if (uploaded)
                        state.upload = std::move(*uploaded);
                    else
                        uploadFailure(record, uploaded.error());
                }
                else
                {
                    const auto& data = state.read->value->data();
                    const auto dependencyFor = [&](asset::AssetId asset) noexcept -> ResourceRecord* {
                        for (auto id : state.textures)
                            if (auto* dependency = id.isValid() ? find(id) : nullptr;
                                dependency && dependency->asset().key.asset == asset)
                                return dependency;
                        return nullptr;
                    };
                    bool ready = true;
                    for (auto asset : data.texture_slot_ids)
                    {
                        if (asset.isNull())
                            continue;
                        auto* dependency = dependencyFor(asset);
                        if (!dependency)
                        {
                            auto id = request(record.input, asset, EResourceKind::TEXTURE, record.retry_failed);
                            if (!id)
                            {
                                ready = false;
                                // Capacity may clear after another resource retires. Other
                                // admission failures are terminal and are never silently retried.
                                if (id.error().code != render::ERendererError::CAPACITY)
                                {
                                    record.status.state = ERenderAssetState::FAILED;
                                    record.status.failure = id.error();
                                    record.status.failed_dependency = asset;
                                }
                                continue;
                            }
                            *std::ranges::find_if(state.textures, [](auto value) { return !value.isValid(); }) = *id;
                            dependency = find(*id);
                        }
                        ready &= dependency->asset().status.state == ERenderAssetState::READY;
                    }
                    if (!state.forward.isValid() && !state.forward_upload.valid() && runtime.controlAvailable())
                        state.forward_upload = runtime.control()->get().compileShader(
                            std::as_bytes(std::span{data.forward_spirv}),
                            rdesc::ShaderInfo::serialize(data.forward_info)
                        );
                    if (!state.gbuffer.isValid() && !state.gbuffer_upload.valid() && runtime.controlAvailable())
                        state.gbuffer_upload = runtime.control()->get().compileShader(
                            std::as_bytes(std::span{data.gbuffer_spirv}),
                            rdesc::ShaderInfo::serialize(data.gbuffer_info)
                        );
                    if (!ready || !state.forward.isValid() || !state.gbuffer.isValid() || terminal(record.status.state))
                        return;
                    render::GraphMaterialData output{};
                    output.param_count = data.parameter_count;
                    for (std::size_t index{}; index < output.param_count; ++index)
                        std::copy_n(data.parameter_defaults[index].data(), 4, output.params[index]);
                    for (std::size_t slot{}; slot < data.texture_slot_ids.size(); ++slot)
                        if (const auto asset = data.texture_slot_ids[slot]; !asset.isNull())
                        {
                            output.tex_mask |= 1U << slot;
                            output.textures[slot] =
                                std::get<TextureState>(dependencyFor(asset)->asset().payload).handle;
                        }
                    auto uploaded = render::uploadGraphMaterial(
                        render::MaterialUploadClient{*runtime.upload(), state.operations},
                        output,
                        state.gbuffer,
                        state.forward,
                        std::uint32_t(data.alpha_mode),
                        data.double_sided
                    );
                    if (uploaded)
                        state.upload = std::move(*uploaded);
                    else
                        uploadFailure(record, uploaded.error());
                }
            },
            record.payload
        );
    }

    bool RenderResources::Impl::retire(Record& owner) noexcept
    {
        auto& record = owner.asset();
        std::erase_if(owner.submissions, [](const auto& state) { return state.complete(); });
        if (!owner.submissions.empty())
            return false;
        if (!std::exchange(record.retiring, true))
            std::visit([](auto& state) noexcept { state.read.reset(); }, record.payload);
        acceptUploads(record);
        if (runtime.status().state == render::ERenderRuntimeState::RETIRED)
            return true;
        auto admitted = runtime.control();
        if (!admitted)
            return false;
        auto& control = admitted->get();
        return std::visit(
            [&](auto& state) noexcept {
                using State = std::remove_reference_t<decltype(state)>;
                if (state.upload.valid())
                    return false;
                if constexpr (std::same_as<State, MaterialState>)
                {
                    if (state.forward_upload.valid() || state.gbuffer_upload.valid())
                        return false;
                    if (state.handle.isValid())
                    {
                        if (!control.canSubmit())
                            return false;
                        render::MaterialControlClient{control, state.operations}.destroyMaterial(
                            {std::exchange(state.handle, {})}
                        );
                    }
                    for (auto* shader : {&state.forward, &state.gbuffer})
                    {
                        if (!shader->isValid())
                            continue;
                        if (!control.canSubmit())
                            return false;
                        control.destroyShader(std::exchange(*shader, {}));
                    }
                    return true;
                }
                else
                {
                    if (!state.handle.isValid())
                        return true;
                    if (!control.canSubmit())
                        return false;
                    if constexpr (std::same_as<State, MeshState>)
                        render::MeshStackControlClient{control, state.operations}.destroyMesh(
                            {std::exchange(state.handle, {})}
                        );
                    else
                        control.destroyTexture(std::exchange(state.handle, {}));
                    return true;
                }
            },
            record.payload
        );
    }
} // namespace lux::scene
