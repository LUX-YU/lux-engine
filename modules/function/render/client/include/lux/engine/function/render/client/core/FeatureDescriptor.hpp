#pragma once
/**
 * @file FeatureDescriptor.hpp
 * @brief Static, type-level metadata for a render feature.
 *
 * Promotes the relationships that today live only in comments + add-order (draft
 * §5.1) into declared data: a feature's stable type, its dependencies, the types
 * it conflicts with, and capability flags. Lives at the TYPE level (carried by the
 * FeatureFactory), because the FeatureManager must resolve dependencies BEFORE any
 * instance exists.
 *
 * This introduces it as data; the dependency/conflict RESOLUTION that consumes
 * it (topological install order, conflict rejection, transactional rollback) lands
 * in the SceneFeatureManager — see .internal/UNFINISHED-WORK.md §2bis (slices 3c).
 * A descriptor with empty deps/conflicts (the makeSimpleFactory default) preserves
 * today's "caller-ordered, no validation" behaviour exactly.
 */

#include <lux/engine/function/render/client/core/FeatureTypeId.hpp>
#include <lux/engine/function/render/client/core/EFeatureLevel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::render
{
    /// How many instances of a feature type a single scene may hold.
    enum class FeatureMultiplicity : std::uint8_t
    {
        MultiplePerScene,  ///< default — any number of instances (today's behaviour)
        SinglePerScene,    ///< at most one; a second install of this type is rejected
    };

    /// One declared dependency of a feature on another feature type.
    struct FeatureDependency
    {
        FeatureTypeId type{kInvalidFeatureTypeId};
        /// Optional dependency: if absent the dependent still installs (it adapts);
        /// a required (optional=false) dependency that cannot be resolved fails the install.
        bool optional{false};
    };

    /// `LUX_COMM_CONFIG(requires=)` 的编译期解析产物。上限 4 条 —— 超限时
    /// parseFeatureDependencies 里的数组下标越界让 constexpr 求值**编译期失败**,
    /// 不存在静默截断。
    inline constexpr std::size_t kMaxDeclaredFeatureDeps = 4;

    struct FeatureDependencyList
    {
        std::array<FeatureDependency, kMaxDeclaredFeatureDeps> items{};
        std::size_t count{0};
    };

    /// 解析 `requires=` 注解值:逗号分隔多条稳定 id 名,`?` 尾缀 = 可选依赖
    /// (缺席不拒装;两者都在 attach 集合里时参与排序)。多条时注解值必须用双引号
    /// 包裹 —— `#__VA_ARGS__` 字符串化会被裸逗号切开,而生成器的分词器对引号内
    /// 不切分、且会剥掉包裹引号,模板拿到的就是本函数吃的裸串。
    /// 唯一预期调用方是生成器模板(comm_ops_cpp.template);全显式类型,零 CTAD
    /// (libclang 不吃别名模板 CTAD —— 本仓已知坑)。
    [[nodiscard]] constexpr FeatureDependencyList
        parseFeatureDependencies(std::string_view spec) noexcept
    {
        FeatureDependencyList out{};
        std::size_t begin = 0;
        while (begin <= spec.size())
        {
            std::size_t end = spec.find(',', begin);
            if (end == std::string_view::npos) end = spec.size();
            std::string_view item = spec.substr(begin, end - begin);
            while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
            while (!item.empty() && item.back() == ' ')  item.remove_suffix(1);
            bool optional = false;
            if (!item.empty() && item.back() == '?')
            {
                optional = true;
                item.remove_suffix(1);
            }
            if (!item.empty())
            {
                out.items[out.count] = FeatureDependency{featureId(item), optional};
                ++out.count;
            }
            begin = end + 1;
        }
        return out;
    }

    /// Static, type-level descriptor. Spans point at storage with static lifetime
    /// (a feature's own `static constexpr` arrays), so the descriptor is trivially
    /// copyable and safe to embed in the (trivially-copyable) FeatureFactory.
    struct FeatureDescriptor
    {
        FeatureTypeId    type{kInvalidFeatureTypeId};
        std::string_view name{};
        std::uint32_t    abi_version{1};

        std::span<const FeatureDependency> dependencies{};
        std::span<const FeatureTypeId>     conflicts{};

        /// Needs per-view state (allocate/deallocateViewState) tracked by the manager.
        bool creates_view_state{false};
        /// May be toggled at runtime; when false the manager rejects setEnabled(false).
        bool supports_runtime_disable{true};

        /// At most one instance per scene? SinglePerScene → the install path rejects a
        /// second instance of this type (default MultiplePerScene = today's behaviour).
        FeatureMultiplicity multiplicity{FeatureMultiplicity::MultiplePerScene};

        /// Per-tier device requirements (static storage, like deps/conflicts).
        /// EMPTY = installable at any tier with no whitelisted-feature needs
        /// (today's behaviour). Non-empty: attach negotiation (①-4) looks up
        /// the row matching the resolved EFeatureLevel — no row for that tier
        /// or unmet required_features → the install is rejected.
        std::span<const FeatureLevelProfile> level_profiles{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return type != kInvalidFeatureTypeId;
        }
    };

} // namespace lux::render
