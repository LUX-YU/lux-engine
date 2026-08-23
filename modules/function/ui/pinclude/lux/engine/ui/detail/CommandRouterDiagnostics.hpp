#pragma once

#if !defined(LUX_UI_TEST_DIAGNOSTICS)
#error CommandRouter diagnostics are available only to the benchmark test build
#endif

#include <cstdint>

#include <lux/engine/ui/CommandRouter.hpp>

namespace lux::ui::detail {
struct CommandRouterDiagnosticsAccess final {
  [[nodiscard]] static std::uint64_t
  rebuildCount(const CommandRouter &router) noexcept {
    return router.rebuildCountForTest();
  }

  [[nodiscard]] static std::uint64_t
  rebuildElapsedNanoseconds(const CommandRouter &router) noexcept {
    return router.rebuildElapsedForTest();
  }

  [[nodiscard]] static std::uint64_t
  storageGrowthCount(const CommandRouter &router) noexcept {
    return router.storageGrowthCountForTest();
  }
};
} // namespace lux::ui::detail
