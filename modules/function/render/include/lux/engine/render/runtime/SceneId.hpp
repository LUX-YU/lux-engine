#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    struct SceneTag;
    using SceneId = cxx::SlotKey<SceneTag>;
    struct ViewTag;
    using ViewId = cxx::SlotKey<ViewTag>;
} // namespace lux::render
