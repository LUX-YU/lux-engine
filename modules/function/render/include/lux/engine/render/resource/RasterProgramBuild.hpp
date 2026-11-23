#pragma once

#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/extensions/graph/ParameterBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceProgramUses.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <array>
#include <memory>
#include <tuple>

namespace lux::render
{
    namespace detail
    {
        class RasterProgramInputs;
        struct RasterProgramBuildData;
        struct LinkedRasterBuild;
    }

    // Index into the shader artifacts submitted for this stream and selection.
    struct ShaderCandidateSlot final { std::uint32_t index = kGraphInvalidIndex; };

    struct RuntimeRasterProgram final
    {
        ShaderCandidateSlot shader;
        GraphicsState graphics;
        ParameterBinding parameters;
        ResourceProgramSource source;
        // In graphics.vertex_attributes order. Empty uses the shader locations directly.
        std::vector<std::uint32_t> geometry_locations;
    };

    class LUX_RENDER_PUBLIC RasterProgramBuildContext final
    {
    public:
        RasterProgramBuildContext(const RasterProgramBuildContext&) = delete;
        RasterProgramBuildContext& operator=(const RasterProgramBuildContext&) = delete;

        // Only declared Resource inputs are visible during synchronous program selection.
        template <class Storage>
        [[nodiscard]] RenderResult<const Storage*>
        readResource(DrawStreamSlot stream, ResourceInstanceId id) const noexcept
        {
            auto result = readResourceValue(stream, id, cxx::make_basic_type_info<Storage>());
            if (!result) return cxx::unexpected(result.error());
            return static_cast<const Storage*>(*result);
        }

        // Select every program for this revision. The renderer owns the selected inputs across scheduler hops.
        [[nodiscard]] RenderResult<void>
        setPrograms(
            DrawStreamSlot stream, std::vector<rdesc::ShaderProgramArtifact> shaders,
            std::span<const RuntimeRasterProgram> programs
        ) noexcept;

        // Use the existing ordered bindings when the producer's complete program key set is unchanged.
        [[nodiscard]] RenderResult<void> reusePrograms(DrawStreamSlot stream) noexcept;

    private:
        friend class RasterProgramBuilder;
        friend class detail::RasterProgramInputs;
        explicit RasterProgramBuildContext(detail::RasterProgramBuildData& data) noexcept : data_(data) {}

        detail::RasterProgramBuildData& data_;
        [[nodiscard]] RenderResult<const void*>
        readResourceValue(DrawStreamSlot, ResourceInstanceId, cxx::basic_type_info) const noexcept;
        [[nodiscard]] RevisionedValue<const void*> snapshot(std::size_t input) const noexcept;

        template <class... T> [[nodiscard]] SnapshotValues<T...> values() const noexcept
        {
            return extension_detail::snapshotValues<T...>(
                [this](std::size_t index) noexcept { return snapshot(index); }, std::index_sequence_for<T...>{}
            );
        }
    };

    // A cold build reads the linked inputs and the last published draw mapping on the owner.
    // Its result moves with the candidate program set until that complete update is adopted.
    class RasterProgramBuilder final
    {
    public:
        using Mapping = std::unique_ptr<const void, void (*)(const void*) noexcept>;
        using Build = RenderResult<Mapping> (*)(const void*, RasterProgramBuildContext&, const void*) noexcept;

        constexpr RasterProgramBuilder() noexcept = default;
        bool operator==(const RasterProgramBuilder&) const noexcept = default;

    private:
        template <class K, auto Method, class... T>
        static constexpr RasterProgramBuilder bind(RenderResult<void> (*)(
            const typename K::LinkedData&, RasterProgramBuildContext&, const SnapshotValues<T...>&
        ) noexcept) noexcept
        {
            using Inputs = typename K::LinkedData;
            return RasterProgramBuilder{registration<Inputs, void, &build<Inputs, Method, T...>, T...>};
        }

        template <class K, auto Method, class... T>
        static constexpr RasterProgramBuilder bind(RenderResult<std::unique_ptr<typename K::DrawMapping>> (*)(
            const typename K::LinkedData&, RasterProgramBuildContext&, const SnapshotValues<T...>&,
            const typename K::DrawMapping*
        ) noexcept) noexcept requires std::is_nothrow_destructible_v<typename K::DrawMapping>
        {
            using Inputs = typename K::LinkedData;
            using Map = typename K::DrawMapping;
            return RasterProgramBuilder{registration<Inputs, Map, &buildMapped<Inputs, Map, Method, T...>, T...>};
        }

    public:
        template <class K, auto Method> requires (Method != nullptr) && requires { bind<K, Method>(Method); }
        [[nodiscard]] static constexpr RasterProgramBuilder make() noexcept { return bind<K, Method>(Method); }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return registration_ != nullptr; }

    private:
        friend struct detail::LinkedRasterBuild;
        friend class EffectCompileContext;

        struct Registration final
        {
            cxx::basic_type_info inputs, mapping;
            Build build;
            std::span<const cxx::basic_type_info> snapshots;
        };

        template <class Inputs, class Map, Build Method, class... T>
        static constexpr Registration registration{
            cxx::make_basic_type_info<Inputs>(), cxx::make_basic_type_info<Map>(), Method, extension_detail::snapshot_types<T...>
        };

        explicit constexpr RasterProgramBuilder(const Registration& entry) noexcept : registration_(&entry) {}

        [[nodiscard]] Build link(cxx::basic_type_info inputs, cxx::basic_type_info mapping) const noexcept
        {
            if (!registration_) return nullptr;
            const auto matches = [](cxx::basic_type_info a, cxx::basic_type_info b) noexcept
            {
                return a == b && a.name() == b.name();
            };
            const bool compatible = matches(registration_->inputs, inputs) && matches(registration_->mapping, mapping);
            return compatible ? registration_->build : nullptr;
        }

        template <class Inputs, auto Method, class... T>
        static RenderResult<Mapping>
        build(const void* linked, RasterProgramBuildContext& selection, const void*) noexcept
        {
            auto built = Method(
                *static_cast<const Inputs*>(linked), selection, selection.values<T...>()
            );
            if (!built) return cxx::unexpected(built.error());
            return Mapping{nullptr, nullptr};
        }

        template <class Inputs, class Map, auto Method, class... T>
        static RenderResult<Mapping>
        buildMapped(const void* linked, RasterProgramBuildContext& selection, const void* previous) noexcept
        {
            auto built = Method(
                *static_cast<const Inputs*>(linked), selection, selection.values<T...>(),
                static_cast<const Map*>(previous)
            );
            if (!built) return cxx::unexpected(built.error());
            if (!*built) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return Mapping{built->release(), [](const void* value) noexcept { delete static_cast<const Map*>(value); }};
        }

        const Registration* registration_ = nullptr;
    };

    struct RasterProgramSource final
    {
        RasterProgramBuilder builder;
        std::vector<GraphStorageSnapshotId> snapshots;
        bool operator==(const RasterProgramSource&) const noexcept = default;
    };

} // namespace lux::render
