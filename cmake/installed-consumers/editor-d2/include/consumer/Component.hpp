#pragma once

#include <Eigen/Geometry>
#include <array>
#include <deque>
#include <list>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#if defined(CONSUMER_MEASURE_COPIES)
#include <atomic>
#endif

namespace consumer
{
#if defined(CONSUMER_MEASURE_COPIES)
    inline std::atomic_uint64_t settings_copies{};
#endif
    enum class LUX_ENUM_INFO(compile_time) EMode
    {
        FIRST,
        SECOND,
    };

    struct LUX_TYPE_INFO(compile_time) Settings final
    {
#if defined(CONSUMER_MEASURE_COPIES)
        Settings() = default;
        Settings(const Settings &other) : gain(other.gain), name(other.name), mode(other.mode)
        {
            settings_copies.fetch_add(1, std::memory_order_relaxed);
        }
        Settings &operator=(const Settings &other)
        {
            gain = other.gain;
            name = other.name;
            mode = other.mode;
            settings_copies.fetch_add(1, std::memory_order_relaxed);
            return *this;
        }
        Settings(Settings &&) noexcept = default;
        Settings &operator=(Settings &&) noexcept = default;
#endif
        double LUX_MEMBER(widget = slider, min = 0, max = 10) gain{1.5};
        std::string LUX_MEMBER() name { "Unicode 中文" };
        EMode LUX_MEMBER() mode { EMode::FIRST };
    };

    struct LUX_COMPONENT(schema = "consumer.Component", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT,
                         editor = true) Component final
    {
        Settings LUX_MEMBER() settings;
        std::array<double, 3> LUX_MEMBER() fixed { 1, 2, 3 };
        std::vector<Settings> LUX_MEMBER() sequence { {} };
        std::vector<bool> LUX_MEMBER() flags { true, false };
        std::deque<std::string> LUX_MEMBER() queue { "first" };
        std::list<int> LUX_MEMBER() list { 4, 5 };
        std::map<std::string, std::vector<int>> LUX_MEMBER() map { {"key", {1, 2}} };
        std::unordered_map<int, std::string> LUX_MEMBER() lookup { {3, "value"} };
        std::set<std::string> LUX_MEMBER() names { "alpha" };
        std::unordered_set<int> LUX_MEMBER() values { 1, 2 };
        std::variant<std::monostate, int, int, std::vector<std::string>> LUX_MEMBER() choice;
        int LUX_MEMBER(readonly = true) identity{42};
        Eigen::Quaternionf LUX_MEMBER() rotation_float { Eigen::Quaternionf::Identity() };
        Eigen::Quaterniond LUX_MEMBER() rotation_double { Eigen::Quaterniond::Identity() };
    };

    struct LUX_COMPONENT(schema = "consumer.Derived", version = 1, snapshot = REBUILD, semantic = RUNTIME_DERIVED,
                         editor = false) Derived final
    {
        int cached{};
    };
} // namespace consumer

#if !defined(__LUX_PARSE_TIME__)
#include <consumer/Component.type_static_info.hpp>
#endif
