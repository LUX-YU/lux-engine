#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    // Reusable settlement workspace for the active frame's Resource and producer uploads.
    class LUX_RENDER_PUBLIC Preparation final
    {
    public:
        explicit Preparation(std::size_t resource_capacity) noexcept;
        ~Preparation() noexcept;
        Preparation(const Preparation&) = delete;
        Preparation& operator=(const Preparation&) = delete;
        using Submitted = void (*)(void*, std::uint32_t, RenderResult<void>) noexcept;
        // Borrowed replica/Feature settlement. The actual frame owns this record, not the Uploader.
        void onSubmitted(void*, Submitted) noexcept;
        // The owner supplies the actual native result, after staging has committed or cancelled.
        // New callbacks appended during delivery wait for the next settlement of this preparation.
        void finishCallbacks(std::uint32_t slot, RenderResult<void>) noexcept;

    private:
        bool delivering_ = false;
        std::vector<std::pair<void*, Submitted>> receivers_;
    };
} // namespace lux::render::detail
