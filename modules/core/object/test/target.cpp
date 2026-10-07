#include <cassert>
#include <cstdio>
#include <lux/engine/object/LuxObject.hpp>
#include <memory>
#include <string_view>
#include <thread>

using namespace lux::object;
namespace
{
    class Target final : public LuxObject
    {
    public:
        void close() noexcept
        {
            beginDestruction();
        }
    };
    unsigned shutdown_calls{};
} // namespace
int main(int argc, char** argv)
{
    auto& runtime = ObjectRuntime::instance();
    if (argc > 1 && std::string_view{argv[1]} == "--shutdown")
    {
        Target target;
        auto endpoint = target.target();
        assert(
            post(
                endpoint,
                [endpoint](LuxObject* value) noexcept
                {
                    assert(!value && ObjectRuntime::instance().isCurrent() && ++shutdown_calls == 1);
                    assert(post(endpoint, [](LuxObject*) noexcept { std::abort(); }) == EObjectPostStatus::CLOSED);
                    std::puts("Target shutdown: completed once with nullptr; CLOSED rejected");
                    std::fflush(stdout);
                }
            ) == EObjectPostStatus::POSTED
        );
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--destroy")
    {
        auto target = std::make_unique<Target>();
        assert(
            post(
                target->target(),
                [&](LuxObject*) noexcept
                {
                    std::puts("Destroying borrowed target");
                    std::fflush(stdout);
                    target.reset();
                }
            ) == EObjectPostStatus::POSTED
        );
        (void)runtime.dispatchPending();
        return 1;
    }
    Target target;
    auto endpoint = target.target();
    assert(endpoint && endpoint == target.target());
    auto owned = std::make_shared<int>(1);
    std::weak_ptr<int> observed = owned;
    unsigned calls{};
    std::thread worker(
        [&, endpoint, owned]() noexcept
        {
            assert(
                post(
                    endpoint,
                    [&, owned](LuxObject* value) noexcept
                    {
                        assert(value == &target && runtime.isCurrent());
                        ++calls;
                        assert(
                            post(
                                endpoint,
                                [&](LuxObject* later) noexcept
                                {
                                    assert(!later);
                                    ++calls;
                                }
                            ) == EObjectPostStatus::POSTED
                        );
                    }
                ) == EObjectPostStatus::POSTED
            );
        }
    );
    worker.join();
    owned.reset();
    assert(!observed.expired() && runtime.dispatchPending() == 1 && observed.expired() && calls == 1);
    target.close();
    assert(runtime.dispatchPending() == 1 && calls == 2);
    assert(post({}, [](LuxObject*) noexcept { std::abort(); }) == EObjectPostStatus::CLOSED);
    std::puts("PASS target: worker post, fixed batch, close, payload release");
}
