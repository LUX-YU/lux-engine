#include <lux/engine/flowforge/detail/FlowSourceMetadata.hpp>
#include <lux/engine/meta/Meta.hpp>

namespace lux::flowforge::detail
{
    std::vector<FlowSourceArgument> captureSourceArguments(const std::vector<FuncArgInfo>& values)
    {
        std::vector<FlowSourceArgument> result;
        for (const auto& value : values)
        {
            result.push_back({value.name, value.type ? std::string(value.type->name) : std::string{}});
        }
        return result;
    }

    const lux::meta::RefType* findSourceType(std::string_view name, const FlowSourceEnvironment& environment) noexcept
    {
        const lux::meta::RefType* result{};
        const auto builtin = [&]<class... T>()
        {
            ((name == lux::meta::builtin_ref_type_ptr<T>()->name ? result = lux::meta::builtin_ref_type_ptr<T>()
                                                                 : result),
             ...);
        };
        // clang-format off
        builtin.template operator()<
            void, bool, char, signed char, unsigned char, short, unsigned short, int, unsigned int,
            long, unsigned long, long long, unsigned long long, float, double, void*
        >();
        // clang-format on
        if (result)
        {
            return result;
        }
        for (const auto* type : environment.types)
        {
            if (type && type->name == name)
            {
                return type;
            }
        }
        for (const auto* type : environment.classes)
        {
            if (type && type->type.name == name)
            {
                return &type->type;
            }
        }
        return nullptr;
    }

    const lux::meta::RefClass* findSourceClass(std::string_view name, const FlowSourceEnvironment& environment) noexcept
    {
        for (const auto* type : environment.classes)
        {
            if (type && type->full_name == name)
            {
                return type;
            }
        }
        return nullptr;
    }

    FlowSourceResult<std::vector<FuncArgInfo>> restoreSourceArguments(
        const std::vector<FlowSourceArgument>& values,
        const FlowSourceEnvironment& environment
    ) noexcept
    {
        std::vector<FuncArgInfo> result;
        for (const auto& value : values)
        {
            const auto* type = findSourceType(value.type, environment);
            if (!type)
            {
                return sourceFailure(EFlowSourceError::UNKNOWN_TYPE, value.type);
            }
            result.push_back({type, value.name});
        }
        return result;
    }

} // namespace lux::flowforge::detail
