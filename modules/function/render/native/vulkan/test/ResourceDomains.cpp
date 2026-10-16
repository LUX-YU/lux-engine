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
        const auto scene = take(resources.createDomain(resources.root(), EResourceScope::SCENE));
        const auto view = take(resources.createDomain(scene, EResourceScope::VIEW));
        const native_fixture::Config config{32, 32, 1, 1};
        auto assembly = take(native_fixture::assemble(config));
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings,
            native_fixture::programs(config), native_fixture::target(config))));
        CHECK(!session->prepare(code, native_fixture::target(config), other->resources().root()));
        CHECK(!session->prepare(code, native_fixture::target(config), resources.root()));
        for (unsigned failure = 1; failure <= 6; ++failure)
        {
            fail_buffer = failure;
            CHECK(!session->prepare(code, native_fixture::target(config), view));
            CHECK(fail_buffer == 0 && resources.size() == 0);
            CHECK((live == std::array<int, 3>{}));
        }
        auto* accepted = take(session->prepare(code, native_fixture::target(config), view));
        CHECK(resources.size() == 4 && take(accepted->execute()));
        const auto ticket = *accepted->lastUse();
        const auto before = live;
        fail_buffer = 1;
        CHECK(!session->prepare(code, native_fixture::target(config), view));
        CHECK(resources.size() == 4 && live == before);
        hold_completion = true;
        CHECK(session->release(accepted));
        CHECK(resources.closeDomain(scene));
        CHECK(session->collect() && live == before && resources.size() == 4);
        CHECK(!session->prepare(code, native_fixture::target(config), view));
        hold_completion = false;
        CHECK(take(session->queue().wait(ticket, UINT64_MAX)) && session->collect());
        CHECK(resources.size() == 1); // the Root instance alone survives Scene close
        CHECK(resources.closeDomain(resources.root()));
        CHECK(resources.size() == 0 && (live == std::array<int, 3>{}));
    }
    CHECK(validation.errors == 0);
    std::printf("Registry GPU candidate rollback=6; stale/cross-session/scope rejection; held-ticket domain-close; errors=0 warnings=%u\n",
        validation.warnings.load());
}
