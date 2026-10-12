// Fault seams are test-only. The production Server source is compiled unchanged.
#define main rd3OrdinaryTestsMain
#include "RD3Runtime.cpp"
#undef main
#include "RuntimeChannel.hpp"

namespace
{
    std::atomic<unsigned> host_failure{}, pipeline_creations{};
    std::atomic<unsigned> fail_submit{}, fail_image{};
    constexpr std::array names{
        "instance",
        "messenger",
        "device",
        "allocator",
        "buffer",
        "image",
        "image_alias",
        "image_view",
        "sampler",
        "descriptor_layout",
        "descriptor_pool",
        "shader",
        "pipeline_layout",
        "pipeline",
        "graphics_pipeline",
        "command_pool",
        "fence",
        "timeline"
    };
    std::array<std::atomic<unsigned>, names.size()> live{};

    bool trip(std::atomic<unsigned>& counter) noexcept
    {
        auto value = counter.load();
        return value != 0 && counter.fetch_sub(1) == 1;
    }

    HANDLE faultEvent(LPSECURITY_ATTRIBUTES a, BOOL b, BOOL c, LPCWSTR d) noexcept
    {
        if (host_failure.load() == 1)
        {
            host_failure = 0;
            SetLastError(ERROR_NOT_ENOUGH_QUOTA);
            return nullptr;
        }
        return CreateEventW(a, b, c, d);
    }

    HANDLE faultThread(
        LPSECURITY_ATTRIBUTES a,
        SIZE_T b,
        LPTHREAD_START_ROUTINE c,
        LPVOID d,
        DWORD e,
        LPDWORD f
    ) noexcept
    {
        if (host_failure.load() == 2)
        {
            host_failure = 0;
            SetLastError(ERROR_NOT_ENOUGH_QUOTA);
            return nullptr;
        }
        return CreateThread(a, b, c, d, e, f);
    }
} // namespace

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name{operation};
        if ((name == "submit" && trip(fail_submit)) || (name == "image" && trip(fail_image)))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        const std::string_view name{operation};
        if (result == VK_SUCCESS && (name == "pipeline" || name == "graphics_pipeline"))
        {
            ++pipeline_creations;
        }
        auto i = std::find(names.begin(), names.end(), name);
        if (i != names.end() && result == VK_SUCCESS)
        {
            ++live[i - names.begin()];
        }
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        auto i = std::find(names.begin(), names.end(), std::string_view{operation});
        CHECK(i != names.end() && live[i - names.begin()].fetch_sub(1) > 0);
    }
} // namespace lux::render::vulkan::test

#define CreateEventW faultEvent
#define CreateThread faultThread
#include "../src/Backend.cpp"
#undef CreateThread
#undef CreateEventW

void emptyOwners()
{
    for (std::size_t i = 0; i < live.size(); ++i)
    {
        if (live[i] != 0)
        {
            std::fprintf(stderr, "LIVE %s %u\n", names[i], live[i].load());
        }
        CHECK(live[i] == 0);
    }
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    BackendOptions native;
    native.instance = {{}, true, diagnostic, &validation};
    native.device.dynamic_rendering = true;
    native.device.multiple_queues = true;
    RuntimeOptions options;
    options.frame_interval_ns = 2'000'000;

    host_failure = 1;
    CHECK(!RenderBackend::create(native, options));
    CHECK(host_failure == 0);
    emptyOwners();
    {
        auto server = checked(RenderBackend::create(native, options));
        host_failure = 2;
        CHECK(!server->start());
        CHECK(host_failure == 0 && server->assemblyDevice());
        auto discarded = effect(argv[1]);
        auto stale_effect = checked(server->addEffect(std::move(discarded.source)));
        CHECK(server->removeEffect(stale_effect));
        CHECK(!server->removeEffect(stale_effect));
        auto fixture = effect(argv[1]);
        auto selected = checked(server->addEffect(std::move(fixture.source)));
        CHECK(selected.local.index == stale_effect.local.index && selected.local.gen != stale_effect.local.gen);
        const auto pipelines = pipeline_creations.load();
        CHECK(pipelines > 0);
        auto duplicate = effect(argv[1], 7.0f);
        auto same_graph_effect = checked(server->addEffect(std::move(duplicate.source)));
        CHECK(pipeline_creations == pipelines);
        CHECK(server->removeEffect(same_graph_effect));
        auto client = checked(server->start());
        const auto owner = client->ownerIdentity();
        auto s = await(checked(client->createScene()));
        auto t = await(checked(client->createTarget(4, 4)));
        RuntimeScene scene{owner, {s.index, s.generation}};
        RuntimeTarget target{owner, {t.index, t.generation}};
        CHECK(!receive(checked(client->createView(scene, target, stale_effect))));
        fail_image = 1;
        CHECK(!receive(checked(client->createView(scene, target, selected))));
        CHECK(fail_image == 0);
        // Native allocation failure leaves the Scene and Target available for retry.
        auto v = await(checked(client->createView(scene, target, selected)));
        RuntimeView view{owner, {{v.scene_index, v.scene_generation}, {v.index, v.generation}}};
        const auto graph = await(checked(client->graph(view)));
        CHECK(graph.index == 0 && graph.generation == 2);
        frames(*client, 3);
        CHECK(client->graph(view));
        fail_submit = 2; // one real queue submission is accepted before the failure
        for (unsigned i = 0; i < 5000 && client->state() != ERuntimeState::FAILED; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(fail_submit == 0 && client->state() == ERuntimeState::FAILED);
        CHECK(client->terminalError());
        server.reset(); // must wait accepted work before destroying Native owners
        CHECK(!client->createScene());
        client.reset();
    }
    emptyOwners();
    CHECK(validation.errors == 0);
    std::puts("RD3 event/thread rollback, Native allocation retry, partial-submit drain, owners=0 PASS");
}
