#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/features/Program.hpp>

namespace lux::render
{
    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        std::uint32_t physical_device_index = UINT32_MAX;
        std::uint32_t requests_per_tick = 8;
        std::size_t max_wire_bytes = 1024 * 1024;
        std::size_t max_reply_bytes = 16 * 1024 * 1024;
    };

    struct FeatureRegistration final
    {
        const FeatureDefinition* definition;
        RenderResult<std::vector<PassProgram>> (*programs)(const WireView*, std::string_view) noexcept;
        // Immutable metadata/code remain loaded through run()/shutdown.
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
    };

    // The host owns the rendering thread. Create/register are cold; run owns all
    // object and Native mutation. Only requestStop()/stats() may race with run().
    class RenderServer final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderServer>> create(RenderServerConfig config) noexcept;
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;
        [[nodiscard]] RenderResult<void> registerFeature(FeatureRegistration feature) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;
    private:
        struct State;
        explicit RenderServer(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
