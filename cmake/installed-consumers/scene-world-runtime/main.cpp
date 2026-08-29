#include <lux/engine/scene/WorldRuntime.hpp>
int main()
{
    return !lux::scene::WorldStorageSource{} ? 0 : 1;
}
