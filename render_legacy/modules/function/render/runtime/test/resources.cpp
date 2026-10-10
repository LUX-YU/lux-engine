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
    auto diagnostics = [](auto severity, auto message)
    {
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
    const auto pump = [&]
    {
        std::size_t controls = 2, programs = 1;
        const auto adopted = runtime->collectCompletions(3);
        assert(runtime->submitPending(controls, programs));
        assert(adopted && *adopted <= 3 && controls <= 2 && programs <= 1);
    };
    const auto until = [&](auto condition)
    {
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

    // Destruction must not stop the backend while prior packets are queued
    // behind response-ring backpressure. These requests precede close admission.
    until([&] { return runtime->controlAvailable(); });
    auto final_scene = runtime->control()->get().createScene({.name = "accepted before runtime close"});
    assert(final_scene.valid());
    runtime.reset(); // Application-global physical barrier; no public close protocol.
    assert(final_scene.isReady());
    const auto final_result = final_scene.tryResult();
    assert(final_result && final_result->get().error.ok());
    {
        auto automatic = RenderRuntime::create({});
        assert(automatic);
        auto scene = (*automatic)->control()->get().createScene({.name = "RAII pending creation"});
        automatic->reset(); // No manual poll/close/join; the request must reach a terminal state.
        assert(scene.isReady());
    }
    {
        auto stopped = RenderRuntime::create({});
        assert(stopped);
        auto control = (*stopped)->control();
        assert(control);
        std::array<TRenderRequest<SceneCreatedReply>, 8> pending;
        std::array<unsigned, 8> deliveries{};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            pending[index] = control->get().createScene({.name = "terminal queued control"});
            assert(pending[index].valid());
            assert(pending[index].then(
                [&, index](const Expected<SceneCreatedReply>& outcome) noexcept
                {
                    ++deliveries[index];
                    assert(pending[index].isReady());
                    assert(outcome.has_value() == !pending[index].failed());
                    if (!outcome)
                    {
                        assert(outcome.error().type == pending[index].error().type);
                        assert(outcome.error().args == pending[index].error().args);
                    }
                    const auto reentrant = (*stopped)->collectCompletions(1);
                    assert(!reentrant && reentrant.error().code == ERendererError::BUSY);
                }
            ));
        }
        control->get().requestStop(); // Real backend stop, with outstanding accepted requests.
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while ((*stopped)->status().state != ERenderRuntimeState::RETIRED)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            const auto collected = (*stopped)->collectCompletions(1);
            assert(collected && *collected <= 1);
            std::this_thread::sleep_for(1ms);
        }
        unsigned failed{};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            assert(pending[index].isReady() && deliveries[index] == 1);
            const auto result = pending[index].tryResult();
            if (result)
            {
                assert(result->get().error.ok() && result->get().scene_id.isValid());
            }
            else
            {
                ++failed;
                assert(result.error().type == renderError<err::comm::ChannelStopping>().type);
            }
        }
        assert(failed != 0); // The full response ring must exercise terminal settlement.
        assert((*stopped)->collectCompletions(1).value() == 0);
        stopped->reset();
        assert(std::ranges::all_of(deliveries, [](auto value) { return value == 1; }));
        std::cout << "PASS terminal backend: " << failed << " failures, eight exact completions, budget one\n";
    }
    // An accepted upload is owned independently of both its public client and
    // Runtime. Exercise both the local admission queue and forwarded packets.
    for (const bool forward : {false, true})
    {
        auto owner = RenderRuntime::create({});
        assert(owner);
        auto client = (*owner)->upload();
        assert(client);
        std::array<TRenderRequest<Texture2DCreatedReply>, 8> requests;
        for (auto& request : requests)
        {
            auto accepted = client->tryCreateTexture2DCopy(pixels, 1, 1, 4, EPixelFormat::RGBA8_UNORM, false);
            assert(accepted);
            request = std::move(*accepted);
        }
        if (forward)
        {
            std::size_t commands = requests.size(), programs{};
            assert((*owner)->submitPending(commands, programs));
            assert(commands == 0);
        }
        owner->reset();
        for (const auto& request : requests)
        {
            assert(request.isReady());
            const auto result = request.tryResult();
            assert(result && result->get().status == 0 && result->get().handle.isValid());
        }
        const auto late = client->tryCreateTexture2DCopy(pixels, 1, 1, 4, EPixelFormat::RGBA8_UNORM, false);
        assert(!late && late.error() == ERenderUploadSubmitError::STOPPING);
    }
    RendererConfig unsupported;
    unsupported.instance_extensions.emplace_back("VK_LUX_nonexistent_runtime_test_extension");
    const auto failed_startup = RenderRuntime::create(std::move(unsupported));
    assert(!failed_startup && failed_startup.error().code == ERendererError::DEVICE_FAILURE);
    auto after_failed_startup = RenderRuntime::create({});
    assert(after_failed_startup);
    after_failed_startup->reset();
    std::cout << "PASS upload reply-ring backpressure and idle transport retirement\n";
}
