#pragma once

#include <lux/engine/render/extensions/execution/StorageSnapshot.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/extensions/graph/ParameterBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceProgramUses.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <memory>

namespace lux::render
{
    namespace detail
    {
        class RenderGraphInstance;
        struct RasterProgramBuildData;
        struct LinkedPreparationProducer;
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

        // The selected value is valid for the duration of the cold build.
        template <class T>
        [[nodiscard]] RenderResult<PreparedStorageSnapshot<T>> readStorage(StorageSnapshotSlot<T> slot) const noexcept
        {
            auto selected = readStorageValue(slot.operand_);
            if (!selected) return cxx::unexpected(selected.error());
            return PreparedStorageSnapshot<T>{std::cref(*static_cast<const T*>(selected->value)), selected->revision};
        }

    private:
        friend class detail::RenderGraphInstance;
        explicit RasterProgramBuildContext(detail::RasterProgramBuildData& data) noexcept : data_(data) {}

        detail::RasterProgramBuildData& data_;
        [[nodiscard]] RenderResult<const void*>
        readResourceValue(DrawStreamSlot, ResourceInstanceId, cxx::basic_type_info) const noexcept;
        [[nodiscard]] RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(extension_detail::SnapshotOperand) const noexcept;
    };

    // A cold build reads the linked inputs and the last published draw mapping on the owner.
    // Its result moves with the candidate program set until that complete update is adopted.
    class RasterProgramBuilder final
    {
    public:
        using Mapping = std::unique_ptr<const void, void (*)(const void*) noexcept>;
        using Build = RenderResult<Mapping> (*)(const void*, RasterProgramBuildContext&, const void*) noexcept;

        constexpr RasterProgramBuilder() noexcept = default;

        template <class Kernel, RenderResult<void> (*Method)(
            const typename Kernel::LinkedData&, RasterProgramBuildContext&
        ) noexcept>
            requires (Method != nullptr)
        [[nodiscard]] static constexpr RasterProgramBuilder make() noexcept
        {
            using Inputs = typename Kernel::LinkedData;
            return RasterProgramBuilder{registration<Inputs, void, &build<Inputs, Method>>};
        }

        template <class Kernel, RenderResult<std::unique_ptr<typename Kernel::DrawMapping>> (*Method)(
            const typename Kernel::LinkedData&, RasterProgramBuildContext&, const typename Kernel::DrawMapping*
        ) noexcept>
            requires (Method != nullptr) && std::is_nothrow_destructible_v<typename Kernel::DrawMapping>
        [[nodiscard]] static constexpr RasterProgramBuilder make() noexcept
        {
            using Inputs = typename Kernel::LinkedData;
            using Map = typename Kernel::DrawMapping;
            return RasterProgramBuilder{registration<Inputs, Map, &buildMapped<Inputs, Map, Method>>};
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return registration_ != nullptr; }

    private:
        friend struct detail::LinkedPreparationProducer;

        struct Registration final
        {
            cxx::basic_type_info inputs, mapping;
            Build build;
        };

        template <class Inputs, class Map, Build Method>
        static constexpr Registration registration{
            cxx::make_basic_type_info<Inputs>(), cxx::make_basic_type_info<Map>(), Method
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

        template <class Inputs, auto Method>
        static RenderResult<Mapping>
        build(const void* linked, RasterProgramBuildContext& selection, const void*) noexcept
        {
            auto built = Method(*static_cast<const Inputs*>(linked), selection);
            if (!built) return cxx::unexpected(built.error());
            return Mapping{nullptr, nullptr};
        }

        template <class Inputs, class Map, auto Method>
        static RenderResult<Mapping>
        buildMapped(const void* linked, RasterProgramBuildContext& selection, const void* previous) noexcept
        {
            auto built = Method(*static_cast<const Inputs*>(linked), selection, static_cast<const Map*>(previous));
            if (!built) return cxx::unexpected(built.error());
            if (!*built) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return Mapping{built->release(), [](const void* value) noexcept { delete static_cast<const Map*>(value); }};
        }

        const Registration* registration_ = nullptr;
    };

} // namespace lux::render
