#pragma once
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <limits>
#include <string>

namespace lux::ui
{
    struct PaneTypeIdTag;
}

namespace lux::editor::views
{
    struct ViewId final
    {
        std::uint64_t domain{};
        std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
        std::uint64_t generation{};
        [[nodiscard]] bool valid() const noexcept
        {
            return domain && generation;
        }
        friend bool operator==(ViewId, ViewId) = default;
    };
    using ViewTypeId = lux::cxx::StableNameId<lux::ui::PaneTypeIdTag>;
    struct ViewRestoreKeyTag final
    {};
    using ViewRestoreKey = lux::cxx::StableNameId<ViewRestoreKeyTag>;
    struct ViewInfo final
    {
        ViewId id;
        ViewTypeId type;
        ViewRestoreKey restore_key;
        std::string title;
        bool visible{}, focused{};
    };
    enum class EViewError : std::uint8_t
    {
        INVALID_ID,
        NOT_ATTACHED,
        CAPACITY,
        CLOSED,
        BUSY
    };
    template <class T> using ViewResult = lux::cxx::expected<T, EViewError>;
    struct ViewCloseFailure final
    {
        std::string domain;
        std::uint64_t code{};
        std::string message;
        bool retryable{};
    };
    using ViewCloseResult = lux::cxx::expected<void, ViewCloseFailure>;

}
