#pragma once

#include <lux/engine/ui/Element.hpp>
#include <vector>

namespace lux::ui
{
    enum class ELayoutType : std::uint8_t
    {
        HORIZONTAL,
        VERTICAL,
        GRID,
        FORM
    };
    enum class ELayoutStatus : std::uint8_t
    {
        VALID,
        INCOMPLETE_FORM
    };

    class LUX_FUNCTION_PUBLIC Layout final : public Element
    {
    public:
        Layout(Pane& parent, ElementId id, ELayoutType type = ELayoutType::VERTICAL);
        Layout(Element& parent, ElementId id, ELayoutType type = ELayoutType::VERTICAL);
        void setType(ELayoutType type) noexcept;
        void setSpacing(Vec2 spacing) noexcept;
        void setMargins(Insets margins) noexcept;
        void setColumns(std::size_t columns) noexcept;
        void setScrollable(bool horizontal, bool vertical) noexcept;
        [[nodiscard]] ELayoutStatus status() const noexcept;

    private:
        struct Cell final
        {
            Element* element{};
            SizeHint hint;
            std::size_t column{}, row{};
            float width{};
        };
        struct Track final
        {
            float minimum{}, preferred{}, maximum{}, weight{}, size{}, offset{};
        };
        [[nodiscard]] SizeHint sizeHintContent() noexcept override;
        [[nodiscard]] SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
        void collect() noexcept;
        void columns() noexcept;
        void rows() noexcept;
        void fit(std::vector<Track>& tracks, float available, float spacing, float origin) noexcept;
        [[nodiscard]] SizeHint trackHint() const noexcept;
        static float extent(const std::vector<Track>& tracks, float Track::*member, float spacing) noexcept;
        void place() noexcept;

        ELayoutType type_;
        Vec2 spacing_{6.F, 6.F};
        Insets margins_{};
        std::size_t column_count_{2};
        bool horizontal_scroll_{}, vertical_scroll_{};
        // Scratch is reused; identity and ownership remain in the Object child chain.
        std::vector<Cell> cells_;
        std::vector<Track> columns_, rows_;
        std::vector<std::size_t> saturated_;
    };
}
