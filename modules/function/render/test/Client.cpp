#include "Sample.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <exec/start_detached.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <latch>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/commands/MaterialCommands.hpp>
#include <lux/engine/render/commands/Mesh3dCommands.hpp>
#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/features/HzbFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/frame/FrameStorage.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <map>
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

        // Standalone host: each calling thread drives its own Client. No Engine or ECS is involved.
        template <RenderCommand C, class Sender>
        typename C::Response receive(RenderClient& client, Sender sender)
        {
            typename C::Response response;
            bool completed = false;
            exec::start_detached(stdexec::then(std::move(sender), [&](auto value) noexcept
            {
                response = std::move(value);
                completed = true;
            }));
            const auto deadline = Clock::now() + 20s;
            auto& notifications = ipc::IpcCore::instance();
            while (!completed)
            {
                const auto observed = notifications.workEpoch();
                const auto consumed = take(client.dispatchResponses(128));
                check(Clock::now() < deadline);
                if (!completed && consumed < 128) notifications.waitUntil(observed, deadline);
            }
            if (response.error.type != 0)
            {
                std::fprintf(stderr, "Command %.*s failed error=%llu\n",
                    int(C::name.size()), C::name.data(), static_cast<unsigned long long>(response.error.type));
                std::abort();
            }
            return response;
        }

        template <RenderCommand C>
        typename C::Response call(RenderClient& client, CommandId id, typename C::Request input)
        {
            return receive<C>(client, client.request<C>(id, input));
        }

        template <RenderCommand C>
        typename C::Response call(RenderClient& client, typename C::Request input)
        {
            return receive<C>(client, client.request<C>(input));
        }

        struct ClientRoutes final
        {
            EffectId effect;
            CapabilityDescription capability;
            CapabilityImplementationId implementation;
        };

        ClientRoutes resourceRoutes(RenderClient& client)
        {
            const auto choices =
                call<ListCapabilities>(client, {std::string(kRasterSurfaceCapability)});
            check(!choices.implementations.empty());
            const auto& found = choices.implementations.front();
            CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
            request.composition.contributions.push_back(
                {"mesh",
                 found.capability.type, found.implementation_id,
                 SerializedRenderConfiguration{found.configuration.schema, found.configuration.version, {}}}
            );
            request.composition.outputs.push_back({"mesh", "albedo_metallic"});
            const auto effect = call<CreateEffect>(client, std::move(request)).effect;
            return {effect, found.capability, found.implementation_id};
        }

        template <CapabilityCommand C>
        CapabilityImplementationId installCommand(
            const ClientRoutes& routes,
            VRenderScope scope = {},
            FrameDataBinding<TransformData>* transforms = nullptr
        )
        {
            check(std::ranges::any_of(
                routes.capability.commands,
                [](const auto* entry) { return entry->name.name() == C::name; }
            ));
            if (transforms)
            {
                const auto& types = routes.capability.per_frame_data;
                const auto input = std::ranges::find(types, lux::cxx::make_basic_type_info<TransformData>());
                check(input != types.end());
                *transforms = {{routes.effect, 0, static_cast<std::uint32_t>(input - types.begin()), scope}};
            }
            return routes.implementation;
        }

        rdesc::MaterialProgramSet materialProgram(const char* path)
        {
            rdesc::MaterialProgramSet program;
            for (const auto purpose : {rdesc::EMaterialPurpose::FORWARD, rdesc::EMaterialPurpose::SHADOW})
            {
                const bool shadow = purpose == rdesc::EMaterialPurpose::SHADOW;
                const auto source = std::string(path) + (shadow ? ".shadow" : "");
                std::ifstream input(source, std::ios::binary | std::ios::ate);
                check(bool(input));
                const auto size = input.tellg();
                check(size > 0 && size % 4 == 0);
                program.programs.push_back(
                    {purpose, shadow ? "lux.material.coverage.forward-z.v1" : "lux.material.forward.linear.v1", {}}
                );
                auto& artifact = program.programs.back().shader;
                artifact.stages.push_back(
                    {rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(size) / 4)}
                );
                input.seekg(0);
                check(bool(input.read(reinterpret_cast<char*>(artifact.stages.front().words.data()), size)));
                std::ifstream metadata(source + ".meta", std::ios::binary | std::ios::ate);
                const auto metadata_size = metadata.tellg();
                check(metadata && metadata_size > 0);
                std::vector<std::byte> encoded(static_cast<std::size_t>(metadata_size));
                metadata.seekg(0);
                check(bool(metadata.read(reinterpret_cast<char*>(encoded.data()), metadata_size)));
                check(rdesc::ShaderInfo::deserialize(encoded, artifact.interface));
                for (const auto& set : artifact.interface.sets)
                {
                    for (const auto& binding : set.bindings)
                    {
                        if (binding.name == rdesc::kMaterialTableName)
                        {
                            program.parameters = binding.block;
                        }
                    }
                }
            }
            program.texture_count = 1;
            program.parameter_defaults.resize(1);
            return program;
        }

        ImageCommands::Upload::Request
        textureInput(CapabilityImplementationId storage, unsigned green, std::vector<std::byte>& bytes)
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
            return {storage, {info, bytes}};
        }

        struct Assets final
        {
            MeshId mesh;
            MaterialId material;
            TextureId texture;
            TextureId old_texture;
            CapabilityImplementationId mesh_input;
            CapabilityImplementationId texture_input;
            CapabilityImplementationId material_input;
            MeshDescription description;
            rdesc::MaterialProgramSet program;
        };

    } // namespace

    void runClients(
        ipc::IpcRequester requester,
        std::string_view channel,
        const char* cooked_fragment,
        std::array<CapabilityImplementationId, 2> implementations
    )
    {
        auto main_owner = take(RenderClient::create(std::move(requester)));
        auto& main = *main_owner;
        auto main_routes = resourceRoutes(main);
        const std::array command_names{
            CommandName{MaterialCommands::Create::name},
            CommandName{Mesh3dCommands::CreateInstances::name},
            CommandName{MaterialCommands::Update::name},
            CommandName{ImageCommands::Release::name},
            CommandName{Mesh3dCommands::ReleaseInstance::name},
            CommandName{MaterialCommands::Release::name},
            CommandName{Mesh3dCommands::ReleaseMesh::name}
        };
        const auto commands = call<QueryCommandIds>(main, command_names).ids;
        const auto create_material = commands[0];
        const auto create_instances = commands[1];
        const auto update_material = commands[2];
        const auto release_texture = commands[3];
        const auto release_instance = commands[4];
        const auto release_material = commands[5];
        const auto release_mesh = commands[6];
        std::uint64_t next_frame = 1;
        Assets assets;
        std::latch loaded(1), replace_texture(1), texture_ready(1), release(1);
        // One long-lived application Asset thread/Client. No per-request thread, Future or extra Render queue.
        std::thread asset_thread(
            [&]
            {
                auto endpoint = ipc::IpcCore::instance().createRequester(channel);
                check(bool(endpoint));
                auto client_owner = take(RenderClient::create(std::move(*endpoint)));
                auto& client = *client_owner;
                auto routes = resourceRoutes(client);
                const auto mesh_input = installCommand<Mesh3dCommands::UploadMesh>(routes);
                const auto material_input = installCommand<MaterialCommands::Create>(routes);
                const auto texture_input = installCommand<ImageCommands::Upload>(routes);
                assets.mesh_input = mesh_input;
                assets.texture_input = texture_input;
                assets.material_input = material_input;
                const std::array upload_commands{
                    CommandName{ImageCommands::Upload::name}, CommandName{Mesh3dCommands::UploadMesh::name}
                };
                const auto uploads = call<QueryCommandIds>(client, upload_commands).ids;
                const auto set_texture = uploads[0];
                const auto upload_mesh = uploads[1];
                const std::array<float, 20> vertices{-.22f, -.2f, .3f, 0,   0, .22f, -.2f, .3f, 1,   0,
                                                     0,     .23f, .3f, .5f, 1, 0,    0,    .8f, .5f, .5f};
                const std::array<std::uint32_t, 12> indices{0, 1, 2, 1, 0, 3, 2, 1, 3, 0, 2, 3};
                rdesc::Mesh mesh_data;
                mesh_data.indices.assign(indices.begin(), indices.end());
                mesh_data.vertices.resize(4);
                for (std::size_t i = 0; i < mesh_data.vertices.size(); ++i)
                {
                    auto& vertex = mesh_data.vertices[i];
                    vertex.position = Eigen::Map<const Eigen::Vector3f>(vertices.data() + i * 5);
                    vertex.uv = Eigen::Map<const Eigen::Vector2f>(vertices.data() + i * 5 + 3);
                    vertex.normal = {0, 0, 1};
                    vertex.tangent = {1, 0, 0};
                    vertex.bitangent = {0, 1, 0};
                }
                auto mesh_description = describeIndexedMesh(mesh_data);
                check(bool(mesh_description));
                assets.description = std::move(*mesh_description);
                assets.mesh =
                    call<Mesh3dCommands::UploadMesh>(client, upload_mesh, {mesh_input, &mesh_data}).mesh;
                mesh_data = {}; // Response ended all CPU reads.
                std::vector<std::byte> bytes;
                assets.texture =
                    call<ImageCommands::Upload>(client, set_texture, textureInput(texture_input, 0, bytes))
                        .texture;
                assets.program = materialProgram(cooked_fragment);
                loaded.count_down();

                replace_texture.wait();
                auto changed = textureInput(texture_input, 255, bytes);
                assets.old_texture = assets.texture;
                assets.texture = call<ImageCommands::Upload>(client, set_texture, std::move(changed)).texture;
                texture_ready.count_down();
                release.wait();
                (void)call<RemoveEffect>(client, {routes.effect});
            }
        );

        loaded.wait();
        assets.material = call<MaterialCommands::Create>(
                              main, create_material,
                              {assets.material_input, {assets.program, {{1, 1, 1, 1}}, {assets.texture}}}
        ).material;
        check(!assets.mesh.isNull() && !assets.material.isNull() && !assets.texture.isNull());
        const auto scene = call<CreateScene>(main, {}).scene;
        FrameDataBinding<TransformData> transforms;
        const auto instance_input =
            installCommand<Mesh3dCommands::CreateInstance>(main_routes, scene, &transforms);
        MeshInstanceData description{assets.mesh, assets.material};
        description.render_enabled = true;
        description.transform[12] = -.45f;
        std::vector<MeshInstanceData> instance_descriptions{description};
        description.transform[12] = .45f;
        instance_descriptions.push_back(description);
        auto created = call<Mesh3dCommands::CreateInstances>(
            main, create_instances,
            {installCommand<Mesh3dCommands::CreateInstances>(main_routes, scene), scene,
             instance_descriptions}
        );
        instance_descriptions.clear(); // Response ended the source borrow; capacity remains available to the Client.
        check(created.instances.size() == 2);
        const auto left = created.instances[0];
        const auto right = created.instances[1];
        check(left != right); // The Client keeps the creation description; no resource query mirror is needed.

        MeshViewParameters first_camera, second_camera;
        second_camera.projection[0] = .5f;
        second_camera.projection[12] = .2f;
        const auto first_view = call<CreateView>(main, {scene}).view;
        const auto second_view = call<CreateView>(main, {scene}).view;
        MeshFeatureConfig config;
        const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        config.output = {target.format, target.samples, target.kind, target.sampled};
        CreateEffectRequest first_request{config.output}, second_request{config.output};
        first_request.composition.contributions.push_back(
            {"probe", lux::cxx::make_basic_type_info<RenderProbeCapability>(), implementations[0],
             take(ConfigValue::make(config))}
        );
        second_request.composition.contributions.push_back(
            {"probe", lux::cxx::make_basic_type_info<RenderProbeCapability>(), implementations[1],
             take(ConfigValue::make(config))}
        );
        first_request.composition.outputs.push_back({"probe", "color"});
        second_request.composition.outputs = {{"probe", "color"}, {"probe", "depth"}};
        const auto first_effect =
            call<CreateEffect>(main, std::move(first_request)).effect;
        const auto second_effect =
            call<CreateEffect>(main, std::move(second_request)).effect;
        const auto first_target = call<CreateTarget>(main, {target}).target;
        const auto second_target = call<CreateTarget>(main, {target}).target;
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
        const auto first_bindings = call<Attach>(main, first);
        const auto second_bindings = call<Attach>(main, second);
        check(!first_bindings.attachment.isNull() && !second_bindings.attachment.isNull());
        // Reinstalling at the same business scope returns identical input addresses, and both
        // attachments consume the already-uploaded resources in the frames below.
        check(
            installCommand<Mesh3dCommands::CreateInstance>(main_routes, scene) ==
            instance_input
        );
        check(installCommand<Mesh3dCommands::UploadMesh>(main_routes) == assets.mesh_input);
        const auto await_frame = [&]
        {
            const auto result = call<SubmitFrame>(main, {next_frame++, {}});
            check(result.outcome == EFrameOutcome::SUBMITTED);
        };
        await_frame();
        const auto original_compiles = call<QueryRenderStats>(main, {}).compiled_graphs;

        // The application submits an already coalesced sparse update. The Scene retains the authoritative record.
        std::vector<MeshInstanceId> changed_instances;
        std::vector<TransformMatrix> changed_transforms;
        changed_instances.reserve(8);
        changed_transforms.reserve(8);
        auto matrix = description.transform;
        matrix[12] = -.2f;
        changed_instances.push_back(left);
        changed_transforms.push_back(matrix);
        FrameStorage update_frame;
        FrameWriter update_writer{update_frame};
        check(bool(update_writer.write(
            transforms,
            {7,
             take(update_writer.copy<MeshInstanceId>(changed_instances)),
             take(update_writer.copy<TransformMatrix>(changed_transforms))}
        )));
        // The packet owns its snapshot; clearing the application arrays cannot change the pending update.
        changed_instances.clear();
        changed_transforms.clear();
        auto applied = call<SubmitFrame>(main, update_frame.seal(next_frame++));
        check(applied.error.type == 0 && applied.outcome == EFrameOutcome::SUBMITTED);
        update_frame.reset();
        await_frame();

        // Two isolated CPU packets can be in flight independently of the occupied GPU FIF slices.
        // Each produces one terminal reply; no child command correlation exists.
        const std::array keys{left};
        std::array<TransformMatrix, 2> matrices{matrix, matrix};
        matrices[0][12] = -.1f;
        matrices[1][12] = -.3f;
        std::array<FrameStorage, 2> frame_storage;
        std::array<std::uint64_t, 2> sequences;
        unsigned frame_completions = 0;
        std::array<bool, 2> completed{};
        for (std::size_t i = 0; i < frame_storage.size(); ++i)
        {
            FrameWriter writer{frame_storage[i]};
            check(bool(writer.write(
                transforms,
                {8 + i,
                 take(writer.copy<MeshInstanceId>(keys)),
                 take(writer.copy<TransformMatrix>(std::span(&matrices[i], 1)))}
            )));
            sequences[i] = next_frame++;
            exec::start_detached(stdexec::then(
                main.request<SubmitFrame>(frame_storage[i].seal(sequences[i])),
                [&, i](FrameCompletion result) noexcept
                {
                    check(!completed[i]);
                    check(result.error.type == 0 && result.outcome == EFrameOutcome::SUBMITTED);
                    check(result.sequence == sequences[i] && result.submitted_batches != 0);
                    completed[i] = true;
                    frame_storage[i].reset();
                    ++frame_completions;
                }
            ));
        }
        const auto pair_deadline = Clock::now() + 20s;
        while (frame_completions != 2)
        {
            auto& notifications = ipc::IpcCore::instance();
            const auto observed = notifications.workEpoch();
            const auto consumed = take(main.dispatchResponses(128));
            check(Clock::now() < pair_deadline);
            if (frame_completions != 2 && consumed < 128) notifications.waitUntil(observed, pair_deadline);
        }

        // The installed-consumer extension verifies A/B routing with distinct Handler implementations.
        // This scene keeps exercising the real built-in Texture upload and explicit Material update.
        replace_texture.count_down();
        texture_ready.wait();
        check(!assets.texture.isNull());
        await_frame(); // Material still uses the old Texture until the explicit update below.
        static_cast<void>(call<MaterialCommands::Update>(
            main, update_material,
            {{assets.material, {{1, 1, 1, 1}}, {assets.texture}}}
        ));
        (void)call<ImageCommands::Release>(
            main, release_texture,
            {assets.old_texture}
        );
        await_frame();
        check(call<QueryRenderStats>(main, {}).compiled_graphs == original_compiles);

        (void)call<Detach>(main, second_bindings.attachment);
        (void)call<Mesh3dCommands::ReleaseInstance>(
            main, release_instance,
            {right}
        );
        await_frame();
        (void)call<Mesh3dCommands::ReleaseInstance>(
            main, release_instance,
            {left}
        );
        (void)call<Detach>(main, first_bindings.attachment);
        (void)call<RemoveEffect>(main, {first_effect});
        (void)call<RemoveEffect>(main, {second_effect});
        (void)call<RemoveScene>(main, {scene});
        (void)call<MaterialCommands::Release>(
            main, release_material,
            {assets.material}
        );
        (void)call<ImageCommands::Release>(
            main, release_texture,
            {assets.texture}
        );
        (void)call<Mesh3dCommands::ReleaseMesh>(
            main, release_mesh,
            {assets.mesh}
        );
        (void)call<RemoveEffect>(main, {main_routes.effect});
        release.count_down();
        asset_thread.join();
    }
} // namespace mesh_sample
