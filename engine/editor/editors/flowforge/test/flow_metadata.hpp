#pragma once

#include <array>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <memory>

struct NativeRecord final
{
    std::int32_t value{};
};
struct MetadataOwner final
{
    lux::meta::RefClass record;
    lux::meta::RefFunction function;
    std::array<const lux::meta::RefClass *, 1> classes;
    std::array<const lux::meta::RefFunction *, 1> functions;
    MetadataOwner()
    {
        record.name = "NativeRecord";
        record.full_name = "NativeRecord";
        record.type = lux::meta::ref_type_of_v<NativeRecord>;
        record.type.name = record.full_name;
        record.type.ptr = &record;
        record.fields.push_back({"value", lux::meta::ref_type_of_v<std::int32_t>});
        function.invokable.name = "nativeProbe";
        function.invokable.full_name = "nativeProbe";
        function.invokable.type_signature = "int32_t()";
        function.invokable.return_type = lux::meta::ref_type_of_v<std::int32_t>;
        classes = {&record};
        functions = {&function};
    }
};
lux::flowforge::FlowSourceEnvironment flowMetadata(const std::shared_ptr<MetadataOwner> &owner)
{
    lux::flowforge::FlowSourceEnvironment environment;
    environment.classes = owner->classes;
    environment.functions = owner->functions;
    environment.code_lifetime = owner;
    return environment;
}
