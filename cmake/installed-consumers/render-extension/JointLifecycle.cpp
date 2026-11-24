#include "BufferProductFixture.hpp"
#include "JointAverage.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <cmath>

namespace
{
    namespace graph = lux::graph;
    using sdk_buffers::ProductBuffer;
    using sdk_buffers::ProductDescriptor;
    constexpr auto hdr = ToneMappingPorts::color.semantic;
    constexpr std::uint32_t equal_word = 0x5a5a5a5a, distinct_word = 0x6b6b6b6b;
    constexpr auto display = effectPortSemantic("display");
    constexpr auto result = effectPortSemantic("result");
    constexpr std::string_view source_name = "test.sdk.joint_source";
    constexpr std::string_view report_name = "test.sdk.joint_report";
    constexpr std::string_view reader_name = "test.sdk.joint_reader";
    constexpr auto half_float = rdesc::ETextureFormat::RGBA16_SFLOAT;
    constexpr auto unorm = rdesc::ETextureFormat::RGBA8_UNORM;
    constexpr GraphBufferRange report_bytes{0, 16};
    constexpr std::uint32_t shared_word = 0xc37ac37a, fallback_word = 0xe1e1e1e1;
    std::atomic_uint source_builds{}, report_builds{};

    class SharedWord final
    {
    public:
        using Descriptor = SharedWord;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.shared_word"}, 0, 0, {}, "One immutable GPU word.", EResourceDomain::DEVICE
        };

        explicit SharedWord(DeviceSession& device) noexcept : device_(device) {}
        SharedWord(const SharedWord&) = delete;
        SharedWord& operator=(const SharedWord&) = delete;

        ~SharedWord() noexcept
        {
            device_.deferRelease([core = &device_.nriCore(), buffer = buffer_]() noexcept
            {
                core->DestroyBuffer(buffer);
            });
        }

        static RenderResult<std::unique_ptr<SharedWord>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            auto word = std::make_unique<SharedWord>(context.device);
            const auto& core = context.device.nriCore();
            const nri::BufferDesc desc{sizeof(shared_word), 0, nri::BufferUsageBits::NONE};
            const auto created = core.CreatePlacedBuffer(
                context.device.device(), NriHostUploadHeap, desc, word->buffer_
            );
            CHECK(created == nri::Result::SUCCESS);
            auto* data = core.MapBuffer(*word->buffer_, 0, desc.size);
            CHECK(data);
            std::memcpy(data, &shared_word, sizeof(shared_word));
            CHECK(context.device.flushMappedRange(*word->buffer_, 0, desc.size) == nri::Result::SUCCESS);
            core.UnmapBuffer(*word->buffer_);
            return word;
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps binding{
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<ResourceProjection>
                {
                    if (projection) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return ResourceProjection{
                        GraphBufferDesc{4}, {GraphBufferRange{0, 4}},
                        EGraphUse::TRANSFER_SOURCE, EGraphUse::TRANSFER_SOURCE
                    };
                },
                .resolveGraphImport = [](
                    void* owner, std::uint32_t, const VGraphResourceDesc&, std::span<ResourceInput> frames
                ) noexcept -> RenderResult<void>
                {
                    auto& word = *static_cast<SharedWord*>(owner);
                    std::ranges::fill(frames, ResourceInput{word.buffer_, 0, 4});
                    return {};
                }
            };
            return makeResourceRegistration<Descriptor, SharedWord, &create>(&binding);
        }

    private:
        DeviceSession& device_;
        nri::Buffer* buffer_ = nullptr;
    };

    ResourceVersion clear(EffectCompileContext& builder, const GraphImageDesc& desc, std::array<float, 4> value)
    {
        const auto image = take(builder.declareResource({"solid color", desc}));
        return take(builder.addTransferPass("clear color", TransferWork{{ClearImage{{{image, 0}}, value}}})).written[0];
    }

    RenderResult<ResourceVersion>
    fill(EffectCompileContext& builder, std::string key, GraphResourceId buffer, std::uint32_t word) noexcept
    {
        auto pass = builder.addTransferPass(std::move(key), TransferWork{{FillBuffer{{{buffer, 0}}, word}}});
        if (!pass) return lux::cxx::unexpected(pass.error());
        return pass->written.front();
    }

    ResourceVersion outputBuffer(EffectCompileContext& context, std::string_view name, GraphBufferRange bytes)
    {
        return take(context.importFinalBuffer(name, ProductDescriptor::descriptor.type, {
            "", GraphBufferDesc{ProductBuffer::bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_SOURCE, bytes
        }));
    }

    ResourceVersion captureBuffer(EffectCompileContext& context, std::string_view name)
    {
        return take(context.importFinalBuffer(name, GraphCaptureDescriptor::descriptor.type, {
            "", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
            EGraphUse::TRANSFER_DESTINATION, report_bytes
        }));
    }

    ResourceVersion
    inputBuffer(EffectCompileContext& context, std::string_view name, ResourceNameView type, std::uint32_t bytes)
    {
        return take(context.importResource(name, type, {
            GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::TRANSFER_SOURCE
        }));
    }

    RenderResult<void> buildSource(EffectCompileContext& context, std::string_view mode) noexcept
    {
        const GraphImageDesc desc{64, 64, 1, 1, half_float};
        if (mode != "inputs" && mode != "feedback")
        {
            const bool bright = mode == "bright";
            const auto rgb = bright ? std::array{.25f, .5f, .75f, 1.f} : std::array{.125f, .375f, .625f, 1.f};
            const auto color = clear(context, desc, rgb);
            return context.publish(hdr, {{hdr, color}});
        }

        ++source_builds;
        const auto first = take(context.importTargetImage("first", half_float));
        const auto second = take(context.importTargetImage("second", half_float));
        const auto mixed = take(context.declareResource({"average", desc}));
        const auto program = take(context.addProgram(GraphProgram{kJointAverage.load(context.shaderTarget())}));
        const auto average = [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
        {
            ComputeWork work;
            work.interface.program = program;
            work.interface.resources = {
                {"first_image", {first}}, {"second_image", {second}},
                {"generated", {{mixed, 0}, {}, EGraphAccess::WRITE}}
            };
            work.dispatch = ImageDispatchArguments{mixed};
            auto pass = arm.addComputePass("average HDR sources", std::move(work));
            if (!pass) return lux::cxx::unexpected(pass.error());
            return pass->written.front();
        };
        if (mode == "feedback")
        {
            const auto value = take(average(context));
            const auto output = take(context.declareResource({"published color", desc}));
            const auto copy = take(context.addTransferPass("publish average", TransferWork{{CopyImage{{value}, {{output, 0}}}}}));
            return context.publish(hdr, {{hdr, copy.written.front()}});
        }
        const auto same = take(context.sameBinding(first.resource, second.resource));
        const auto selected = take(context.choose(same,
            [&](EffectCompileContext&) noexcept -> RenderResult<ResourceVersion> { return first; }, average
        ));
        return context.publish(hdr, {{hdr, selected}});
    }

    RenderResult<void> buildReport(EffectCompileContext& context, std::string_view) noexcept
    {
        ++report_builds;
        const auto colors = context.inputs(display);
        CHECK(colors.size() == 1 && colors.front().size() == 1);
        const auto output = outputBuffer(context, "report", report_bytes);
        const auto capture = captureBuffer(context, "capture");
        const auto other = captureBuffer(context, "comparison");
        const auto seed = inputBuffer(context, "shared_seed", SharedWord::descriptor.type, 4);
        const auto pixel = take(context.declareResource({"display texel", GraphBufferDesc{256}}));
        const auto packed = take(context.addTransferPass("read display pixel", TransferWork{{CopyImageBuffer{
            {colors.front().front().version}, {{pixel, 0}}, 256, 256, ImageCopyRegion{0, 0, 1, 1}
        }}}));
        const auto branch = take(context.declareCondition("write seven"));
        const auto publish = take(context.declareCondition("publish history"));
        const auto history = take(context.declareHistory("per job", {
            GraphBufferDesc{4}, {}, EGraphUse::TRANSFER_SOURCE, 2
        }));
        const auto initial = take(context.declareResource({"first sample", GraphBufferDesc{4}}));
        const auto previous = take(context.choose({EConditionSource::HISTORY_VALID, history.previous.index},
            [&](EffectCompileContext&) noexcept -> RenderResult<ResourceVersion>
            {
                return ResourceVersion{history.previous, 0};
            },
            [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
            {
                return fill(arm, "history fallback", initial, fallback_word);
            }
        ));
        const auto next = take(context.choose({EConditionSource::INPUT, branch},
            [&](EffectCompileContext& arm) noexcept { return fill(arm, "sample seven", history.current, 7); },
            [&](EffectCompileContext& arm) noexcept { return fill(arm, "sample eleven", history.current, 11); }
        ));
        CHECK(context.publishHistory(next, PassCondition{EConditionSource::INPUT, publish}));
        const auto history_word = take(context.addTransferPass("capture previous sample", TransferWork{{
            CopyBuffer{{previous}, {packed.written.front(), GraphBufferRange{8, 4}}}
        }})).written.front();
        const auto base = take(context.addTransferPass("read shared word", TransferWork{{
            CopyBuffer{{seed}, {history_word, GraphBufferRange{12, 4}}}
        }})).written.front();
        const auto stamp = [&](EffectCompileContext& arm, std::string key, std::uint32_t word) noexcept
            -> RenderResult<ResourceVersion>
        {
            auto marked = arm.addTransferPass(std::move(key), TransferWork{{
                FillBuffer{{base, GraphBufferRange{4, 4}}, word}
            }});
            if (!marked) return lux::cxx::unexpected(marked.error());
            return marked->written.front();
        };
        const auto selected = take(context.choose(take(context.sameBinding(other.resource, capture.resource)),
            [&](EffectCompileContext& arm) noexcept { return stamp(arm, "equal outputs", equal_word); },
            [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
            {
                auto marked = stamp(arm, "distinct outputs", distinct_word);
                if (!marked) return lux::cxx::unexpected(marked.error());
                auto copy = arm.addTransferPass("publish other output", TransferWork{{
                    CopyBuffer{{*marked, report_bytes}, {other, report_bytes}}
                }});
                if (!copy) return lux::cxx::unexpected(copy.error());
                return *marked;
            }
        ));
        const auto captured = take(context.addTransferPass("capture primary output", TransferWork{{
            CopyBuffer{{selected, report_bytes}, {capture, report_bytes}}
        }}));
        CHECK(!captured.written.empty());
        const auto report = take(context.addTransferPass("publish selected frame", TransferWork{{
            CopyBuffer{{selected, report_bytes}, {output, report_bytes}}
        }}));
        return context.publish(result, {{result, report.written.front()}});
    }

    RenderResult<void> buildReadback(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto published = inputBuffer(context, "version", ProductDescriptor::descriptor.type, 16);
        const auto capture = captureBuffer(context, "capture");
        const auto copy = take(context.addTransferPass("read receipt", TransferWork{{
            CopyBuffer{{published}, {capture, report_bytes}}
        }}));
        return context.publish(result, {{result, copy.written.front()}});
    }

    EffectCapability definition(std::string_view name)
    {
        static constexpr EffectPortDefinition source[]{
            {hdr, "color", {HdrColorBundle::type, 1}, graph::EPinDirection::OUTPUT}
        };
        static constexpr EffectPortDefinition report[]{
            {result, "result", {"test.sdk.report", 1}, graph::EPinDirection::OUTPUT},
            {display, "display", {kDisplayColorType, 1}, graph::EPinDirection::INPUT, 1, true}
        };
        auto ports = std::span<const EffectPortDefinition>{report}.first(name == report_name ? 2 : 1);
        if (name == source_name) ports = source;
        return {.id = graph::nodeTypeId(name), .canonical_name = name, .ports = ports};
    }

    void install(RenderServer& server, std::string_view name, decltype(EffectImplementation::build) build)
    {
        const auto cap = definition(name);
        CHECK(server.registerEffectCapability(cap));
        EffectImplementation impl;
        impl.capability = cap.id;
        impl.identity = {std::string{name}, 1};
        impl.supported_outputs = name == source_name ? std::vector{hdr} : std::vector{result};
        impl.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
        {
            return {};
        };
        impl.build = build;
        CHECK(server.registerEffectImplementation(std::move(impl)));
    }

    CreateEffectRequest recipe(std::string_view mode)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = [&](std::string_view name, std::string args)
        {
            return take(edit.addNode(definition(name), {std::string{name}, 1}, std::move(args)));
        };
        if (mode == "receipt")
        {
            CHECK(edit.setOutput("pixels", edit.pinId(node(reader_name, {}), result)));
        }
        else
        {
            const auto source = node(source_name, std::string{mode});
            const auto color = edit.pinId(source, hdr);
            if (mode == "inputs")
            {
                const auto tone = take(edit.addNode(
                    capabilities::ToneMapping, {std::string{"lux.render.tone_mapping.linear"}, 1}, "exposure=0"
                ));
                const auto sink = node(report_name, {});
                const auto mapped = edit.pinId(tone, ToneMappingPorts::color.semantic);
                CHECK(edit.connect(color, edit.pinId(tone, ToneMappingPorts::hdr_color.semantic)));
                CHECK(edit.connect(mapped, edit.pinId(sink, display)));
                CHECK(edit.setOutput("color", mapped, true));
                CHECK(edit.setOutput("frame", edit.pinId(sink, result)));
            }
            else
            {
                request.output = {half_float, 1, ETargetKind::OFFSCREEN, true};
                CHECK(edit.setOutput("color", color, true));
            }
        }
        CHECK(edit.commit());
        return request;
    }

    struct Writer final
    {
        RenderJobId job;
        TargetId target;
        JobBinding binding;

        ResourceInstanceId report() const noexcept { return binding.resources.back().resource; }
    };

    struct ExpectedReport final
    {
        bool same_targets;
        std::uint32_t pattern, previous;
    };

    void verify(const CapturedPixels& capture, std::uint32_t slot, ExpectedReport expected)
    {
        CHECK(slot < capture.frames.size());
        std::array<std::uint32_t, 4> words{};
        const auto& frame = capture.frames[slot];
        CHECK(frame.size() >= sizeof(words));
        std::memcpy(words.data(), frame.data(), sizeof(words));
        const auto rgb = expected.same_targets ? std::array{.125, .375, .625} : std::array{.1875, .4375, .6875};
        for (std::size_t i = 0; i < rgb.size(); ++i)
        {
            const auto x = rgb[i];
            const auto mapped = (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14);
            CHECK(std::abs(std::to_integer<long>(frame[i]) - std::lround(mapped * 255)) <= 2);
        }
        CHECK(frame[3] == std::byte{255});
        CHECK(words[1] == expected.pattern);
        CHECK(words[2] == expected.previous && words[3] == shared_word);
        std::printf("receipt data: rgba=%u,%u,%u,%u pattern=%08x previous=%08x shared=%08x\n",
            std::to_integer<unsigned>(frame[0]), std::to_integer<unsigned>(frame[1]),
            std::to_integer<unsigned>(frame[2]), std::to_integer<unsigned>(frame[3]), words[1], words[2], words[3]);
    }

    struct Session final
    {
        ipc::IpcRequester& client;
        const CommandList& commands;
        DeviceSessionId device;
        SceneId scene;

        template <class C> typename C::Response send(typename C::Request request, bool success = true) const
        {
            return call<C>(client, commands, std::move(request), success);
        }

        ViewId view() const { return send<CreateView>({scene}).view; }

        ResourceInstanceId product(ViewId owner) const
        {
            return send<GetOrCreateResource>({
                device, ViewScope{scene, owner}, ProductDescriptor::descriptor.type
            }).resource_id;
        }

        TargetId target(bool sampled = false) const
        {
            const TargetDesc desc{64, 64, sampled ? half_float : unorm, 1, ETargetKind::OFFSCREEN, sampled};
            return send<CreateTarget>({device, desc}).target;
        }

        EffectId prepare(std::string_view mode) const
        {
            const auto effect = send<CreateEffect>(recipe(mode)).effect;
            const auto ready = send<PrepareEffect>({effect, device}, false);
            if (ready.error.type)
                std::fprintf(stderr, "Prepare %.*s error=%llu args=%llu,%llu,%llu\n",
                    int(mode.size()), mode.data(), static_cast<unsigned long long>(ready.error.type),
                    ready.error.args[0], ready.error.args[1], ready.error.args[2]);
            CHECK(!ready.error.type);
            return effect;
        }

        RenderJobId start(EffectId effect, TargetId output, JobBinding binding) const
        {
            const auto job = send<CreateRenderJob>({scene, view(), effect, EJobCadence::REQUESTED}).job;
            send<StartRenderJob>({device, job, output, std::move(binding)});
            return job;
        }

        Writer writer(EffectId effect, JobBinding binding) const
        {
            const auto output = product(view());
            binding.resources.push_back({"report", output});
            const auto surface = target();
            return {start(effect, surface, binding), surface, std::move(binding)};
        }

        JobSubmitted frame(Writer& writer, std::array<std::uint8_t, 2> conditions) const
        {
            writer.binding.inputs.conditions.assign(conditions.begin(), conditions.end());
            send<UpdateJobInputs>({writer.job, writer.binding.inputs});
            const auto receipt = send<RequestRenderJob>({writer.job});
            CHECK(receipt.job == writer.job && receipt.slot_index < 2);
            return receipt;
        }
    };

    struct CapturePair final
    {
        CapturedPixels primary, comparison;
        ViewId owner, other;

        void bind(const Session& session, JobBinding& binding, bool distinct)
        {
            const auto make = [&](ViewId view, CapturedPixels& pixels)
            {
                const CaptureConfig config{&pixels};
                return GraphCaptureBuffer::bind(
                    {session.client, session.commands, session.device}, {session.scene, view}, config
                );
            };
            owner = session.view();
            const auto buffer = make(owner, primary);
            auto alternate = buffer;
            if (distinct)
            {
                other = session.view();
                alternate = make(other, comparison);
            }
            binding.resources.push_back({"capture", buffer});
            binding.resources.push_back({"comparison", alternate});
        }

        void finish(const Session& session, const JobSubmitted& last, ExpectedReport expected)
        {
            session.send<RemoveView>({session.scene, owner});
            if (!other.isNull()) session.send<RemoveView>({session.scene, other});
            const bool distinct = expected.pattern == distinct_word;
            const auto deadline = Clock::now() + 10s;
            while (!primary.ready.load(std::memory_order_acquire) ||
                   (distinct && !comparison.ready.load(std::memory_order_acquire)))
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            verify(primary, last.slot_index, expected);
            if (distinct) verify(comparison, last.slot_index, expected);
        }
    };

    class ReceiptReader final
    {
    public:
        explicit ReceiptReader(const Session& session)
            : session_(session), effect_(session.prepare("receipt")), target_(session.target()) {}

        JobSubmitted read(
            ResourceInstanceId product, JobSubmitted receipt, ExpectedReport expected, bool exercise_reuse = false
        ) const
        {
            const auto view = session_.view();
            CapturedPixels pixels;
            const CaptureConfig config{.result = &pixels};
            const auto buffer = GraphCaptureBuffer::bind(
                {session_.client, session_.commands, session_.device}, {session_.scene, view}, config
            );
            JobBinding binding;
            binding.resources = {{"capture", buffer}, {"version", product}};
            binding.resources.back().publication = receipt.bufferVersion(report_bytes);
            const auto job = session_.send<CreateRenderJob>({
                session_.scene, view, effect_, EJobCadence::REQUESTED
            }).job;
            session_.send<StartRenderJob>({session_.device, job, target_, binding});
            auto& probe = *ProductBuffer::live;
            if (exercise_reuse && probe.canDelayRead()) probe.delayRead();
            const auto read = session_.send<RequestRenderJob>({job});
            stopAndRemoveJob(session_.client, session_.commands, job);
            JobSubmitted reused;
            if (exercise_reuse)
            {
                CHECK(read.slot_index != receipt.slot_index);
                reused = sdk_buffers::reuseAfterReader(session_.client, session_.commands, receipt.job);
                CHECK(reused.slot_index == receipt.slot_index && reused.frame_index > receipt.frame_index);
                std::printf("FIF reuse: published=%u reader=%u resumed=%u waited=%u\n",
                    receipt.slot_index, read.slot_index, reused.slot_index, unsigned(probe.canDelayRead()));
            }
            session_.send<RemoveView>({session_.scene, view});
            const auto deadline = Clock::now() + 10s;
            while (!pixels.ready.load(std::memory_order_acquire))
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            verify(pixels, read.slot_index, expected);
            return reused;
        }

    private:
        const Session& session_;
        EffectId effect_;
        TargetId target_;
    };

    bool jointLifecycle(const Session& session, const RenderServer& server, bool single_slot)
    {
        const auto dim = session.target(true);
        const auto bright = session.target(true);
        const auto seed = session.send<GetOrCreateResource>({
            session.device, DeviceScope{}, SharedWord::descriptor.type
        }).resource_id;
        const auto dim_job = session.start(session.prepare("dim"), dim, {});
        const auto bright_job = session.start(session.prepare("bright"), bright, {});
        session.send<RequestRenderJob>({dim_job});
        session.send<RequestRenderJob>({bright_job});

        const auto publish_average = [&]
        {
            JobBinding inputs;
            inputs.target_images = {
                {dim, "first", EProductRead::LATEST_PUBLISHED}, {bright, "second", EProductRead::CURRENT_RUN}
            };
            const auto feedback = session.prepare("feedback");
            session.send<RebindRenderJobEffect>({session.device, dim_job, feedback, dim, inputs});
            // Making bright read dim in the current round would close a real two-Target cycle.
            inputs.target_images.front().read = EProductRead::CURRENT_RUN;
            inputs.target_images.back().read = EProductRead::LATEST_PUBLISHED;
            const auto cycle = session.send<RebindRenderJobEffect>({session.device, bright_job, feedback, bright, inputs}, false);
            CHECK(cycle.error.type == kJobCycle);
            session.send<RequestRenderJob>({bright_job});
            const auto self = session.send<RebindRenderJobEffect>({session.device, dim_job, feedback, dim, inputs}, false);
            CHECK(self.error.type == kCommandContract);
            inputs.target_images.front().read = EProductRead::LATEST_PUBLISHED;
            session.send<RebindRenderJobEffect>({session.device, dim_job, feedback, dim, inputs});
            session.send<RequestRenderJob>({dim_job});
        };

        ReceiptReader reader{session};
        const auto before = server.stats().compiled_graphs;
        const auto effect = session.prepare("inputs");
        CHECK(server.stats().compiled_graphs == before + 1);
        session.send<PrepareEffect>({effect, session.device});
        JobBinding binding;
        binding.inputs.conditions = {1, 1};
        binding.resources = {{"shared_seed", seed}};
        binding.target_images = {{dim, "first", EProductRead::LATEST_PUBLISHED}, {dim, "second", EProductRead::LATEST_PUBLISHED}};
        CapturePair captures_a, captures_b;
        auto inputs_a = binding;
        captures_a.bind(session, inputs_a, false);
        auto a = session.writer(effect, std::move(inputs_a));
        if (single_slot)
        {
            publish_average();
            const auto frame = session.frame(a, {1, 1});
            CHECK(frame.slot_index == 0);
            for (const auto job : {a.job, dim_job, bright_job}) stopAndRemoveJob(session.client, session.commands, job);
            captures_a.finish(session, frame, {false, equal_word, fallback_word});
            std::puts("ONE FIF PASS: writer sampled its previous physical image and published the expected mixed pixel");
            return true;
        }
        binding.target_images.front().target = bright;
        captures_b.bind(session, binding, true);
        auto b = session.writer(effect, std::move(binding));
        const auto first = session.frame(a, {1, 1});
        const auto peer = session.frame(b, {0, 1});
        CHECK(first.slot_index != peer.slot_index);
        reader.read(a.report(), first, {true, equal_word, fallback_word});
        reader.read(b.report(), peer, {false, distinct_word, fallback_word});

        auto clash = b.binding;
        clash.resources.back().resource = a.report();
        const auto rejected = session.send<RebindRenderJobEffect>(
            {session.device, b.job, effect, b.target, clash}, false
        );
        CHECK(rejected.error.type == kJobBusy);
        reader.read(b.report(), session.frame(b, {1, 1}), {false, distinct_word, fallback_word});
        reader.read(a.report(), session.frame(a, {0, 1}), {true, equal_word, fallback_word});
        JobSubmitted held_peer;
        for (unsigned i = 0; i < 2; ++i)
        {
            reader.read(a.report(), session.frame(a, {0, 0}), {true, equal_word, 7});
            held_peer = session.frame(b, {1, 0});
            reader.read(b.report(), held_peer, {false, distinct_word, 11});
        }

        const auto old_product = b.report();
        b.binding.target_images.front().target = dim;
        b.binding.resources.back().resource = session.product(session.view());
        session.send<RebindRenderJobEffect>({session.device, b.job, effect, b.target, b.binding});
        // A consumer can still install the published sample while its old backing has not been overwritten.
        reader.read(old_product, held_peer, {false, distinct_word, 11});
        const auto rebound = session.frame(b, {0, 1});
        const auto refreshed = reader.read(b.report(), rebound, {true, distinct_word, fallback_word}, true);
        reader.read(b.report(), refreshed, {true, distinct_word, fallback_word});

        session.send<StopRenderJob>(a.job);
        publish_average();
        // Both operands now read that single average. The report also verifies the equal-binding branch.
        session.send<StartRenderJob>({session.device, a.job, a.target, a.binding});
        const auto progress = session.send<QueryExecutionProgress>(a.job);
        CHECK(progress.progress.count(EExecutionOutcome::EXECUTED) == 0);
        const auto restarted = session.frame(a, {1, 1});
        reader.read(a.report(), restarted, {false, equal_word, fallback_word});
        const auto compiles = server.stats().compiled_graphs - before;
        CHECK(compiles == 2 && source_builds == 2 && report_builds == 1);
        std::printf("Prepare/Start/Rebind/restart compiled graphs=%llu; conflict=kJobBusy\n",
            static_cast<unsigned long long>(compiles));
        for (const auto job : {a.job, b.job, bright_job, dim_job})
            stopAndRemoveJob(session.client, session.commands, job);
        captures_a.finish(session, restarted, {false, equal_word, fallback_word});
        captures_b.finish(session, refreshed, {true, distinct_word, fallback_word});
        return ProductBuffer::live->canDelayRead();
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"installed.joint.lifecycle", nativeValidationEnabled()};
    const bool single_slot = argc > 2 && std::string_view{argv[2]} == "single-slot";
    config.frames_in_flight = single_slot ? 1 : 2;
    auto server = take(RenderServer::create(config, **execution));
    take(server->registerResource<ProductBuffer>());
    take(server->registerResource<SharedWord>());
    take(server->registerResource<GraphCaptureBuffer>());
    install(*server, source_name, buildSource);
    install(*server, report_name, buildReport);
    install(*server, reader_name, buildReadback);
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    RenderResult<void> running;
    std::thread owner([&] { running = server->run(); });
    const auto commands = queryCommands(*requester);
    DeviceOptions options;
    if (argc > 1 && std::string_view{argv[1]} == "d3d12") options.backend = EGraphicsBackend::D3D12;
    options.multiple_queues = true;
    options.texture_descriptors = true;
    const auto device = call<CreateDeviceSession>(*requester, commands, options).device;
    const auto scene = call<CreateScene>(*requester, commands, {}).scene;
    const bool delayed = jointLifecycle({*requester, commands, device, scene}, *server, single_slot);
    server->stop();
    owner.join();
    CHECK(running && server->stats().validation_errors == 0);
    if (single_slot) return 0;
    std::printf("joint lifecycle: independent History, accepted Rebind and restart, delayed reader=%d\n",
        int(delayed));
    return delayed ? 0 : 77;
}
