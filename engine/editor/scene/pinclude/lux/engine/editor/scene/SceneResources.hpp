#pragma once
#include <lux/engine/editor/scene/ResourceRequestKey.hpp>

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <atomic>
#include <array>
#include <algorithm>

namespace lux::editor::workbench::detail
{
    inline asset::AssetId seedAssetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x53;
        bytes[1] = 0x56;
        bytes[2] = 1;
        bytes.back() = tail;
        return asset::AssetId{bytes};
    }

    template<class Asset>
    class AssetRead final
    {
        struct Receiver
        {
            using receiver_concept = stdexec::receiver_t;
            AssetRead* owner;
            void set_value(std::shared_ptr<const Asset> asset) && noexcept
            {
                owner->asset = std::move(asset);
                owner->ready.store(true, std::memory_order_release);
            }
            void set_error(process::asset_loading::AssetLoadFailure error) && noexcept
            {
                owner->failure = error;
                owner->ready.store(true, std::memory_order_release);
            }
            void set_stopped() && noexcept { owner->ready.store(true, std::memory_order_release); }
            stdexec::empty_env get_env() const noexcept { return {}; }
        };
        using Sender = decltype(process::asset_loading::loadAsset<Asset>(
            std::declval<process::asset_loading::AssetReadPort>(), asset::AssetId{},
            asset::AssetDecodeLimits{1, 1, 1}));
        using Operation = decltype(stdexec::connect(std::declval<Sender>(), Receiver{}));
        std::unique_ptr<Operation> operation_;
    public:
        AssetRead(process::asset_loading::AssetReadPort port, asset::AssetId id)
            : operation_(new Operation(stdexec::connect(process::asset_loading::loadAsset<Asset>(
                std::move(port), id, asset::AssetDecodeLimits{16 * 1024 * 1024, 32 * 1024 * 1024, 16}),
                Receiver{this})))
        {}
        void start() noexcept { started = true; stdexec::start(*operation_); }
        bool started{};
        std::atomic<bool> ready{};
        std::shared_ptr<const Asset> asset;
        process::asset_loading::AssetLoadFailure failure{};
        bool done() const noexcept { return !started || ready.load(std::memory_order_acquire); }
    };

    struct ResourceJob final
    {
        EditorSceneHandle scene;
        simulation::ecs::Entity entity{};
        asset::AssetId mesh_source, material_source;
        std::uint64_t serial{1};
        std::unique_ptr<AssetRead<asset::MeshAsset>> mesh_read;
        std::unique_ptr<AssetRead<asset::MaterialAsset>> material_read;
        render::RenderRequest<render::MeshUploadedReply> mesh_upload;
        render::RenderRequest<render::MaterialUploadedReply> material_upload;
        render::RenderRequest<render::ShaderCompiledReply> forward_compile, gbuffer_compile;
        render::RMeshHandle mesh;
        render::RMaterialHandle material;
        render::ShaderHandle forward, gbuffer;
        bool mesh_submitted{}, shaders_submitted{}, material_submitted{}, adopted{}, failed{}, released{};
        const char* status{"Reading assets"};

        ResourceJob(EditorContext& context, EditorSceneHandle scene_id, simulation::ecs::Entity target,
                    asset::AssetId mesh_id, asset::AssetId material_id, std::uint64_t request_serial = 1)
            : scene(scene_id), entity(target), mesh_source(mesh_id), material_source(material_id),
              serial(request_serial),
              mesh_read(std::make_unique<AssetRead<asset::MeshAsset>>(context.assetRead(), mesh_id)),
              material_read(std::make_unique<AssetRead<asset::MaterialAsset>>(context.assetRead(), material_id)) {}
        void start() noexcept { mesh_read->start(); material_read->start(); }

        void poll(lux::scene::RenderRuntimeLease& runtime, EditorContext& context, bool closing,
            std::uint64_t current_serial)
        {
            if (released)
                return;
            auto& control = runtime.control();
            const auto mesh_ops = runtime.features().ops<render::MeshStackOperationIds>("StandardMeshStack");
            const auto material_ops = runtime.features().ops<render::MaterialOperationIds>("StandardMaterial");
            const auto shader_reply = [&](auto& request, auto& handle) {
                if (!request.valid() || !request.isReady())
                    return;
                const auto result = request.tryResult();
                if (result)
                    handle = result->get().shader;
                if (!result || result->get().status != 0)
                    failed = true;
                request = {};
            };
            shader_reply(forward_compile, forward);
            shader_reply(gbuffer_compile, gbuffer);
            if (mesh_upload.valid() && mesh_upload.isReady())
            {
                const auto result = mesh_upload.tryResult();
                if (result)
                    mesh = result->get().handle;
                if (!result || result->get().status != 0)
                    failed = true;
                mesh_upload = {};
            }
            if (material_upload.valid() && material_upload.isReady())
            {
                const auto result = material_upload.tryResult();
                if (result)
                    material = result->get().handle;
                if (!result || result->get().status != 0)
                    failed = true;
                material_upload = {};
            }
            if (!mesh_read->done() || !material_read->done())
                return;
            if (!mesh_read->asset || !material_read->asset)
                failed = true;
            if (failed)
                status = "Asset read, decode or GPU upload failed";
            if (closing || failed)
                return;
            if (!mesh_submitted)
            {
                auto request = render::uploadMesh(render::MeshStackUploadClient{runtime.upload(), mesh_ops},
                    mesh_read->asset->data());
                if (request)
                {
                    mesh_upload = std::move(*request);
                    mesh_submitted = true;
                }
                else if (request.error() == render::ERenderUploadSubmitError::PAYLOAD_INVALID ||
                    request.error() == render::ERenderUploadSubmitError::STOPPING)
                    failed = true;
                status = "Uploading geometry";
            }
            const auto& description = material_read->asset->data();
            if (!shaders_submitted)
            {
                const auto forward_info = rdesc::ShaderInfo::serialize(description.forward_info);
                const auto gbuffer_info = rdesc::ShaderInfo::serialize(description.gbuffer_info);
                forward_compile = control.compileShader(
                    std::as_bytes(std::span{description.forward_spirv}), forward_info);
                gbuffer_compile = control.compileShader(
                    std::as_bytes(std::span{description.gbuffer_spirv}), gbuffer_info);
                shaders_submitted = true;
                if (!forward_compile.valid() || !gbuffer_compile.valid())
                    failed = true;
            }
            if (forward.isValid() && gbuffer.isValid() && !material_submitted)
            {
                render::GraphMaterialData data{};
                data.param_count = description.parameter_count;
                for (std::size_t index = 0; index < data.param_count; ++index)
                    std::copy_n(description.parameter_defaults[index].data(), 4, data.params[index]);
                auto request = render::uploadGraphMaterial(
                    render::MaterialUploadClient{runtime.upload(), material_ops}, data, gbuffer, forward,
                    static_cast<std::uint32_t>(description.alpha_mode), description.double_sided);
                if (request)
                {
                    material_upload = std::move(*request);
                    material_submitted = true;
                }
                else if (request.error() == render::ERenderUploadSubmitError::PAYLOAD_INVALID ||
                    request.error() == render::ERenderUploadSubmitError::STOPPING)
                    failed = true;
                status = "Uploading material";
            }
            if (!adopted && mesh.isValid() && material.isValid())
            {
                auto* current = context.selection().resolve(scene);
                if (!current || !current->registry().valid(entity))
                {
                    failed = true;
                    status = "Resource result belongs to a closed scene/entity";
                    return;
                }
                auto& registry = current->registry();
                const auto* visual = registry.try_get<simulation::ecs::Mesh3D>(entity);
                if (!visual || !acceptsResource(scene, entity, mesh_source, material_source, serial,
                    context.selection().current().scene, entity, visual->value.mesh, visual->value.material,
                    current_serial))
                {
                    failed = true;
                    status = "Resource result superseded";
                    return;
                }
                registry.emplace_or_replace<lux::scene::ResolvedMeshResources>(entity,
                    lux::scene::ResolvedMeshResources{mesh_source, material_source, mesh, material});
                adopted = true;
                status = "Ready";
            }
        }

        bool settled() const noexcept
        {
            return mesh_read->done() && material_read->done() && !mesh_upload.valid() &&
                !material_upload.valid() && !forward_compile.valid() && !gbuffer_compile.valid();
        }

        void release(lux::scene::RenderRuntimeLease& runtime)
        {
            if (released)
                return;
            const auto mesh_ops = runtime.features().ops<render::MeshStackOperationIds>("StandardMeshStack");
            const auto material_ops = runtime.features().ops<render::MaterialOperationIds>("StandardMaterial");
            if (mesh.isValid())
                render::MeshStackControlClient{runtime.control(), mesh_ops}.destroyMesh({mesh});
            if (material.isValid())
                render::MaterialControlClient{runtime.control(), material_ops}.destroyMaterial({material});
            if (forward.isValid())
                runtime.control().destroyShader(forward);
            if (gbuffer.isValid())
                runtime.control().destroyShader(gbuffer);
            released = true;
        }
    };
}
