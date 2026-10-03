#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <any>

namespace lux::editor::material
{
    struct MaterialPreviewEnvironment final
    {
        scene::ProjectionEnvironment scene;
        std::vector<render::RenderFeatureRegistration> features;
    };
    enum class EMaterialPreviewError : std::uint8_t { BUSY, CLOSED, INVALID_INPUT, CAPACITY, PREPARATION };
    struct MaterialPreviewFailure final
    {
        EMaterialPreviewError code;
        std::string domain;
        std::any cause;
    };
    template<class T> using MaterialPreviewResult = cxx::expected<T, MaterialPreviewFailure>;
    // A fixed mesh asset image, independent of any live preview or compilation target.
    // SharedBytes must retain immutable encoded MeshAsset bytes, including their asset identity.
    struct MaterialPreviewRecipe final
    {
        asset::AssetId mesh;
        cxx::SharedBytes<> mesh_image;
    };
    [[nodiscard]] MaterialPreviewResult<MaterialPreviewRecipe> makeSphereMaterialPreviewRecipe();

    struct PreviewAdoptionKey final
    {
        uuids::uuid target;
        std::uint64_t generation{};
        MaterialCompileInputKey input;
        std::uint64_t recipe{}, environment{1};
        friend bool operator==(const PreviewAdoptionKey&, const PreviewAdoptionKey&) = default;
    };
    struct MaterialPreviewStatus final
    {
        PreviewAdoptionKey desired;
        std::optional<PreviewAdoptionKey> prepared, accepted;
        bool stale{};
        std::string diagnostic;
        std::optional<VMaterialCompileFailure> compilation_failure;
        std::optional<MaterialPreviewFailure> failure;
    };
    // One live target, with independent adoption generations and original Runtime retirement.
    class MaterialPreview final
    {
    public:
        MaterialPreview(lux::scene::SceneRuntime&, MaterialPreviewEnvironment);
        ~MaterialPreview();
        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;
        MaterialPreview(MaterialPreview&&) = delete;
        MaterialPreview& operator=(MaterialPreview&&) = delete;
        // An omitted recipe retains the current one; first use selects the public sphere recipe.
        // Explicitly supplying different mesh bytes also changes identity when the AssetId is unchanged.
        [[nodiscard]] MaterialPreviewResult<PreviewAdoptionKey>
        setDesired(MaterialCompileInputKey, std::optional<MaterialPreviewRecipe> = {}) noexcept;
        // Accept owning completion facts; never borrow task control. Stale completions only settle.
        [[nodiscard]] MaterialPreviewResult<void> receive(PreviewAdoptionKey,
            MaterialCompileResult<std::shared_ptr<const CompiledMaterial>>, lux::scene::RenderAssetInput);
        void update() noexcept;
        [[nodiscard]] MaterialPreviewResult<void> reset(lux::scene::RenderAssetInput);
        [[nodiscard]] MaterialPreviewResult<void>
        navigate(const simulation::ecs::Transform3D&, const lux::scene::Camera&);
        [[nodiscard]] MaterialPreviewStatus status() const;
        [[nodiscard]] simulation::ecs::Entity camera() const noexcept;
        [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] lux::scene::InstanceRetirement close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
