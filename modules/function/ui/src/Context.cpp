#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/detail/ContextAccess.hpp>
#include <lux/engine/ui/detail/ContextImpl.hpp>
#include <lux/engine/ui/detail/FontValidation.hpp>
#include <lux/engine/ui/detail/UiContract.hpp>

#include <lux/engine/ui/detail/ContextActivation.hpp>

#include <imgui_internal.h>
#include <utility>

namespace lux::ui
{
namespace detail
{
ContextActivation::ContextActivation(void *context) noexcept
{
    previous_ = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(context));
}

ContextActivation::~ContextActivation()
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(previous_));
}

} // namespace detail

Context::Impl::~Impl()
{
    if (native)
    {
        auto *previous = ImGui::GetCurrentContext();
        if (previous == native)
        {
            previous = nullptr;
        }
        ImGui::DestroyContext(native);
        ImGui::SetCurrentContext(previous);
    }
}

Context::Context(std::unique_ptr<Impl> value) noexcept : impl_(std::move(value))
{
}
Context::~Context() = default;
Context::Context(Context &&) noexcept = default;
Context &Context::operator=(Context &&) noexcept = default;

lux::cxx::expected<Context, EUiInitError> Context::create(ContextConfig config, const UiFontSource *font)
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
        ImGuiContext *previous{ImGui::GetCurrentContext()};
        ~Restore()
        {
            ImGui::SetCurrentContext(previous);
        }
    } restore;
    auto data = std::make_unique<Impl>();
    data->native = ImGui::CreateContext();
    ImGui::SetCurrentContext(data->native);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendRendererName = "lux.ui";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    if (config.docking)
    {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    // The host installs the platform-independent anchor collector.
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
    auto *atlas = ImGui::GetIO().Fonts;
    if (font)
    {
        data->font_bytes = font->bytes;
        data->font_ranges.reserve(font->ranges.size() * 2 + 1);
        for (const auto range : font->ranges)
        {
            data->font_ranges.push_back(static_cast<ImWchar>(range.first));
            data->font_ranges.push_back(static_cast<ImWchar>(range.last));
        }
        data->font_ranges.push_back(0);
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontNo = static_cast<int>(font->face);
        config.OversampleH = config.OversampleV = 1;
        atlas->TexDesiredWidth = 4096;
        if (!atlas->AddFontFromMemoryTTF(data->font_bytes.data(), static_cast<int>(data->font_bytes.size()),
                                         font->size_pixels, &config, data->font_ranges.data()))
        {
            return lux::cxx::unexpected(EUiInitError::ATLAS_FAILURE);
        }
    }
    if (!atlas->Build())
    {
        return lux::cxx::unexpected(EUiInitError::ATLAS_FAILURE);
    }
    const bool valid_extent =
        atlas->TexWidth > 0 && atlas->TexHeight > 0 && atlas->TexWidth <= 8192 && atlas->TexHeight <= 8192;
    if (!valid_extent || std::uint64_t(atlas->TexWidth) * atlas->TexHeight * 4 > 64U * 1024U * 1024U)
    {
        return lux::cxx::unexpected(EUiInitError::ATLAS_LIMIT);
    }
    unsigned char *pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    return Context(std::move(data));
}

bool Context::frameOpen() const noexcept
{
    return impl_->frame_open;
}

lux::cxx::expected<void, EUiCaptureError> Context::capture(UiFrameSnapshot &slot)
{
    if (impl_->frame_open)
    {
        return lux::cxx::unexpected(EUiCaptureError::FRAME_OPEN);
    }
    if (!impl_->output_ready)
    {
        return lux::cxx::unexpected(EUiCaptureError::NO_FRAME);
    }
    auto *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(impl_->native);
    const auto result = slot.captureCurrent();
    ImGui::SetCurrentContext(previous);
    if (result != EUiCaptureError::NONE)
    {
        return lux::cxx::unexpected(result);
    }
    impl_->output_ready = false;
    return {};
}

void *detail::ContextAccess::native(Context &context) noexcept
{
    return context.impl_->native;
}

detail::UiFontAtlasResult detail::ContextAccess::fontAtlas(Context &context)
{
    const auto *atlas = context.impl_->native->IO.Fonts;
    if (!atlas->TexPixelsRGBA32 || !atlas->TexReady || atlas->TexWidth <= 0 || atlas->TexHeight <= 0)
    {
        return lux::cxx::unexpected(EUiInitError::ATLAS_FAILURE);
    }
    const auto size = std::uint64_t(atlas->TexWidth) * atlas->TexHeight * 4U;
    UiFontAtlasSnapshot result;
    result.width = atlas->TexWidth;
    result.height = atlas->TexHeight;
    const auto *pixels = reinterpret_cast<const std::uint8_t *>(atlas->TexPixelsRGBA32);
    result.pixels.assign(pixels, pixels + size);
    return result;
}
} // namespace lux::ui
