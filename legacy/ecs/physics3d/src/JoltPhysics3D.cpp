// ============================================================================
//  JoltPhysics3D.cpp — all JPH types confined here. Jolt::Jolt is PRIVATE to
//  the physics target, so its configuration macros + symbols stay inside
//  physics.dll (the internal-encapsulation ruling).
// ============================================================================

#include <lux/engine/ecs/physics3d/systems/JoltPhysics3D.hpp>

#include <Jolt/Jolt.h>                 // must be the first Jolt include
#include <Jolt/Core/Core.h>           // JPH_VERSION_* macros
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>

namespace lux::ecs
{
    JoltBuildInfo joltBuildInfo()
    {
        return JoltBuildInfo{
            JPH_VERSION_MAJOR,
            JPH_VERSION_MINOR,
            JPH_VERSION_PATCH,
        };
    }

    bool joltInitSelfCheck()
    {
        JPH::RegisterDefaultAllocator();

        if (JPH::Factory::sInstance == nullptr)
            JPH::Factory::sInstance = new JPH::Factory();

        JPH::RegisterTypes();

        // A 1 MiB scratch allocator — exercises the allocator wiring the real
        // 3D world will lean on every step.
        {
            JPH::TempAllocatorImpl temp(1024 * 1024);
            (void)temp;
        }

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;

        return true;
    }
} // namespace lux::ecs
