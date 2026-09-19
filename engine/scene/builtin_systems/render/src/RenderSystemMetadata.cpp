#include <lux/engine/object/ObjectReflection.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>

#include <algorithm>
#include <unordered_map>

namespace lux::scene
{
    struct RenderSystemMetadata::Impl final
    {
        std::vector<render::RenderFeatureRegistration> render_feature_registrations;
        std::vector<RenderFeatureMeta> render_features;
        std::vector<std::vector<std::byte>> render_feature_default_configuration_storage;
        std::unordered_map<std::uint64_t, std::size_t> render_feature_by_hash;
    };

    namespace
    {
        RenderSystemMetaFailure failure(ERenderSystemMetaError code, std::uint64_t subject = 0,
                                        lux::serialization::SerializationFailure configuration = {})
        {
            return {code, subject, configuration};
        }

        [[nodiscard]] const meta::RefClass *configurationReflection(
            const lux::serialization::PortableValueCodec &codec) noexcept
        {
            if (!codec.valid())
            {
                return nullptr;
            }
            const auto *reflection = meta::ReflectionRegistry::instance().findClass(codec.type.name());
            return reflection != nullptr && reflection->construct != nullptr && reflection->destruct != nullptr
                       ? reflection
                       : nullptr;
        }

    } // namespace

    RenderSystemMetadata::RenderSystemMetadata(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    RenderSystemMetadata::RenderSystemMetadata(RenderSystemMetadata &&) noexcept = default;
    RenderSystemMetadata &RenderSystemMetadata::operator=(RenderSystemMetadata &&) noexcept = default;
    RenderSystemMetadata::~RenderSystemMetadata() = default;

    lux::cxx::expected<RenderSystemMetadata, RenderSystemMetaFailure> RenderSystemMetadata::build(
        const SceneMetaManager &scene, std::vector<render::RenderFeatureRegistration> features,
        std::span<const RenderFeatureSceneBinding> bindings)
    {
        auto impl = std::make_unique<Impl>();
        impl->render_feature_registrations = std::move(features);
        impl->render_feature_default_configuration_storage.reserve(impl->render_feature_registrations.size());
        impl->render_features.reserve(impl->render_feature_registrations.size());
        impl->render_feature_by_hash.reserve(impl->render_feature_registrations.size());
        std::vector<std::string_view> render_display_names;
        render_display_names.reserve(impl->render_feature_registrations.size());
        for (std::size_t index{}; index < impl->render_feature_registrations.size(); ++index)
        {
            const auto &registration = impl->render_feature_registrations[index];
            const bool invalid_identity =
                registration.stable_name.empty() || registration.descriptor == nullptr ||
                !registration.descriptor->valid() || registration.descriptor->name.empty() ||
                render::featureId(registration.stable_name) != registration.descriptor->type ||
                !registration.configuration.valid();
            if (invalid_identity)
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::INVALID_RENDER_FEATURE,
                            registration.descriptor != nullptr ? registration.descriptor->type : 0U));
            }
            const auto existing = impl->render_feature_by_hash.find(registration.descriptor->type);
            if (existing != impl->render_feature_by_hash.end())
            {
                const auto &previous = impl->render_feature_registrations[existing->second];
                const auto code = previous.stable_name == registration.stable_name
                                      ? ERenderSystemMetaError::DUPLICATE_RENDER_FEATURE
                                      : ERenderSystemMetaError::RENDER_FEATURE_TYPE_COLLISION;
                return lux::cxx::unexpected(failure(code, registration.descriptor->type));
            }
            if (std::ranges::find(render_display_names, registration.descriptor->name) != render_display_names.end())
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::DUPLICATE_RENDER_FEATURE, registration.descriptor->type));
            }
            const auto *reflection = configurationReflection(registration.configuration.portable);
            if (reflection == nullptr)
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::CONFIGURATION_REFLECTION_NOT_FOUND, registration.descriptor->type));
            }
            impl->render_feature_default_configuration_storage.emplace_back();
            auto &defaults = impl->render_feature_default_configuration_storage.back();
            auto encoded = registration.configuration.portable.encode_default(defaults);
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(ERenderSystemMetaError::CONFIGURATION_DEFAULT_ENCODING_FAILURE,
                                                    registration.descriptor->type, encoded.error()));
            }
            std::vector<std::byte> attach;
            auto materialized = registration.configuration.materialize_attach(defaults, attach);
            if (!materialized || attach.size() != registration.configuration.attach_wire_size)
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::INVALID_RENDER_FEATURE, registration.descriptor->type,
                            materialized ? lux::serialization::SerializationFailure{} : materialized.error()));
            }
            impl->render_feature_by_hash.emplace(registration.descriptor->type, index);
            render_display_names.push_back(registration.descriptor->name);
            impl->render_features.push_back({registration.descriptor->type,
                                             registration.stable_name,
                                             registration.descriptor->name,
                                             &registration,
                                             reflection,
                                             defaults,
                                             registration.scene_configurable,
                                             {},
                                             {}});
        }

        std::vector<std::uint8_t> binding_present(impl->render_features.size(), 0U);
        for (const auto &binding : bindings)
        {
            const auto feature = impl->render_feature_by_hash.find(binding.feature);
            if (feature == impl->render_feature_by_hash.end())
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::UNKNOWN_RENDER_FEATURE_BINDING, binding.feature));
            }
            if (scene.getSceneSystemMeta(binding.scene_system) == nullptr)
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::UNKNOWN_SCENE_SYSTEM_BINDING, binding.scene_system.hash));
            }
            const std::size_t feature_ordinal = feature->second;
            if (binding_present[feature_ordinal] != 0U)
            {
                return lux::cxx::unexpected(
                    failure(ERenderSystemMetaError::DUPLICATE_RENDER_FEATURE_BINDING, binding.feature));
            }
            binding_present[feature_ordinal] = 1U;
            auto &meta = impl->render_features[feature_ordinal];
            for (const auto &observation : binding.observations)
            {
                if (!observation.component.isValid())
                {
                    return lux::cxx::unexpected(
                        failure(ERenderSystemMetaError::INVALID_RENDER_FEATURE_BINDING, binding.feature));
                }
                if (scene.getComponentMeta(observation.component) == nullptr)
                {
                    return lux::cxx::unexpected(
                        failure(ERenderSystemMetaError::UNKNOWN_COMPONENT_SCHEMA, observation.component.hash()));
                }
            }
            meta.create_sync_stage = binding.create_sync_stage;
            meta.observations = binding.observations;
        }

        return RenderSystemMetadata(std::move(impl));
    }

    const RenderFeatureMeta *RenderSystemMetadata::feature(render::FeatureTypeId type) const noexcept
    {
        const auto found = impl_->render_feature_by_hash.find(type);
        return found != impl_->render_feature_by_hash.end() ? &impl_->render_features[found->second] : nullptr;
    }

    const RenderFeatureMeta *RenderSystemMetadata::feature(std::string_view stable_name) const noexcept
    {
        const auto type = render::featureId(stable_name);
        const auto *meta = feature(type);
        return meta != nullptr && meta->stable_name == stable_name ? meta : nullptr;
    }

    std::span<const RenderFeatureMeta> RenderSystemMetadata::features() const noexcept
    {
        return impl_->render_features;
    }
} // namespace lux::scene
