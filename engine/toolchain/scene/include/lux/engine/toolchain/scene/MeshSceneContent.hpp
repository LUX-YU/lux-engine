#pragma once

namespace lux::engine { class EngineMetaFunctionRegistry; }

namespace lux::engine::toolchain
{
    // Registers the existing Mesh3D/Transform3D author geometry; no GPU resources or running systems.
    [[nodiscard]] bool registerMeshSceneContent(EngineMetaFunctionRegistry& metadata) noexcept;
}
