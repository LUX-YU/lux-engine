#pragma once

#include <Eigen/Geometry>
#include <array>
#include <deque>
#include <list>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace consumer
{
    enum class LUX_ENUM_INFO(compile_time) EMode
    {
        FIRST,
        SECOND,
    };

    struct LUX_TYPE_INFO(compile_time) Settings final
    {
        double LUX_MEMBER(widget = slider, min = 0, max = 10) gain{1.5};
        std::string LUX_MEMBER() name { "Unicode 中文" };
        EMode LUX_MEMBER() mode { EMode::FIRST };
    };

    struct LUX_COMPONENT(schema = "consumer.Component",
                         version = 1,
                         snapshot = COPY,
                         semantic = DOMAIN_CONTRACT,
                         editor = true) Component final
    {
        Settings LUX_MEMBER() settings;
        std::array<double, 3> LUX_MEMBER() fixed { 1, 2, 3 };
        using Grid = int[2][3];
        Grid LUX_MEMBER() grid{{1, 2, 3}, {4, 5, 6}};
        std::vector<Settings> LUX_MEMBER() sequence { {} };
        std::vector<std::array<int, 2>> LUX_MEMBER() pairs { {11, 12} };
        std::tuple<int, std::vector<int>> LUX_MEMBER() grouped { 13, {14, 15} };
        std::vector<bool> LUX_MEMBER() flags { true, false };
        std::deque<std::string> LUX_MEMBER() queue { "first" };
        std::list<int> LUX_MEMBER() list { 4, 5 };
        std::map<std::string, std::vector<int>> LUX_MEMBER() map { {"key", {1, 2}} };
        std::unordered_map<int, std::string> LUX_MEMBER() lookup { {3, "value"} };
        std::set<std::string> LUX_MEMBER() names { "alpha" };
        std::unordered_set<int> LUX_MEMBER() values { 1, 2 };
        std::variant<std::monostate, int, int, std::vector<std::string>> LUX_MEMBER() choice;
        std::optional<int> LUX_MEMBER() optional { 7 };
        int LUX_MEMBER(readonly = true) identity{42};
        Eigen::Quaternionf LUX_MEMBER() rotation_float { Eigen::Quaternionf::Identity() };
        Eigen::Quaterniond LUX_MEMBER() rotation_double { Eigen::Quaterniond::Identity() };
    };

    struct LUX_COMPONENT(schema = "consumer.Derived",
                         version = 1,
                         snapshot = REBUILD,
                         semantic = RUNTIME_DERIVED,
                         editor = false) Derived final
    {
        int cached{};
    };
} // namespace consumer

#if !defined(__LUX_PARSE_TIME__)
#include <consumer/Component.type_static_info.hpp>
#include <lux/engine/simulation/ecs/DecodedComponent.hpp>

namespace lux::simulation::ecs
{
    // Component has implicit memberwise moves. Its standard containers and Eigen
    // values perform no domain validation; MSVC deque may allocate its sentinel.
    template <> inline constexpr bool componentInstallHasNoBusinessFailure<consumer::Component> = true;
} // namespace lux::simulation::ecs
#endif
