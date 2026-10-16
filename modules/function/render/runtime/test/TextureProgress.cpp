#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
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
            if (!bytes) return cxx::unexpected(RenderError{kWireLimit});
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

static RenderResult<void> ready(Pin&, TextureUpload& upload, DeviceSession& session) noexcept
{
    CHECK(upload.lastUse() && session.queue().owns(*upload.lastUse()));
    CHECK(session.queue().completed() >= upload.lastUse()->serial());
    ++completed;
    return {};
}

static const auto operation = commandDescriptor<TextureProbe>();
static const std::array texture_operations{operation};
static const FeatureDefinition definition{FeatureTypeIdView("test.texture.progress"), {1, 0}, nullptr,
    [](const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; },
    nullptr, texture_operations};

struct ProbeWork final
{
    std::unique_ptr<TextureUpload> upload;
    Pin pin;
    bool unstartedIn(const DeviceSession& session) const noexcept { return upload->unstartedIn(session); }
    RenderResult<bool> advance() noexcept { return upload->advance(); }
    void setCompletionPost(CompletionPost post, std::shared_ptr<const void> implementation,
        std::shared_ptr<const void> registration) noexcept
    {
        upload->setCompletionPost(post, std::move(implementation), std::move(registration));
    }
    void cancel() noexcept { upload->cancel(); }
    std::optional<SubmissionTicket> lastUse() const noexcept { return upload->lastUse(); }
};

static RenderResult<void> invoke(void* native, CommandInvocation& invocation) noexcept
{
    CommandContext<TextureProbe> context(invocation);
    CHECK(!context.complete(Ack{})); // ordinary reply cannot manufacture GPU readiness
    auto& session = *static_cast<DeviceSession*>(native);
    Ack response;
    auto reservation = GpuCompletionReservation::reserve(invocation, session.queue(), &response);
    if (!reservation) return lux::cxx::unexpected(reservation.error());
    const auto& value = context.request();
    const auto source = value.source.bytes().view();
    observed.store(source.data());
    lux::rdesc::TextureInfo info{};
    info.width = info.height = value.dimension;
    info.channel = 4;
    info.mip_count = 1;
    info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = lux::rdesc::ETextureColorSpace::DATA;
    info.mip_ranges[0] = {0, source.size(), value.dimension, value.dimension};
    auto candidate = vulkan::TextureUpload::create(session, info, source);
    if (!candidate) return lux::cxx::unexpected(candidate.error());
    auto probe = std::make_unique<ProbeWork>();
    probe->upload = std::move(*candidate);
    auto work = GpuWork::pending<[](ProbeWork& value, DeviceSession& session) noexcept {
        return ready(value.pin, *value.upload, session);
    }>(session, probe);
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
        config.transfer_queues = true;
        config.uploads = TransferOptions{1024 * 1024, 1, 32 * 1024 * 1024};
        auto server = take(RenderServer::create(withCpu(worker, config)));
        const std::array handlers{CommandHandler{&operation, invoke, nullptr, ECommandOwner::DEVICE_SESSION, true}};
        CHECK(server->registerFeature<TextureProbe>({&definition, nullptr, handlers}));
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
        config.transfer_queues = mode != 0;
        config.prefer_dedicated_transfer = mode == 2;
        config.completion_capacity = 1;
        config.uploads = TransferOptions{1024 * 1024, 1, 128 * 1024 * 1024};
        auto server = take(RenderServer::create(withCpu(host, config)));
        const std::array handlers{CommandHandler{&operation, invoke, nullptr, ECommandOwner::DEVICE_SESSION, true}};
        CHECK(server->registerFeature<TextureProbe>({&definition, nullptr, handlers}));
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
