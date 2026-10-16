#include <lux/engine/render/native/vulkan/Registration.hpp>
#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <fstream>
#include <latch>

using namespace foundation_test;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct TextureProbe final
{
    static constexpr std::string_view name = "test.textureProgress";
    static constexpr auto completion = ECompletionContract::RESOURCE_GPU_READY;
    struct Request final { RenderPayload source; std::uint32_t dimension; };
    using Response = Ack;
};
namespace lux::render
{
    template <> struct CommandValueTraits<TextureProbe::Request>
    {
        static RenderResult<std::size_t> retainedBytes(const TextureProbe::Request& value) noexcept
        {
            const auto bytes = cxx::checkedAdd(sizeof(value), value.source.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kEncodingLimit});
            return *bytes;
        }
    };
}

static std::atomic<unsigned> sources{0}, pins{0}, completed{0}, submitted{0}, fail_at{0};
static std::atomic<bool> hold{false};
static std::atomic<VkResult> submit_error{VK_ERROR_OUT_OF_HOST_MEMORY};
static std::atomic<const std::byte*> observed{};
namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "submit" && ++submitted == fail_at.load()) return submit_error.load();
        if (name == "fence_status" && hold.load()) return VK_NOT_READY;
        return VK_SUCCESS;
    }
    VkResult after(const char*, VkResult result) noexcept { return result; }
    void destroyed(const char*) noexcept {}
}

struct Pin final
{
    Pin() noexcept { ++pins; }
    ~Pin() noexcept { --pins; }
};

static RenderResult<void> ready(Pin&, Transfer& upload, DeviceSession& session) noexcept
{
    CHECK(upload.lastSubmission() && session.queue().owns(*upload.lastSubmission()));
    CHECK(upload.requirements().completed() && upload.result());
    ++completed;
    return {};
}

static const auto operation = commandDescriptor<TextureProbe>();
static const std::array texture_operations{operation};
static const FeatureDescriptor definition{FeatureName("test.texture.progress"), {1, 0}, nullptr};
static const FeatureGraphOps definition_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; }, nullptr};

static RenderResult<void> invoke(DeviceSession& session, const TextureProbe::Request& value, Reply<TextureProbe>& context) noexcept
{
    CHECK(!context.complete(Ack{})); // ordinary reply cannot manufacture GPU readiness
    Ack response;
    auto reservation = GpuCompletionReservation::reserve(context, session.queue(), response);
    if (!reservation) return lux::cxx::unexpected(reservation.error());
    const auto source = value.source.bytes().view();
    observed.store(source.data());
    const ImageDescription description{{value.dimension, value.dimension}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    const ImageTransferRegion region{0, std::uint64_t(value.dimension) * 4, 1, 4,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {value.dimension, value.dimension, 1}};
    auto transfer = session.uploadImage(description, source, std::span(&region, 1));
    if (!transfer) return lux::cxx::unexpected(transfer.error());
    auto pin = std::make_unique<Pin>();
    auto work = GpuWork::pending<ready>(session, *transfer, pin);
    if (!work) return lux::cxx::unexpected(work.error());
    return reservation->commitWork(*work);
}

template <class Predicate> static void until(Predicate predicate)
{
    const auto end = Clock::now() + 15s;
    while (!predicate()) { CHECK(Clock::now() < end); std::this_thread::yield(); }
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    RenderCompletion reply;
    until([&] {
        const auto result = take(client.tryReceive(reply));
        CHECK(result != ERenderReceive::CLOSED);
        return result == ERenderReceive::RECEIVED;
    });
    CHECK(reply.header.request == id);
    return reply;
}

static auto asset(unsigned dimension)
{
    ++sources;
    return std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(std::size_t(dimension) * dimension * 4, std::byte{37}),
        [](auto* value) { delete value; --sources; });
}

static void shutdownAndLoss()
{
    struct Pause final
    {
        std::latch entered{1}, resume{1};
        std::atomic<bool> first{true};
        static void before(void* pointer) noexcept
        {
            auto& self = *static_cast<Pause*>(pointer);
            if (self.first.exchange(false))
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
        config.device.multiple_queues = true;
        config.uploads = TransferOptions{1024 * 1024, 1, 32 * 1024 * 1024};
        auto server = take(RenderServer::create(withCpu(worker, config)));
        const std::array handlers{nativeCommand<TextureProbe, invoke>()};
        CHECK(server->registerFeature<TextureProbe>({&definition, &definition_graph, nullptr, handlers}));
        std::atomic<bool> run_finished{false};
        bool run_ok = false;
        std::thread render([&] { run_ok = bool(server->run()); run_finished = true; });
        auto client = take(RenderClient::create(config.channel_name));
        CHECK(wait(*client, take(client->hello())).header.outcome == ECommandOutcome::SUCCESS);
        auto bound = take(client->bind<TextureProbe>());
        auto source = asset(2048);
        std::array payload{take(RenderPayload::fromVector(source, 0, source->size()))};
        if (lost) { submit_error = VK_ERROR_DEVICE_LOST; fail_at = submitted + 2; }
        const auto id = take(submitValue(*client, bound, TextureProbe::Request{payload[0], 2048}));
        source.reset(); payload = {};
        host.entered.wait();
        CHECK(sources == 1 && pins == 1 && !run_finished);
        if (!lost) server->stop();
        host.resume.count_down();
        until([&] { return run_finished.load(); });
        render.join();
        CHECK(run_ok != lost && pins == 0 && sources == 1);
        auto reply = wait(*client, id);
        CHECK(reply.header.outcome == ECommandOutcome::OUTCOME_UNKNOWN);
        reply = {};
        CHECK(sources == 0 && server->stats().validation_errors == 0);
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
        config.device.multiple_queues = mode != 0;
        config.device.prefer_dedicated_queues = mode == 2;
        config.completion_capacity = 1;
        config.uploads = TransferOptions{1024 * 1024, 1, 128 * 1024 * 1024};
        auto server = take(RenderServer::create(withCpu(host, config)));
        const std::array handlers{nativeCommand<TextureProbe, invoke>()};
        CHECK(server->registerFeature<TextureProbe>({&definition, &definition_graph, nullptr, handlers}));
        bool run_ok = false;
        std::thread render([&] { run_ok = bool(server->run()); });
        auto client = take(RenderClient::create(config.channel_name, {128 * 1024 * 1024}));
        CHECK(wait(*client, take(client->hello())).header.outcome == ECommandOutcome::SUCCESS);
        auto bound = take(client->bind<TextureProbe>());
        for (unsigned dimension : {512u, 2048u, 4096u})
        {
            auto source = asset(dimension);
            const auto* address = source->data();
            const auto size = source->size();
            std::array payload{take(RenderPayload::fromVector(source, 0, size))};
            const auto begin = Clock::now();
            const auto before = server->stats().ticks;
            const auto submits = submitted.load();
            auto id = take(submitValue(*client, bound, TextureProbe::Request{payload[0], dimension}));
            source.reset(); payload = {};
            auto reply = wait(*client, id);
            CHECK(reply.header.outcome == ECommandOutcome::SUCCESS);
            CHECK(observed == address && sources == 1 && pins == 0);
            reply = {};
            CHECK(sources == 0 && client->retainedBytes() == 0);
            csv << mode << ',' << size << ',' << (Clock::now() - begin).count() << ','
                << server->stats().ticks - before << ',' << submitted - submits << '\n';
        }
        // Partial submit failure and rejected CPU scheduling both settle exactly once and retain no pins.
        auto source = asset(2048);
        std::array payload{take(RenderPayload::fromVector(source, 0, source->size()))};
        fail_at = submitted + 2;
        auto id = take(submitValue(*client, bound, TextureProbe::Request{payload[0], 2048}));
        CHECK(wait(*client, id).header.outcome == ECommandOutcome::FAILED);
        CHECK(pins == 0);
        fail_at = 0;
        host.reject(true);
        id = take(submitValue(*client, bound, TextureProbe::Request{payload[0], 2048}));
        CHECK(wait(*client, id).header.outcome == ECommandOutcome::FAILED);
        host.reject(false);
        CHECK(pins == 0);
        source.reset(); payload = {};
        CHECK(sources == 0);
        // Original Source remains alive after client close while GPU completion observation is held.
        hold = true;
        source = asset(512);
        payload = {take(RenderPayload::fromVector(source, 0, source->size()))};
        const auto before = submitted.load();
        (void)take(submitValue(*client, bound, TextureProbe::Request{payload[0], 512}));
        source.reset(); payload = {};
        until([&] { return submitted > before; });
        client.reset();
        CHECK(sources == 1 && pins == 1);
        hold = false;
        until([] { return pins == 0; });
        server->stop(); render.join();
        CHECK(run_ok && sources == 0);
        CHECK(server->stats().validation_errors == 0);
        std::printf("texture progress mode=%u ready=%u validation_errors=0 warnings=%llu\n", mode,
            completed.load(), server->stats().validation_warnings);
    }
    shutdownAndLoss();
}
