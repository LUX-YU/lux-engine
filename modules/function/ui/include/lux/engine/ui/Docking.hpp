#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/Geometry.hpp>
#include <memory>
#include <cstdint>

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

    // Window placement only. Persistent layout/source identities belong to the caller.
    enum class EDockSplit : std::uint8_t
    {
        LEAF,
        HORIZONTAL,
        VERTICAL
    };
    struct DockNode final
    {
        EDockSplit split{EDockSplit::LEAF};
        std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
        float ratio{0.5F};
        std::vector<std::string> windows;
    };
    struct DockSurface final
    {
        std::uint32_t node{};
        Rect bounds;
        bool floating{};
    };
    struct DockTree final
    {
        std::vector<DockNode> nodes;
        std::vector<DockSurface> surfaces;
    };
    class LUX_FUNCTION_PUBLIC PreparedDockTree final
    {
    public:
        ~PreparedDockTree();
        PreparedDockTree(PreparedDockTree&&) noexcept;
        PreparedDockTree& operator=(PreparedDockTree&&) noexcept;
        PreparedDockTree(const PreparedDockTree&) = delete;
        PreparedDockTree& operator=(const PreparedDockTree&) = delete;

    private:
        friend class Root;
        struct Data;
        explicit PreparedDockTree(std::unique_ptr<Data>) noexcept;
        std::unique_ptr<Data> data_;
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
