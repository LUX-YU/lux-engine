#pragma once

#include <lux/engine/ui_next/UISession.hpp>

namespace lux::ui
{
    class UISystem final
    {
      public:
        [[nodiscard]] UISession& session() noexcept { return session_; }
        [[nodiscard]] const UISession& session() const noexcept { return session_; }

      private:
        UISession session_;
    };
}
