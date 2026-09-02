#include <lux/engine/ui/Theme.hpp>

namespace lux::ui
{
    Theme Theme::luxDark() noexcept
    {
        return Theme{
            {15.0F, 17.0F},
            {
                {0.10F, 0.10F, 0.12F, 1.0F},
                {0.13F, 0.13F, 0.15F, 1.0F},
                {0.18F, 0.18F, 0.21F, 1.0F},
                {0.92F, 0.92F, 0.94F, 1.0F},
                {0.58F, 0.59F, 0.63F, 1.0F},
                {0.28F, 0.29F, 0.33F, 1.0F},
                {0.25F, 0.50F, 0.80F, 1.0F},
                {0.22F, 0.39F, 0.62F, 1.0F},
                {0.42F, 0.43F, 0.47F, 1.0F},
                {0.95F, 0.72F, 0.24F, 1.0F},
                {0.95F, 0.30F, 0.25F, 1.0F},
            },
            {
                {4.0F, 3.0F},
                {8.0F, 6.0F},
                {12.0F, 10.0F},
                {8.0F, 8.0F},
            },
            {}
        };
    }
} // namespace lux::ui
