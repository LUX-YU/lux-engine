#include <lux/engine/scene/WorldRuntime.hpp>
#include <stdexec/execution.hpp>
int main()
{
    auto sender = lux::scene::loadWorldPartition({}, {}, 1U, {});
    static_assert(stdexec::sender<decltype(sender)>);
    return !lux::scene::WorldStorageSource{} ? 0 : 1;
}
