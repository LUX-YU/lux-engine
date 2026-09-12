#pragma once
#include <array>
#include <cassert>
#include <string_view>
namespace inspector_test
{
    struct Item final
    {
        std::string_view field, label;
        ImVec2 low, high;
    };
    inline std::array<const char *, 32> path{};
    inline unsigned depth{}, count{};
    inline std::array<Item, 1024> items{};
    inline void push(const char *field) noexcept { assert(depth < path.size()); path[depth++] = field; }
    inline void pop() noexcept { assert(depth); --depth; }
    inline void item(const char *label, ImVec2 low, ImVec2 high) noexcept
    {
        assert(count < items.size());
        items[count++] = {depth ? path[0] : "", label, low, high};
    }
    inline Item find(std::string_view field, std::string_view label = "value", unsigned index = 0)
    {
        for (unsigned i = 0; i < count; ++i)
            if (items[i].field == field && items[i].label == label && index-- == 0) return items[i];
        assert(false && "The generated widget was not drawn");
        return {};
    }
}
