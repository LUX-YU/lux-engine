#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include "ServerFixture.hpp"
#include "Support.hpp"
#include "TextureFixture.hpp"
#include "TypedTest.hpp"
#include <fstream>
#include <latch>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/data/Encoding.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>

using namespace foundation_test;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct TextureProbe final
{
    static constexpr std::string_view name = "test.textureProgress";

    struct Request final
    {
        RenderPayload source;
        std::uint32_t dimension;
    };

    using Response = RenderResult<Ack>;
};

static std::atomic<unsigned> sources{0}, pins{0}, completed{0}, submitted{0}, fail_at{0};
static std::atomic<bool> hold{false};
static std::atomic<VkResult> submit_error{VK_ERROR_OUT_OF_HOST_MEMORY};
static std::atomic<const std::byte*> observed{};

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "submit" && ++submitted == fail_at.load())
        {
            return submit_error.load();
        }
        if (name == "fence_status" && hold.load())
        {
            return VK_NOT_READY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* call, VkResult result) noexcept
    {
        if (std::string_view(call) == "cpuPrepare")
        {
            lux::render::test::CpuFixture::runBeforeWork();
        }
        return result;
    }

    void destroyed(const char*) noexcept {}
} // namespace lux::render::vulkan::test

struct Pin final
{
    Pin() noexcept
    {
        ++pins;
    }

    ~Pin() noexcept
    {
        --pins;
    }
};

static const auto operation = commandDescriptor<TextureProbe>();
static const std::array texture_operations{operation};
static const FeatureDescriptor definition{FeatureName("test.texture.progress"), {}};
static const FeatureGraphOps definition_graph{
    [](const void*, std::string_view, ResourceClaims&) noexcept -> RenderResult<void> { return {}; },
    nullptr
};

// This probe intentionally requires observed GPU completion. Production resource uploads
// publish after Copy/Acquire admission; readback/this terminal observation have the stronger contract.
static exec::task<TextureProbe::Response> invoke(DeviceSession& session, TextureProbe::Request value) noexcept
{
    Pin pin;
    observed.store(value.source.bytes().view().data());
    const ImageDescription description{
        {value.dimension, value.dimension},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    };
    auto image = session.allocator().createImage(description);
    if (!image)
    {
        co_return lux::cxx::unexpected(image.error());
    }
    std::vector<ImageTransferRegion> regions{
        {0,
         std::uint64_t(value.dimension) * 4,
         1,
         4,
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         {},
         {value.dimension, value.dimension, 1}}
    };
    auto uploaded = co_await session.uploads().writeImage(*image, value.source, std::move(regions));
    if (!uploaded)
    {
        co_return lux::cxx::unexpected(uploaded.error());
    }
    // The upload's final Acquire is on Graphics (or the aliased Copy queue). Capture its
    // actual latest Ticket on this Owner callback before any other work can be dispatched.
    auto& queue = session.queue();
    const auto ticket = queue.lastSubmitted();
    CHECK(ticket && queue.owns(*ticket));
    auto ready = co_await queue.whenComplete(*ticket);
    if (!ready)
    {
        co_return lux::cxx::unexpected(RenderError{kOutcomeUnknown});
    }
    const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
    if (stop.stop_requested())
    {
        co_return lux::cxx::unexpected(RenderError{kOutcomeUnknown});
    }
    CHECK(uploaded->completed());
    ++completed;
    co_return Ack{};
}

struct ProbeCommands final
{
    static constexpr std::string_view name = "test.texture.progress.commands";
    DeviceSession& device;

    explicit ProbeCommands(DeviceSession& value) noexcept : device(value) {}

    exec::task<TextureProbe::Response> uploadRequest(TextureProbe::Request&& input) noexcept
    {
        return invoke(device, std::move(input));
    }

    using Bindings = std::tuple<CommandMethod<TextureProbe, &ProbeCommands::uploadRequest>>;
};

template <class Predicate> static void until(Predicate predicate)
{
    const auto end = Clock::now() + 15s;
    while (!predicate())
    {
        CHECK(Clock::now() < end);
        std::this_thread::yield();
    }
}

static auto asset(unsigned dimension)
{
    ++sources;
    return std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(std::size_t(dimension) * dimension * 4, std::byte{37}),
        [](auto* value)
        {
            delete value;
            --sources;
        }
    );
}

static void shutdownAndLoss()
{
    struct Pause final
    {
        std::latch entered{1}, resume{1};
        std::atomic<unsigned> preparations{0};

        static void before(void* pointer) noexcept
        {
            auto& self = *static_cast<Pause*>(pointer);
            if (++self.preparations == 2)
            {
                self.entered.count_down();
                self.resume.wait();
            }
        }
    };

    for (bool lost : {false, true})
    {
        CpuFixture worker;
        Pause host;
        worker.setBeforeWork(&host, Pause::before);
        RenderServerConfig config{"runtime.texture.stop", true};
        vulkan::DeviceOptions config_device{.dynamic_rendering = true};
        config_device.multiple_queues = true;
        config.uploads = TransferOptions{1024 * 1024, 1, 32 * 1024 * 1024};
        auto server = take(createTestServer(worker, config, config_device));
        const auto catalog = take(server->commandId<ListCommands>());
        CHECK(server->registerFeature<TextureProbe>(
            feature_test::NativeProvider<ProbeCommands>{{&definition, &definition_graph, nullptr}}
        ));
        std::atomic<bool> run_finished{false};
        bool run_ok = false;
        std::thread render(
            [&]
            {
                run_ok = bool(server->run());
                run_finished = true;
            }
        );
        auto client = take(runtime_test::connect(config.channel_name, catalog));
        CHECK(runtime_test::refresh(*client));
        auto bound = take(runtime_test::find<TextureProbe>(*client));
        auto source = asset(2048);
        std::array payload{take(RenderPayload::fromVector(source, 0, source->size()))};
        if (lost)
        {
            submit_error = VK_ERROR_DEVICE_LOST;
            fail_at = submitted + 2;
        }
        const auto id = take(runtime_test::send<TextureProbe>(*client, bound, TextureProbe::Request{payload[0], 2048}));
        source.reset();
        payload = {};
        host.entered.wait();
        CHECK(sources == 1 && pins == 1 && !run_finished);
        if (!lost)
        {
            server->stop();
        }
        host.resume.count_down();
        until([&] { return run_finished.load(); });
        render.join();
        CHECK(run_ok != lost && pins == 0 && sources == 0);
        auto reply = wait(*client, id);
        CHECK(reply.get<TextureProbe::Response>().error().type == kOutcomeUnknown);
        reply = {};
        CHECK(sources == 0 && server->stats().validation_errors == 0);
        fail_at = 0;
        submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

static void productionShutdownAndLoss()
{
    struct Pause final
    {
        std::latch entered{1}, resume{1};
        std::atomic<unsigned> preparations{0};

        static void before(void* pointer) noexcept
        {
            auto& self = *static_cast<Pause*>(pointer);
            if (++self.preparations == 2)
            {
                self.entered.count_down();
                self.resume.wait();
            }
        }
    };

    for (bool lost : {false, true})
    {
        CpuFixture worker;
        Pause host;
        worker.setBeforeWork(&host, Pause::before);
        RenderServerConfig config{"runtime.texture.production.stop", true};
        vulkan::DeviceOptions config_device{.dynamic_rendering = true};
        config_device.multiple_queues = true;
        config_device.texture_descriptors = true;
        config.uploads = TransferOptions{1024 * 1024, 1, 32 * 1024 * 1024};
        auto server = take(createTestServer(worker, config, config_device));
        const auto catalog = take(server->commandId<ListCommands>());
        CHECK(server->registerStorage<lux::render::TextureStorage>({16, 32 * 1024 * 1024}));
        std::atomic<bool> run_finished{false};
        bool run_ok = false;
        std::thread render(
            [&]
            {
                run_ok = bool(server->run());
                run_finished = true;
            }
        );
        auto client = take(runtime_test::connect(config.channel_name, catalog));
        CHECK(runtime_test::refresh(*client));
        const auto storage = take(runtime_test::prepareStorage(*client, kTextureStorageType.name(), "default"));
        auto bound = take(runtime_test::find<SetTexture>(*client));
        auto source = asset(2048);
        SetTexture::Request input{
            {storage, {}},
            0,
            information(2048, 1, false),
            take(RenderPayload::fromVector(source, 0, source->size()))
        };
        const auto before = submitted.load();
        if (lost)
        {
            submit_error = VK_ERROR_DEVICE_LOST;
            fail_at = before + 2;
        }
        const auto id = take(runtime_test::send<SetTexture>(*client, bound, input));
        source.reset();
        input.source = {};
        host.entered.wait();
        // The production Texture handler has accepted a real first chunk. The CPU host holds
        // preparation of a later chunk; neither stop nor a failed later submit may free that source.
        CHECK(submitted > before && sources == 1 && !run_finished);
        if (!lost)
        {
            server->stop();
        }
        host.resume.count_down();
        until([&] { return run_finished.load(); });
        render.join();
        CHECK(run_ok != lost && sources == 0);
        auto reply = wait(*client, id);
        CHECK(reply.get<SetTexture::Response>().error().type == kOutcomeUnknown);
        CHECK(sources == 0 && server->stats().validation_errors == 0);
        // Error Reply owns only its RenderError; it never implicitly returns the accepted source.
        server.reset();
        CHECK(reply.get<SetTexture::Response>().error().type == kOutcomeUnknown);
        fail_at = 0;
        submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

int main()
{
    std::ofstream csv("texture-progress.csv");
    csv << "queue_mode,bytes,wall_ns,ticks,submissions\n";
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        CpuFixture host;
        RenderServerConfig config{"runtime.texture.progress", true};
        vulkan::DeviceOptions config_device{.dynamic_rendering = true};
        config_device.multiple_queues = mode != 0;
        config_device.prefer_dedicated_queues = mode == 2;
        config.uploads = TransferOptions{1024 * 1024, 1, 128 * 1024 * 1024};
        auto server = take(createTestServer(host, config, config_device));
        const auto catalog = take(server->commandId<ListCommands>());
        CHECK(server->registerFeature<TextureProbe>(
            feature_test::NativeProvider<ProbeCommands>{{&definition, &definition_graph, nullptr}}
        ));
        bool run_ok = false;
        std::thread render([&] { run_ok = bool(server->run()); });
        auto client = take(runtime_test::connect(config.channel_name, catalog));
        CHECK(runtime_test::refresh(*client));
        auto bound = take(runtime_test::find<TextureProbe>(*client));
        for (unsigned dimension : {512u, 2048u, 4096u})
        {
            auto source = asset(dimension);
            const auto* address = source->data();
            const auto size = source->size();
            std::array payload{take(RenderPayload::fromVector(source, 0, size))};
            const auto begin = Clock::now();
            const auto before = server->stats().ticks;
            const auto submits = submitted.load();
            auto id =
                take(runtime_test::send<TextureProbe>(*client, bound, TextureProbe::Request{payload[0], dimension}));
            source.reset();
            payload = {};
            auto reply = wait(*client, id);
            CHECK(reply.get<TextureProbe::Response>().has_value());
            until([] { return sources == 0 && pins == 0; });
            CHECK(observed == address);
            reply = {};
            CHECK(sources == 0);
            csv << mode << ',' << size << ',' << (Clock::now() - begin).count() << ',' << server->stats().ticks - before
                << ',' << submitted - submits << '\n';
        }
        // Partial submit failure and rejected CPU scheduling both settle exactly once and retain no pins.
        auto source = asset(2048);
        std::array payload{take(RenderPayload::fromVector(source, 0, source->size()))};
        fail_at = submitted + 2;
        auto id = take(runtime_test::send<TextureProbe>(*client, bound, TextureProbe::Request{payload[0], 2048}));
        CHECK(wait(*client, id).get<TextureProbe::Response>().has_value() == false);
        CHECK(pins == 0);
        fail_at = 0;
        host.reject(true);
        id = take(runtime_test::send<TextureProbe>(*client, bound, TextureProbe::Request{payload[0], 2048}));
        CHECK(wait(*client, id).get<TextureProbe::Response>().has_value() == false);
        host.reject(false);
        CHECK(pins == 0);
        source.reset();
        payload = {};
        CHECK(sources == 0);
        // Original Source remains alive after client close while GPU completion observation is held.
        hold = true;
        source = asset(512);
        payload = {take(RenderPayload::fromVector(source, 0, source->size()))};
        const auto before = submitted.load();
        (void)take(runtime_test::send<TextureProbe>(*client, bound, TextureProbe::Request{payload[0], 512}));
        source.reset();
        payload = {};
        until([&] { return submitted > before; });
        client.reset();
        CHECK(sources == 1 && pins == 1);
        hold = false;
        until([] { return pins == 0; });
        server->stop();
        render.join();
        CHECK(run_ok && sources == 0);
        CHECK(server->stats().validation_errors == 0);
        std::printf(
            "texture progress mode=%u ready=%u validation_errors=0 warnings=%llu\n",
            mode,
            completed.load(),
            server->stats().validation_warnings
        );
    }
    shutdownAndLoss();
    productionShutdownAndLoss();
}
