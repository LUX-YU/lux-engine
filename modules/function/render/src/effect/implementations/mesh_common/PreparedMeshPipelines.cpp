#include <lux/engine/render/effect/implementations/mesh_common/PreparedMeshPipelines.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>
#include <lux/engine/render/resource/instance/Data.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/description/GBufferLayout.hpp>

namespace lux::render::detail
{
    namespace
    {
        template <class Program> bool matchesGeometry(const Program& program, const MeshVertexLayout& mesh) noexcept
        {
            if (program.vertices.size() != 1 || program.vertices.front().stride != mesh.vertex_stride) return false;
            for (const auto& field : program.geometry_attributes)
            {
                const auto attribute = std::ranges::find(mesh.attributes, field.location, &GeometryAttribute::location);
                const bool compatible = attribute != mesh.attributes.end() &&
                    attribute->offset == field.offset && attribute->format == field.format;
                if (!compatible) return false;
            }
            return true;
        }

        bool matchesGeometry(const PublishedRasterProgram& program, const MeshVertexLayout& mesh) noexcept
        {
            return matchesGeometry(*program, mesh);
        }

        RenderResult<MeshVertexLayout>
        resolveMesh(const ResourceRegistry& resources, const DeviceSession& device, MeshId mesh) noexcept
        {
            const auto owner = extension_detail::ResourceIdentity::parent(mesh);
            if (resources.deviceOf(owner) != &device) return cxx::unexpected(RenderError{kStorageIdentity});
            if (resources.hasExclusiveCommand(owner)) return cxx::unexpected(RenderError{kDeviceBusy});
            const auto meshes = resources.borrow<IndexedTriangleMeshStorage>(owner);
            if (!meshes) return cxx::unexpected(meshes.error());
            const auto* description = (*meshes)->find(mesh);
            if (!description) return cxx::unexpected(RenderError{kStorageIdentity});
            return MeshVertexLayout{description->vertex_stride, description->attributes};
        }

        RenderResult<RasterStreamTarget> meshTarget(
            const DeviceSession& device, rdesc::EMaterialPurpose purpose, const MeshPipelineUse& use
        ) noexcept
        {
            using Purpose = rdesc::EMaterialPurpose;
            using Format = rdesc::ETextureFormat;
            if (use.section_depth_bias && purpose != Purpose::SHADOW)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto available = device.resourceFeatures();
            const auto depth = purpose == Purpose::GBUFFER ? rdesc::GBufferLayout::depth_format : Format::D32_SFLOAT;
            RasterStreamTarget target{
                {}, imageFormat(depth).native,
                1, use.section_depth_bias,
                available.containsAny(EGpuResourceFeature::SAMPLED_IMAGE_ARRAY),
                available.containsAny(EGpuResourceFeature::STORAGE_BUFFER_ARRAY)
            };
            if (purpose == Purpose::GBUFFER)
            {
                for (const auto color : rdesc::GBufferLayout::color_formats)
                    target.colors.push_back(imageFormat(color).native);
            }
            else if (purpose == Purpose::FORWARD)
            {
                if (use.forward_format != Format::RGBA8_UNORM && use.forward_format != Format::RGBA8_SRGB)
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                target.colors.push_back(imageFormat(use.forward_format).native);
            }
            return target;
        }
    } // namespace

    exec::task<PreparedMeshPipelines::Result> PreparedMeshPipelines::create(
        DeviceEntry& device, const MaterialMeshPipelines& request,
        const MeshMaterialShader& shader, const ResourceRegistry& resources
    ) noexcept
    {
        auto candidate = std::unique_ptr<PreparedMeshPipelines>(new PreparedMeshPipelines(device));
        auto recipes = candidate->link(request, shader, resources);
        if (!recipes) co_return cxx::unexpected(recipes.error());
        if (auto compiled = co_await candidate->table_.compile(std::move(*recipes)); !compiled)
            co_return cxx::unexpected(compiled.error());
        co_return std::unique_ptr<const PreparedMeshPipelines>{std::move(candidate)};
    }

    template <class Program> std::size_t PreparedMeshPipelines::findUse(
        std::span<const Program> programs, const MeshVertexLayout& mesh, bool front_counter_clockwise,
        const RasterStreamTarget& target
    ) const noexcept
    {
        for (std::size_t i = 0; i < uses_.size(); ++i)
        {
            const auto& use = uses_[i];
            if (use.front_counter_clockwise == front_counter_clockwise && use.target == target &&
                matchesGeometry(programs[kMeshWindingCount * i], mesh)) return i;
        }
        return uses_.size();
    }

    RenderResult<bool> PreparedMeshPipelines::matches(
        const MaterialMeshPipelines& request, const ResourceRegistry& resources
    ) const noexcept
    {
        if (request.uses.size() < uses_.size()) return false;
        const auto& session = device_.session();
        std::vector<bool> selected(uses_.size());
        for (const auto& use : request.uses)
        {
            const auto geometry = resolveMesh(resources, session, use.mesh);
            if (!geometry) return cxx::unexpected(geometry.error());
            const auto target = meshTarget(session, request.purpose, use);
            if (!target) return cxx::unexpected(target.error());
            const auto index = findUse(table_.programs(), *geometry, use.front_counter_clockwise, *target);
            if (index == uses_.size()) return false;
            selected[index] = true;
        }
        return std::ranges::find(selected, false) == selected.end();
    }

    RenderResult<std::vector<RasterProgramLink>> PreparedMeshPipelines::link(
        const MaterialMeshPipelines& request, const MeshMaterialShader& shader,
        const ResourceRegistry& resources
    ) noexcept
    {
        auto& session = device_.session();
        std::vector<RasterProgramLink> recipes;
        constexpr auto maximum_uses = kGraphInvalidIndex / kMeshWindingCount;
        if (request.uses.size() > maximum_uses) return cxx::unexpected(RenderError{kDeviceCapacity});
        recipes.reserve(request.uses.size() * kMeshWindingCount);
        uses_.reserve(request.uses.size());
        const auto arguments = meshRasterInputs(session.resourceFeatures().containsAny(EGpuResourceFeature::STORAGE_BUFFER_ARRAY));
        ParameterBinding parameters;
        parameters.setInlineConstant(MeshViewLayer{});
        for (const auto& use : request.uses)
        {
            const auto geometry = resolveMesh(resources, session, use.mesh);
            if (!geometry) return cxx::unexpected(geometry.error());
            auto target = meshTarget(session, request.purpose, use);
            if (!target) return cxx::unexpected(target.error());
            const auto linked = std::span<const RasterProgramLink>(recipes);
            const auto index = findUse(linked, *geometry, use.front_counter_clockwise, *target);
            if (index != uses_.size()) continue;
            auto layout = meshRasterLayout(*geometry, shader);
            if (!layout) return cxx::unexpected(layout.error());
            // D3D12 flips the viewport. Reflection reverses the resulting framebuffer winding once more.
            const bool front_ccw = use.front_counter_clockwise != (shader.target == rdesc::EShaderTarget::DXIL);
            for (const bool reflected : {false, true})
            {
                layout->graphics.front_counter_clockwise = front_ccw != reflected;
                auto recipe = linkRasterStreamProgram(session, {
                    arguments, parameters, *target, shader.artifact(reflected), layout->graphics, layout->geometry_locations
                });
                if (!recipe) return cxx::unexpected(recipe.error());
                recipes.push_back(std::move(*recipe));
            }
            uses_.push_back({use.front_counter_clockwise, std::move(*target)});
        }
        return recipes;
    }

    std::span<const PublishedRasterProgram> PreparedMeshPipelines::find(
        const MeshVertexLayout& mesh, bool front_counter_clockwise, const RasterStreamTarget& target
    ) const & noexcept
    {
        const auto index = findUse(table_.programs(), mesh, front_counter_clockwise, target);
        if (index == uses_.size()) return {};
        return table_.programs().subspan(kMeshWindingCount * index, kMeshWindingCount);
    }

} // namespace lux::render::detail
