#pragma once
#include <lux/engine/ui/Ids.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <limits>
#include <string>

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
    using ViewTypeId = ui::PaneTypeId;
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
}
