#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/ui/detail/DragDropEncoding.hpp>

#include <imgui.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

int main()
{
    constexpr std::array<std::uint32_t, 2U> values{17U, 29U};
    const auto value_bytes = std::as_bytes(std::span{values});
    std::array<std::byte, lux::ui::detail::kInlineDragDropBytes> inline_storage{};
    std::vector<std::byte> heap_storage;
    const auto encoded = lux::ui::detail::encodeDragDropPayload(
        lux::ui::PayloadTypeIdView{"test.drag-drop-scope"},
        value_bytes,
        inline_storage,
        heap_storage
    );

    ImGui::resetDragDropDiagnostics();
    ImGui::payload = {encoded.data(), static_cast<int>(encoded.size())};
    const auto scoped = lux::ui::detail::acceptDragDropPayloadInActiveTarget();
    assert(scoped);
    assert(scoped->type == lux::ui::PayloadTypeIdView{"test.drag-drop-scope"});
    assert(scoped->bytes.size() == value_bytes.size());
    assert(ImGui::begin_count == 0);
    assert(ImGui::accept_count == 1);
    assert(ImGui::end_count == 0);

    ImGui::resetDragDropDiagnostics();
    ImGui::payload = {encoded.data(), static_cast<int>(encoded.size())};
    const auto one_shot = lux::ui::detail::acceptDragDropPayload();
    assert(one_shot);
    assert(ImGui::begin_count == 1);
    assert(ImGui::accept_count == 1);
    assert(ImGui::end_count == 1);

    ImGui::resetDragDropDiagnostics();
    ImGui::begin_result = false;
    assert(!lux::ui::detail::acceptDragDropPayload());
    assert(ImGui::begin_count == 1);
    assert(ImGui::accept_count == 0);
    assert(ImGui::end_count == 0);
    return 0;
}
