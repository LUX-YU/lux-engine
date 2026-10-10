#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>

#include <cassert>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <vector>

namespace
{
    struct Trace
    {
        std::vector<unsigned> released;
        unsigned unexpected_shutdowns{};
        unsigned frames{};
        unsigned views{};
        unsigned hook_destructions{};
    };

    class CompleteResource final
    {
    public:
        CompleteResource(Trace& trace, unsigned identity) noexcept : trace_(trace), identity_(identity) {}

        ~CompleteResource()
        {
            trace_.released.push_back(identity_);
        }

        CompleteResource(const CompleteResource&) = delete;
        CompleteResource& operator=(const CompleteResource&) = delete;
        CompleteResource(CompleteResource&&) = delete;
        CompleteResource& operator=(CompleteResource&&) = delete;

        // A method name is not permission to invoke an unrelated lifecycle protocol.
        void shutdown() noexcept
        {
            ++trace_.unexpected_shutdowns;
        }

    private:
        Trace& trace_;
        unsigned identity_;
    };

    struct HookBacking final
    {
        explicit HookBacking(Trace& trace) noexcept : trace(trace) {}

        ~HookBacking()
        {
            assert((trace.released == std::vector<unsigned>{3, 2, 1}));
            ++trace.hook_destructions;
        }

        Trace& trace;
    };
} // namespace

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<ResourceRegistry>);
    static_assert(!std::is_move_constructible_v<ResourceRegistry>);
    Trace trace;
    trace.released.reserve(3);
    std::weak_ptr<HookBacking> weak;
    {
        ResourceRegistry registry;
        auto& first = registry.ensure<CompleteResource>(trace, 1);
        auto& same = registry.ensure<CompleteResource>(trace, 99);
        assert(&first == &same && registry.find<CompleteResource>() == &first);
        auto second = registry.emplace<CompleteResource>(trace, 2);
        auto third_owner = std::make_unique<CompleteResource>(trace, 3);
        auto* third_address = third_owner.get();
        auto third = registry.insert(std::move(third_owner));
        assert(!third_owner && third.get() == third_address);
        assert(second.get() != &first && registry.size() == 3);
        assert(&registry.must<CompleteResource>() == &first);
        auto backing = std::make_shared<HookBacking>(trace);
        weak = backing;
        registry.addBeginFrameHook(EUploadPhase::UPLOAD, [backing](const FrameStamp&) { ++backing->trace.frames; });
        registry.addViewDestroyedHook(
            [backing](std::uint32_t scene, std::uint32_t view)
            {
                assert(scene == 10 && view == 20);
                ++backing->trace.views;
            }
        );
        backing.reset();
        for (const auto& hook : registry.beginFrameHooks(EUploadPhase::UPLOAD))
        {
            hook(FrameStamp{});
        }
        registry.notifySceneViewDestroyed(10, 20);
        assert(trace.frames == 1 && trace.views == 1 && !weak.expired());
    }
    assert((trace.released == std::vector<unsigned>{3, 2, 1}));
    assert(trace.hook_destructions == 1 && weak.expired());
    if (trace.unexpected_shutdowns != 0)
    {
        std::fprintf(
            stderr,
            "FAIL registry invoked undeclared shutdown protocol %u times\n",
            trace.unexpected_shutdowns
        );
        return 1;
    }
    std::puts("PASS complete owners, stable discovery, reverse destruction and callback backing lifetime");
}
