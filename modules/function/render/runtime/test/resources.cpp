#include <lux/engine/render/RenderRuntime.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace lux::render;
    using namespace std::chrono_literals;
    RendererConfig config;
    config.validation = true;
    auto diagnostics = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::cerr << message << '\n';
        }
    };
    auto created = RenderRuntime::create(std::move(config), std::move(diagnostics));
    if (!created)
    {
        std::cerr << "Runtime startup failed: " << static_cast<unsigned>(created.error().code) << '\n';
        return 1;
    }
    auto runtime = std::move(*created);
    const auto pump = [&] {
        std::size_t controls = 2, programs = 1;
        const auto adopted = runtime->collectCompletions(3);
        assert(runtime->submitPending(controls, programs));
        assert(adopted && *adopted <= 3 && controls <= 2 && programs <= 1);
    };
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return;
            }
            pump();
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "The requested completion did not arrive");
    };

    // Fill the reply ring with a fixed upload set before Main adopts replies.
    // Once forwarded, no more requests/Programs may rescue a deferred reply:
    // freeing reply capacity must itself make the backend retry publication.
    std::array<TRenderRequest<Texture2DCreatedReply>, 8> textures;
    const std::array pixels{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    auto upload = runtime->upload();
    assert(upload);
    for (auto& request : textures)
    {
        auto submitted = upload->tryCreateTexture2DCopy(pixels, 1, 1, 4, EPixelFormat::RGBA8_UNORM, false);
        assert(submitted);
        request = std::move(*submitted);
    }
    std::size_t uploads = textures.size(), no_programs{};
    assert(runtime->submitPending(uploads, no_programs));
    assert(uploads == 0);
    std::this_thread::sleep_for(50ms);
    until([&] { return std::ranges::all_of(textures, [](const auto& request) { return request.isReady(); }); });
    assert(runtime->statistics().accepted_frames == 0);
    auto upload_control = runtime->control();
    assert(upload_control);
    for (auto& request : textures)
    {
        const auto result = request.tryResult();
        assert(result && result->get().status == 0 && result->get().handle.isValid());
        until([&] { return upload_control->get().canSubmit(); });
        upload_control->get().destroyTexture(result->get().handle);
    }

    assert(runtime->beginClose());
    bool complete{};
    until([&] {
        std::size_t replies = 8, controls = 4, programs = 1;
        auto closing = runtime->advanceClose(replies, controls, programs);
        assert(closing);
        complete = *closing == ERenderClose::COMPLETE;
        return complete;
    });
    assert(runtime->joinStopped());
    assert(runtime->status().state == ERenderRuntimeState::RETIRED);
    {
        auto automatic = RenderRuntime::create({});
        assert(automatic);
        auto scene = (*automatic)->control()->get().createScene({.name = "RAII pending creation"});
        automatic->reset(); // No manual poll/close/join; the request must reach a terminal state.
        assert(scene.isReady());
    }
    std::cout << "PASS upload reply-ring backpressure and idle transport retirement\n";
}
