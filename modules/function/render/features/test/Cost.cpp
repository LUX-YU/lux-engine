#include "Scenarios.hpp"
#include <lux/engine/render/graph/Analysis.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace
{
    bool counting = false;
    std::size_t allocations = 0;
    std::size_t allocated_bytes = 0;

    void account(std::size_t size) noexcept
    {
        if (counting)
        {
            ++allocations;
            allocated_bytes += size;
        }
    }
}

void* operator new(std::size_t size)
{
    account(size);
    if (auto p = std::malloc(size ? size : 1)) return p;
    std::abort();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* p) noexcept { std::free(p); }

void operator delete[](void* p) noexcept { std::free(p); }

void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    account(size);
    const auto align = static_cast<std::size_t>(alignment);
#ifdef _MSC_VER
    auto p = _aligned_malloc(size ? size : 1, align);
#else
    auto p = std::aligned_alloc(align, ((size ? size : 1) + align - 1) / align * align);
#endif
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* p, std::align_val_t) noexcept
{
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }

void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

using namespace lux::render;
static constexpr FeatureName kType("test.cost");

static RenderResult<void>
describe(const void* wire, std::string_view, ResourceClaims& out) noexcept
{
    const auto* config = static_cast<const fixture::Config*>(wire);
    const auto& c = *config;
    if (c.first > c.total || c.count > c.total - c.first) return lux::cxx::unexpected(RenderError{kFeatureContract});
    for (std::uint32_t i = c.first; i < c.first + c.count; ++i)
        out.resources.push_back({std::to_string(i), EResourceClaim::PROVIDE, GraphBufferDesc{64}});
    if (c.chain && c.first && c.count)
        out.resources.push_back({std::to_string(c.first - 1), EResourceClaim::REQUIRE, GraphBufferDesc{64}});
    return {};
}

static RenderResult<void> build(
    const void* wire, std::string_view instance, std::span<const ResolvedResource> resolved, RenderGraphBuilder& b
) noexcept
{
    const auto* config = static_cast<const fixture::Config*>(wire);
    const auto& c = *config;
    for (std::uint32_t i = c.first; i < c.first + c.count; ++i)
    {
        const auto output = findResource(resolved, std::to_string(i))->version;
        GraphPass pass{featureKey(kType, instance, std::to_string(i))};
        if (c.chain && i)
            pass.uses.push_back({findResource(resolved, std::to_string(i - 1))->version, {}, EGraphAccess::READ});
        pass.uses.push_back({output, {}, EGraphAccess::WRITE});
        if (auto r = b.addPass(std::move(pass)); !r) return lux::cxx::unexpected(r.error());
        if (!c.chain || i + 1 == c.total)
            if (auto r = b.exportOutput({output}); !r) return r;
    }
    return {};
}

int main()
{
    const FeatureDescriptor type{kType, cppType<fixture::Config>()};
    const FeatureGraphOps type_graph{describe, build};
    std::puts("shape,features,passes,sample,assemble_ns,allocations,allocated_bytes,destroy_ns");
    for (bool chain : {false, true})
    {
        for (std::uint32_t count : {8u, 32u, 128u})
        {
            for (std::uint32_t passes : {32u, 128u, 512u})
            {
                std::vector<ConfigValue> configs;
                std::vector<std::string> names;
                std::vector<FeatureInstanceView> selected;
                configs.reserve(count);
                names.reserve(count);
                selected.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto first = passes * i / count;
                    fixture::Config c{first, passes * (i + 1) / count - first, passes, chain ? 1u : 0u};
                    auto config = ConfigValue::make(c);
                    if (!config) return 1;
                    configs.push_back(std::move(*config));
                    names.push_back(std::to_string(i));
                    selected.push_back({&type, &type_graph, &configs.back(), names.back()});
                }
                for (int sample = -5; sample < 100; ++sample)
                {
                    allocations = allocated_bytes = 0;
                    counting = true;
                    const auto start = std::chrono::steady_clock::now();
                    auto result = assembleFeatures(selected, fixture::limits.max_encoded_bytes);
                    const auto stop = std::chrono::steady_clock::now();
                    counting = false;
                    if (!result || result->analysis.execution_order.size() != passes) return 2;
                    const auto destroy_start = std::chrono::steady_clock::now();
                    result = lux::cxx::unexpected(RenderError{});
                    const auto destroy_stop = std::chrono::steady_clock::now();
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
                    const auto destroy = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        destroy_stop - destroy_start
                    ).count();
                    if (sample >= 0)
                        std::printf("%s,%u,%u,%d,%lld,%zu,%zu,%lld\n", chain ? "chain" : "independent",
                            count, passes, sample, ns, allocations, allocated_bytes, destroy);
                }
            }
        }
    }
}
