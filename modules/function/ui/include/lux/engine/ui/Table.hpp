#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/Ids.hpp>

namespace lux::ui
{
    struct TableSpec final
    {
        ElementIdView id;
        std::uint32_t columns{};
        bool headers{};
        bool borders{true};
        bool row_background{true};
        float first_column_width{};
    };

    class LUX_FUNCTION_PUBLIC TableScope final
    {
    public:
        explicit TableScope(const TableSpec& spec) noexcept;
        TableScope(const TableScope&) = delete;
        TableScope& operator=(const TableScope&) = delete;
        TableScope(TableScope&& other) noexcept;
        TableScope& operator=(TableScope&& other) noexcept;
        ~TableScope() noexcept;
        [[nodiscard]] bool visible() const noexcept
        {
            return active_;
        }
        void nextRow();
        void nextColumn();
        void headersRow();

    private:
        bool active_{};
    };

    LUX_FUNCTION_PUBLIC void propertyRow(std::string_view label) noexcept;
}
