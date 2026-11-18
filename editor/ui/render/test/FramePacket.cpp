#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/editor/ui/render/Ui.hpp>
#include <lux/editor/ui/render/Resource.hpp>
#include <lux/engine/render/resources/texture/Commands.hpp>
using namespace lux::editor::ui;
#include <optional>
#include <source_location>

#undef CHECK
#define CHECK(...) ::foundation_test::require(static_cast<bool>((__VA_ARGS__)), __LINE__)

namespace
{
    struct Delta final { int value = 0; };
    struct Stamp final { int value = 0; };
    struct Read final
    {
        static constexpr std::string_view name = "test.frame.read";
        using Data = int;
        struct Request final { ResourceInstanceId resource; };
        struct Response final { RenderError error{}; int sum = 0, stamp = 0, calls = 0; };
        static ResourceInstanceId resourceId(const Request& r) noexcept { return r.resource; }
    };
    template <int I> struct Inputs final
    {
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<Read>;
        using PerFrameData = std::tuple<Delta, Stamp>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{I == 0 ? "test.frame.a" : "test.frame.b"}, 0, 0, {}, "column probe",
            EResourceDomain::SCENE, resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
    struct FrameInputCapability final
    {
        using Resources = std::tuple<Inputs<0>, Inputs<1>, InstanceResource, LightResource>;
        using Commands = std::tuple<>;
        using PrerequisiteCapabilities = std::tuple<>;
    };

    struct Counter final
    {
        int sum = 0, stamp = 0, calls = 0;
        static inline std::atomic<bool> gate = false, entered = false;
        static RenderResult<std::unique_ptr<Counter>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
        { return std::make_unique<Counter>(); }
        static RenderResult<void>
        apply(Counter& value, std::span<const Delta> deltas, std::span<const Stamp> stamps) noexcept
        {
            if (std::ranges::any_of(deltas, [](auto d) { return d.value < 0; }))
                return lux::cxx::unexpected(RenderError{kStorageContract});
            if (gate.load()) { entered.store(true); gate.wait(true); }
            ++value.calls;
            for (auto d : deltas) value.sum += d.value;
            for (auto s : stamps) value.stamp = s.value;
            return {};
        }
        static void read(Counter& value, const Read::Request&, Read::Response& response) noexcept
        { response = {{}, value.sum, value.stamp, value.calls}; }
        template <int I> static ResourceRegistration factory() noexcept
        {
            static const std::array commands{CommandBinding::declare<Read, &read>()};
            return makeResourceRegistration<Inputs<I>, Counter, &create>(nullptr, nullptr, commands,
                FrameDataCall::declare<Inputs<I>, Counter, &apply>());
        }
    };
    struct Configuration final { bool green = false; bool operator==(const Configuration&) const noexcept = default; };
    struct ClearFeature final
    {
        using Config = Configuration;
        static constexpr FeatureName name{"test.frame.clear"};
        using Ports = ClearFeature;

        static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
        {
            GraphImageDesc image{32, 32, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
            image.extent.source = EImageExtentSource::TARGET;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
            return {};
        }

        static RenderResult<void> build(const void* value, std::string_view, FeatureBuildContext& context) noexcept
        {
            const auto green = static_cast<const Configuration*>(value)->green;
            const std::array rgba{green ? 0.F : 1.F, green ? 1.F : 0.F, 0.F, 1.F};
            auto pass = context.graph.addTransferPass("clear", TransferWork{
                {ClearImage{{context.resource("color")}, rgba}}
            });
            if (!pass) return lux::cxx::unexpected(pass.error());
            return context.publish("color", pass->written.front());
        }
    };
    template <class Response> struct Reply final
    {
        using receiver_concept = stdexec::receiver_t;
        std::optional<Response>* result;
        stdexec::inplace_stop_token stop;
        std::weak_ptr<const void> completion_code;
        const FrameData* packet = nullptr;

        auto get_env() const noexcept
        {
            return stdexec::env{
                stdexec::prop{stdexec::get_stop_token, stop},
                stdexec::prop{lux::execution::detail::getCompletionCode, completion_code.lock()}
            };
        }

        void set_value(Response value) && noexcept
        {
            if (packet)
            {
                CHECK(packet->writable() && !completion_code.expired());
            }
            result->emplace(std::move(value));
        }
    };
    template <class Predicate>
    void until(Predicate ready, std::source_location caller = std::source_location::current())
    {
        const auto deadline = Clock::now() + 20s;
        while (!ready())
        {
            if (Clock::now() >= deadline)
            {
                std::fprintf(stderr, "response or fixture gate timed out at line %u\n", caller.line());
                CHECK(false);
            }
            std::this_thread::yield();
        }
    }
    template <class C> typename C::Response request(
        RenderClient& client, CommandId id, typename C::Request input, bool success = true,
        std::source_location location = std::source_location::current()
    )
    {
        std::optional<typename C::Response> response;
        stdexec::inplace_stop_source stop;
        auto op = stdexec::connect(
            client.request<C>(id, input), Reply<typename C::Response>{&response, stop.get_token()}
        );
        stdexec::start(op);
        until([&] { take(client.dispatchResponses(64)); return response.has_value(); }, location);
        if (success && response->error.type)
            std::fprintf(
                stderr, "%.*s line=%u error=%llu args=%llu,%llu,%llu\n", int(C::name.size()), C::name.data(),
                location.line(), response->error.type, response->error.args[0], response->error.args[1],
                response->error.args[2]
            );
        if (success) CHECK(response->error.type == 0);
        return std::move(*response);
    }
}

int main(int argc, char** argv)
{
    auto execution_result = lux::execution::ExecutionRuntime::create({2, 128, {64}});
    CHECK(execution_result);
    auto execution = std::move(*execution_result);
    RenderServerConfig config; config.channel_name = "f2.frame.packet"; config.uploads.emplace();
    config.frames_in_flight = 3;
    const auto* native_validation = std::getenv("LUX_NATIVE_VALIDATION");
    config.validation = native_validation && std::string_view(native_validation) != "0";
    DeviceOptions options; options.texture_descriptors = true; options.multiple_queues = true;
    if (argc > 1 && std::string_view(argv[1]) == "d3d12") options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, *execution, options));
    take(registerUi(*server));
    take(server->registerResource<CaptureBuffer>());
    const auto capture_feature = take(server->registerFeature<CaptureFeature>());
    const auto capture_probe = registerCaptureProbe(*server, capture_feature);
    take(server->registerResource(Counter::factory<0>()));
    take(server->registerResource(Counter::factory<1>()));
    const auto feature = take(server->registerFeature<ClearFeature>());
    const auto implementation = registerRenderProbe<Configuration, FrameInputCapability>(*server, "test.frame.columns", {feature},
        probePorts({"color"}, {}),
        [](const EffectOutputDesc& output, const Configuration& config, std::span<const FeatureId> features)
            noexcept -> RenderResult<EffectDefinition>
        {
            EffectDefinition graph;
            graph.features.push_back({
                features.front(), "clear", take(ConfigValue::make(config))
            });
            graph.outputs.push_back({{0, "color"}, GraphOutputRequirements{output.format, 1}});
            return graph;
        });
    auto wire = ipc::IpcCore::instance().createRequester(config.channel_name); CHECK(wire);
    auto frame_wire = ipc::IpcCore::instance().createRequester(config.channel_name); CHECK(frame_wire);
    std::thread render([&] { CHECK(server->run()); });
    auto client = take(RenderClient::create(std::move(*frame_wire)));
    const auto commands = queryCommands(*wire);
    const auto command = [&](std::string_view name)
    {
        return std::ranges::find(commands.commands, name, &CommandInfo::name)->id;
    };
    const auto run_job = command(RequestRenderJob::name);
    const auto update = command(UpdateFrameData::name);
    const auto scene = request<CreateScene>(*client, command(CreateScene::name), {}).scene;
    const auto view = request<CreateView>(*client, command(CreateView::name), {scene}).view;
    const auto a = call<GetOrCreateResource>(*wire, commands, {scene, Inputs<0>::descriptor.type}).resource_id;
    const auto b = call<GetOrCreateResource>(*wire, commands, {scene, Inputs<1>::descriptor.type}).resource_id;
    const TargetDesc target{32, 32, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    auto recipe = probeRequest<Configuration, FrameInputCapability>(target, implementation, Configuration{}, {"color"});
    // Both selections require the same Resource declarations. Each input channel is present once.
    recipe.implementations.push_back({implementation.implementation, "same-resource"});
    const auto effect = request<CreateEffect>(*client, command(CreateEffect::name), std::move(recipe)).effect;
    const auto output = request<CreateTarget>(*client, command(CreateTarget::name), {target}).target;
    auto attachment = request<CreateRenderJob>(*client, command(CreateRenderJob::name), {
        scene, view, effect, EJobCadence::REQUESTED
    }).job;
    request<StartRenderJob>(*client, command(StartRenderJob::name), {attachment, output});
    const auto schema = call<QueryRenderJobInputLayout>(*wire, commands, attachment).layout;
    using Resources = std::tuple<Inputs<0>, Inputs<1>, InstanceResource, LightResource>;
    CHECK(schema.hasSameChannels(take(makeFrameLayout(std::type_identity<Resources>{}, 1))));
    std::array<FrameData, 2> packets{
        take(FrameData::create<Resources>(schema)), take(FrameData::create<Resources>(schema))
    };
    const auto light_resource = call<GetOrCreateResource>(*wire, commands, {scene, kLightResourceName}).resource_id;
    const std::array<VDirectLight, 1> initial_light{DirectionalLightGpu{}};
    const auto light = call<LightingCommands::CreateLights>(*wire, commands,
        {light_resource, {0, initial_light, {}}}).lights.front();
    auto& packet = packets[0];
    DirectionalLightGpu brighter; brighter.radiance = {4, 3, 2, 0};
    CHECK(packet.addData<LightResource>(DirectLightUpdate{light, brighter, {}}));
    auto deltas = take(packet.appendData<Inputs<0>, Delta>(3));
    for (auto& d : deltas) d.value = 2;
    CHECK(packet.addData<Inputs<0>>(Stamp{9}));
    CHECK(packet.addData<Inputs<1>>(Delta{50}));
    CHECK(packet.seal());
    CHECK(request<RequestRenderJob>(*client, run_job, {attachment, &packet}).job == attachment);
    CHECK(packet.writable());
    auto first = call<Read>(*wire, commands, {a}), second = call<Read>(*wire, commands, {b});
    CHECK(first.sum == 6 && first.stamp == 9 && first.calls == 1);
    CHECK(second.sum == 50 && second.calls == 1); // Shared Storage and element types remain distinct declarations.
    packet.reset(); CHECK(packet.seal());
    request<UpdateFrameData>(*client, update, {attachment, &packet});
    CHECK(call<QueryExecutionProgress>(*wire, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) == 1);
    CHECK(call<Read>(*wire, commands, {a}).calls == 1); // Empty columns are no update.
    packet.reset(); CHECK(packet.addData<Inputs<0>>(Delta{1})); CHECK(packet.seal());
    Counter::gate = true; Counter::entered = false;
    std::optional<JobSubmitted> terminal;
    stdexec::inplace_stop_source stop;
    RequestRenderJob::Request input{attachment, &packet};
    auto operation = stdexec::connect(
        client->request<RequestRenderJob>(input), Reply<JobSubmitted>{&terminal, stop.get_token()}
    );
    stdexec::start(operation);
    until([] { return Counter::entered.load(); });
    CHECK(!packet.appendData<Inputs<0>, Delta>(1));
    CHECK(packets[1].appendData<Inputs<0>, Delta>(4097));
    CHECK(request<RequestRenderJob>(*client, run_job, input, false).error.type == kFrameContract);
    CHECK(!packet.writable() && !terminal);
    auto& other = packets[1];
    other.reset();
    CHECK(other.seal());
    std::optional<AckResponse> update_terminal;
    UpdateFrameData::Request next_update{attachment, &other};
    auto update_operation = stdexec::connect(
        client->request<UpdateFrameData>(next_update), Reply<AckResponse>{&update_terminal, {}}
    );
    stdexec::start(update_operation);
    auto extra = take(FrameData::create<Resources>(schema));
    CHECK(extra.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &extra}, false).error.type == kRuntimeCapacity);
    extra.release();
    stop.request_stop();
    CHECK(!packet.reserve<Inputs<0>, Delta>(8192) && !other.writable());
    Counter::gate = false; Counter::gate.notify_all();
    until([&] { take(client->dispatchResponses(64)); return terminal.has_value() && update_terminal.has_value(); });
    CHECK(packet.writable() && terminal->error.type == 0);
    CHECK(other.writable() && update_terminal->error.type == 0);
    std::puts("FRAME DATA PASS: two Client borrows, terminal-only reuse and update without execution");
    CHECK(call<QueryRenderJobInputLayout>(*wire, commands, attachment).layout.hasSameIdentity(schema));
    auto wrong_layout = take(FrameLayout::create(schema.channels(), schema.revision()));
    CHECK(wrong_layout == schema && !wrong_layout.hasSameIdentity(schema));
    auto wrong_packet = take(FrameData::create<Resources>(wrong_layout)); CHECK(wrong_packet.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &wrong_packet}, false).error.type == kFrameContract);
    CHECK(wrong_packet.writable() && call<Read>(*wire, commands, {a}).sum == 7);
    packet.reset(); CHECK(packet.addData<InstanceResource>(TransformUpdate{})); CHECK(packet.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &packet}, false).error.type == kStorageIdentity);
    CHECK(packet.writable());
    packet.reset(); CHECK(packet.seal());
    request<UpdateFrameData>(*client, update, {attachment, &packet});
    packet.reset();
    CHECK(packet.addData<Inputs<0>>(Delta{11}));
    CHECK(packet.addData<Inputs<1>>(Delta{-1}));
    CHECK(packet.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &packet}, false).error.type == kStorageContract);
    CHECK(packet.writable());
    first = call<Read>(*wire, commands, {a});
    second = call<Read>(*wire, commands, {b});
    CHECK(first.sum == 18 && first.calls == 3);
    CHECK(second.sum == 50 && second.calls == 1);
    CameraInput camera;
    call<UpdateViewCamera>(*wire, commands, {{scene, view}, camera});
    camera.near_distance = -1;
    CHECK(call<UpdateViewCamera>(*wire, commands, {{scene, view}, camera}, false).error.type == kFrameContract);

    const auto device = request<QueryRenderDevice>(*client, command(QueryRenderDevice::name), {});
    CHECK(device.backend == options.backend && device.frames_in_flight == 3 && device.physical_queues > 0);

    const auto choices = call<ListCapabilities>(*wire, commands, {});
    const auto ui = std::ranges::find_if(choices.implementations, [](const auto& value)
        { return value.capability.type == lux::cxx::make_basic_type_info<UiCapability>(); });
    CHECK(ui != choices.implementations.end());
    const UiConfiguration ui_configuration{};
    auto saved_ui = take(encodeUiConfiguration(ui_configuration));
    const auto loaded_ui = take(decodeUiConfiguration(saved_ui.bytes));
    CHECK(loaded_ui == ui_configuration);
    saved_ui.bytes.push_back(std::byte{0});
    CHECK(!decodeUiConfiguration(saved_ui.bytes));
    auto wrong_index_type = loaded_ui;
    wrong_index_type.index_type = EIndexType::UINT32;
    CHECK(!makeUiRenderEffect({target.format}, *ui, wrong_index_type));
    CapturedPixels ui_pixels;
    const EffectOutputDesc ui_output{target.format, target.samples, target.kind, target.sampled};
    auto ui_request = take(makeUiRenderEffect(ui_output, *ui, loaded_ui));
    ui_request.implementations.push_back({capture_probe, "capture"});
    ui_request.definition.features.push_back({
        capture_feature, "capture/pixels", take(ConfigValue::make(CaptureConfig{&ui_pixels}))
    });
    ui_request.definition.connections.push_back({{0, "color"}, {1, "color"}});
    ui_request.definition.outputs.push_back({{1, "pixels"}, {}});
    const auto ui_effect = call<CreateEffect>(*wire, commands, std::move(ui_request)).effect;
    const auto ui_view = call<CreateView>(*wire, commands, {scene}).view;
    const auto ui_target = call<CreateTarget>(*wire, commands, {target}).target;
    const auto ui_attachment = startRenderJob(*wire, commands, scene, ui_view, ui_target, ui_effect);
    const auto ui_resource = call<QueryResource>(*wire, commands,
        {ViewRequest{scene, ui_view}, UiResource::descriptor.type}).resource_id;
    CHECK(!ui_resource.isNull());

    const auto texture_storage = call<GetOrCreateResource>(
        *wire, commands, {GlobalRenderScope{}, kTextureResourceName}
    ).resource_id;
    rdesc::TextureInfo texture_info{};
    texture_info.width = texture_info.height = texture_info.mip_count = 1;
    texture_info.channel = 4;
    texture_info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    texture_info.color_space = rdesc::ETextureColorSpace::DATA;
    texture_info.mip_ranges[0] = {0, 4, 1, 1};
    const std::array blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
    const auto texture = request<TextureCommands::Upload>(
        *client, command(TextureCommands::Upload::name), {texture_storage, {texture_info, blue}}
    ).texture;

    std::array images{ImguiImage{9, {}, output}, ImguiImage{10, texture, {}}};
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {32, 32};
    unsigned char* atlas = nullptr;
    int atlas_width = 0, atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
    CHECK(atlas && atlas_width > 0 && atlas_height > 0);
    DrawCapture capture;
    CHECK(!capture.capture(ImDrawData{}, images));
    ImGui::NewFrame();
    auto& list = *ImGui::GetBackgroundDrawList();
    list.AddImage(9, {0, 0}, {32, 32});
    list.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    list.AddImage(10, {16, 0}, {32, 32});
    ImGui::Render();
    const auto ui_command = command(SubmitDrawData::name);
    const auto draw = take(capture.capture(*ImGui::GetDrawData(), images));
    request<SubmitDrawData>(*client, ui_command, {ui_resource, draw});


    const std::array<ImDrawVert, 3> invalid_vertices{};
    const std::array<ImDrawIdx, 3> invalid_indices{0, 1, 3};
    ImDrawCmd invalid_draw;
    invalid_draw.ElemCount = 3;
    invalid_draw.TextureId = 9;
    invalid_draw.ClipRect = {0, 0, 32, 32};
    const ImguiDrawList invalid_list{invalid_vertices, invalid_indices, {&invalid_draw, 1}};
    const ImguiFrameData invalid_input{{&invalid_list, 1}, {0, 0}, {32, 32}, {1, 1}, images};
    CHECK(request<SubmitDrawData>(*client, ui_command, {ui_resource, invalid_input}, false).error.type == kFrameContract);
    invalid_draw.UserCallback = [](const ImDrawList*, const ImDrawCmd*) { CHECK(false); };
    CHECK(request<SubmitDrawData>(*client, ui_command, {ui_resource, invalid_input}, false).error.type == kFrameContract);
    packet.reset(); CHECK(packet.seal());
    CHECK(request<RequestRenderJob>(*client, run_job, {attachment, &packet}).job == attachment);
    CHECK(packet.writable());
    request<RequestRenderJob>(*client, run_job, {ui_attachment});
    CHECK(call<RemoveTarget>(*wire, commands, output, false).error.type == kRuntimeState);
    call<SetTargetActive>(*wire, commands, {output, false});
    request<RequestRenderJob>(*client, run_job, {ui_attachment}); // Uses the last accepted producer output.
    // Replace the sampled Target while the reader and both writers keep their original bindings.
    auto green_recipe = probeRequest<Configuration, FrameInputCapability>(target, implementation, {true}, {"color"});
    const auto green_effect = call<CreateEffect>(*wire, commands, std::move(green_recipe)).effect;
    const auto alternate = call<CreateTarget>(*wire, commands, {target});
    const auto green_writer = startRenderJob(*wire, commands, scene, ui_view, alternate.target, green_effect);
    const auto before_change = server->developmentStats();
    images.front().target = alternate.target;
    request<SubmitDrawData>(*client, ui_command, {ui_resource, draw});
    const auto missing = request<RequestRenderJob>(*client, run_job, {ui_attachment}, false);
    CHECK(missing.error.type == kRuntimeState);
    if (const auto after = server->developmentStats(); after && before_change)
        CHECK(after->input_rebuilds > before_change->input_rebuilds);
    const auto produced = request<RequestRenderJob>(*client, run_job, {green_writer});
    call<SetTargetActive>(*wire, commands, {alternate.target, false});
    const auto historical = request<RequestRenderJob>(*client, run_job, {ui_attachment});
    CHECK(historical.frame_index > produced.frame_index && historical.slot_index != produced.slot_index);

    auto resized_desc = target;
    resized_desc.width = 48;
    const auto resized = call<ResizeTarget>(*wire, commands, {alternate.target, resized_desc});
    CHECK(resized.epoch == alternate.epoch + 1);
    CHECK(request<RequestRenderJob>(*client, run_job, {ui_attachment}, false).error.type == kRuntimeState);
    CHECK(call<SetTargetActive>(*wire, commands, {alternate.target, true, alternate.epoch}, false).error.type ==
        kRuntimeState);
    call<SetTargetActive>(*wire, commands, {alternate.target, true, resized.epoch});
    request<RequestRenderJob>(*client, run_job, {green_writer});
    request<RequestRenderJob>(*client, run_job, {ui_attachment});

    request<SubmitDrawData>(*client, ui_command, {ui_resource, {}});
    request<RequestRenderJob>(*client, run_job, {ui_attachment}); // Empty snapshot replaces old geometry.

    auto second_reader_recipe = take(makeUiRenderEffect(ui_output, *ui, loaded_ui));
    const auto reader_effect = call<CreateEffect>(*wire, commands, std::move(second_reader_recipe)).effect;
    const auto reader_target = call<CreateTarget>(*wire, commands, {target}).target;
    const auto reader = startRenderJob(*wire, commands, scene, ui_view, reader_target, reader_effect);
    request<SubmitDrawData>(*client, ui_command, {ui_resource, draw});
    request<RequestRenderJob>(*client, run_job, {reader}); // A second Job can use the same producer's history.
    stopAndRemoveJob(*wire, commands, reader);
    call<RemoveTarget>(*wire, commands, reader_target);
    call<RemoveEffect>(*wire, commands, {reader_effect});

    images.front().target = ui_target;
    request<SubmitDrawData>(*client, ui_command, {ui_resource, draw});
    CHECK(request<RequestRenderJob>(*client, run_job, {ui_attachment}, false).error.type == kGraphDependencyCycle);
    request<SubmitDrawData>(*client, ui_command, {ui_resource, {}});
    ImGui::DestroyContext(context);
    call<SetTargetActive>(*wire, commands, {output, true});
    call<SetEffectParameters>(*wire, commands, {effect, {2.F}});
    CHECK(
        call<SetEffectParameters>(*wire, commands, {effect, {std::numeric_limits<float>::infinity()}}, false)
            .error.type == kFrameContract
    );
    stopAndRemoveJob(*wire, commands, ui_attachment);
    call<RemoveTarget>(*wire, commands, ui_target);
    stopAndRemoveJob(*wire, commands, green_writer);
    call<RemoveTarget>(*wire, commands, alternate.target);
    call<RemoveEffect>(*wire, commands, {green_effect});
    call<RemoveView>(*wire, commands, {scene, ui_view});
    until([&] { return ui_pixels.ready.load(std::memory_order_acquire) == 1; });
    CHECK(ui_pixels.frames.size() == 3);
    const std::array red{255u, 0u, 0u, 255u};
    const std::array blue_pixel{0u, 0u, 255u, 255u};
    const std::array green{0u, 255u, 0u, 255u};
    unsigned first_source = 0, second_source = 0, empty = 0;
    for (const auto& bytes : ui_pixels.frames)
    {
        const auto pixel = [&](unsigned x)
        {
            std::array<unsigned, 4> rgba{};
            for (std::size_t c = 0; c < rgba.size(); ++c)
                rgba[c] = std::to_integer<unsigned>(bytes[8 * ui_pixels.pitch + 4 * x + c]);
            return rgba;
        };
        const auto left = pixel(8), right = pixel(24);
        if (left == red || left == green)
        {
            CHECK(right == blue_pixel);
            if (left == red) ++first_source; else ++second_source;
        }
        else
        {
            CHECK(left == right && left[0] > 0 && left[0] < 50);
            CHECK(left[0] == left[1] && left[1] == left[2] && left[3] == 255);
            ++empty;
        }
    }
    CHECK(first_source == 1 && second_source == 1 && empty == 1);
    std::printf(
        "UI SAMPLED TARGET PASS: source swap, cycle=%llu, missing=%llu, epoch=%llu->%llu, historical slots=%u->%u\n",
        kGraphDependencyCycle, missing.error.type, static_cast<unsigned long long>(alternate.epoch),
        static_cast<unsigned long long>(resized.epoch), produced.slot_index, historical.slot_index
    );
    call<TextureCommands::Release>(*wire, commands, {texture});
    std::puts("UI PIXELS PASS: real ImGui capture, ordered texture/output draws, paused output and empty snapshot");
    call<RemoveEffect>(*wire, commands, {ui_effect});
    const auto second_output = call<CreateTarget>(*wire, commands, {target}).target;
    const auto second_attachment = startRenderJob(*wire, commands, scene, view, second_output, effect);
    const auto second_schema = call<QueryRenderJobInputLayout>(*wire, commands, second_attachment).layout;
    auto second_packet = take(FrameData::create<Resources>(second_schema));
    const auto old_attachment = attachment;
    stopAndRemoveJob(*wire, commands, attachment);
    attachment = startRenderJob(*wire, commands, scene, view, output, effect);
    CHECK(attachment != old_attachment);
    CHECK(second_packet.seal());
    request<RequestRenderJob>(*client, run_job, {second_attachment, &second_packet});
    CHECK(second_packet.writable()); // Removing and starting another Job retained this execution’s layout and cold bindings.
    stopAndRemoveJob(*wire, commands, second_attachment);
    const auto restarted = call<QueryRenderJobInputLayout>(*wire, commands, attachment).layout;
    call<RebindRenderJobEffect>(*wire, commands, {attachment, effect});
    CHECK(call<QueryRenderJobInputLayout>(*wire, commands, attachment).layout.hasSameIdentity(restarted));
    auto rebound_packet = take(FrameData::create<Resources>(restarted));
    call<RebindRenderJobEffect>(*wire, commands, {attachment, effect, second_output});
    CHECK(rebound_packet.seal());
    CHECK(request<RequestRenderJob>(*client, run_job, {attachment, &rebound_packet}, false).error.type == kFrameContract);
    CHECK(rebound_packet.writable());
    call<RebindRenderJobEffect>(*wire, commands, {attachment, effect, output});
    call<RemoveTarget>(*wire, commands, second_output);
    packet.reset(); CHECK(packet.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &packet}, false).error.type == kFrameContract);
    CHECK(packet.writable());
    call<StopRenderJob>(*wire, commands, attachment);
    packet.reset();
    CHECK(packet.seal());
    CHECK(request<UpdateFrameData>(*client, update, {attachment, &packet}, false).error.type == kJobStopped);
    CHECK(packet.writable());
    call<StartRenderJob>(*wire, commands, {attachment, output});
    const auto live_layout = call<QueryRenderJobInputLayout>(*wire, commands, attachment).layout;
    auto shutdown_packet = take(FrameData::create<Resources>(live_layout));
    CHECK(shutdown_packet.addData<Inputs<0>>(Delta{1}));
    CHECK(shutdown_packet.seal());
    auto completion_code = std::make_shared<int>(1);
    std::weak_ptr<const void> weak_code = completion_code;
    std::optional<AckResponse> applied_at_shutdown;
    UpdateFrameData::Request shutdown_input{attachment, &shutdown_packet};
    auto shutdown_update = stdexec::connect(
        client->request<UpdateFrameData>(shutdown_input),
        Reply<AckResponse>{&applied_at_shutdown, {}, weak_code, &shutdown_packet}
    );
    completion_code.reset();
    Counter::entered = false;
    Counter::gate = true;
    stdexec::start(shutdown_update);
    until([] { return Counter::entered.load(); });
    server->stop();
    CHECK(!shutdown_packet.writable() && !weak_code.expired() && !applied_at_shutdown);
    Counter::gate = false;
    Counter::gate.notify_all();
    render.join();
    CHECK(!shutdown_packet.writable() && !weak_code.expired());
    client->serverStopped({kOutcomeUnknown});
    CHECK(shutdown_packet.writable() && weak_code.expired());
    CHECK(applied_at_shutdown && applied_at_shutdown->error.type == 0);
    std::puts("CPU SHUTDOWN PASS: accepted update and completion code retained through terminal callback");
    packet.reset();
    CHECK(packet.seal());
    std::optional<AckResponse> closed_reply;
    UpdateFrameData::Request closed_input{attachment, &packet};
    auto closed = stdexec::connect(
        client->request<UpdateFrameData>(closed_input), Reply<AckResponse>{&closed_reply, {}}
    );
    stdexec::start(closed);
    CHECK(closed_reply && closed_reply->error.type == kOutcomeUnknown && packet.writable());
    CHECK(server->stats().validation_errors == 0);
    std::puts("F2: real column submission, batch apply, declaration isolation, empty update, grow while borrowed,");
    std::puts("duplicate/cancel terminal ownership, View/Effect commands, owned UI snapshots, empty draw and stopped "
              "outputs passed.");
}
