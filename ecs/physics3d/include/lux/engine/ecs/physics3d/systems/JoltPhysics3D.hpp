#pragma once
// ============================================================================
//  JoltPhysics3D.hpp — build/runtime connectivity diagnostics for the private
//  Jolt backend. Production scene ownership, ECS bodies/characters, static
//  static batches and origin rebasing live in Physics3DScene/Physics3DSystem;
//  no JPH type crosses either public boundary.
// ============================================================================

#include <lux/engine/function/visibility.h>

namespace lux::ecs
{
    struct JoltBuildInfo
    {
        int version_major = 0;
        int version_minor = 0;
        int version_patch = 0;
    };

    /// The Jolt version the physics target was compiled against (proves the
    /// headers + configuration macros are wired correctly).
    LUX_FUNCTION_PUBLIC JoltBuildInfo joltBuildInfo();

    /// Runs Jolt's global bring-up — default allocator, Factory, and the type
    /// registry (RegisterTypes, the most link-sensitive path) — plus a temp
    /// allocator, then tears it all down. Returns true on success. This is the
    /// Standalone connectivity proof used by build diagnostics and tests.
    LUX_FUNCTION_PUBLIC bool joltInitSelfCheck();
} // namespace lux::ecs
