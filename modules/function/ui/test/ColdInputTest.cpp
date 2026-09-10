#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <imgui.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>

namespace
{
    class TextPane final : public lux::object::Object<TextPane, lux::ui::Pane>
    {
      public:
        explicit TextPane(lux::ui::UISession &session)
            : Object(session.dispatcherRef(), lux::ui::PaneId{"test.font"}, lux::ui::PaneTypeId{"test.font"}, "Text")
        {
        }
        std::string text;
        ImVec2 minimum{}, maximum{};
        unsigned draws{};

      private:
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            static_cast<void>(frame.inputText("Filter", text));
            minimum = ImGui::GetItemRectMin();
            maximum = ImGui::GetItemRectMax();
            ++draws;
        }
    };
    void draw(lux::ui::UISession &session)
    {
        auto frame = session.beginFrame({{800, 600}, 1.0F / 60, {1, 1}});
        assert(!session.textInputAnchor().valid);
        frame.drawPanes();
        frame.finish();
        assert(session.captureFrame());
    }
    void glyphs(lux::ui::UISession &session, bool chinese)
    {
        auto atlas = lux::ui::detail::captureUiFontAtlas(session);
        assert(atlas && atlas->width > 0 && atlas->height > 0);
        assert(atlas->pixels.size() == std::size_t(atlas->width) * atlas->height * 4);
        auto *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(atlas->context));
        auto *font = ImGui::GetIO().Fonts->Fonts[0];
        assert(font->FindGlyphNoFallback('A'));
        for (const ImWchar character : {0x4F60, 0x597D, 0x3002, 0xFF0C})
        {
            const bool present = font->FindGlyphNoFallback(character) != nullptr;
            assert(present == chinese);
            std::printf("glyph U+%04X present=%d fallback=%d atlas=%dx%d\n", unsigned(character), present,
                        font->FindGlyph(character) == font->FallbackGlyph, atlas->width, atlas->height);
        }
        ImGui::SetCurrentContext(previous);
    }
} // namespace
int main(int argc, char **argv)
{
    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto default_session = lux::ui::UISession::create();
        assert(default_session);
        glyphs(**default_session, false);
        assert(!(*default_session)->textInputAnchor().valid);
        lux::ui::UiFontSource source;
        auto invalid = lux::ui::UISession::create({}, &source);
        assert(!invalid && invalid.error() == lux::ui::EUiInitError::INVALID_FONT_DATA);
        if (argc == 2)
        {
            std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
            assert(file && file.tellg() > 0);
            source.bytes.resize(static_cast<std::size_t>(file.tellg()));
            file.seekg(0);
            assert(file.read(reinterpret_cast<char *>(source.bytes.data()), source.bytes.size()));
            source.ranges = {{0x20, 0x7E}, {0x3000, 0x303F}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}};
            source.face = 1000;
            invalid = lux::ui::UISession::create({}, &source);
            assert(!invalid && invalid.error() == lux::ui::EUiInitError::INVALID_FONT_FACE);
            source.face = 0;
            source.size_pixels = std::numeric_limits<float>::infinity();
            invalid = lux::ui::UISession::create({}, &source);
            assert(!invalid && invalid.error() == lux::ui::EUiInitError::INVALID_FONT_SIZE);
            source.size_pixels = 18;
            source.ranges.push_back({0xD800, 0xDFFF});
            invalid = lux::ui::UISession::create({}, &source);
            assert(!invalid && invalid.error() == lux::ui::EUiInitError::INVALID_GLYPH_RANGE);
            source.ranges.pop_back();
        }
        const auto cold_start = std::chrono::steady_clock::now();
        auto made = lux::ui::UISession::create({}, argc == 2 ? &source : nullptr);
        const auto cold_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - cold_start).count();
        std::printf("cold_factory external_font=%d seconds=%.9f includes_owned_copy_and_atlas=1 excludes_file_io=1\n",
                    argc == 2, cold_seconds);
        assert(made);
        // The UI owns independent backing; releasing caller inputs cannot invalidate the font or ranges.
        source = {};
        auto &session = **made;
        glyphs(session, argc == 2);
        TextPane pane(session);
        auto registered = session.registerPane(pane);
        assert(registered);
        session.setSplitLayout({"test.font", "", "", "", 260, 0, 0});
        for (unsigned i = 0; i < 3; ++i)
            draw(session);
        assert(session.requestFocus(pane.id().view()));
        draw(session);
        std::fprintf(stderr, "auxiliary target draws=%u focused=%d rect=%.1f,%.1f..%.1f,%.1f\n", pane.draws,
                     pane.focused(), pane.minimum.x, pane.minimum.y, pane.maximum.x, pane.maximum.y);
        assert(pane.draws > 0 && pane.focused());
        // Ordinary UI events in a headless protocol test, not OS automation or an IME substitute.
        session.feedInput(lux::ui::UiPointerMove{{pane.minimum.x + 10, (pane.minimum.y + pane.maximum.y) / 2}});
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
        draw(session);
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(session);
        assert(session.inputSnapshot().keyboard_blocked);
        for (const char32_t character : {U'你', U'好', U'。', U'，', U'A'})
        {
            session.feedInput(lux::ui::UiText{character});
            draw(session);
        }
        std::fprintf(stderr, "auxiliary input bytes=%zu utf8=", pane.text.size());
        for (unsigned char byte : pane.text)
            std::fprintf(stderr, "%02X", unsigned(byte));
        std::fputc('\n', stderr);
        assert(pane.text == "\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82\xEF\xBC\x8C"
                            "A");
        const auto first = session.textInputAnchor();
        assert(first.valid && first.want_visible && first.line_height > 0 && first.frame > 0);
        draw(session);
        const auto next = session.textInputAnchor();
        assert(next.valid && next.frame > first.frame && next.caret.x == first.caret.x);
        assert(!(*default_session)->textInputAnchor().valid);
        session.feedInput(lux::ui::UiWindowFocus{false});
        assert(!session.textInputAnchor().valid);
        draw(session);
        assert(!session.textInputAnchor().valid);
        session.feedInput(lux::ui::UiWindowFocus{true});
        pane.setVisible(false);
        draw(session);
        assert(!session.textInputAnchor().valid);
        std::printf("UI cold input protocol PASS external_font=%d exact_utf8=1 stable_backing=1 "
                    "fresh_frame=1 focus_and_hidden_invalidation=1 native_ime=NOT_TESTED\n",
                    argc == 2);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
