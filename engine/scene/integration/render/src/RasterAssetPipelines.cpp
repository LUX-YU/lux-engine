#include <lux/engine/process/render_upload/RasterAssetPreparation.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/scene/render/RasterAssetPipelines.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace engine::render;
    namespace preparation = process::render_upload;

    namespace
    {
        template <RenderCommand C>
        exec::task<RenderResult<void>> releaseResource(
            RenderExecution& execution,
            BoundCommand<C> command,
            typename C::Request input
        ) noexcept
        {
            auto& client = execution.mainClient();
            for (;;)
            {
                auto response = co_await client.request(command, input);
                const bool frame_rejected =
                    response.error.type == kCommandCancelled || response.error.type == kFrameResynchronization;
                if (!frame_rejected)
                {
                    if (response.error.type != 0)
                    {
                        co_return cxx::unexpected(response.error);
                    }
                    co_return RenderResult<void>{};
                }
                co_await client.whenSubmittedFramesComplete();
                co_await stdexec::schedule(execution.execution().mainScheduler());
            }
        }
    } // namespace

    exec::task<RenderResult<RenderMesh>> MeshAssetPipeline::load(RenderAssetLoad load) noexcept
    {
        auto packed = co_await (
            preparation::decodeAsset<asset::MeshAsset>(
                std::move(load.source),
                execution.execution().cpu(),
                load.version.asset,
                load.limits,
                load.stop,
                std::move(load.snapshot)
            ) |
            stdexec::then(
                [](auto decoded) noexcept -> RenderResult<preparation::PackedMesh>
                {
                    if (!decoded)
                    {
                        return cxx::unexpected(decoded.error());
                    }
                    return preparation::packMesh((*decoded)->data());
                }
            )
        );
        if (!packed)
        {
            co_return cxx::unexpected(packed.error());
        }
        co_await stdexec::schedule(execution.assetScheduler());
        MeshWrite request{input, packed->description, packed->bytes};
        auto response = co_await execution.assetClient().request(create, request);
        if (response.error.type != 0)
        {
            co_return cxx::unexpected(response.error);
        }
        co_return RenderMesh{response.mesh, std::move(packed->description)};
    }

    exec::task<RenderResult<void>> MeshAssetPipeline::release(Value& value) noexcept
    {
        co_return co_await releaseResource(execution, remove, value.mesh);
    }

    exec::task<RenderResult<RenderTexture>> TextureAssetPipeline::load(RenderAssetLoad load) noexcept
    {
        auto decoded = co_await preparation::decodeAsset<asset::TextureAsset>(
            std::move(load.source),
            execution.execution().cpu(),
            load.version.asset,
            load.limits,
            load.stop,
            std::move(load.snapshot)
        );
        if (!decoded)
        {
            co_return cxx::unexpected(decoded.error());
        }
        co_await stdexec::schedule(execution.assetScheduler());
        const auto& texture = (*decoded)->data();
        const auto info = texture.info();
        TextureWrite request{input, info, {static_cast<const std::byte*>(texture.data()), texture.size()}};
        auto response = co_await execution.assetClient().request(create, request);
        if (response.error.type != 0)
        {
            co_return cxx::unexpected(response.error);
        }
        co_return RenderTexture{
            response.texture,
            {static_cast<std::uint32_t>(info.width),
             static_cast<std::uint32_t>(info.height),
             info.layers,
             info.mip_count,
             rdesc::textureImageFormat(info.pixel_format)}
        };
    }

    exec::task<RenderResult<void>> TextureAssetPipeline::release(Value& value) noexcept
    {
        co_return co_await releaseResource(execution, remove, value.texture);
    }

    exec::task<RenderResult<MaterialAssetPipeline::Value>> MaterialAssetPipeline::load(RenderAssetLoad load) noexcept
    {
        co_return co_await preparation::decodeAsset<asset::MaterialAsset>(
            std::move(load.source),
            cpu,
            load.version.asset,
            load.limits,
            load.stop,
            std::move(load.snapshot)
        );
    }

    exec::task<RenderResult<void>> MaterialAssetPipeline::release(Value& value) noexcept
    {
        value.reset();
        co_return RenderResult<void>{};
    }

    const rdesc::MaterialDescription* cookedMaterial(const RenderAssets& assets, RenderAssetVersion key) noexcept
    {
        const auto* value = assets.ready<MaterialAssetPipeline>(key);
        return value ? &(*value)->data() : nullptr;
    }

    exec::task<RenderResult<void>> installRasterAssetPipelines(
        RenderAssets& assets,
        CapabilityQuery capability,
        VCapabilityConfiguration configuration
    ) noexcept
    {
        auto errors = preparation::registerAssetErrors();
        if (!errors)
        {
            co_return cxx::unexpected(errors.error());
        }
        auto& execution = assets.executionContext();
        auto& client = execution.mainClient();
        auto mesh_create = execution.assetClient().bind<CreateMesh>();
        auto texture_create = execution.assetClient().bind<CreateTexture>();
        auto mesh_release = client.bind<ReleaseMesh>();
        auto texture_release = client.bind<ReleaseTexture>();
        auto list = client.bind<ListCapabilities>();
        auto install = client.bind<InstallCapabilities>();
        const bool missing_commands =
            !mesh_create || !texture_create || !mesh_release || !texture_release || !list || !install;
        if (missing_commands)
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        capability.input_schemas = {std::string(kIndexedMeshInput), std::string(kSampledTextureInput)};
        auto choices = co_await client.request(*list, capability);
        if (choices.error.type != 0)
        {
            co_return cxx::unexpected(choices.error);
        }
        if (choices.implementations.size() != 1 || choices.implementations.front().unavailable.type != 0)
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        const auto& contract = choices.implementations.front().capabilities.front();
        const auto accepts = [&](std::string_view schema, cxx::TypeToken type) noexcept
        {
            const auto input = std::ranges::find(contract.inputs, schema, &RenderPayloadContract::schema);
            return input != contract.inputs.end() && input->version == 1 && input->cppType() == type &&
                   input->use == ERenderPayloadUse::RESOURCE_INPUT &&
                   input->lifetime == ERenderPayloadLifetime::UNTIL_RESPONSE;
        };
        const bool compatible = contract.kind == ERenderCapabilityKind::RESOURCES &&
                                accepts(kIndexedMeshInput, cxx::typeToken<MeshWrite>()) &&
                                accepts(kSampledTextureInput, cxx::typeToken<TextureWrite>());
        if (!compatible)
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        CapabilityInstallation request{
            choices.implementations.front().selection,
            GlobalRenderScope{},
            std::move(configuration)
        };
        const auto installed = co_await client.request(*install, request);
        if (installed.error.type != 0)
        {
            co_return cxx::unexpected(installed.error);
        }
        CapabilityInput mesh_input, texture_input;
        for (const auto& input : installed.inputs)
        {
            if (input.schema == kIndexedMeshInput)
            {
                mesh_input = input.destination;
            }
            else if (input.schema == kSampledTextureInput)
            {
                texture_input = input.destination;
            }
        }
        if (mesh_input.implementation.isNull() || texture_input.implementation.isNull())
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        assets.install(MeshAssetPipeline{execution, mesh_input, *mesh_create, *mesh_release}, mesh_input);
        assets.install(
            TextureAssetPipeline{execution, texture_input, *texture_create, *texture_release},
            texture_input
        );
        assets.install(MaterialAssetPipeline{execution.execution().cpu()});
        assets.addInputs(installed.inputs);
        co_return RenderResult<void>{};
    }
} // namespace lux::scene
