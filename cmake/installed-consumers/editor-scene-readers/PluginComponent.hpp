#pragma once
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <Eigen/Geometry>
#include <string>
namespace consumer
{
    enum class EMode { BASIC, DETAILED };
    struct LUX_COMPONENT(schema = "consumer.RichComponent", version = 3, snapshot = COPY,
        semantic = DOMAIN_CONTRACT, editor = true) RichComponent final
    {
        std::string LUX_MEMBER(display_name = Caption) caption{"Plugin component"};
        bool LUX_MEMBER(display_name = Enabled) enabled{true};
        std::int64_t LUX_MEMBER(display_name = Signed) signed_value{-1234567890123};
        std::uint64_t LUX_MEMBER(display_name = Unsigned) unsigned_value{12345678901234};
        double LUX_MEMBER(display_name = Precision) precision{1.23456789012345};
        float LUX_MEMBER(display_name = Weight) weight{0.75F};
        EMode LUX_MEMBER(display_name = Mode) mode{EMode::DETAILED};
        Eigen::Vector3d LUX_MEMBER(display_name = Position) position{1, 2, 3};
        Eigen::Quaterniond LUX_MEMBER(display_name = Rotation) rotation{Eigen::Quaterniond::Identity()};
        Eigen::Matrix3d LUX_MEMBER(display_name = Matrix) matrix{Eigen::Matrix3d::Identity()};
        lux::asset::AssetId LUX_MEMBER(display_name = Resource) resource;
        int LUX_NO_MEMBER() hidden{17};
    };
}
