#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::ui
{
    struct DockLayout final
    {
        std::string left, center, right, bottom;
        float left_width{260}, right_width{350}, bottom_height{200};
        std::string toolbar;
    };

    enum class EDockError
    {
        INVALID_DATA
    };

    struct DockIdentity final
    {
        std::string saved;
        std::string current;
    };

    // The bytes remain ImGui's existing ini representation.
    class DockState final
    {
    public:
        DockState() = default;
        explicit DockState(std::vector<std::byte> bytes) noexcept : bytes_(std::move(bytes)) {}
        [[nodiscard]] static lux::cxx::expected<DockState, EDockError> fromBytes(std::span<const std::byte> bytes)
        {
            if (bytes.empty())
                return lux::cxx::unexpected<EDockError>{EDockError::INVALID_DATA};
            return DockState{std::vector<std::byte>{bytes.begin(), bytes.end()}};
        }
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept
        {
            return bytes_;
        }

    private:
        std::vector<std::byte> bytes_;
    };
}
