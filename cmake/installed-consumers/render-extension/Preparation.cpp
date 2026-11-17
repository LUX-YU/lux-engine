#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

using namespace lux::render;
using lux::render::detail::Preparation;

namespace
{
    void check(bool value)
    {
        if (!value) std::abort();
    }

    struct Receiver final
    {
        Preparation* preparation = nullptr;
        unsigned calls = 0, slot = UINT32_MAX;
        RenderError error;
        bool append = false, reenter = false;

        static void complete(void* pointer, std::uint32_t slot, RenderResult<void> result) noexcept
        {
            auto& self = *static_cast<Receiver*>(pointer);
            ++self.calls;
            self.slot = slot;
            self.error = result ? RenderError{} : result.error();
            if (self.reenter) self.preparation->finishCallbacks(slot, result);
            if (std::exchange(self.append, false))
            {
                // Deliberately grow the list while its first callback is executing.
                for (unsigned i = 0; i < 32; ++i)
                    self.preparation->onSubmitted(&self, complete);
            }
        }
    };
}

int main(int argc, char** argv)
{
    std::set_terminate([]() noexcept { std::_Exit(86); });
    Preparation first(1), second(1);
    Receiver a{&first}, b{&second};
    if (argc > 1)
    {
        first.onSubmitted(&a, Receiver::complete);
        if (std::string_view(argv[1]) == "reenter")
        {
            a.reenter = true;
            first.finishCallbacks(0, {});
        }
        return 1; // A pending receiver must not disappear with its Preparation owner.
    }
    a.append = true;
    first.onSubmitted(&a, Receiver::complete);
    second.onSubmitted(&b, Receiver::complete);
    first.finishCallbacks(2, {});
    check(a.calls == 1 && a.slot == 2 && a.error.type == 0 && b.calls == 0);
    const RenderError rejected{kDeviceFailure, {91, 2, 3}};
    second.finishCallbacks(1, lux::cxx::unexpected(rejected));
    check(b.calls == 1 && b.slot == 1 && b.error == rejected);
    const RenderError unknown{kOutcomeUnknown, {92, 4, 5}};
    first.finishCallbacks(2, lux::cxx::unexpected(unknown));
    check(a.calls == 33 && a.slot == 2 && a.error == unknown);
    first.finishCallbacks(0, {});
    check(a.calls == 33);
    // The existing frozen-Resource contract remains independent of callback delivery.
    Preparation empty(0);
    check(bool(empty.freeze({}, 0, {})) && bool(empty.beginSubmit()));
    check(!empty.submitted(lux::cxx::unexpected(unknown)) && empty.quarantined());
    check(!empty.freeze({}, 0, {}));
    std::puts("Preparation: per-frame owners, exact results, callback growth/reentrancy and unknown quarantine passed");
}
