#pragma once
#include <functional>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::project
{
    struct SceneRegistrations;
}
namespace lux::editor
{
    class EditorContext;
    struct SceneCreateInfo final
    {
        asset::AssetId id;
        std::string_view name;
        const project::SceneRegistrations& registrations;
        std::stop_token stop;
    };
    using SceneCreateResult = cxx::expected<scene::ScenePackage, scene::ScenePackageFailure>;

    // A copy retains provider code. Creation synchronously borrows the verified project
    // registrations; the returned package owns only authored values, never a runtime Scene.
    struct SceneProfileRegistration final
    {
        std::shared_ptr<const void> code_lifetime;
        std::string id;
        std::string display_name;
        std::vector<std::string> capabilities;
        SceneCreateResult (*create)(const SceneCreateInfo&) noexcept {};
    };

    class SceneProfileRegistry final
    {
    public:
        SceneProfileRegistry(const SceneProfileRegistry&) = delete;
        SceneProfileRegistry& operator=(const SceneProfileRegistry&) = delete;
        SceneProfileRegistry(SceneProfileRegistry&&) = delete;
        SceneProfileRegistry& operator=(SceneProfileRegistry&&) = delete;

        [[nodiscard]] FrameworkResult<std::reference_wrapper<const SceneProfileRegistration>> find(std::string_view id
        ) const noexcept;
        // Immutable entries have stable addresses until their EditorContext is destroyed.
        [[nodiscard]] std::span<const SceneProfileRegistration> profiles() const noexcept;

    private:
        friend class EditorContext;
        friend class EditorComposition;
        explicit SceneProfileRegistry(std::vector<SceneProfileRegistration>) noexcept;
        std::vector<SceneProfileRegistration> entries_;
    };
} // namespace lux::editor
