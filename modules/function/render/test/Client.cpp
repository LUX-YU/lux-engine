#include "Sample.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <latch>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>
#include <source_location>
#include <thread>

#if defined(VK_VERSION_1_0) || defined(VMA_VERSION) || defined(_WINDOWS_)
#error Client sample must not transitively include Native or platform implementation headers
#endif

namespace mesh_sample
{
    namespace
    {
        using namespace lux::render;
        namespace rdesc = lux::rdesc;
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        void check(bool value, std::source_location location = std::source_location::current())
        {
            if (value)
            {
                return;
            }
            std::fprintf(stderr, "3D sample assertion at %s:%u\n", location.file_name(), location.line());
            std::abort();
        }

        template <class T> T take(RenderResult<T> result)
        {
            if (!result)
            {
                std::fprintf(
                    stderr,
                    "Client Render error %llu\n",
                    static_cast<unsigned long long>(result.error().type)
                );
                std::abort();
            }
            return std::move(*result);
        }

        namespace ipc = lux::platform::ipc;

        // Application-owned correlation and catalog; the library has no Client Pending table.
        template <RenderCommand C> CommandId command(const CommandList& directory)
        {
            const auto found = std::ranges::find(directory.commands, C::name, &CommandInfo::name);
            check(found != directory.commands.end());
            return found->id;
        }

        template <RenderCommand C>
        typename C::Response call(
            ipc::IpcRequester& client,
            std::uint64_t& next,
            CommandId id,
            typename C::Request input
        )
        {
            const RequestId request{next++};
            ipc::IpcMessage message;
            message.emplace<typename C::Request>(std::move(input));
            const auto sent = client.trySend(commandTag(id), request.value, message);
            if (!sent)
            {
                std::fprintf(
                    stderr,
                    "Command %.*s request=%llu send failed code=%u\n",
                    int(C::name.size()),
                    C::name.data(),
                    static_cast<unsigned long long>(request.value),
                    static_cast<unsigned>(sent.error())
                );
            }
            check(bool(sent));
            const auto deadline = Clock::now() + 20s;
            for (;;)
            {
                std::uint64_t correlation = 0;
                const auto status = client.tryReceive(correlation, message);
                if (status == ipc::EIpcReceive::CLOSED || Clock::now() >= deadline)
                {
                    std::fprintf(
                        stderr,
                        "Command %.*s request=%llu receive=%u timed out or closed\n",
                        int(C::name.size()),
                        C::name.data(),
                        static_cast<unsigned long long>(request.value),
                        static_cast<unsigned>(status)
                    );
                }
                check(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
                if (status == ipc::EIpcReceive::RECEIVED)
                {
                    check(correlation == request.value);
                    auto& response = message.get<typename C::Response>();
                    if (response.error.type != 0)
                    {
                        const auto error = response.error;
                        std::fprintf(
                            stderr,
                            "Command %.*s failed error=%llu\n",
                            int(C::name.size()),
                            C::name.data(),
                            static_cast<unsigned long long>(error.type)
                        );
                        std::abort();
                    }
                    return std::move(response);
                }
                std::this_thread::sleep_for(1ms); // Application scheduling, not queue latency.
            }
        }

        template <RenderCommand C>
        typename C::Response coldCall(
            ipc::IpcRequester& client,
            const CommandList& directory,
            std::uint64_t& next,
            typename C::Request input
        )
        {
            return call<C>(client, next, command<C>(directory), std::move(input));
        }

        ipc::IpcRequester connect(std::string_view channel)
        {
            auto created = ipc::IpcCore::instance().createRequester(channel);
            check(bool(created));
            return std::move(*created);
        }

        StorageId prepare(
            ipc::IpcRequester& client,
            const CommandList& commands,
            std::uint64_t& next,
            StorageNameView name,
            std::string key,
            VStorageLocation location = {}
        )
        {
            const auto directory = coldCall<ListStorages>(client, commands, next, {});
            const auto found = std::ranges::find(directory.storages, name.name(), &StorageInfo::name);
            check(found != directory.storages.end());
            check(!found->description.empty());
            return coldCall<PrepareStorage>(
                       client,
                       commands,
                       next,
                       {{std::move(location), found->id, std::move(key)}, {}}
            ).storage;
        }

        FeatureId feature(ipc::IpcRequester& client, const CommandList& commands, std::uint64_t& next, FeatureName name)
        {
            const auto directory = coldCall<ListFeatures>(client, commands, next, {});
            const auto found = std::ranges::find(directory.features, name.name(), &FeatureInfo::name);
            check(found != directory.features.end());
            check(!found->description.empty());
            return found->id;
        }

        MaterialProgram materialProgram(const char* path)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            check(bool(input));
            const auto size = input.tellg();
            check(size > 0 && size % 4 == 0);
            MaterialProgram program;
            program
                .fragment = {rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(size) / 4)};
            input.seekg(0);
            check(bool(input.read(reinterpret_cast<char*>(program.fragment.words.data()), size)));
            std::ifstream metadata(std::string(path) + ".meta", std::ios::binary | std::ios::ate);
            const auto metadata_size = metadata.tellg();
            check(metadata && metadata_size > 0);
            std::vector<std::byte> encoded(static_cast<std::size_t>(metadata_size));
            metadata.seekg(0);
            check(bool(metadata.read(reinterpret_cast<char*>(encoded.data()), metadata_size)));
            check(rdesc::ShaderInfo::deserialize(encoded, program.interface));
            program.texture_count = 1;
            program.parameter_count = 1;
            return program;
        }

        TextureWrite textureInput(StorageId storage, unsigned green, std::vector<std::byte>& bytes)
        {
            bytes.assign(16 * 16 * 4, std::byte{});
            for (std::size_t i = 0; i < bytes.size(); i += 4)
            {
                bytes[i] = std::byte(255 - green);
                bytes[i + 1] = std::byte(green);
                bytes[i + 3] = std::byte{255};
            }
            rdesc::TextureInfo info{};
            info.width = info.height = 16;
            info.channel = 4;
            info.mip_count = 1;
            info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
            info.color_space = rdesc::ETextureColorSpace::DATA;
            info.mip_ranges[0] = {0, bytes.size(), 16, 16};
            return {storage, info, bytes};
        }

        struct Assets final
        {
            MeshId mesh;
            MaterialId material;
            TextureId texture;
            MeshDescription description;
            MaterialProgram program;
        };

    } // namespace

    void runClients(std::string_view channel, CommandId list_commands, const char* cooked_fragment)
    {
        auto main = connect(channel);
        std::uint64_t main_next = 1;
        const auto main_commands = call<ListCommands>(main, main_next, list_commands, {});
        Assets assets;
        std::latch loaded(1), replace_texture(1), texture_ready(1), publish_material(1), material_ready(1), release(1);
        // One long-lived application Asset thread/Client. No per-request thread, Future or extra Render queue.
        std::thread asset_thread(
            [&]
            {
                auto client = connect(channel);
                std::uint64_t next = 1;
                const auto commands = call<ListCommands>(client, next, list_commands, {});
                const auto mesh_storage = prepare(client, commands, next, kMeshStorageType, "meshes");
                const auto material_storage = prepare(client, commands, next, kMaterialStorageType, "materials");
                const auto texture_storage = prepare(client, commands, next, kTextureStorageType, "textures");
                const auto set_texture = command<CreateTexture>(commands);
                const auto set_material = command<CreateMaterial>(commands);
                const std::array<float, 20> vertices{-.22f, -.2f, .3f, 0,   0, .22f, -.2f, .3f, 1,   0,
                                                     0,     .23f, .3f, .5f, 1, 0,    0,    .8f, .5f, .5f};
                const std::array<std::uint32_t, 12> indices{0, 1, 2, 1, 0, 3, 2, 1, 3, 0, 2, 3};
                std::vector<std::byte> bytes(sizeof(vertices) + sizeof(indices));
                std::memcpy(bytes.data(), vertices.data(), sizeof(vertices));
                std::memcpy(bytes.data() + sizeof(vertices), indices.data(), sizeof(indices));
                assets.description = {
                    20,
                    4,
                    sizeof(vertices),
                    4,
                    12,
                    {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT}, {1, 12, rdesc::ETextureFormat::RG32_SFLOAT}},
                    {{0, 12, 0}}
                };
                assets.mesh =
                    coldCall<CreateMesh>(client, commands, next, {mesh_storage, assets.description, bytes}).mesh;
                bytes.clear(); // CreateMesh Response ended the source borrow.
                assets.texture =
                    call<CreateTexture>(client, next, set_texture, textureInput(texture_storage, 0, bytes)).texture;
                assets.program = materialProgram(cooked_fragment);
                MaterialWrite material{material_storage, assets.program};
                material.parameters = {{1, 1, 1, 1}};
                material.textures = {assets.texture};
                assets.material = call<CreateMaterial>(client, next, set_material, material).material;
                loaded.count_down();

                replace_texture.wait();
                auto changed = textureInput(texture_storage, 255, bytes);
                const auto old_texture = assets.texture;
                assets.texture = call<CreateTexture>(client, next, set_texture, std::move(changed)).texture;
                texture_ready.count_down();
                publish_material.wait();
                (void)coldCall<UpdateMaterial>(
                    client,
                    commands,
                    next,
                    {assets.material, material.parameters, {assets.texture}}
                );
                // The Client explicitly replaces all references before releasing the previous resource.
                (void)coldCall<ReleaseTexture>(client, commands, next, old_texture);
                material_ready.count_down();
                release.wait();
                (void)coldCall<ReleaseMaterial>(client, commands, next, assets.material);
                (void)coldCall<ReleaseTexture>(client, commands, next, assets.texture);
                (void)coldCall<ReleaseMesh>(client, commands, next, assets.mesh);
                for (const auto id : {material_storage, texture_storage, mesh_storage})
                {
                    (void)coldCall<RemoveStorage>(client, commands, next, id);
                }
            }
        );

        loaded.wait();
        check(!assets.mesh.isNull() && !assets.material.isNull() && !assets.texture.isNull());
        const auto scene = coldCall<CreateScene>(main, main_commands, main_next, {}).scene;
        const auto instance_storage =
            prepare(main, main_commands, main_next, kMeshInstanceStorageType, "instances", scene);
        MeshInstanceDescription description{assets.mesh, assets.material};
        description.transform[12] = -.45f;
        MeshInstancesCreate instances{instance_storage, {description}};
        description.transform[12] = .45f;
        instances.descriptions.push_back(description);
        auto created = coldCall<CreateMeshInstances>(main, main_commands, main_next, std::move(instances));
        check(created.instances.size() == 2);
        const auto left = created.instances[0];
        const auto right = created.instances[1];
        check(left != right); // The Client keeps the creation description; no resource query mirror is needed.

        MeshViewParameters first_camera, second_camera;
        second_camera.projection[0] = .5f;
        second_camera.projection[12] = .2f;
        const auto first_view = coldCall<CreateView>(main, main_commands, main_next, {scene}).view;
        const auto second_view = coldCall<CreateView>(main, main_commands, main_next, {scene}).view;
        MeshFeatureConfig config;
        config.target = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        config.geometry = assets.description;
        config.program = assets.program;
        config.group = {assets.mesh, assets.material};
        config.textures = {{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
        const auto basic = feature(main, main_commands, main_next, MaterialMeshRasterDescriptor::name);
        const auto depth_feature = feature(main, main_commands, main_next, MeshDepthRasterDescriptor::name);
        CreateEffectRequest first_request{config.target}, second_request{config.target};
        first_request.features.push_back({basic, "main", take(ConfigValue::make(config))});
        second_request.features.push_back({depth_feature, "main", take(ConfigValue::make(config))});
        const auto first_effect =
            coldCall<CreateEffect>(main, main_commands, main_next, std::move(first_request)).effect;
        const auto second_effect =
            coldCall<CreateEffect>(main, main_commands, main_next, std::move(second_request)).effect;
        const auto first_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const auto second_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const AttachmentRequest first{
            scene,
            first_view,
            first_target,
            first_effect,
            {meshViewParameterBytes(first_camera).begin(), meshViewParameterBytes(first_camera).end()}
        };
        const AttachmentRequest second{
            scene,
            second_view,
            second_target,
            second_effect,
            {meshViewParameterBytes(second_camera).begin(), meshViewParameterBytes(second_camera).end()}
        };
        const auto first_bindings = coldCall<Attach>(main, main_commands, main_next, first);
        const auto second_bindings = coldCall<Attach>(main, main_commands, main_next, second);
        for (const auto* bindings : {&first_bindings, &second_bindings})
        {
            check(std::ranges::any_of(
                bindings->storages,
                [&](const auto& binding) { return binding.storage == assets.mesh.storage; }
            ));
            check(std::ranges::any_of(
                bindings->storages,
                [&](const auto& binding) { return binding.storage == instance_storage; }
            ));
        }
        const auto await_frame = [&]
        {
            const auto before = coldCall<QueryRenderStats>(main, main_commands, main_next, {}).submissions;
            const auto deadline = Clock::now() + 10s;
            while (coldCall<QueryRenderStats>(main, main_commands, main_next, {}).submissions <= before)
            {
                check(Clock::now() < deadline);
                std::this_thread::yield();
            }
        };
        await_frame();
        const auto original_compiles = coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs;

        // The application submits an already coalesced sparse update. The Scene retains the authoritative record.
        TransformBatch batch{instance_storage, 7};
        batch.updates.reserve(8);
        auto matrix = description.transform;
        matrix[12] = -.2f;
        batch.updates.push_back({left, matrix});
        auto applied =
            call<UpdateTransforms>(main, main_next, command<UpdateTransforms>(main_commands), std::move(batch));
        check(applied.sequence == 7 && applied.changed == 1);
        await_frame();

        // Handler selection is Server-global and observed by the already bound Asset Client.
        const auto set_texture = command<CreateTexture>(main_commands);
        const auto choices = coldCall<ListCommandBindings>(main, main_commands, main_next, set_texture);
        const auto alternate = std::ranges::find(choices.handlers, "B", &HandlerInfo::instance);
        check(alternate != choices.handlers.end() && choices.selected != alternate->id);
        (void)coldCall<SelectCommandBinding>(main, main_commands, main_next, {set_texture, alternate->id});
        check(coldCall<ListCommandBindings>(main, main_commands, main_next, set_texture).selected == alternate->id);
        replace_texture.count_down();
        texture_ready.wait();
        check(!assets.texture.isNull());
        await_frame(); // Material still uses the old Texture until the explicit update below.
        publish_material.count_down();
        material_ready.wait();
        await_frame();
        check(coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs == original_compiles);

        (void)coldCall<Detach>(main, main_commands, main_next, second_bindings.attachment);
        (void)coldCall<ReleaseMeshInstance>(main, main_commands, main_next, right);
        await_frame();
        (void)coldCall<ReleaseMeshInstance>(main, main_commands, main_next, left);
        (void)coldCall<Detach>(main, main_commands, main_next, first_bindings.attachment);
        (void)coldCall<RemoveEffect>(main, main_commands, main_next, {first_effect});
        (void)coldCall<RemoveEffect>(main, main_commands, main_next, {second_effect});
        (void)coldCall<RemoveScene>(main, main_commands, main_next, {scene});
        release.count_down();
        asset_thread.join();
    }
} // namespace mesh_sample
