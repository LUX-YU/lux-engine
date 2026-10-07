#include <cmath>
#include <limits>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/detail/Context.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/FontValidation.hpp>
#include <utility>

namespace lux::ui
{
    namespace detail
    {
        ContextActivation::ContextActivation(void* context) noexcept
        {
            previous_ = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
        }

        ContextActivation::~ContextActivation()
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(previous_));
        }

    } // namespace detail

} // namespace lux::ui

namespace lux::ui::detail
{
    namespace
    {
        [[nodiscard]] int toImGuiButton(EPointerButton button) noexcept
        {
            switch (button)
            {
            case EPointerButton::LEFT:
                return ImGuiMouseButton_Left;
            case EPointerButton::MIDDLE:
                return ImGuiMouseButton_Middle;
            case EPointerButton::RIGHT:
                return ImGuiMouseButton_Right;
            }
            return ImGuiMouseButton_Left;
        }

        [[nodiscard]] ImGuiKey toImGuiKey(EKey key) noexcept
        {
            switch (key)
            {
            case EKey::A:
                return ImGuiKey_A;
            case EKey::B:
                return ImGuiKey_B;
            case EKey::C:
                return ImGuiKey_C;
            case EKey::D:
                return ImGuiKey_D;
            case EKey::E:
                return ImGuiKey_E;
            case EKey::F:
                return ImGuiKey_F;
            case EKey::G:
                return ImGuiKey_G;
            case EKey::H:
                return ImGuiKey_H;
            case EKey::I:
                return ImGuiKey_I;
            case EKey::J:
                return ImGuiKey_J;
            case EKey::K:
                return ImGuiKey_K;
            case EKey::L:
                return ImGuiKey_L;
            case EKey::M:
                return ImGuiKey_M;
            case EKey::N:
                return ImGuiKey_N;
            case EKey::O:
                return ImGuiKey_O;
            case EKey::P:
                return ImGuiKey_P;
            case EKey::Q:
                return ImGuiKey_Q;
            case EKey::R:
                return ImGuiKey_R;
            case EKey::S:
                return ImGuiKey_S;
            case EKey::T:
                return ImGuiKey_T;
            case EKey::U:
                return ImGuiKey_U;
            case EKey::V:
                return ImGuiKey_V;
            case EKey::W:
                return ImGuiKey_W;
            case EKey::X:
                return ImGuiKey_X;
            case EKey::Y:
                return ImGuiKey_Y;
            case EKey::Z:
                return ImGuiKey_Z;
            case EKey::LEFT_SHIFT:
                return ImGuiKey_LeftShift;
            case EKey::RIGHT_SHIFT:
                return ImGuiKey_RightShift;
            case EKey::LEFT_CONTROL:
                return ImGuiKey_LeftCtrl;
            case EKey::RIGHT_CONTROL:
                return ImGuiKey_RightCtrl;
            case EKey::LEFT_ALT:
                return ImGuiKey_LeftAlt;
            case EKey::RIGHT_ALT:
                return ImGuiKey_RightAlt;
            case EKey::COUNT:
                return ImGuiKey_None;
            case EKey::NONE:
                return ImGuiKey_None;
            case EKey::TAB:
                return ImGuiKey_Tab;
            case EKey::ENTER:
                return ImGuiKey_Enter;
            case EKey::ESCAPE:
                return ImGuiKey_Escape;
            case EKey::SPACE:
                return ImGuiKey_Space;
            case EKey::BACKSPACE:
                return ImGuiKey_Backspace;
            case EKey::DELETE_KEY:
                return ImGuiKey_Delete;
            case EKey::LEFT:
                return ImGuiKey_LeftArrow;
            case EKey::RIGHT:
                return ImGuiKey_RightArrow;
            case EKey::UP:
                return ImGuiKey_UpArrow;
            case EKey::DOWN:
                return ImGuiKey_DownArrow;
            case EKey::HOME:
                return ImGuiKey_Home;
            case EKey::END:
                return ImGuiKey_End;
            }
            return ImGuiKey_None;
        }

        [[nodiscard]] ImVec4 toImGuiColor(Color value) noexcept
        {
            return ImVec4{value.red, value.green, value.blue, value.alpha};
        }

        void applyTheme(const Theme& theme) noexcept
        {
            auto& style = ImGui::GetStyle();
            style.WindowPadding = {theme.spacing.panel_padding.x, theme.spacing.panel_padding.y};
            style.FramePadding = {theme.spacing.item.x, theme.spacing.compact.y};
            style.ItemSpacing = {theme.spacing.item.x, theme.spacing.item.y};
            style.IndentSpacing = theme.metrics.tree_indent;
            style.WindowRounding = theme.metrics.rounding;
            style.ChildRounding = theme.metrics.rounding;
            style.FrameRounding = theme.metrics.rounding;
            style.PopupRounding = theme.metrics.rounding;
            style.WindowBorderSize = theme.metrics.border_width;
            style.ChildBorderSize = theme.metrics.border_width;
            style.FrameBorderSize = 0.0F;
            style.Colors[ImGuiCol_WindowBg] = toImGuiColor(theme.palette.window_background);
            style.Colors[ImGuiCol_ChildBg] = toImGuiColor(theme.palette.panel_background);
            style.Colors[ImGuiCol_FrameBg] = toImGuiColor(theme.palette.field_background);
            style.Colors[ImGuiCol_Text] = toImGuiColor(theme.palette.text);
            style.Colors[ImGuiCol_TextDisabled] = toImGuiColor(theme.palette.muted_text);
            style.Colors[ImGuiCol_Border] = toImGuiColor(theme.palette.border);
            style.Colors[ImGuiCol_CheckMark] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_SliderGrab] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_Header] = toImGuiColor(theme.palette.selection);
            style.Colors[ImGuiCol_HeaderHovered] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_HeaderActive] = toImGuiColor(theme.palette.selection);
        }

    } // namespace

    Context::~Context() noexcept
    {
        if (!native_)
        {
            return;
        }
        auto* previous = ImGui::GetCurrentContext();
        if (previous == native_)
        {
            previous = nullptr;
        }
        ImGui::DestroyContext(native_);
        ImGui::SetCurrentContext(previous);
    }

    Context::CreateResult Context::create(const RootConfig& config) noexcept
    {
        if (config.input_capacity < 2 || config.input_capacity > std::size_t(std::numeric_limits<int>::max()))
        {
            return lux::cxx::unexpected(EInitError::INVALID_INPUT_CAPACITY);
        }
        const bool is_invalid_scale = !std::isfinite(config.scale) || config.scale < 0.5f || config.scale > 4.f;
        if (is_invalid_scale)
        {
            return lux::cxx::unexpected(EInitError::INVALID_SCALE);
        }
        const auto* font = config.font;
        {
            if (font)
            {
                if (auto valid = detail::validateFont(*font); !valid)
                {
                    return lux::cxx::unexpected(valid.error());
                }
            }
            // The partial owner releases a newly-created context on every business
            // failure, before restoring the caller's still-live context.
            struct Restore final
            {
                ImGuiContext* previous{ImGui::GetCurrentContext()};
                ~Restore()
                {
                    ImGui::SetCurrentContext(previous);
                }
            } restore;
            auto data = std::unique_ptr<Context>(new Context());
            data->theme_ = config.theme;
            data->scale_ = config.scale;
            data->native_ = ImGui::CreateContext();
            data->input_capacity_ = static_cast<int>(config.input_capacity);
            data->input_records_.reserve(config.input_capacity);
            data->native_->InputEventsQueue.reserve(data->input_capacity_);
            data->native_->InputEventsTrail.reserve(data->input_capacity_);
            ImGui::SetCurrentContext(data->native_);
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendRendererName = "lux.ui";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            if (config.docking)
            {
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            }
            auto* atlas = ImGui::GetIO().Fonts;
            if (font)
            {
                data->font_bytes_ = font->bytes;
                data->font_ranges_.reserve(font->ranges.size() * 2 + 1);
                for (const auto range : font->ranges)
                {
                    data->font_ranges_.push_back(static_cast<ImWchar>(range.first));
                    data->font_ranges_.push_back(static_cast<ImWchar>(range.last));
                }
                data->font_ranges_.push_back(0);
                ImFontConfig font_config;
                font_config.FontDataOwnedByAtlas = false;
                font_config.FontNo = static_cast<int>(font->face);
                font_config.OversampleH = font_config.OversampleV = 1;
                atlas->TexDesiredWidth = 4096;
                if (!atlas->AddFontFromMemoryTTF(
                        data->font_bytes_.data(),
                        static_cast<int>(data->font_bytes_.size()),
                        font->size_pixels * config.scale,
                        &font_config,
                        data->font_ranges_.data()
                    ))
                {
                    return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
                }
            }
            else
            {
                ImFontConfig font_config;
                font_config.SizePixels = 13.f * config.scale;
                atlas->AddFontDefault(&font_config);
            }
            if (!atlas->Build())
            {
                return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
            }
            const bool valid_extent =
                atlas->TexWidth > 0 && atlas->TexHeight > 0 && atlas->TexWidth <= 8192 && atlas->TexHeight <= 8192;
            if (!valid_extent || std::uint64_t(atlas->TexWidth) * atlas->TexHeight * 4 > 64U * 1024U * 1024U)
            {
                return lux::cxx::unexpected(EInitError::ATLAS_LIMIT);
            }
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

            applyTheme(data->theme_);
            ImGui::GetStyle().ScaleAllSizes(config.scale);
            return data;
        }
    }

    lux::cxx::expected<FontAtlas, EInitError> Context::fontAtlas() const noexcept
    {
        const auto* atlas = native_->IO.Fonts;
        const bool valid = atlas->TexPixelsRGBA32 && atlas->TexReady && atlas->TexWidth > 0 && atlas->TexHeight > 0;
        if (!valid)
        {
            return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
        }
        {
            FontAtlas result;
            result.width = atlas->TexWidth;
            result.height = atlas->TexHeight;
            const auto size = std::size_t(result.width) * result.height * 4;
            const auto* pixels = reinterpret_cast<const std::uint8_t*>(atlas->TexPixelsRGBA32);
            result.pixels.assign(pixels, pixels + size);
            return result;
        }
    }
    lux::cxx::expected<void, EInputError> Context::feedInput(const VInputEvent& event, std::uint64_t sequence) noexcept
    {
        detail::ContextActivation context{native_};
        auto& io = ImGui::GetIO();
        if (!io.AppAcceptingEvents)
        {
            return lux::cxx::unexpected(EInputError::CLOSED);
        }
        const bool valid = std::visit(
            [](const auto& value) noexcept
            {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, PointerMove>)
                {
                    return std::isfinite(value.position.x) && std::isfinite(value.position.y);
                }
                else if constexpr (std::same_as<Value, PointerWheel>)
                {
                    return std::isfinite(value.delta.x) && std::isfinite(value.delta.y);
                }
                else if constexpr (std::same_as<Value, PointerButton>)
                {
                    return value.button >= EPointerButton::LEFT && value.button <= EPointerButton::RIGHT;
                }
                else if constexpr (std::same_as<Value, Key>)
                {
                    return value.key > EKey::NONE && value.key < EKey::COUNT;
                }
                else if constexpr (std::same_as<Value, Text>)
                {
                    return value.codepoint > 0 && value.codepoint <= 0x10FFFF &&
                           (value.codepoint < 0xD800 || value.codepoint > 0xDFFF);
                }
                else if constexpr (std::same_as<Value, Composition>)
                {
                    return value.stage >= ECompositionStage::STARTED && value.stage <= ECompositionStage::CANCELLED;
                }
                else
                {
                    return !std::same_as<Value, PointerCancel>;
                }
            },
            event
        );
        const bool invalid_sequence = sequence && sequence <= accepted_input_;
        if (!valid || invalid_sequence || accepted_input_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EInputError::INVALID_INPUT);
        }
        const auto* key_event = std::get_if<Key>(&event);
        const bool modifier = key_event && key_event->key >= EKey::LEFT_SHIFT;
        const int required = modifier ? 2 : 1;
        // Preflight the entire native_ event, including its aggregate modifier.
        // The existing ImGui queue is the only pending input store.
        if (native_->InputEventsQueue.Size > input_capacity_ - required ||
            input_records_.size() == std::size_t(input_capacity_))
        {
            return lux::cxx::unexpected(EInputError::FULL);
        }
        const auto first = native_->InputEventsNextEventId;
        std::visit(
            [&](const auto& value)
            {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, PointerMove>)
                {
                    io.AddMousePosEvent(value.position.x, value.position.y);
                }
                else if constexpr (std::same_as<Value, PointerButton>)
                {
                    io.AddMouseButtonEvent(toImGuiButton(value.button), value.down);
                }
                else if constexpr (std::same_as<Value, PointerWheel>)
                {
                    io.AddMouseWheelEvent(value.delta.x, value.delta.y);
                }
                else if constexpr (std::same_as<Value, Key>)
                {
                    constexpr std::array physical_modifiers{
                        EKey::LEFT_CONTROL,
                        EKey::RIGHT_CONTROL,
                        EKey::LEFT_SHIFT,
                        EKey::RIGHT_SHIFT,
                        EKey::LEFT_ALT,
                        EKey::RIGHT_ALT
                    };
                    constexpr std::array aggregate_modifiers{ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt};
                    for (std::size_t index = 0; index < physical_modifiers.size(); ++index)
                    {
                        if (value.key == physical_modifiers[index])
                        {
                            // ImGui does not derive aggregate modifiers from
                            // side-specific events. Preserve the other side.
                            modifier_keys_[index] = value.down;
                            const auto pair = index / 2;
                            io.AddKeyEvent(
                                aggregate_modifiers[pair],
                                modifier_keys_[pair * 2] || modifier_keys_[pair * 2 + 1]
                            );
                            break;
                        }
                    }
                    const auto key = toImGuiKey(value.key);
                    if (key != ImGuiKey_None)
                    {
                        io.AddKeyEvent(key, value.down);
                    }
                }
                else if constexpr (std::same_as<Value, Text>)
                {
                    io.AddInputCharacter(static_cast<unsigned int>(value.codepoint));
                }
                else if constexpr (std::same_as<Value, WindowFocus>)
                {
                    window_focused_ = value.focused;
                    if (!value.focused)
                    {
                        modifier_keys_.fill(false);
                    }
                    io.AddFocusEvent(value.focused);
                }
            },
            event
        );
        const auto accepted = sequence ? sequence : accepted_input_ + 1;
        if (const auto* focus = std::get_if<WindowFocus>(&event); focus && !focus->focused)
        {
            focus_loss_ = accepted;
        }
        const auto* composition = std::get_if<Composition>(&event);
        input_records_.push_back(
            {first,
             native_->InputEventsNextEventId,
             accepted,
             composition ? std::optional{composition->stage} : std::nullopt}
        );
        accepted_input_ = accepted;
        return {};
    }

    InputSnapshot Context::inputSnapshot(bool composing, bool pointer_captured) const noexcept
    {
        detail::ContextActivation context{native_};
        InputSnapshot result;
        const auto& io = ImGui::GetIO();
        for (std::size_t index = 1; index < result.held.size(); ++index)
        {
            const auto key = toImGuiKey(static_cast<EKey>(index));
            result.held[index] = ImGui::IsKeyDown(key);
            result.pressed[index] = ImGui::IsKeyPressed(key, false);
        }
        for (std::size_t index = 0; index < result.buttons.size(); ++index)
        {
            result.buttons[index] = ImGui::IsMouseDown(toImGuiButton(static_cast<EPointerButton>(index)));
        }
        result.pointer_delta = {io.MouseDelta.x, io.MouseDelta.y};
        result.wheel = {io.MouseWheelH, io.MouseWheel};
        result.window_focused = !io.AppFocusLost;
        result.composing = composing;
        result.keyboard_captured = io.WantCaptureKeyboard || composing;
        result.pointer_captured = io.WantCaptureMouse || pointer_captured;
        result.sequence = adopted_input_;
        result.modal_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
        result.keyboard_blocked = composing || io.WantTextInput || ImGui::IsAnyItemActive() || result.modal_open;
        return result;
    }

    void Context::bindWindow(void* window) noexcept
    {
        ContextActivation active{native_};
        ImGui::GetMainViewport()->PlatformHandleRaw = window;
    }

    EKey Context::keyFromNative(ImGuiKey native_key) noexcept
    {
        for (unsigned key = unsigned(EKey::NONE) + 1; key < unsigned(EKey::COUNT); ++key)
        {
            if (toImGuiKey(static_cast<EKey>(key)) == native_key)
            {
                return static_cast<EKey>(key);
            }
        }
        return EKey::NONE;
    }

    cxx::expected<void, ECaptureError> Context::beginFrame(FrameInfo info) noexcept
    {
        if (input_pending_)
        {
            return cxx::unexpected(ECaptureError::INPUT_PENDING);
        }
        const bool valid_size = std::isfinite(info.display_size.width) && info.display_size.width > 0 &&
                                std::isfinite(info.display_size.height) && info.display_size.height > 0;
        const bool valid_time = std::isfinite(info.delta_seconds) && info.delta_seconds > 0;
        const bool valid_scale = std::isfinite(info.framebuffer_scale.x) && info.framebuffer_scale.x > 0 &&
                                 std::isfinite(info.framebuffer_scale.y) && info.framebuffer_scale.y > 0;
        const bool invalid_input = !valid_size || !valid_time || !valid_scale;
        if (invalid_input)
        {
            return cxx::unexpected(ECaptureError::INVALID_INPUT);
        }
        auto& io = native_->IO;
        io.DisplaySize = {info.display_size.width, info.display_size.height};
        io.DeltaTime = info.delta_seconds;
        io.DisplayFramebufferScale = {info.framebuffer_scale.x, info.framebuffer_scale.y};
        ImGui::NewFrame();
        return {};
    }

    cxx::expected<void, ECaptureError> Context::endFrame(DrawData& output, UpdateStatistics& statistics) noexcept
    {
        ImGui::Render();
        input_pending_ = !native_->InputEventsTrail.empty() || !input_records_.empty();
        const auto start = std::chrono::steady_clock::now();
        const auto captured = output.captureCurrent();
        statistics.capture = std::chrono::steady_clock::now() - start;
        if (captured != ECaptureError::NONE)
        {
            return cxx::unexpected(captured);
        }
        const auto* data = ImGui::GetDrawData();
        if (data && data->Valid)
        {
            statistics.draw_lists = data->CmdListsCount;
            statistics.draw_vertices = data->TotalVtxCount;
            statistics.draw_indices = data->TotalIdxCount;
            for (const auto* list : data->CmdLists)
            {
                statistics.draw_commands += list->CmdBuffer.Size;
            }
        }
        statistics.textures = output.textures().size();
        return {};
    }

    bool Context::closeInput() noexcept
    {
        if (!native_->IO.AppAcceptingEvents)
        {
            return false;
        }
        native_->IO.SetAppAcceptingEvents(false);
        cancelled_input_ = accepted_input_;
        window_focused_ = false;
        return true;
    }

    bool Context::takeFocusLoss() noexcept
    {
        const bool pending_loss = !input_pending_ && focus_loss_ > cancelled_input_;
        if (pending_loss)
        {
            cancelled_input_ = focus_loss_;
        }
        return pending_loss;
    }

    void Context::consumeInput(
        cxx::function_ref<void(const ImGuiInputEvent&)> route,
        cxx::function_ref<void(ECompositionStage)> composition
    ) noexcept
    {
        input_pending_ = false;
        std::size_t complete{};
        int trail{};
        for (auto& record : input_records_)
        {
            if (record.first != record.end)
            {
                while (trail < native_->InputEventsTrail.Size)
                {
                    const auto& input = native_->InputEventsTrail[trail];
                    if (input.EventId >= record.end)
                    {
                        break;
                    }
                    ++trail;
                    if (input.EventId < record.first)
                    {
                        continue;
                    }
                    adopted_input_ = record.sequence;
                    if (record.sequence > cancelled_input_)
                    {
                        route(input);
                    }
                    record.first = input.EventId + 1;
                }
                if (record.first != record.end)
                {
                    break; // ImGui trickles the remainder.
                }
            }
            adopted_input_ = record.sequence;
            if (record.composition && record.sequence > cancelled_input_)
            {
                composition(*record.composition);
            }
            ++complete;
        }
        input_records_.erase(input_records_.begin(), input_records_.begin() + complete);
    }
} // namespace lux::ui::detail
