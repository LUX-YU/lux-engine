#pragma once
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include "AllocationProbe.hpp"
#include <Eigen/Geometry>
#include <array>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace inspector_fixture
{
    enum class LUX_ENUM_INFO(static) EMode { FIRST = 3, SECOND = 7 };
    struct LUX_TYPE_INFO(static) Nested
    {
        friend bool operator==(const Nested &, const Nested &) = default;
        double LUX_MEMBER(widget = slider, min = -10, max = 10, display_name = Amount) amount{2.0};
        std::string LUX_MEMBER(display_name = Text) text{"nested"};
    };
    struct Angle
    {
        double radians{};
        friend bool operator==(const Angle &, const Angle &) = default;
    };
    struct LUX_COMPONENT(schema = "test.GeneratedInspector", version = 1, snapshot = COPY,
        semantic = DOMAIN_CONTRACT, editor = true) Component
    {
        friend bool operator==(const Component &, const Component &) = default;
        bool enabled{};
        std::int8_t small{-3};
        std::uint8_t byte{7};
        std::int16_t short_value{-100};
        std::uint16_t word{100};
        std::int32_t signed_value{-123};
        std::uint32_t unsigned_value{123};
        std::int64_t wide{-1234567890123};
        std::uint64_t unsigned_wide{1234567890123};
        float LUX_MEMBER(widget = input, step = 0.25) weight{1};
        double LUX_MEMBER(widget = drag, speed = 0.05) precise{2};
        std::string caption{"Generated input"};
        EMode mode{EMode::SECOND};
        Nested nested;
        std::array<float, 3> fixed{1, 2, 3};
        int raw[2][2]{{4, 5}, {6, 7}};
        std::vector<int, AllocationProbe<int>> failing{1, 2};
        std::vector<bool> bits{true, false};
        std::vector<Nested> records{{}};
        std::deque<int> deque{1, 2};
        std::list<double> list{1, 2};
        std::map<int, std::string> map{{1, "one"}};
        std::unordered_map<int, float> hash_map{{2, 3}};
        std::set<int> set{1, 2};
        std::unordered_set<int> hash_set{3, 4};
        std::optional<Nested> optional{Nested{}};
        std::variant<int, std::string> variant{std::string{"choice"}};
        std::pair<int, std::string> pair{3, "pair"};
        std::tuple<int, double, Nested> tuple{1, 2, {}};
        Angle LUX_MEMBER(widget = custom) angle{0.5};
        Eigen::Vector3d vector{1, 2, 3};
        Eigen::Quaterniond rotation{Eigen::Quaterniond::Identity()};
        double LUX_MEMBER(readonly = true) derived{9};
        int LUX_NO_MEMBER() hidden{42};
    };
    struct LUX_COMPONENT(schema = "test.GeneratedSmall", version = 1, snapshot = COPY,
        semantic = DOMAIN_CONTRACT, editor = true) SmallComponent
    {
        bool LUX_MEMBER(display_name = Enabled) enabled{};
        double LUX_MEMBER(display_name = Number, widget = input, step = 1) number{1};
        std::string LUX_MEMBER(display_name = Text) text{"abc"};
        std::vector<int> LUX_MEMBER(display_name = Values) values{1, 2};
    };
}
