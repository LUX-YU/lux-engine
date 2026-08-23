#pragma once

#if !defined(LUX_UI_TEST_DIAGNOSTICS)
#error UISession diagnostics are available only to the benchmark test build
#endif

#include <cstdint>

#include <lux/engine/ui/UISession.hpp>

namespace lux::ui::detail {
struct UISessionDiagnosticsAccess final {
  [[nodiscard]] static std::uint64_t
  wrapperGrowthCount(const UISession &session) noexcept {
    return session.wrapperGrowthCountForTest();
  }

  [[nodiscard]] static const void *
  contextIdentity(const UISession &session) noexcept {
    return session.contextIdentityForTest();
  }
};
} // namespace lux::ui::detail
