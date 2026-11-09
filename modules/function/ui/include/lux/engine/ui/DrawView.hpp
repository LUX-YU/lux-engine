#pragma once

#include <cstdint>
#include <lux/engine/description/UiImageToken.hpp>
#include <lux/engine/function/visibility.h>
#include <span>
#include <vector>

struct ImDrawData;

namespace lux::ui
{
    namespace detail
    {
        class Context;
    }

    enum class ECaptureError : std::uint8_t
    {
        NONE,
        WRONG_THREAD,
        FRAME_OPEN,
        INPUT_PENDING,
        NO_FRAME,
        INVALID_INPUT,
        UNSUPPORTED_CALLBACK,
    };

    // Borrow of the completed ImGui frame, valid until the next Root update.
    // Render collectors must copy its arrays into their FrameWriter before that update.
    // This view owns no draw geometry and does not claim asynchronous lifetime.
    class LUX_FUNCTION_PUBLIC DrawView final
    {
    public:
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] const ImDrawData& data() const noexcept;
        // Optional UI inspection; normal rendering never builds this unique-token list.
        [[nodiscard]] std::span<const rdesc::UiImageToken> textures() const noexcept;

    private:
        friend class detail::Context;
        [[nodiscard]] ECaptureError borrowCurrent() noexcept;
        const ImDrawData* data_ = nullptr;
        mutable std::vector<rdesc::UiImageToken> textures_;
        mutable bool textures_ready_ = false;
    };
} // namespace lux::ui
