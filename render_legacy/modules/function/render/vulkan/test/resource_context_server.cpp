#include <lux/engine/render/comm/server/RenderServer.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <type_traits>

template <class T>
concept HasServerInit = requires(T& server) { server.init(lux::render::ServerConfig{}); };
static_assert(!HasServerInit<lux::render::GeneralRenderServer>);
static_assert(!std::is_constructible_v<
              lux::render::GeneralRenderServer,
              std::shared_ptr<lux::render::GeneralRenderServer::Channel>,
              std::shared_ptr<lux::render::TRenderControlChannel<>>,
              std::shared_ptr<lux::render::TRenderUploadChannel<>>,
              std::shared_ptr<lux::render::RenderChannelSync>>);
static_assert(!std::is_copy_constructible_v<lux::render::GeneralRenderServer>);
static_assert(!std::is_move_constructible_v<lux::render::GeneralRenderServer>);

// Separate TU: this calls the real backend DLL, without the native pool probes.
void checkServerStartupPrefix()
{
    using namespace lux::render;
    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        ServerConfig config;
        config.capacity_request.set(capacityId("test.unknown.capacity"), CapacityValue::exact(1));
        const auto result = GeneralRenderServer::create(
            TRenderProgramChannel<>::create(2),
            TRenderControlChannel<>::create(2),
            TRenderUploadChannel<>::create(2, 1024),
            std::make_shared<RenderChannelSync>(),
            std::move(config)
        );
        assert(!result && isError<err::memory::CapacityExhausted>(result.error()));
        // Device creation succeeded, but ResourceContext has not been adopted.
        // Destruction must not access pools or dependent frame/target owners.
    }

    for (const auto frames : {0u, kMaxFramesInFlight + 1u})
    {
        ServerConfig config;
        config.frames_in_flight = frames;
        auto rejected = GeneralRenderServer::create(
            TRenderProgramChannel<>::create(2),
            TRenderControlChannel<>::create(2),
            TRenderUploadChannel<>::create(2, 1024),
            std::make_shared<RenderChannelSync>(),
            config
        );
        assert(!rejected && isError<err::device::InvalidFramesInFlight>(rejected.error()));
        assert(rejected.error().args[0] == frames && rejected.error().args[1] == kMaxFramesInFlight);
    }
    std::atomic<int> validation_errors{};
    for (unsigned iteration = 0; iteration < 4; ++iteration)
    {
        auto sync = std::make_shared<RenderChannelSync>();
        ServerConfig config;
        config.enable_validation = true;
        config.validation_error_counter = &validation_errors;
        std::fprintf(stderr, "server iteration %u prepare\n", iteration);
        auto server = GeneralRenderServer::create(
            TRenderProgramChannel<>::create(2),
            TRenderControlChannel<>::create(2),
            TRenderUploadChannel<>::create(2, 1024),
            sync,
            config
        );
        std::fprintf(stderr, "server iteration %u prepared=%d\n", iteration, bool(server));
        assert(server && (*server)->deviceCaps());
        const auto before = (*server)->uploadLifecycle();
        assert(before.accepted == 0 && before.active == 0);
        sync->requestStop();
        std::fprintf(stderr, "server iteration %u close\n", iteration);
        const auto closed = (*server)->closeAcceptedUploads();
        assert(closed.active == 0 && closed.accepted == 0);
        std::fprintf(stderr, "server iteration %u destroy\n", iteration);
        server->reset();
        std::fprintf(stderr, "server iteration %u destroyed\n", iteration);
        assert(validation_errors.load() == 0);
    }
    std::puts("server construction: invalid frames/capacity publish no server; retry/complete backing/worker close PASS"
    );
}
