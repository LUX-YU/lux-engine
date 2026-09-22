#pragma once

#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <variant>

namespace lux::scene
{
namespace detail
{
class RenderAssets;
}

enum class ERenderAssetState : std::uint8_t
{
    UNREFERENCED,
    READING,
    UPLOADING,
    READY,
    FAILED,
    CANCELLED,
    CAPACITY
};

struct RenderAssetKey final
{
    SceneInstanceId instance;
    simulation::ecs::Entity entity{simulation::ecs::NullEntity};
    asset::AssetId mesh, material;
    std::uint64_t source_version{}, sequence{};
    friend bool operator==(const RenderAssetKey &, const RenderAssetKey &) = default;
};

struct RenderAssetStatus final
{
    RenderAssetKey key;
    ERenderAssetState state{ERenderAssetState::READING};
    std::variant<std::monostate, process::asset_loading::AssetLoadFailure, process::ETaskStartError,
                 render::ERenderUploadSubmitError>
        failure;
    render::RenderError render_failure;
    std::uint32_t backend_status{};
    asset::AssetId failed_dependency;
};

struct RenderAssetLimits final
{
    std::size_t requests{1024};
    std::size_t transitions_per_turn{16};
    asset::AssetDecodeLimits decode{16 * 1024 * 1024, 32 * 1024 * 1024, 16};
};

// One immutable asset source/version, shared by author and Run instances.
// Its read port must retain that exact source; a version number does not
// turn a mutable VFS into a snapshot. Tasks belong to the host scope.
class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderAssetSource final
{
  public:
    RenderAssetSource(render::RenderRuntime &, process::TaskScope &, process::asset_loading::AssetReadPort,
                      std::uint64_t source_version, RenderAssetLimits = {}, std::shared_ptr<const void> code = {});
    ~RenderAssetSource();
    RenderAssetSource(const RenderAssetSource &) = delete;
    RenderAssetSource &operator=(const RenderAssetSource &) = delete;
    [[nodiscard]] std::uint64_t version() const noexcept;

  private:
    friend class detail::RenderAssets;
    friend class RenderSystem;
    [[nodiscard]] bool uses(const render::RenderRuntime &) const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::scene
