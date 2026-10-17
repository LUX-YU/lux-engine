#include "Sample.hpp"
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/features/MeshFeature.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <latch>
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
            if (value) return;
            std::fprintf(stderr, "3D sample assertion at %s:%u\n", location.file_name(), location.line());
            std::abort();
        }

        template <class T> T take(RenderResult<T> result)
        {
            if (!result)
            {
                std::fprintf(stderr, "Client Render error %llu\n", static_cast<unsigned long long>(result.error().type));
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

        template <RenderCommand C> typename C::Response call(
            ipc::IpcRequester& client, std::uint64_t& next, CommandId id, typename C::Request input
        )
        {
            const RequestId request{next++};
            ipc::IpcMessage message;
            message.emplace<typename C::Request>(std::move(input));
            message.setMetadata(RequestHeader{request, id});
            check(bool(client.trySend(message)));
            const auto deadline = Clock::now() + 20s;
            for (;;)
            {
                const auto status = client.tryReceive(message);
                check(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
                if (status == ipc::EIpcReceive::RECEIVED)
                {
                    const auto header = message.metadata<ReplyHeader>();
                    check(header.request == request);
                    if (header.outcome != ECommandOutcome::SUCCESS)
                    {
                        const auto error = message.get<RenderError>();
                        std::fprintf(stderr, "Command %.*s failed error=%llu\n", int(C::name.size()), C::name.data(),
                            static_cast<unsigned long long>(error.type));
                        std::abort();
                    }
                    return std::move(message.get<typename C::Response>());
                }
                std::this_thread::sleep_for(1ms); // Application scheduling, not queue latency.
            }
        }

        template <RenderCommand C> typename C::Response coldCall(
            ipc::IpcRequester& client, const CommandList& directory, std::uint64_t& next, typename C::Request input
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

        StorageId prepare(ipc::IpcRequester& client, const CommandList& commands, std::uint64_t& next,
            StorageNameView name, std::string key, VStorageOwner owner = {})
        {
            const auto directory = coldCall<ListStorages>(client, commands, next, {});
            const auto found = std::ranges::find(directory.storages, name.name(), &StorageInfo::name);
            check(found != directory.storages.end());
            return coldCall<PrepareStorage>(client, commands, next,
                {{std::move(owner), found->id, std::move(key)}, {}});
        }

        FeatureId feature(ipc::IpcRequester& client, const CommandList& commands,
            std::uint64_t& next, FeatureName name)
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
            program.fragment = {rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(size) / 4)};
            input.seekg(0);
            check(bool(input.read(reinterpret_cast<char*>(program.fragment.words.data()), size)));
            program.parameter_count = program.texture_count = 1;
            program.defaults[0] = {1, 1, 1, 1};
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

        void pixel(const RenderPayload& image, unsigned x, unsigned y, std::array<unsigned, 4> expected)
        {
            const auto bytes = image.bytes().view();
            check(bytes.size() == 64 * 64 * 4);
            for (unsigned c = 0; c < 4; ++c)
                check(std::abs(int(std::to_integer<unsigned>(bytes[(y * 64 + x) * 4 + c])) - int(expected[c])) <= 1);
        }

        void depth(const RenderPayload& image, unsigned x, unsigned y, float expected)
        {
            const auto bytes = image.bytes().view();
            check(bytes.size() == 64 * 64 * 4);
            float value;
            std::memcpy(&value, bytes.data() + (y * 64 + x) * 4, sizeof(value));
            check(std::abs(value - expected) < .00001f);
        }
    }

    lux::render::RenderPayload runClients(std::string_view channel, CommandId list_commands, const char* cooked_fragment)
    {
        auto main = connect(channel);
        std::uint64_t main_next = 1;
        const auto main_commands = call<ListCommands>(main, main_next, list_commands, {});
        Assets assets;
        std::latch loaded(1), replace_texture(1), texture_ready(1), publish_material(1), material_ready(1), release(1);
        // One long-lived application Asset thread/Client. No per-request thread, Future or extra Render queue.
        std::thread asset_thread([&] {
            auto client = connect(channel);
            std::uint64_t next = 1;
            const auto commands = call<ListCommands>(client, next, list_commands, {});
            const auto mesh_storage = prepare(client, commands, next, kMeshStorageType, "meshes");
            const auto material_storage = prepare(client, commands, next, kMaterialStorageType, "materials");
            const auto texture_storage = prepare(client, commands, next, kTextureStorageType, "textures");
            const auto set_texture = command<SetTexture>(commands);
            const auto set_material = command<SetMaterial>(commands);
            const std::array<float, 20> vertices{
                -.22f, -.2f, .3f, 0, 0, .22f, -.2f, .3f, 1, 0,
                0, .23f, .3f, .5f, 1, 0, 0, .8f, .5f, .5f
            };
            const std::array<std::uint32_t, 12> indices{0, 1, 2, 1, 0, 3, 2, 1, 3, 0, 2, 3};
            auto owner = std::make_shared<std::vector<std::byte>>(sizeof(vertices) + sizeof(indices));
            std::memcpy(owner->data(), vertices.data(), sizeof(vertices));
            std::memcpy(owner->data() + sizeof(vertices), indices.data(), sizeof(indices));
            assets.description = {20, 4, sizeof(vertices), 4, 12,
                {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT}, {1, 12, rdesc::ETextureFormat::RG32_SFLOAT}}, {{0, 12, 0}}};
            assets.mesh = coldCall<SetMesh>(client, commands, next, {{mesh_storage, {}}, 0, assets.description,
                take(RenderPayload::fromVector(owner, 0, owner->size()))});
            owner.reset();
            assets.texture = call<SetTexture>(client, next, set_texture, textureInput(texture_storage, 0));
            assets.program = materialProgram(cooked_fragment);
            MaterialWrite material{{material_storage, {}}, 0, assets.program};
            material.parameters = assets.program.borrow<MaterialProgram>()->defaults;
            material.textures[0] = assets.texture;
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
            material.textures[0] = assets.texture;
            assets.material = call<SetMaterial>(client, next, set_material, std::move(material));
            material_ready.count_down();
            release.wait();
            (void)coldCall<ReleaseMaterial>(client, commands, next, assets.material);
            (void)coldCall<ReleaseTexture>(client, commands, next, assets.texture);
            (void)coldCall<ReleaseMesh>(client, commands, next, assets.mesh);
            for (const auto id : {material_storage, texture_storage, mesh_storage})
                (void)coldCall<ReleasePreparedStorage>(client, commands, next, id);
        });

        loaded.wait();
        check(assets.mesh.revision == 1 && assets.material.revision == 1 && assets.texture.revision == 1);
        const auto scene = coldCall<CreateScene>(main, main_commands, main_next, {}).scene;
        const auto instance_storage = prepare(main, main_commands, main_next, kInstanceStorageType, "instances", scene);
        InstanceDescription description{assets.mesh.mesh, assets.material.material};
        description.transform[12] = -.45f;
        auto left = coldCall<CreateInstance>(main, main_commands, main_next, {instance_storage, description});
        description.transform[12] = .45f;
        auto right = coldCall<CreateInstance>(main, main_commands, main_next, {instance_storage, description});
        const auto a = coldCall<QueryInstance>(main, main_commands, main_next, left.instance);
        const auto b = coldCall<QueryInstance>(main, main_commands, main_next, right.instance);
        check(a.description.mesh == b.description.mesh && a.description.material == b.description.material);

        CameraState first_camera, second_camera;
        second_camera.projection[0] = .5f;
        second_camera.projection[12] = .2f;
        const auto first_view = coldCall<CreateView>(main, main_commands, main_next, {scene, first_camera}).view;
        const auto second_view = coldCall<CreateView>(main, main_commands, main_next, {scene, second_camera}).view;
        MeshFeatureConfig config;
        config.target = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        config.mesh = {assets.mesh.mesh, assets.description};
        config.material = {assets.material.material, assets.program};
        config.instances = {2, InstanceDrawGroup{assets.mesh.mesh, assets.material.material}};
        config.textures = {{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
        config.readback = true;
        const auto basic = feature(main, main_commands, main_next, BasicMeshFeature::name);
        const auto depth_feature = feature(main, main_commands, main_next, DepthOnlyFeature::name);
        const auto first_effect = coldCall<CreateEffect>(main, main_commands, main_next,
            {config.target, {{basic, "main", take(ConfigValue::make(config))}}}).effect;
        const auto second_effect = coldCall<CreateEffect>(main, main_commands, main_next,
            {config.target, {{depth_feature, "main", take(ConfigValue::make(config))}}}).effect;
        const auto first_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const auto second_target = coldCall<CreateTarget>(main, main_commands, main_next, {config.target}).target;
        const AttachmentRequest first{scene, first_view, first_target, first_effect};
        const AttachmentRequest second{scene, second_view, second_target, second_effect};
        const auto first_bindings = coldCall<Attach>(main, main_commands, main_next, first);
        const auto second_bindings = coldCall<Attach>(main, main_commands, main_next, second);
        for (const auto* bindings : {&first_bindings, &second_bindings})
        {
            check(std::ranges::any_of(bindings->storages, [&](const auto& binding) {
                return binding.storage == assets.mesh.mesh.storage;
            }));
            check(std::ranges::any_of(bindings->storages, [&](const auto& binding) {
                return binding.storage == instance_storage;
            }));
        }
        (void)coldCall<ReleasePreparedStorage>(main, main_commands, main_next, instance_storage); // Attach leases now retain it.
        const auto read = command<Readback>(main_commands);
        const auto read_color = [&] { return call<Readback>(main, main_next, read, {first, {}}).pixels; };
        auto color = read_color();
        pixel(color, 17, 30, {255, 0, 0, 255});
        pixel(color, 46, 30, {255, 0, 0, 255});
        const auto depth_key = featureKey(DepthOnlyFeature::name, "main", "depth.pixels");
        auto depths = call<Readback>(main, main_next, read, {second, depth_key}).pixels;
        depth(depths, 31, 30, .3f);
        depth(depths, 46, 30, .3f);
        depth(depths, 17, 30, 1);
        const auto original_compiles = coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs;

        // A typed batch uses the real Scene records. Duplicate IDs are Last-Wins; input ownership transfers.
        TransformBatch batch{instance_storage, 7};
        batch.updates.reserve(8);
        auto matrix = a.description.transform;
        matrix[12] = -.3f;
        batch.updates.push_back({left.instance, left.revision, matrix});
        matrix[12] = -.2f;
        batch.updates.push_back({left.instance, left.revision, matrix});
        auto applied = call<UpdateTransforms>(main, main_next, command<UpdateTransforms>(main_commands), std::move(batch));
        check(applied.sequence == 7 && applied.changed == 1);
        left = coldCall<QueryInstance>(main, main_commands, main_next, left.instance).published;
        const auto transform_deadline = Clock::now() + 10s;
        for (;;)
        {
            color = read_color();
            const auto bytes = color.bytes().view();
            if (bytes[(30 * 64 + 25) * 4] == std::byte{255} && bytes[(30 * 64 + 17) * 4] == std::byte{0}) break;
            check(Clock::now() < transform_deadline);
        }
        pixel(color, 25, 30, {255, 0, 0, 255});

        // Handler selection is Server-global and observed by the already bound Asset Client.
        const auto set_texture = command<SetTexture>(main_commands);
        const auto choices = coldCall<ListCommandBindings>(main, main_commands, main_next, set_texture);
        const auto alternate = std::ranges::find(choices.handlers, "example.TextureB", &HandlerInfo::name);
        check(alternate != choices.handlers.end() && choices.selected != alternate->id);
        (void)coldCall<SelectCommandBinding>(main, main_commands, main_next, {set_texture, alternate->id});
        replace_texture.count_down();
        texture_ready.wait();
        check(assets.texture.revision == 2);
        pixel(read_color(), 25, 30, {255, 0, 0, 255}); // The Material still pins its original Texture version.
        publish_material.count_down();
        material_ready.wait();
        const auto deadline = Clock::now() + 10s;
        for (;;)
        {
            color = read_color();
            if (color.bytes().view()[(30 * 64 + 25) * 4 + 1] == std::byte{255}) break;
            check(Clock::now() < deadline);
        }
        pixel(color, 46, 30, {0, 255, 0, 255});
        check(coldCall<QueryRenderStats>(main, main_commands, main_next, {}).compiled_graphs == original_compiles);
        check(coldCall<QueryMesh>(main, main_commands, main_next, assets.mesh.mesh).published.revision == 1);

        (void)coldCall<Detach>(main, main_commands, main_next, second);
        (void)coldCall<ReleaseInstance>(main, main_commands, main_next, right);
        const auto removal_deadline = Clock::now() + 10s;
        for (;;)
        {
            color = read_color();
            if (color.bytes().view()[(30 * 64 + 46) * 4 + 1] == std::byte{0}) break;
            check(Clock::now() < removal_deadline);
        }
        pixel(color, 25, 30, {0, 255, 0, 255});
        pixel(color, 46, 30, {0, 0, 0, 255});
        (void)coldCall<ReleaseInstance>(main, main_commands, main_next, left);
        (void)coldCall<Detach>(main, main_commands, main_next, first);
        (void)coldCall<RemoveEffect>(main, main_commands, main_next, {first_effect});
        (void)coldCall<RemoveEffect>(main, main_commands, main_next, {second_effect});
        (void)coldCall<RemoveScene>(main, main_commands, main_next, {scene});
        release.count_down();
        asset_thread.join();
        return color; // Real returned output ownership may outlive both Clients and the Server.
    }
}
