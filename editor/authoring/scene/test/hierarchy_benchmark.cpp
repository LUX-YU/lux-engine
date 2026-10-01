#include "../src/SceneSourceData.hpp"
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <string>
#include <numeric>
using namespace lux;
namespace a = lux::editor::scene;
namespace ecs = lux::simulation::ecs;
int main(int argc, char** argv)
{
    assert(argc == 4);
    const auto count = std::stoull(argv[1]);
    const std::string shape = argv[2];
    const auto repetitions = std::stoull(argv[3]);
    a::detail::SceneSourceAccess::Data data;
    data.objects.reserve(count);
    data.identities.reserve(count);
    std::vector<ecs::Entity> entities;
    entities.reserve(count);
    const auto name_space = *uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef");
    for (std::size_t i{}; i != count; ++i)
    {
        const world::WorldObjectId id{uuids::uuid_name_generator(name_space)(std::to_string(i))};
        const auto entity = data.registry.create();
        assert(data.identities.bind(id, entity));
        data.objects.push_back({id, {0}, {}});
        entities.push_back(entity);
    }
    for (std::size_t i = 1; i != count; ++i)
    {
        if (shape == "roots" && i % 100 == 0)
            continue;
        const auto parent = shape == "wide" ? 0 : i - 1;
        data.registry.emplace<ecs::Parent>(entities[i], entities[parent]);
    }
    if (shape == "cycle")
        data.registry.emplace<ecs::Parent>(entities[0], entities.back());
    std::vector<double> samples;
    samples.reserve(repetitions);
    for (std::size_t i{}; i != repetitions + 10; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto result = a::detail::SceneSourceAccess::validate(data);
        const auto end = std::chrono::steady_clock::now();
        assert(bool(result) == (shape != "cycle"));
        std::fprintf(
            stderr,
            "sample=%zu warmup=%d duration_us=%.3f\n",
            i,
            i < 10,
            std::chrono::duration<double, std::micro>(end - start).count()
        );
        std::fflush(stderr);
        if (i >= 10)
            samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::ranges::sort(samples);
    std::printf(
        "{\"n\":%zu,\"shape\":\"%s\",\"warmup\":10,\"samples\":%zu,\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,"
        "\"max_us\":%.3f}\n",
        std::size_t(count),
        shape.c_str(),
        samples.size(),
        samples[samples.size() / 2],
        samples[samples.size() * 95 / 100],
        samples[samples.size() * 99 / 100],
        samples.back()
    );
}
