#include "SharedFixture.hpp"

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    const std::filesystem::path binaries(argv[1]), sources(argv[2]), evidence(argv[3]);
    std::filesystem::create_directories(evidence);
    Validation validation;
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = take(VulkanDevice::create(host, options));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 2));
        const std::array schemas{PassSchema<F3SharedA>::contract(), PassSchema<F3SharedB>::contract()};
        const std::array owners{
            take(declareOwnerShape("a.scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas)),
            take(declareOwnerShape("b.feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas))
        };
        auto first = take(program<F3SharedA>(device, binaries, sources, owners));
        auto second = take(program<F3SharedB>(device, binaries, sources, owners));
        CHECK(first.identity().layout.owners[0].fields.size() == 2);
        CHECK(first.identity().layout.owners[1].fields.size() == 2);
        CHECK(first.identity().layout.sets.size() == 1 && first.identity().layout.sets[0].bindings.size() == 4);
        CHECK(first.identity().layout.fields.size() == 2);
        CHECK(first.identity().layout.owners == second.identity().layout.owners);
        CHECK(first.identity().layout.sets == second.identity().layout.sets);
        CHECK(first.pipelineLayout() != second.pipelineLayout());
        saveProgram(first, evidence, "first");
        saveProgram(second, evidence, "second");
        const std::array data{
            F3SharedValue{1, 2, 3, 4},
            F3SharedValue{10, 20, 30, 40},
            F3SharedValue{100, 200, 300, 400},
            F3SharedValue{10, 20, 30, 40}
        };
        std::vector<Buffer> buffers;
        for (const auto& value : data)
        {
            buffers.push_back(
                take(Buffer::create(allocator, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD))
            );
            CHECK(buffers.back().write(0, std::as_bytes(std::span(&value, 1))));
        }
        const std::array values{
            OwnerDescriptorValue{owners[0].identity, "scene_a", 0, BufferDescriptorValue{std::cref(buffers[0]), 0, 16}},
            OwnerDescriptorValue{
                owners[1].identity,
                "feature_a",
                0,
                BufferDescriptorValue{std::cref(buffers[1]), 0, 16}
            },
            OwnerDescriptorValue{owners[0].identity, "scene_b", 0, BufferDescriptorValue{std::cref(buffers[2]), 0, 16}},
            OwnerDescriptorValue{
                owners[1].identity,
                "feature_b",
                0,
                BufferDescriptorValue{std::cref(buffers[3]), 0, 16}
            }
        };
        auto first_sets = take(BoundDescriptorSets::create(device, first, values));
        auto second_sets = take(BoundDescriptorSets::create(device, second, values));
        CHECK(!BoundDescriptorSets::create(device, first, std::span(values).first(2)));
        auto first_draw = submitSharedDraw(allocator, *queue, first, first_sets);
        checkSharedDraw(*queue, first_draw, {11, 22, 33, 44}, evidence / "first.bin");
        auto second_draw = submitSharedDraw(allocator, *queue, second, second_sets);
        checkSharedDraw(*queue, second_draw, {110, 220, 330, 440}, evidence / "second.bin");
        std::puts("G03/W15 complete shared owner shape: 4 bindings, 2 complementary graphics programs, exact GPU pixels"
        );
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
