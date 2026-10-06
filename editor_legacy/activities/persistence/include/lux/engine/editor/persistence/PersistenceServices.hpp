#pragma once

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::persistence
{
    // Declaration/Process binding belongs to persistence_execution. The coordinator and save algorithms
    // remain in the pure persistence target. Every producer resolves the root-scope coordinator.
    extern const services::ServiceDescriptor kWriteCoordinatorService;
    extern const services::ServiceDescriptor kSaveService;
    extern const services::ServiceDescriptor kSaveExecutionService;
}
