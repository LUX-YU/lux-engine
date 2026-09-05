#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>
namespace lux::simulation::benchmark
{
struct alignas(64) LUX_TYPE_INFO(compile_time) CppSlabObject final
{
    std::uint64_t value{};
    LUX_METHOD(script_export = "slab.update")
    void update() noexcept
    {
        ++value;
    }
};
} // namespace lux::simulation::benchmark
