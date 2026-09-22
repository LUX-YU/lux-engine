#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/UiFontSource.hpp>
#include <lux/engine/ui/UiFrameSnapshot.hpp>

#include <memory>

namespace lux::ui
{
namespace detail
{
struct ContextAccess;
}

struct ContextConfig final
{
    bool docking{};
};

// CPU ImGui resources and cold font configuration. No window, dispatcher,
// Pane, draw queue or GPU renderer is owned by this object.
class LUX_FUNCTION_PUBLIC Context final
{
  public:
    [[nodiscard]] static lux::cxx::expected<Context, EUiInitError> create(ContextConfig config = {},
                                                                          const UiFontSource *font = nullptr);
    ~Context();
    Context(Context &&) noexcept;
    Context &operator=(Context &&) noexcept;
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    [[nodiscard]] bool frameOpen() const noexcept;
    // Capture into a returned pool slot, retaining its previous buffer capacity.
    // Failure leaves the slot unchanged. A discarded Frame has no output.
    [[nodiscard]] lux::cxx::expected<void, EUiCaptureError> capture(UiFrameSnapshot &slot);

  private:
    friend class Frame;
    friend struct detail::ContextAccess;
    struct Impl;
    explicit Context(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::ui
