#include "SharedImageRead.hpp"
#include "SceneCapture.hpp"
#include <future>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace
{
    constexpr std::uint32_t extent = 64;
    constexpr std::string_view reader_name = "test.sdk.shared_image_reader";

    struct UpdateImages final
    {
        static constexpr std::string_view name = "test.sdk.update_images";
        enum class EAction : std::uint8_t
        {
            INSTALL, REJECT_UPLOAD, INVALID_SECOND, EXCLUSIVE_SECOND, ALIAS, STAGE, ADOPT
        };
        struct Request final
        {
            ResourceInstanceId resource;
            EAction action;
            std::uint8_t value;
        };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct ImagePairDescriptor final
    {
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<UpdateImages>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.image_pair"}, 0, 0, {}, "Sampled and copy-source images.",
            EResourceDomain::SCENE, resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };
    };

    struct ImageReplica final
    {
        nri::Texture* texture = nullptr;
        SubmissionFrontiers preparation, readers;
    };

    struct ImagePair final
    {
        ImagePair(DeviceSession& device, std::uint8_t value) noexcept : device(device), value(value)
        {
            for (auto& image : images)
            {
                image.preparation = device.makeFrontiers();
                image.readers = device.makeFrontiers();
            }
        }

        ~ImagePair() noexcept
        {
            for (auto& image : images)
            {
                if (!image.texture) continue;
                image.preparation.merge(image.readers);
                device.deferRelease(image.preparation, [
                    destroy = device.nriCore().DestroyTexture, texture = image.texture, value = value
                ]() noexcept
                {
                    destroy(texture);
                    if (value == 17) ++old_retired;
                });
            }
        }

        ImagePair(const ImagePair&) = delete;
        ImagePair& operator=(const ImagePair&) = delete;
        DeviceSession& device;
        std::uint8_t value;
        std::array<ImageReplica, 2> images;
        static inline std::atomic_uint old_retired{};
    };

    class ImageStore final
    {
    public:
        using Descriptor = ImagePairDescriptor;
        static inline ImageStore* live = nullptr;

        ImageStore(ResourceCreateContext& context, ResourceInstanceId id) noexcept
            : device_(context.device), uploads_(*context.uploads), registry_(context.resources), id_(id)
        {
            CHECK(device_.nriCore().CreateFence(device_.device(), {}, gate_) == nri::Result::SUCCESS);
            auto& hooks = const_cast<nri::CoreInterface&>(device_.nriCore());
            barrier_ = hooks.CmdBarrier;
            hooks.CmdBarrier = observeBarrier;
            submit_ = hooks.QueueSubmit;
            hooks.QueueSubmit = holdSubmission;
            live = this;
        }

        ~ImageStore() noexcept
        {
            live = nullptr;
            const_cast<nri::CoreInterface&>(device_.nriCore()).CmdBarrier = barrier_;
            const_cast<nri::CoreInterface&>(device_.nriCore()).QueueSubmit = submit_;
            device_.deferRelease([destroy = device_.nriCore().DestroyFence, fence = gate_]() noexcept
            {
                destroy(fence);
            });
        }

        bool hasGateQueue() const noexcept { return device_.queueCount() >= 3; }

        void gateReads(bool release) const noexcept
        {
            const nri::FenceSubmitDesc point{gate_, 1, nri::StageBits::ALL};
            const nri::QueueSubmitDesc descriptor = release ?
                nri::QueueSubmitDesc{.signalFences = &point, .signalFenceNum = 1} :
                nri::QueueSubmitDesc{.waitFences = &point, .waitFenceNum = 1};
            if (release)
            {
                auto& queue = device_.queue(EQueueRole::TRANSFER);
                CHECK(device_.nriCore().QueueSubmit(queue.native(), descriptor) == nri::Result::SUCCESS);
                return;
            }
            for (auto role : {EQueueRole::GRAPHICS, EQueueRole::COMPUTE})
                CHECK(device_.nriCore().QueueSubmit(device_.queue(role).native(), descriptor) == nri::Result::SUCCESS);
        }

        static RenderResult<std::unique_ptr<ImageStore>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            if (!context.uploads) return lux::cxx::unexpected(RenderError{kDeviceUnsupported});
            return std::make_unique<ImageStore>(context, id);
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps ops{.describeGraphImport = describe, .resolveGraphImport = resolve};
            static const std::array commands{CommandBinding::declare<UpdateImages, &update>()};
            return makeResourceRegistration<Descriptor, ImageStore, &create>(nullptr, &ops, commands);
        }

        static inline std::atomic_bool hold_next{}, parked{}, resume{};
        static inline std::atomic_uint frozen_rejections{};

    private:
        static inline decltype(nri::CoreInterface::CmdBarrier) barrier_;
        static inline std::atomic<nri::Texture*> watched_{};
        static inline std::atomic<nri::Layout> layout_{nri::Layout::UNDEFINED};
        static inline decltype(nri::CoreInterface::QueueSubmit) submit_;
        static inline std::atomic_bool reject_upload_{};

        static nri::Result holdSubmission(nri::Queue& queue, const nri::QueueSubmitDesc& desc) noexcept
        {
            if (reject_upload_.exchange(false)) return nri::Result::INVALID_ARGUMENT;
            if (hold_next.exchange(false))
            {
                parked = true;
                resume.wait(false);
            }
            return submit_(queue, desc);
        }

        static void observeBarrier(nri::CommandBuffer& commands, const nri::BarrierDesc& barriers) noexcept
        {
            const auto* target = watched_.load();
            for (const auto& change : std::span{barriers.textures, barriers.textureNum})
                if (change.texture == target) layout_ = change.after.layout;
            barrier_(commands, barriers);
        }

        static RenderResult<StorageGraphImport> describe(const void*, std::uint32_t projection) noexcept
        {
            if (projection >= 2) return lux::cxx::unexpected(RenderError{kStorageContract});
            const auto use = projection ? EGraphUse::TRANSFER_SOURCE : EGraphUse::SAMPLED;
            return StorageGraphImport{GraphImageDesc{extent, extent}, {GraphImageRange{}}, use, use, false, true};
        }

        static RenderResult<void> resolve(
            void* object, std::uint32_t projection, const VGraphResourceDesc&, std::span<ResourceInput> out
        ) noexcept
        {
            auto& store = *static_cast<ImageStore*>(object);
            if (!store.current_ || projection >= 2) return lux::cxx::unexpected(RenderError{kStorageMismatch});
            auto& image = store.current_->images[projection];
            const ResourceInput input{.image = image.texture, .queue_uses = {&image.preparation, &image.readers}};
            std::ranges::fill(out, input);
            return {};
        }

        exec::task<RenderResult<void>> prepare(ImagePair& pair, UpdateImages::EAction action) noexcept
        {
            for (std::uint32_t i = 0; i < pair.images.size(); ++i)
            {
                auto& image = pair.images[i];
                const bool wrong_size = i && action == UpdateImages::EAction::INVALID_SECOND;
                const bool exclusive = i && action == UpdateImages::EAction::EXCLUSIVE_SECOND;
                const auto width = static_cast<nri::Dim_t>(wrong_size ? extent / 2 : extent);
                const auto usage = exclusive ? nri::TextureUsageBits::COLOR_ATTACHMENT :
                    i ? nri::TextureUsageBits::NONE : nri::TextureUsageBits::SHADER_RESOURCE;
                const nri::TextureDesc desc{
                    nri::TextureType::TEXTURE_2D, usage, nri::Format::RGBA8_UNORM, width, extent, 1, 1, 1, 1,
                    exclusive ? nri::SharingMode::EXCLUSIVE : nri::SharingMode::CONCURRENT
                };
                const auto created = device_.nriCore().CreatePlacedTexture(
                    device_.device(), NriDeviceHeap, desc, image.texture
                );
                if (created != nri::Result::SUCCESS)
                    co_return lux::cxx::unexpected(RenderError{kDeviceFailure, {static_cast<unsigned>(created), i}});
                std::vector<std::byte> data(width * extent * 4, std::byte{pair.value});
                std::vector<ImageTransferRegion> regions{{
                    0, width * 4u, 1, 4, {0, 0, 0, width, extent, 1, 0, 0, nri::PlaneBits::COLOR}
                }};
                const auto use = i ? EImageUploadUse::COPY_SOURCE : EImageUploadUse::SAMPLED;
                layout_ = nri::Layout::UNDEFINED;
                watched_ = image.texture;
                reject_upload_ = i && action == UpdateImages::EAction::REJECT_UPLOAD;
                auto wrote = co_await uploads_.writeImage(
                    *image.texture, data, std::move(regions), {use, &image.preparation}
                );
                watched_ = nullptr;
                if (!wrote)
                {
                    if (action == UpdateImages::EAction::REJECT_UPLOAD)
                    {
                        CHECK(i == 1 && !reject_upload_ && wrote.error().type == kDeviceFailure);
                        const auto& serials = image.preparation.serials();
                        CHECK(std::ranges::all_of(serials, [](auto serial) { return serial == 0; }));
                        std::puts("replacement first image accepted, second image upload rejected");
                    }
                    co_return lux::cxx::unexpected(wrote.error());
                }
                CHECK(layout_ == (i ? nri::Layout::COPY_SOURCE : nri::Layout::SHADER_RESOURCE));
                std::printf("image upload: value=%u projection=%u texture=%p layout=%s\n",
                    unsigned(pair.value), i, static_cast<void*>(image.texture), i ? "COPY_SOURCE" : "SHADER_RESOURCE");
                CHECK(std::ranges::any_of(image.preparation.serials(), [](auto serial) { return serial != 0; }));
            }
            CHECK(pair.images[0].texture != pair.images[1].texture);
            co_return RenderResult<void>{};
        }

        static exec::task<AckResponse> update(ImageStore& store, UpdateImages::Request&& request) noexcept
        {
            using Action = UpdateImages::EAction;
            if (store.preparing_) co_return AckResponse{RenderError{kDeviceBusy}};
            if (request.action != Action::ADOPT)
            {
                auto candidate = std::make_unique<ImagePair>(store.device_, request.value);
                store.preparing_ = true;
                auto prepared = co_await store.prepare(*candidate, request.action);
                store.preparing_ = false;
                if (!prepared) co_return AckResponse{prepared.error()};
                store.staged_ = std::move(candidate);
                if (request.action == Action::STAGE) co_return AckResponse{};
            }
            if (!store.staged_) co_return AckResponse{RenderError{kStorageContract}};
            auto& images = store.staged_->images;
            const std::array updates{
                GraphImagePublication{0, UINT32_MAX, images[0].texture, {&images[0].preparation, &images[0].readers}},
                GraphImagePublication{1, UINT32_MAX, images[request.action == Action::ALIAS ? 0 : 1].texture,
                    {&images[1].preparation, &images[1].readers}}
            };
            auto published = store.registry_.publishGraphImages(store.id_, updates);
            if (!published)
            {
                if (published.error().type == kDeviceBusy) ++frozen_rejections;
                else store.staged_.reset();
                co_return AckResponse{published.error()};
            }
            if (store.current_)
            {
                const auto compute = store.device_.queueIndex(store.device_.queue(EQueueRole::COMPUTE));
                const auto graphics = store.device_.queueIndex(store.device_.queue());
                const auto& old_pair = store.current_->images;
                std::printf("image read receipts: sampled compute=%llu, copy graphics=%llu\n",
                    static_cast<unsigned long long>(old_pair[0].readers.serials()[compute]),
                    static_cast<unsigned long long>(old_pair[1].readers.serials()[graphics]));
                CHECK(old_pair[0].readers.serials()[compute] && old_pair[1].readers.serials()[graphics]);
                for (auto& old : store.current_->images)
                {
                    CHECK(std::ranges::any_of(old.readers.serials(), [](auto value) { return value != 0; }));
                    CHECK(!store.device_.reached(old.readers));
                }
            }
            store.current_ = std::move(store.staged_);
            co_return AckResponse{};
        }

        DeviceSession& device_;
        ResourceUploader& uploads_;
        ResourceRegistry& registry_;
        ResourceInstanceId id_;
        nri::Fence* gate_ = nullptr;
        std::unique_ptr<ImagePair> current_, staged_;
        bool preparing_ = false;
    };

    CapabilityDefinition readerDefinition()
    {
        return {
            .identity = {lux::graph::nodeTypeId(reader_name), std::string{reader_name}, 1},
            .ports = {
                {ColorCapture::color, "color", {std::string{ToneMapping::display_type}, 1},
                    lux::graph::EPinDirection::OUTPUT},
                {ColorCapture::pixels, "pixels", {"test.graph.pixels", 1}, lux::graph::EPinDirection::OUTPUT}
            }
        };
    }

    RenderResult<void> buildReader(EffectCompileContext& context, std::string_view arguments) noexcept
    {
        const bool sample = arguments == "sample";
        const auto use = sample ? EGraphUse::SAMPLED : EGraphUse::TRANSFER_SOURCE;
        const GraphImageDesc image{extent, extent};
        const auto imported = take(context.importResource(
            "source", {ResourceName{ImagePairDescriptor::descriptor.type.name()}, true},
            {"", image, EGraphOrigin::IMPORTED, {GraphImageRange{}}, use}, sample ? 0u : 1u
        ));
        const auto color = take(context.declareResource({"target color", image}));
        const auto clear = take(context.addTransferPass(
            "independent graphics work", TransferWork{{ClearImage{{{color, 0}}}}}, {.estimated_gpu_ns = 100000}
        ));
        auto selected = imported;
        if (sample)
        {
            const auto generated = take(context.declareResource({"sampled pixels", image}));
            ComputeWork work;
            work.allow_async_compute = true;
            const GraphProgram program{kSharedImageRead.load(context.shaderTarget())};
            work.interface.program = take(context.addProgram(program));
            work.interface.resources = {
                {"selected", {imported}}, {"result", {{generated, 0}, {}, EGraphAccess::WRITE}}
            };
            work.dispatch = ImageDispatchArguments{generated};
            selected = take(context.addComputePass(
                "sample physical image", std::move(work), {.estimated_gpu_ns = 100000}
            )).written.front();
        }
        const auto capture = take(context.importFinalBuffer(
            ColorCapture::name, {ResourceName{GraphCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
                EGraphUse::TRANSFER_DESTINATION}
        ));
        const auto copy = take(context.addTransferPass(
            "source to readback", TransferWork{{CopyImageBuffer{
                {selected}, {capture}, 256, GraphCaptureBuffer::image_bytes, ImageCopyRegion{0, 0, extent, extent}
            }}}, {.estimated_gpu_ns = 1000}
        ));
        if (auto result = context.publish(ColorCapture::color, {{ColorCapture::color, clear.written.front()}}); !result)
            return result;
        return context.publish(ColorCapture::pixels, {{ColorCapture::pixels, copy.written.front()}});
    }

    CreateEffectRequest readerEffect(bool sample)
    {
        CreateEffectRequest effect;
        effect.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*effect.graph};
        const auto reader = take(edit.addNode(
            readerDefinition(), {std::string{reader_name}, 1}, sample ? "sample" : "copy"
        ));
        const auto output = edit.pinId(reader, ColorCapture::color);
        CHECK(edit.setOutput("color", output, true));
        CHECK(edit.setOutput("pixels", edit.pinId(reader, ColorCapture::pixels)));
        CHECK(edit.commit());
        return effect;
    }

    void verifyPixels(const CapturedPixels& pixels, std::uint32_t slot, std::uint8_t value)
    {
        CHECK(pixels.ready == 1 && pixels.frames.size() == 2 && pixels.pitch >= extent * 4);
        const auto& bytes = pixels.frames[slot];
        for (std::uint32_t y = 0; y < extent; ++y)
        {
            const auto row = std::span{bytes}.subspan(y * pixels.pitch, extent * 4);
            CHECK(std::ranges::all_of(row, [value](std::byte pixel) { return pixel == std::byte{value}; }));
        }
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"installed.shared.images", nativeValidationEnabled()};
    config.frames_in_flight = 2;
    config.uploads.emplace();
    DeviceOptions options;
    if (argc > 1 && std::string_view{argv[1]} == "d3d12") options.backend = EGraphicsBackend::D3D12;
    options.multiple_queues = true;
    options.graph_queue_costs = {100, 100, 100};
    auto server = take(RenderServer::create(config, **execution));
    take(server->registerResource<ImageStore>());
    take(server->registerResource<GraphCaptureBuffer>());
    const auto capability = readerDefinition();
    CHECK(server->registerEffectCapability(capability));
    EffectImplementation impl;
    impl.capability = capability.identity.id;
    impl.identity = {std::string{reader_name}, 1};
    impl.supported_outputs = {ColorCapture::color, ColorCapture::pixels};
    impl.initialize = [](EffectInitializationContext&, std::string_view selection) noexcept -> RenderResult<void>
    {
        if (selection != "sample" && selection != "copy") return lux::cxx::unexpected(RenderError{kInvalidConfig});
        return {};
    };
    impl.build = buildReader;
    CHECK(server->registerEffectImplementation(std::move(impl)));
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto& client = *requester;
    RenderResult<void> ran;
    std::thread worker([&] { ran = server->run(); });
    const auto commands = queryCommands(client);
    const auto device = call<CreateDeviceSession>(client, commands, options).device;
    const auto scene = call<CreateScene>(client, commands, {}).scene;
    const auto source = call<GetOrCreateResource>(
        client, commands, {device, scene, ImagePairDescriptor::descriptor.type}
    ).resource_id;
    if (!ImageStore::live->hasGateQueue())
    {
        server->stop();
        worker.join();
        return 77;
    }
    using Action = UpdateImages::EAction;
    call<UpdateImages>(client, commands, {source, Action::INSTALL, 17});
    std::array<RenderJobId, 2> jobs;
    std::array<CapturedPixels, 2> pixels;
    for (std::uint32_t i = 0; i < jobs.size(); ++i)
    {
        const auto view = call<CreateView>(client, commands, {scene}).view;
        const auto effect = createPreparedEffect(client, commands, device, readerEffect(i == 0));
        const auto target = call<CreateTarget>(
            client, commands, {device, {extent, extent, rdesc::ETextureFormat::RGBA8_UNORM}}
        ).target;
        const CaptureConfig capture_config{&pixels[i]};
        const auto capture = GraphCaptureBuffer::bind(client, commands, device, {scene, view}, capture_config);
        JobBinding binding;
        binding.resources = {{"source", source, i}, {std::string{ColorCapture::name}, capture}};
        jobs[i] = startRenderJob(client, commands, device, scene, view, target, effect, std::move(binding));
    }
    const auto reject = [&](Action action, std::uint8_t value, auto expected)
    {
        const auto failure = call<UpdateImages>(client, commands, {source, action, value}, false).error;
        if (failure.type != expected)
            std::fprintf(stderr, "action=%u unexpected error=%llu args=%llu,%llu\n",
                unsigned(action), failure.type, failure.args[0], failure.args[1]);
        CHECK(failure.type == expected);
    };
    reject(Action::REJECT_UPLOAD, 31, kDeviceFailure);
    reject(Action::INVALID_SECOND, 41, kDeviceArgument);
    reject(Action::EXCLUSIVE_SECOND, 47, kDeviceArgument);
    reject(Action::ALIAS, 53, kStorageMismatch);
    call<UpdateImages>(client, commands, {source, Action::STAGE, 73});
    ImageStore::live->gateReads(false);
    ImageStore::hold_next = true;
    auto other = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(other);
    auto pending = std::async(std::launch::async, [&] { return call<RequestRenderJob>(*other, commands, {jobs[0]}); });
    const auto deadline = Clock::now() + 10s;
    while (!ImageStore::parked)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(call<UpdateImages>(client, commands, {source, Action::ADOPT, 73}, false).error.type == kDeviceBusy);
    CHECK(ImageStore::frozen_rejections == 1);
    ImageStore::resume = true;
    ImageStore::resume.notify_all();
    const auto first = pending.get();
    const auto second = call<RequestRenderJob>(client, commands, {jobs[1]});
    call<UpdateImages>(client, commands, {source, Action::ADOPT, 73});
    CHECK(ImagePair::old_retired == 0);
    ImageStore::live->gateReads(true);
    const auto copy = call<RequestRenderJob>(client, commands, {jobs[1]});
    const auto sampled = call<RequestRenderJob>(client, commands, {jobs[0]});
    CHECK(first.slot_index != sampled.slot_index && second.slot_index != copy.slot_index);
    server->stop();
    worker.join();
    CHECK(ran && !server->stats().validation_errors && ImagePair::old_retired == 2);
    verifyPixels(pixels[0], first.slot_index, 17);
    verifyPixels(pixels[1], second.slot_index, 17);
    verifyPixels(pixels[0], sampled.slot_index, 73);
    verifyPixels(pixels[1], copy.slot_index, 73);
    std::puts("SHARED IMAGES: distinct layouts, frozen CPU and queued GPU reads protected, 17 -> 73, retired=2");
}
