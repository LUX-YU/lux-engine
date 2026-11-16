#pragma once

#include <lux/engine/Engine.hpp>

namespace lux::scene
{
    struct SceneDescription;
}

namespace lux::engine
{
    class EngineMetaFunctionRegistry;

    // Cold synchronous assembly. Metadata is borrowed by the builder; the Scene owns its component schemas.
    // Context and registered operations must outlive all constructed Systems.
    // Build never reads assets, starts tasks or adopts the candidate into Engine.
    class Engine::SceneBuilder final
    {
    public:
        SceneBuilder(Engine& engine, const EngineMetaFunctionRegistry& metadata) noexcept;

        [[nodiscard]] cxx::expected<std::unique_ptr<scene::SceneInstance>, error::Error>
        build(const scene::SceneDescription& description) const noexcept;

    private:
        EngineContext& context_;
        const EngineMetaFunctionRegistry& metadata_;
    };
}
