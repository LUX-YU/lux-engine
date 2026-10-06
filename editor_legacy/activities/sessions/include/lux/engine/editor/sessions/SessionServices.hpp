#pragma once

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::sessions
{
    // Activity declarations, separate from the pure editing allocation and its History implementation.
    extern const services::ServiceDescriptor kSessionStoreService;
    extern const services::ServiceDescriptor kSessionOpeningService;
} // namespace lux::editor::sessions
