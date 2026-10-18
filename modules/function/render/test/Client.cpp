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
#include <lux/engine/render/features/MeshFeature.hpp>
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
        typename C::Response::value_type call(
            ipc::IpcRequester& client,
            std::uint64_t& next,
            CommandId id,
            typename C::Request input
        )
        {
            const RequestId request{next++};
            ipc::IpcMessage message;
            message.emplace<typename C::Request>(std::move(input));
            check(bool(client.trySend(commandTag(id), request.value, message)));
            const auto deadline = Clock::now() + 20s;
            for (;;)
            {
                std::uint64_t correlation = 0;
                const auto status = client.tryReceive(correlation, message);
                check(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
                if (status == ipc::EIpcReceive::RECEIVED)
                {
                    check(correlation == request.value);
                    auto& response = message.get<typename C::Response>();
                    if (!response)
                    {
                        const auto error = response.error();
                        std::fprintf(
                            stderr,
                            "Command %.*s failed error=%llu\n",
                            int(C::name.size()),
                            C::name.data(),
                            static_cast<unsigned long long>(error.type)
                        );
                        std::abort();
                    }
                    return std::move(*response);
                }
                std::this_thread::sleep_for(1ms); // Application scheduling, not queue latency.
            }
        }

        template <RenderCommand C>
        typename C::Response::value_type coldCall(
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
            VStorageOwner owner = {}
        )
        {
            const auto directory = coldCall<ListStorages>(client, commands, next, {});
            const auto found = std::ranges::find(directory.storages, name.name(), &StorageInfo::name);
            check(found != directory.storages.end());
            return coldCall<PrepareStorage>(
                client,
                commands,
                next,
                {{std::move(owner), found->id, std::move(key)}, {}}
            );
        }

        FeatureId feature(ipc::IpcRequester& client, const CommandList& commands, std::uint64_t& next, FeatureName name)
        {
            const auto directory = coldCall<ListFeatures>(client, commands, next, {});
            const auto found = std::ranges::find(directory.features, name.name(), &FeatureInfo::name);
            check(found != directory.features.end());
            return found->id;
        }

        ConfigValue materialProgram(const char* path)
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
            program.texture_count = 1;
            program.defaults = {{1, 1, 1, 1}};
            return take(ConfigValue::make(std::move(program)));
        }

        TextureWrite textureInput(StorageId storage, unsigned green)
        {
            auto owner = std::make_shared<std::vector<std::byte>>(16 * 16 * 4);
            for (std::size_t i = 0; i < owner->size(); i += 4)
            {
                (*owner)[i] = std::byte(255 - green);
                (*owner)[i + 1] = std::byte(green);
                (*owner)[i + 3] = std::byte{255};
            }
            rdesc::TextureInfo info{};
            info.width = info.height = 16;
            info.channel = 4;
            info.mip_count = 1;
            info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
            info.color_space = rdesc::ETextureColorSpace::DATA;
            info.mip_ranges[0] = {0, owner->size(), 16, 16};
            return {{storage, {}}, 0, info, take(RenderPayload::fromVector(owner, 0, owner->size()))};
        }

        struct Assets final
        {
            MeshPublished mesh;
            MaterialPublished material;
            TexturePublished texture;
            MeshDescription description;
            ConfigValue program;
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
                const auto set_texture = command<SetTexture>(commands);
                const auto set_material = command<SetMaterial>(commands);
                const std::array<float, 20> vertices{-.22f, -.2f, .3f, 0,   0, .22f, -.2f, .3f, 1,   0,
                                                     0,     .23f, .3f, .5f, 1, 0,    0,    .8f, .5f, .5f};
                const std::array<std::uint32_t, 12> indices{0, 1, 2, 1, 0, 3, 2, 1, 3, 0, 2, 3};
                auto owner = std::make_shared<std::vector<std::byte>>(sizeof(vertices) + sizeof(indices));
                std::memcpy(owner->data(), vertices.data(), sizeof(vertices));
                std::memcpy(owner->data() + sizeof(vertices), indices.data(), sizeof(indices));
                assets.description = {
                    20,
                    4,
                    sizeof(vertices),
                    4,
                    12,
                    {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT}, {1, 12, rdesc::ETextureFormat::RG32_SFLOAT}},
                    {{0, 12, 0}}
                };
                assets.mesh = coldCall<SetMesh>(
                    client,
                    commands,
                    next,
                    {{mesh_storage, {}}, 0, assets.description, take(RenderPayload::fromVector(owner, 0, owner->size()))
                    }
                );
                owner.reset();
                assets.texture = call<SetTexture>(client, next, set_texture, textureInput(texture_storage, 0));
                assets.program = materialProgram(cooked_fragment);
                MaterialWrite material{{material_storage, {}}, 0, assets.program};
                material.parameters = assets.program.borrow<MaterialProgram>()->defaults;
                material.textures = {assets.texture.texture};
                assets.material = call<SetMaterial>(client, next, set_material, material);
                loaded.count_down();

                replace_texture.wait();
                auto changed = textureInput(texture_storage, 255);
                changed.texture = assets.texture.texture;
                changed.expected_revision = assets.texture.revision;
                assets.texture = call<SetTexture>(client, next, set_texture, std::move(changed));
                texture_ready.count_down();
                publish_material.wait();
                material.material = assets.material.material;
                material.expected_revision = assets.material.revision;
                material.textures = {assets.texture.texture};
                assets.material = call<SetMaterial>(client, next, set_material, std::move(material));
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
        check(assets.mesh.revision == 1 && assets.material.revision == 1 && assets.texture.revision == 1);
        const auto scene = coldCall<CreateScene>(main, main_commands, main_next, {}).scene;
        const auto instance_storage =
            prepare(main, main_commands, main_next, kMeshInstanceStorageType, "instances", scene);
        MeshInstanceDescription description{assets.mesh.mesh, assets.material.material};
        description.transform[12] = -.45f;
        auto left = coldCall<CreateMeshInstance>(main, main_commands, main_next, {instance_storage, description});
        description.transform[12] = .45f;
        auto right = coldCall<CreateMeshInstance>(main, main_commands, main_next, {instance_storage, description});
        const auto a = coldCall<QueryMeshInstance>(main, main_commands, main_next, left.instance);
        const auto b = coldCall<QueryMeshInstance>(main, main_commands, main_next, right.instance);
        check(a.description.mesh == b.description.mesh && a.description.material == b.description.material);

        MeshViewParameters first_camera, second_camera;
        second_camera.projection[0] = .5f;
        second_camera.projection[12] = .2f;
        const auto first_view = coldCall<CreateView>(main, main_commands, main_next, {scene}).view;
        const auto second_view = coldCall<CreateView>(main, main_commands, main_next, {scene}).view;
        MeshFeatureConfig config;
        config.target = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        config.mesh = {assets.mesh.mesh, assets.description};
        config.material = {assets.material.material, assets.program};
        config.instances = {2, MeshInstanceDrawGroup{assets.mesh.mesh, assets.material.material}};
        config.textures = {{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
        const auto basic = feature(main, main_commands, main_next, BasicMeshFeature::name);
        const auto depth_feature = feature(main, main_commands, main_next, DepthOnlyFeature::name);
        const auto first_effect = coldCall<CreateEffect>(
                                      main,
                                      main_commands,
                                      main_next,
                                      {config.target, {{basic, "main", take(ConfigValue::make(config))}}}
        ).effect;
        const auto second_effect = coldCall<CreateEffect>(
                                       main,
                                       main_commands,
                                       main_next,
                                       {config.target, {{depth_feature, "main", take(ConfigValue::make(config))}}}
        ).effect;
        const auto first_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const auto second_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const AttachmentRequest first{
            scene,
            first_view,
            first_target,
            first_effect,
            RenderPayload::copyOf(meshViewParameterBytes(first_camera))
        };
        const AttachmentRequest second{
            scene,
            second_view,
            second_target,
            second_effect,
            RenderPayload::copyOf(meshViewParameterBytes(second_camera))
        };
        const auto first_bindings = coldCall<Attach>(main, main_commands, main_next, first);
        const auto second_bindings = coldCall<Attach>(main, main_commands, main_next, second);
        for (const auto* bindings : {&first_bindings, &second_bindings})
        {
            check(std::ranges::any_of(
                bindings->storages,
                [&](const auto& binding) { return binding.storage == assets.mesh.mesh.storage; }
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
        auto matrix = a.description.transform;
        matrix[12] = -.2f;
        batch.updates.push_back({left.instance, left.revision, matrix});
        auto applied =
            call<UpdateTransforms>(main, main_next, command<UpdateTransforms>(main_commands), std::move(batch));
        check(applied.sequence == 7 && applied.changed == 1);
        left = coldCall<QueryMeshInstance>(main, main_commands, main_next, left.instance).published;
        check(
            coldCall<QueryMeshInstance>(main, main_commands, main_next, left.instance).description.transform[12] == -.2f
        );
        await_frame();

        // Handler selection is Server-global and observed by the already bound Asset Client.
        const auto set_texture = command<SetTexture>(main_commands);
        const auto choices = coldCall<ListCommandBindings>(main, main_commands, main_next, set_texture);
        const auto alternate = std::ranges::find(choices.handlers, "example.TextureB", &HandlerInfo::name);
        check(alternate != choices.handlers.end() && choices.selected != alternate->id);
        (void)coldCall<SelectCommandBinding>(main, main_commands, main_next, {set_texture, alternate->id});
        replace_texture.count_down();
        texture_ready.wait();
        check(assets.texture.revision == 2);
        await_frame(); // Logical Material references follow the current Texture version.
        publish_material.count_down();
        material_ready.wait();
        await_frame();
        check(coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs == original_compiles);
        check(coldCall<QueryMesh>(main, main_commands, main_next, assets.mesh.mesh).published.revision == 1);

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
