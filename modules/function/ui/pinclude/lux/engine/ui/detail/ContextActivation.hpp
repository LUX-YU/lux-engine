#pragma once

#include <lux/engine/function/visibility.h>

namespace lux::ui::detail
{
// Main-thread CPU scope only: temporarily select a Context and restore the
// previous one. It is neither a cross-thread lock nor a GPU resource lease.
class LUX_FUNCTION_PUBLIC ContextActivation final
{
  public:
    explicit ContextActivation(void *context) noexcept;
    ~ContextActivation();
    ContextActivation(const ContextActivation &) = delete;
    ContextActivation &operator=(const ContextActivation &) = delete;

  private:
    void *previous_{};
};
} // namespace lux::ui::detail
