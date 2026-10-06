#pragma once

#include <consumer/Component.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>

#if defined(_WIN32)
#if defined(CONSUMER_DOMAIN_LIBRARY)
#define CONSUMER_DOMAIN_PUBLIC __declspec(dllexport)
#else
#define CONSUMER_DOMAIN_PUBLIC __declspec(dllimport)
#endif
#else
#define CONSUMER_DOMAIN_PUBLIC
#endif

namespace consumer
{
    [[nodiscard]] CONSUMER_DOMAIN_PUBLIC std::span<const lux::simulation::ecs::ComponentSchema> schemas();
    CONSUMER_DOMAIN_PUBLIC void rejectNextDecode();
}
