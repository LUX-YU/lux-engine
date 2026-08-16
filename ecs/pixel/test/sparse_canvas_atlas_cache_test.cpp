#include <lux/engine/ecs/render/subsystems/2d/SparseCanvasAtlasCache.hpp>

#include <cstdio>
#include <cstdlib>

namespace
{
    void require(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "[FAIL] %s\n", message);
        std::abort();
    }
}

int main()
{
    lux::ecs::detail::SparseCanvasAtlasCache cache{2u};
    require(cache.freeSlots() == 2u, "all slots begin free");

    const auto first = cache.acquire();
    const auto second = cache.acquire();
    require(first && *first == 0u, "allocation is deterministic");
    require(second && *second == 1u, "second slot is unique");
    require(!cache.acquire(), "capacity is a hard bound");

    require(cache.beginUpload(*first), "first upload is fenced");
    require(cache.beginUpload(*first), "multiple uploads share the fence");
    require(cache.retire(*first), "retirement waits for both uploads");
    require(cache.freeSlots() == 0u, "retired in-flight slot is unavailable");
    require(!cache.releaseUnpublished(*first),
            "retired slot cannot bypass its upload fence");
    require(cache.protocolErrors() == 1u,
            "invalid transitions remain observable");

    require(cache.finishUpload(*first), "first upload settles");
    require(cache.freeSlots() == 0u, "one upload still owns the slot");
    require(cache.finishUpload(*first), "last upload settles");
    require(cache.freeSlots() == 1u, "last completion releases retirement");

    const auto replacement = cache.acquire();
    require(replacement && *replacement == *first,
            "a slot is reused only after its generation fence settles");
    require(cache.releaseUnpublished(*replacement),
            "unpublished replacement returns immediately");
    require(!cache.releaseUnpublished(*replacement),
            "duplicate return is rejected");

    require(cache.retire(*second), "unused occupant retires immediately");
    require(cache.freeSlots() == 2u, "all slots are returned exactly once");
    require(cache.protocolErrors() == 2u,
            "protocol error counter is exact");
    return EXIT_SUCCESS;
}
