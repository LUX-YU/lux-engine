#include <lux/engine/render/graph/Builder.hpp>
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

int main()
{
    using namespace lux::render;
    std::puts("shape,passes,resources,uses,edges,sample,analyze_ns,allocations,allocated_bytes");
    for (bool chain : {false, true})
    {
        for (std::uint32_t n : {32, 128, 512})
        {
            RenderGraphBuilder b;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                auto r = b.declareResource({std::to_string(i), GraphBufferDesc{4096}});
                if (!r) return 1;
                GraphPass pass{std::to_string(i)};
                if (chain && i > 0) pass.uses.push_back({{{i - 1}, 1}, {}, EGraphAccess::READ});
                pass.uses.push_back({{*r, 1}, {}, EGraphAccess::WRITE});
                if (!b.addPass(std::move(pass))) return 2;
                if ((!chain || i + 1 == n) && !b.exportOutput({{*r, 1}})) return 3;
            }
            auto graph = std::move(b).finish();
            if (!graph) return 4;
            for (int sample = -3; sample < 40; ++sample)
            {
                allocations = allocated_bytes = 0;
                counting = true;
                const auto start = std::chrono::steady_clock::now();
                auto result = analyze(*graph);
                const auto stop = std::chrono::steady_clock::now();
                counting = false;
                if (!result || result->execution_order.size() != n) return 5;
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
                if (sample >= 0) std::printf("%s,%u,%u,%u,%zu,%d,%lld,%zu,%zu\n", chain ? "chain" : "independent",
                    n, n, chain ? 2 * n - 1 : n, result->dependencies.size(), sample, ns, allocations, allocated_bytes);
            }
        }
    }
}
