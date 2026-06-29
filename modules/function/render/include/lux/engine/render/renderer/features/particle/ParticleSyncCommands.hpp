#pragma once

#include <cstdint>

namespace lux::render
{
    enum class EParticleCmdId : uint32_t
    {
        SetParticleSimParams = 0,
        Count
    };

    struct SetParticleSimParamsCmd
    {
        static constexpr EParticleCmdId kCmdId = EParticleCmdId::SetParticleSimParams;
        float delta_time{0.016f};
        float gravity{9.8f};
    };
} // namespace lux::render
