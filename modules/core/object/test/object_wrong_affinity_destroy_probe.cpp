#include <lux/engine/object/Object.hpp>

#include <memory>
#include <thread>

namespace
{
    class ProbeObject final : public lux::object::Object<ProbeObject>
    {
    };
} // namespace

int
main()
{
    auto object = std::make_unique<ProbeObject>();
    std::thread wrong_affinity([object = std::move(object)]() mutable { object.reset(); });
    wrong_affinity.join();
    return 0;
}
