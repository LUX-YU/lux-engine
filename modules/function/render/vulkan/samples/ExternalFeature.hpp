#pragma once
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <tuple>

namespace sample_ext
{
struct Tint final { float red{1}, green{}, blue{}, alpha{1}; };
inline constexpr auto kFeature = lux::render::featureId("sample.render.triangle");
inline constexpr lux::render::FeatureDescriptor kDescriptor{
    .type = kFeature, .name = "SampleExternal", .abi_version = 1,
    .canonical_name = "sample.render.triangle"
};
struct ColorOp final
{
    using Payload = lux::render::SetFeatureParamsPayload;
    static constexpr auto kind = lux::render::EOpKind::Param;
    static constexpr auto lane = lux::render::EOperationLane::Program;
    static constexpr const char *name = "sample.render.triangle.color";
};
}
namespace lux::meta
{
template<> struct TypeStaticInfo<sample_ext::Tint>
{
    static constexpr bool available = true;
    static constexpr auto fields = std::make_tuple(typeStaticField<&sample_ext::Tint::red>("red"),
        typeStaticField<&sample_ext::Tint::green>("green"), typeStaticField<&sample_ext::Tint::blue>("blue"),
        typeStaticField<&sample_ext::Tint::alpha>("alpha"));
};
}
