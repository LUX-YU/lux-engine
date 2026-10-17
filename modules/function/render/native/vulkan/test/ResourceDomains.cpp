#include "Support.hpp"
#include "SceneFixture.hpp"
#include "BufferStorageFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

using namespace lux::render;
using namespace lux::render::vulkan;
using namespace foundation_test;
static unsigned fail_buffer = 0;
static std::array<int, 3> live{};
static constexpr std::array names{"buffer", "image", "image_view"};
static bool hold_completion = false;

namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "fence_status" && hold_completion) return VK_NOT_READY;
        if (std::string_view(call) == "buffer" && fail_buffer && --fail_buffer == 0)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS)
            for (unsigned i = 0; i < names.size(); ++i) if (std::string_view(call) == names[i]) ++live[i];
        return result;
    }
    void destroyed(const char* call) noexcept
    {
        for (unsigned i = 0; i < names.size(); ++i) if (std::string_view(call) == names[i]) --live[i];
    }
}

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        auto other = take(DeviceSession::create(bootstrap, options));
        CHECK(buffer_fixture::install(*session));
        auto& resources = session->resources();
        StorageSet scene(resources);
        StorageSet view(resources);
        const native_fixture::Config config{32, 32, 1, 1};
        auto assembly = take(native_fixture::assemble(config));
        for (auto* group : {&assembly.external_bindings.global, &assembly.external_bindings.scene,
            &assembly.external_bindings.view, &assembly.external_bindings.attachment})
            for (auto& binding : *group) binding.storage.config = take(ConfigValue::make(buffer_fixture::Config{16}));
        {
            auto wrong = assembly.external_bindings;
            wrong.global.front().storage.config = take(ConfigValue::make(std::uint32_t{16}));
            auto wrong_code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly.definition,
                assembly.analysis, wrong, native_fixture::programs(config), native_fixture::target(config))));
            CHECK(!session->prepare(wrong_code, native_fixture::target(config), &scene, &view));
            CHECK(resources.size() == 0 && (live == std::array<int, 3>{}));
        }
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings,
            native_fixture::programs(config), native_fixture::target(config))));
        CHECK(!session->prepare(code, native_fixture::target(config), &other->globalStorages(), &view));
        CHECK(!session->prepare(code, native_fixture::target(config)));
        for (unsigned failure = 1; failure <= 6; ++failure)
        {
            fail_buffer = failure;
            CHECK(!session->prepare(code, native_fixture::target(config), &scene, &view));
            CHECK(fail_buffer == 0 && resources.size() == 0);
            CHECK((live == std::array<int, 3>{}));
        }
        auto* accepted = take(session->prepare(code, native_fixture::target(config), &scene, &view));
        CHECK(resources.size() == 4 && take(accepted->execute()));
        const auto ticket = *accepted->lastUse();
        const auto before = live;
        fail_buffer = 1;
        CHECK(!session->prepare(code, native_fixture::target(config), &scene, &view));
        CHECK(resources.size() == 4 && live == before);
        hold_completion = true;
        CHECK(session->release(accepted));
        view.clear();
        scene.clear();
        // Logical identity ends now; only physical retirement owns the submitted buffers.
        CHECK(resources.size() == 0);
        CHECK(session->collect() && live == before);
        CHECK(!session->prepare(code, native_fixture::target(config), &scene, &view));
        hold_completion = false;
        CHECK(take(session->queue().wait(ticket, UINT64_MAX)) && session->collect());
        CHECK(resources.size() == 0 && (live == std::array<int, 3>{}));
        // Explicit preload survives Attach removal but does not merge distinct actual owners.
        const auto type = resources.typeId(buffer_fixture::description.type);
        auto global = take(session->globalStorages().prepare(type, "preload"));
        StorageSet rebuilt_scene(resources);
        auto local = take(rebuilt_scene.prepare(type, "preload"));
        CHECK(global != local && resources.size() == 2);
        rebuilt_scene.clear();
        CHECK(!resources.native(local) && resources.native(global));
        CHECK(session->globalStorages().releasePrepared(global));
        CHECK(resources.size() == 0 && (live == std::array<int, 3>{}));
    }
    CHECK(validation.errors == 0);
    std::printf("Owner GPU candidate rollback=6; closed/cross-session owner rejection; held-ticket owner-close; errors=0 warnings=%u\n",
        validation.warnings.load());
}
