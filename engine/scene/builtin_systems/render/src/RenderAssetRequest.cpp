#include <algorithm>
#include <lux/engine/scene/detail/RenderAssetRequest.hpp>

namespace lux::scene::detail
{
namespace
{
bool terminal(ERenderAssetState state) noexcept
{
    return state == ERenderAssetState::FAILED || state == ERenderAssetState::CANCELLED ||
           state == ERenderAssetState::UNREFERENCED;
}

void adoptFailure(RenderAssetStatus &destination, const RenderAssetStatus &source) noexcept
{
    if (!terminal(destination.state) && terminal(source.state))
    {
        destination.state = source.state;
        destination.failure = source.failure;
        destination.render_failure = source.render_failure;
        destination.backend_status = source.backend_status;
        destination.failed_dependency = source.failed_dependency;
    }
}

template <class T> void acceptAsset(const AssetResult<T> &value, RenderAssetStatus &row, asset::AssetId id) noexcept
{
    if (terminal(row.state))
    {
        return;
    }
    const auto state = value.state.load(std::memory_order_acquire);
    if (state == AssetResult<T>::State::ERROR)
    {
        row.failure = value.failure;
        row.failed_dependency = id;
        row.state = ERenderAssetState::FAILED;
    }
    else if (state == AssetResult<T>::State::CANCELLED)
    {
        row.state = ERenderAssetState::CANCELLED;
    }
}

template <class Reply, class Handle>
void acceptReply(render::RenderRequest<Reply> &request, Handle &handle, Handle Reply::*field, RenderAssetStatus &row,
                 asset::AssetId id) noexcept
{
    if (!request.valid() || !request.isReady())
    {
        return;
    }
    auto result = request.tryResult();
    if (result)
    {
        handle = result->get().*field;
        if (result->get().status && !terminal(row.state))
        {
            row.backend_status = result->get().status;
            row.failed_dependency = id;
            row.state = ERenderAssetState::FAILED;
        }
    }
    else if (!terminal(row.state))
    {
        row.render_failure = result.error();
        row.failed_dependency = id;
        row.state = ERenderAssetState::FAILED;
    }
    request = {};
}

void uploadFailure(RenderAssetStatus &row, asset::AssetId id, render::ERenderUploadSubmitError error)
{
    if (error != render::ERenderUploadSubmitError::QUEUE_FULL &&
        error != render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
    {
        row.failure = error;
        row.failed_dependency = id;
        row.state = ERenderAssetState::FAILED;
    }
}

template <class Resource, class Cache>
std::shared_ptr<ResourceUse<Resource>> acquire(AssetReads &reads, render::RenderRuntime &runtime, asset::AssetId id,
                                               bool fresh, Cache &cache)
{
    if (const auto found = cache.find(id); found != cache.end())
    {
        if (auto value = found->second.lock(); value && (!fresh || !terminal(value->resource->row.state)))
        {
            return value;
        }
    }
    auto control = runtime.control();
    if (!control)
    {
        return {};
    }
    auto resource = std::make_shared<Resource>();
    resource->id = id;
    if constexpr (std::same_as<Resource, MeshResource>)
    {
        resource->mesh_ops = runtime.features().ops<render::MeshStackOperationIds>("StandardMeshStack");
    }
    else if constexpr (std::same_as<Resource, MaterialResource>)
    {
        resource->material_ops = runtime.features().ops<render::MaterialOperationIds>("StandardMaterial");
        resource->fresh = fresh;
    }
    auto retained = control->get().retainResource(resource, &Resource::release, reads.code);
    if (!retained)
    {
        return {};
    }
    auto use = std::make_shared<ResourceUse<Resource>>(resource, std::move(*retained));
    cache.insert_or_assign(id, use);
    std::erase_if(cache, [](const auto &entry) { return entry.second.expired(); });
    if (id.isNull())
    {
        resource->row.state = ERenderAssetState::UNREFERENCED;
        resource->read->state.store(std::remove_reference_t<decltype(*resource->read)>::State::CANCELLED);
    }
    else if (auto started = reads.read(id, resource->read); !started)
    {
        resource->row.state = ERenderAssetState::FAILED;
        resource->row.failure = started.error();
        resource->row.failed_dependency = id;
    }
    return use;
}
} // namespace

std::shared_ptr<ResourceUse<MeshResource>> AssetReads::acquireMesh(render::RenderRuntime &runtime, asset::AssetId id,
                                                                   bool fresh)
{
    return acquire<MeshResource>(*this, runtime, id, fresh, meshes);
}
std::shared_ptr<ResourceUse<MaterialResource>> AssetReads::acquireMaterial(render::RenderRuntime &runtime,
                                                                           asset::AssetId id, bool fresh)
{
    return acquire<MaterialResource>(*this, runtime, id, fresh, materials);
}
std::shared_ptr<ResourceUse<TextureResource>> AssetReads::acquireTexture(render::RenderRuntime &runtime,
                                                                         asset::AssetId id, bool fresh)
{
    return acquire<TextureResource>(*this, runtime, id, fresh, textures);
}

void MeshResource::acceptReplies() noexcept
{
    acceptReply(mesh_request, mesh, &render::MeshUploadedReply::handle, row, id);
    if (read)
    {
        acceptAsset(*read, row, id);
    }
    if (!terminal(row.state) && mesh.isValid())
    {
        row.state = ERenderAssetState::READY;
    }
}
void MeshResource::prepareStep(render::RenderRuntime &runtime, AssetReads &)
{
    if (terminal(row.state) || mesh.isValid() || mesh_request.valid() ||
        read->state.load(std::memory_order_acquire) != AssetResult<asset::MeshAsset>::State::VALUE)
    {
        return;
    }
    row.state = ERenderAssetState::UPLOADING;
    auto result = render::uploadMesh(render::MeshStackUploadClient{*runtime.upload(), mesh_ops}, read->value->data());
    if (result)
    {
        mesh_request = std::move(*result);
    }
    else
    {
        uploadFailure(row, id, result.error());
    }
}
bool MeshResource::release(void *opaque, render::RenderControlSession &control, std::size_t &budget,
                           bool retired) noexcept
{
    auto &self = *static_cast<MeshResource *>(opaque);
    if (!std::exchange(self.retiring, true))
    {
        self.read.reset();
    }
    self.acceptReplies();
    if (retired)
    {
        return true;
    }
    if (self.mesh_request.valid())
    {
        return false;
    }
    if (!self.mesh.isValid())
    {
        return true;
    }
    if (!budget || !control.canSubmit())
    {
        return false;
    }
    render::MeshStackControlClient{control, self.mesh_ops}.destroyMesh({std::exchange(self.mesh, {})});
    --budget;
    return true;
}

void TextureResource::acceptReplies() noexcept
{
    acceptReply(upload, handle, &render::Texture2DCreatedReply::handle, row, id);
    if (read)
    {
        acceptAsset(*read, row, id);
    }
    if (!terminal(row.state) && handle.isValid())
    {
        row.state = ERenderAssetState::READY;
    }
}
void TextureResource::prepareStep(render::RenderRuntime &runtime, AssetReads &)
{
    if (terminal(row.state) || handle.isValid() || upload.valid() ||
        read->state.load(std::memory_order_acquire) != AssetResult<asset::TextureAsset>::State::VALUE)
    {
        return;
    }
    const auto &image = read->value->data();
    if (image.layers() != 1)
    {
        uploadFailure(row, id, render::ERenderUploadSubmitError::PAYLOAD_INVALID);
        return;
    }
    std::vector<render::OwnedTextureMipLevel> levels;
    levels.reserve(image.mipCount());
    for (std::uint32_t mip{}; mip < image.mipCount(); ++mip)
    {
        const auto &range = image.mipRange(mip);
        levels.push_back(
            {image.pixels().subspan(static_cast<std::size_t>(range.offset), static_cast<std::size_t>(range.size)),
             range.width, range.height});
    }
    const bool mips = image.mipCount() == 1 && !rdesc::isCompressedFormat(image.pixelFormat()) &&
                      !rdesc::hasTextureFlag(image.flags(), rdesc::ETextureAssetFlags::NO_MIPS);
    row.state = ERenderAssetState::UPLOADING;
    auto result =
        runtime.upload()->tryCreateTexture2DMips(std::move(levels), image.channel(), image.pixelFormat(), mips);
    if (result)
    {
        upload = std::move(*result);
    }
    else
    {
        uploadFailure(row, id, result.error());
    }
}
bool TextureResource::release(void *opaque, render::RenderControlSession &control, std::size_t &budget,
                              bool retired) noexcept
{
    auto &self = *static_cast<TextureResource *>(opaque);
    if (!std::exchange(self.retiring, true))
    {
        self.read.reset();
    }
    self.acceptReplies();
    if (retired)
    {
        return true;
    }
    if (self.upload.valid())
    {
        return false;
    }
    if (!self.handle.isValid())
    {
        return true;
    }
    if (!budget || !control.canSubmit())
    {
        return false;
    }
    control.destroyTexture(std::exchange(self.handle, {}));
    --budget;
    return true;
}

void MaterialResource::acceptReplies() noexcept
{
    acceptReply(material_request, material, &render::MaterialUploadedReply::handle, row, id);
    acceptReply(forward_request, forward, &render::ShaderCompiledReply::shader, row, id);
    acceptReply(gbuffer_request, gbuffer, &render::ShaderCompiledReply::shader, row, id);
    if (read)
    {
        acceptAsset(*read, row, id);
    }
    for (const auto &texture : textures)
    {
        if (texture.use)
        {
            texture.use->resource->acceptReplies();
            adoptFailure(row, texture.use->resource->row);
        }
    }
    if (!terminal(row.state) && material.isValid())
    {
        row.state = ERenderAssetState::READY;
    }
}
void MaterialResource::prepareStep(render::RenderRuntime &runtime, AssetReads &reads)
{
    if (terminal(row.state) || material.isValid() ||
        read->state.load(std::memory_order_acquire) != AssetResult<asset::MaterialAsset>::State::VALUE)
    {
        return;
    }
    row.state = ERenderAssetState::UPLOADING;
    const auto &data = read->value->data();
    if (!texture_reads_started)
    {
        for (const auto id : data.texture_slot_ids)
        {
            if (!id.isNull() && std::ranges::find(textures, id, &TextureDependency::asset) == textures.end())
            {
                textures.push_back({id});
            }
        }
        texture_reads_started = true;
    }
    for (auto &texture : textures)
    {
        if (!texture.use)
        {
            texture.use = reads.acquireTexture(runtime, texture.asset, fresh);
        }
        if (texture.use)
        {
            texture.use->resource->prepareStep(runtime, reads);
        }
    }
    if (!forward.isValid() && !forward_request.valid() && runtime.controlAvailable())
    {
        auto info = rdesc::ShaderInfo::serialize(data.forward_info);
        forward_request = runtime.control()->get().compileShader(std::as_bytes(std::span{data.forward_spirv}), info);
    }
    if (!gbuffer.isValid() && !gbuffer_request.valid() && runtime.controlAvailable())
    {
        auto info = rdesc::ShaderInfo::serialize(data.gbuffer_info);
        gbuffer_request = runtime.control()->get().compileShader(std::as_bytes(std::span{data.gbuffer_spirv}), info);
    }
    const bool ready = std::ranges::all_of(textures, [](const auto &value) {
        return value.use && value.use->resource->row.state == ERenderAssetState::READY;
    });
    if (!forward.isValid() || !gbuffer.isValid() || !ready || material_request.valid())
    {
        return;
    }
    render::GraphMaterialData output{};
    output.param_count = data.parameter_count;
    for (std::size_t index{}; index < output.param_count; ++index)
    {
        std::copy_n(data.parameter_defaults[index].data(), 4, output.params[index]);
    }
    for (std::size_t slot{}; slot < data.texture_slot_ids.size(); ++slot)
    {
        const auto id = data.texture_slot_ids[slot];
        if (!id.isNull())
        {
            const auto found = std::ranges::find(textures, id, &TextureDependency::asset);
            output.tex_mask |= 1U << slot;
            output.tex_bindless[slot] = found->use->resource->handle.index;
        }
    }
    auto result =
        render::uploadGraphMaterial(render::MaterialUploadClient{*runtime.upload(), material_ops}, output, gbuffer,
                                    forward, static_cast<std::uint32_t>(data.alpha_mode), data.double_sided);
    if (result)
    {
        material_request = std::move(*result);
    }
    else
    {
        uploadFailure(row, id, result.error());
    }
}
bool MaterialResource::release(void *opaque, render::RenderControlSession &control, std::size_t &budget,
                               bool retired) noexcept
{
    auto &self = *static_cast<MaterialResource *>(opaque);
    if (!std::exchange(self.retiring, true))
    {
        self.read.reset();
    }
    self.acceptReplies();
    if (retired)
    {
        return true;
    }
    if (self.material_request.valid() || self.forward_request.valid() || self.gbuffer_request.valid())
    {
        return false;
    }
    if (!self.material.isValid() && !self.forward.isValid() && !self.gbuffer.isValid())
    {
        return true;
    }
    if (!budget || !control.canSubmit())
    {
        return false;
    }
    if (self.material.isValid())
    {
        render::MaterialControlClient{control, self.material_ops}.destroyMaterial({std::exchange(self.material, {})});
    }
    else if (self.forward.isValid())
    {
        control.destroyShader(std::exchange(self.forward, {}));
    }
    else
    {
        control.destroyShader(std::exchange(self.gbuffer, {}));
    }
    --budget;
    return false;
}

void AssetRequest::acceptReplies() noexcept
{
    mesh_use->resource->acceptReplies();
    material_use->resource->acceptReplies();
    adoptFailure(row, mesh_use->resource->row);
    adoptFailure(row, material_use->resource->row);
    mesh = mesh_use->resource->mesh;
    material = material_use->resource->material;
    if (!terminal(row.state) && mesh.isValid() && material.isValid())
    {
        row.state = ERenderAssetState::READY;
    }
}
void AssetRequest::prepareStep(render::RenderRuntime &runtime, AssetReads &reads)
{
    if (terminal(row.state) || row.state == ERenderAssetState::READY)
    {
        return;
    }
    mesh_use->resource->prepareStep(runtime, reads);
    material_use->resource->prepareStep(runtime, reads);
    if (mesh_use->resource->row.state != ERenderAssetState::READING ||
        material_use->resource->row.state != ERenderAssetState::READING)
    {
        row.state = ERenderAssetState::UPLOADING;
    }
}
} // namespace lux::scene::detail
