#include <lux/engine/function/render/client/features/point_cloud/PointCloudOperation.hpp>

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <tuple>

namespace lux::meta
{
    template <> struct TypeStaticInfo<lux::render::PCSimpleCommConfig>
    {
        static constexpr bool available = true;
        using type = lux::render::PCSimpleCommConfig;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&type::initial_point_size>("initial_point_size"),
            typeStaticField<&type::max_global_points>("max_global_points"),
            typeStaticField<&type::max_octree_nodes>("max_octree_nodes")
        );
    };
    template <> struct TypeStaticInfo<lux::render::PCGPUDrivenCommConfig>
    {
        static constexpr bool available = true;
        using type = lux::render::PCGPUDrivenCommConfig;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&type::initial_point_size>("initial_point_size"),
            typeStaticField<&type::max_nodes>("max_nodes")
        );
    };
    template <> struct TypeStaticInfo<lux::render::PCLODCommConfig>
    {
        static constexpr bool available = true;
        using type = lux::render::PCLODCommConfig;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&type::point_size_world>("point_size_world"),
            typeStaticField<&type::min_size>("min_size"),
            typeStaticField<&type::max_size>("max_size"),
            typeStaticField<&type::max_nodes>("max_nodes")
        );
    };
    template <> struct TypeStaticInfo<lux::render::PCSplattingCommConfig>
    {
        static constexpr bool available = true;
        using type = lux::render::PCSplattingCommConfig;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&type::point_size_world>("point_size_world"),
            typeStaticField<&type::min_size>("min_size"),
            typeStaticField<&type::max_size>("max_size"),
            typeStaticField<&type::max_nodes>("max_nodes")
        );
    };
    template <> struct TypeStaticInfo<lux::render::PCTransientCommConfig>
    {
        static constexpr bool available = true;
        using type = lux::render::PCTransientCommConfig;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&type::point_size>("point_size"),
            typeStaticField<&type::max_points>("max_points")
        );
    };
} // namespace lux::meta

namespace lux::render
{
    const FeatureDescriptor kPCSimpleDescriptor{featureId("lux.render.point_cloud_simple.v1"), "PCSimple"};
    const FeatureDescriptor kPCGPUDrivenDescriptor{featureId("lux.render.point_cloud_gpudriven.v1"), "PCGPUDriven"};
    const FeatureDescriptor kPCLODDescriptor{featureId("lux.render.point_cloud_lod.v1"), "PCLOD"};
    const FeatureDescriptor kPCSplattingDescriptor{featureId("lux.render.point_cloud_splatting.v1"), "PCSplatting"};
    const FeatureDescriptor kPCTransientDescriptor{featureId("lux.render.point_cloud_transient.v1"), "PCTransient"};

    const RenderFeatureRegistration kPCSimpleRegistration{
        "lux.render.point_cloud_simple.v1", &kPCSimpleDescriptor, makeRenderFeatureConfigCodec<PCSimpleCommConfig>(), false};
    const RenderFeatureRegistration kPCGPUDrivenRegistration{
        "lux.render.point_cloud_gpudriven.v1", &kPCGPUDrivenDescriptor,
        makeRenderFeatureConfigCodec<PCGPUDrivenCommConfig>(), false};
    const RenderFeatureRegistration kPCLODRegistration{
        "lux.render.point_cloud_lod.v1", &kPCLODDescriptor, makeRenderFeatureConfigCodec<PCLODCommConfig>(), false};
    const RenderFeatureRegistration kPCSplattingRegistration{
        "lux.render.point_cloud_splatting.v1", &kPCSplattingDescriptor,
        makeRenderFeatureConfigCodec<PCSplattingCommConfig>(), false};
    const RenderFeatureRegistration kPCTransientRegistration{
        "lux.render.point_cloud_transient.v1", &kPCTransientDescriptor,
        makeRenderFeatureConfigCodec<PCTransientCommConfig>(), false};
} // namespace lux::render
