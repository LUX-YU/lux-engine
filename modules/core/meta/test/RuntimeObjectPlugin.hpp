#pragma once

#include <lux/engine/meta/Meta.hpp>

struct RuntimeObjectProbe final
{
    unsigned constructed{};
    unsigned copied{};
    unsigned destroyed{};
    unsigned destruct_tail{};
};

using RuntimeObjectRegistration = lux::meta::ReflectionRegistrationDraft::RegisterFn;
using RuntimeObjectPluginEntry = RuntimeObjectRegistration(RuntimeObjectProbe*) noexcept;
