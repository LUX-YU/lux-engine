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
#include <lux/engine/render/features/HzbFeature.hpp>
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
        typename C::Response frameCall(
            ipc::IpcRequester& client,
            std::uint64_t& next,
            CommandId id,
            typename C::Request input,
            CommandId frame_id
        )
        {
            FrameCommand command{id, RequestId{next++}, {}};
            command.request.emplace<typename C::Request>(std::move(input));
            const FrameCommandGroup group{std::span(&command, 1)};
            const RequestId frame_request{next++};
            ipc::IpcMessage message;
            message.emplace<FrameSubmission>(FrameSubmission{frame_request.value, std::span(&group, 1)});
            check(bool(client.trySend(commandTag(frame_id), frame_request.value, message)));
            typename C::Response result{};
            bool received_result = false;
            bool received_frame = false;
            auto& notifications = ipc::IpcCore::instance();
            const auto deadline = Clock::now() + 20s;
            while (!received_result || !received_frame)
            {
                const auto observed = notifications.workEpoch();
                std::uint64_t correlation = 0;
                const auto status = client.tryReceive(correlation, message);
                check(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
                if (status != ipc::EIpcReceive::RECEIVED)
                {
                    notifications.waitUntil(observed, deadline);
                    continue;
                }
                if (correlation == command.request_id.value)
                {
                    result = std::move(message.get<typename C::Response>());
                    check(result.error.type == 0);
                    received_result = true;
                }
                else
                {
                    check(correlation == frame_request.value);
                    const auto& completed = message.get<FrameCompletion>();
                    check(completed.sequence == frame_request.value && completed.error.type == 0);
                    received_frame = true;
                }
                message.reset();
            }
            return result;
        }

        template <RenderCommand C>
        typename C::Response coldCall(
            ipc::IpcRequester& client,
            const CommandList& directory,
            std::uint64_t& next,
            typename C::Request input
        )
        {
            if constexpr (C::frame_scoped)
            {
                return frameCall<C>(
                    client,
                    next,
                    command<C>(directory),
                    std::move(input),
                    command<SubmitFrame>(directory)
                );
            }
            else
            {
                return call<C>(client, next, command<C>(directory), std::move(input));
            }
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

        rdesc::MaterialProgramSet materialProgram(const char* path)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            check(bool(input));
            const auto size = input.tellg();
            check(size > 0 && size % 4 == 0);
            rdesc::MaterialProgramSet program;
            program.programs.push_back({rdesc::EMaterialPurpose::FORWARD, "lux.material.forward.linear.v1", {}});
            auto& artifact = program.programs.back().shader;
            artifact.stages.push_back(
                {rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(size) / 4)}
            );
            input.seekg(0);
            check(bool(input.read(reinterpret_cast<char*>(artifact.stages.front().words.data()), size)));
            std::ifstream metadata(std::string(path) + ".meta", std::ios::binary | std::ios::ate);
            const auto metadata_size = metadata.tellg();
            check(metadata && metadata_size > 0);
            std::vector<std::byte> encoded(static_cast<std::size_t>(metadata_size));
            metadata.seekg(0);
            check(bool(metadata.read(reinterpret_cast<char*>(encoded.data()), metadata_size)));
            check(rdesc::ShaderInfo::deserialize(encoded, artifact.interface));
            program.texture_count = 1;
            program.parameter_defaults.resize(1);
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
            TextureId old_texture;
            StorageId mesh_storage;
            StorageId texture_storage;
            StorageId material_storage;
            MeshDescription description;
            rdesc::MaterialProgramSet program;
        };

    } // namespace

    void runClients(
        ipc::IpcRequester& main,
        std::string_view channel,
        CommandId list_commands,
        const char* cooked_fragment
    )
    {
        std::uint64_t main_next = 1;
        const auto main_commands = call<ListCommands>(main, main_next, list_commands, {});
        Assets assets;
        std::latch loaded(1), replace_texture(1), texture_ready(1), release(1);
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
                assets.mesh_storage = mesh_storage;
                assets.texture_storage = texture_storage;
                assets.material_storage = material_storage;
                const auto set_texture = command<CreateTexture>(commands);
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
                loaded.count_down();

                replace_texture.wait();
                auto changed = textureInput(texture_storage, 255, bytes);
                assets.old_texture = assets.texture;
                assets.texture = call<CreateTexture>(client, next, set_texture, std::move(changed)).texture;
                texture_ready.count_down();
                release.wait();
            }
        );

        loaded.wait();
        assets.material = coldCall<CreateMaterial>(
                              main,
                              main_commands,
                              main_next,
                              {assets.material_storage, assets.program, {{1, 1, 1, 1}}, {assets.texture}}
        ).material;
        check(!assets.mesh.isNull() && !assets.material.isNull() && !assets.texture.isNull());
        const auto scene = coldCall<CreateScene>(main, main_commands, main_next, {}).scene;
        const auto instance_storage =
            prepare(main, main_commands, main_next, kMeshInstanceStorageType, "instances", scene);
        MeshInstanceDescription description{assets.mesh, assets.material};
        description.transform[12] = -.45f;
        std::vector<MeshInstanceDescription> instance_descriptions{description};
        description.transform[12] = .45f;
        instance_descriptions.push_back(description);
        auto created =
            coldCall<CreateMeshInstances>(main, main_commands, main_next, {instance_storage, instance_descriptions});
        instance_descriptions.clear(); // Response ended the source borrow; capacity remains available to the Client.
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
        config.mesh.geometry = assets.description;
        config.mesh.program = assets.program;
        config.mesh.group = {assets.mesh, assets.material};
        config.mesh.textures = {{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
        const auto basic = feature(main, main_commands, main_next, MaterialMeshRasterDescriptor::name);
        const auto depth_feature = feature(main, main_commands, main_next, MeshDepthRasterDescriptor::name);
        CreateEffectRequest first_request{config.target}, second_request{config.target};
        first_request.definition.features.push_back({basic, "main", take(ConfigValue::make(config))});
        auto depth_config = config;
        depth_config.mesh.program = {};
        depth_config.mesh.textures.clear();
        second_request.definition.features.push_back({depth_feature, "main", take(ConfigValue::make(depth_config))});
        const auto hzb = feature(main, main_commands, main_next, HzbFeatureDescriptor::name);
        HzbFeatureConfig hzb_config{{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT}, 6};
        hzb_config.depth.extent.source = EImageExtentSource::TARGET;
        second_request.definition.features.push_back({hzb, "depth pyramid", take(ConfigValue::make(hzb_config))});
        second_request.definition.connections.push_back({{0, "depth"}, {1, "depth"}});
        first_request.definition.outputs.push_back(
            {{0, "color"}, GraphOutputRequirements{config.target.format, config.target.samples}}
        );
        second_request.definition.outputs.push_back(
            {{0, "color"}, GraphOutputRequirements{config.target.format, config.target.samples}}
        );
        second_request.definition.outputs.push_back({{0, "depth"}, {}});

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
            const auto result = coldCall<SubmitFrame>(main, main_commands, main_next, {main_next, {}});
            check(result.outcome == EFrameOutcome::SUBMITTED);
        };
        await_frame();
        const auto original_compiles = coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs;

        // The application submits an already coalesced sparse update. The Scene retains the authoritative record.
        std::vector<lux::cxx::SlotKey<MeshInstanceTag>> changed_instances;
        std::vector<TransformMatrix> changed_transforms;
        changed_instances.reserve(8);
        changed_transforms.reserve(8);
        auto matrix = description.transform;
        matrix[12] = -.2f;
        changed_instances.push_back(left.local);
        changed_transforms.push_back(matrix);
        auto applied = coldCall<UpdateTransforms>(
            main,
            main_commands,
            main_next,
            {instance_storage, 7, changed_instances, changed_transforms}
        );
        changed_instances.clear();
        changed_transforms.clear();
        check(applied.sequence == 7 && applied.changed == 1);
        await_frame();

        // Two Client input carriers can be in flight while GPU FIF is independently occupied.
        // Each packet borrows its own matrix until both its child and frame terminal arrive.
        const std::array keys{left.local};
        std::array<TransformMatrix, 2> matrices{matrix, matrix};
        matrices[0][12] = -.1f;
        matrices[1][12] = -.3f;
        std::array<FrameCommand, 2> frame_commands;
        std::array<FrameCommandGroup, 2> groups;
        std::array<RequestId, 2> frame_requests;
        for (std::size_t i = 0; i < frame_commands.size(); ++i)
        {
            auto& frame_command = frame_commands[i];
            frame_command.command = command<UpdateTransforms>(main_commands);
            frame_command.request_id = {main_next++};
            frame_command.request.emplace<TransformBatch>(
                TransformBatch{instance_storage, 8 + i, keys, std::span(&matrices[i], 1)}
            );
            groups[i] = {std::span(&frame_command, 1)};
            frame_requests[i] = {main_next++};
            ipc::IpcMessage packet;
            packet.emplace<FrameSubmission>(FrameSubmission{frame_requests[i].value, std::span(&groups[i], 1)});
            check(bool(main.trySend(commandTag(command<SubmitFrame>(main_commands)), frame_requests[i].value, packet)));
        }
        const auto pair_deadline = Clock::now() + 20s;
        unsigned frame_completions = 0;
        unsigned transform_completions = 0;
        while (frame_completions != 2 || transform_completions != 2)
        {
            auto& notifications = ipc::IpcCore::instance();
            const auto observed = notifications.workEpoch();
            ipc::IpcMessage response;
            std::uint64_t correlation = 0;
            const auto status = main.tryReceive(correlation, response);
            check(status != ipc::EIpcReceive::CLOSED && Clock::now() < pair_deadline);
            if (status != ipc::EIpcReceive::RECEIVED)
            {
                notifications.waitUntil(observed, pair_deadline);
                continue;
            }
            const bool is_frame = correlation == frame_requests[0].value || correlation == frame_requests[1].value;
            if (is_frame)
            {
                const auto& result = response.get<FrameCompletion>();
                check(result.error.type == 0 && result.outcome == EFrameOutcome::SUBMITTED);
                check(result.submitted_batches != 0);
                ++frame_completions;
            }
            else
            {
                check(
                    correlation == frame_commands[0].request_id.value ||
                    correlation == frame_commands[1].request_id.value
                );
                const auto& result = response.get<UpdateTransforms::Response>();
                check(result.error.type == 0 && result.changed == 1 && (result.sequence == 8 || result.sequence == 9));
                ++transform_completions;
            }
        }

        // The installed-consumer extension verifies A/B routing with distinct Handler implementations.
        // This scene keeps exercising the real built-in Texture upload and explicit Material update.
        replace_texture.count_down();
        texture_ready.wait();
        check(!assets.texture.isNull());
        await_frame(); // Material still uses the old Texture until the explicit update below.
        (void
        )coldCall<UpdateMaterial>(main, main_commands, main_next, {assets.material, {{1, 1, 1, 1}}, {assets.texture}});
        (void)coldCall<ReleaseTexture>(main, main_commands, main_next, assets.old_texture);
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
        (void)coldCall<ReleaseMaterial>(main, main_commands, main_next, assets.material);
        (void)coldCall<ReleaseTexture>(main, main_commands, main_next, assets.texture);
        (void)coldCall<ReleaseMesh>(main, main_commands, main_next, assets.mesh);
        for (const auto id : {assets.material_storage, assets.texture_storage, assets.mesh_storage})
        {
            (void)coldCall<RemoveStorage>(main, main_commands, main_next, id);
        }
        release.count_down();
        asset_thread.join();
    }
} // namespace mesh_sample
