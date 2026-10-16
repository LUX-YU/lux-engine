#include <cstddef>
#include <memory>
#include <vector>

// The Asset's shared_ptr control block and deleter are instantiated in this DLL,
// independently of any Render Command or its canonical IPC type operations.
extern "C" void payloadCreateAsset(
    std::shared_ptr<const std::vector<std::byte>>* output,
    int* destroyed,
    int* unpinned_destroyed,
    const bool* pin_alive
) noexcept
{
    auto* value = new std::vector<std::byte>(32, std::byte{7});
    value->reserve(4096);
    *output = std::shared_ptr<const std::vector<std::byte>>(
        value,
        [destroyed, unpinned_destroyed, pin_alive](const auto* asset) noexcept {
            if (!*pin_alive) ++*unpinned_destroyed;
            ++*destroyed;
            delete asset;
        }
    );
}
