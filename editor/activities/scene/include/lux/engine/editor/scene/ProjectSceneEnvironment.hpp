#pragma once

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::scene
{
    // Lazy project-owned input for Scene projections, Run preparation and Material preview.
    // Exposes ProjectionEnvironment through the allocation's original shared control block.
    extern const services::ServiceDescriptor kProjectSceneEnvironment;
}
