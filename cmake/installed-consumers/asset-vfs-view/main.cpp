#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <type_traits>

int main()
{
    static_assert(!std::is_copy_constructible_v<lux::asset::AssetVfs>);
    static_assert(std::is_copy_constructible_v<lux::asset::AssetVfsView>);
    lux::asset::AssetVfs vfs;
    const auto view = vfs.view();
    return view && view.resolve("/Game/missing").isNull() ? 0 : 1;
}
