#include "SharedFixture.hpp"
#include <array>
#include <memory>
#include <string_view>

namespace
{
    constexpr std::array names{
        "instance",
        "messenger",
        "device",
        "allocator",
        "buffer",
        "image",
        "image_view",
        "sampler",
        "descriptor_layout",
        "descriptor_pool",
        "shader",
        "pipeline_layout",
        "pipeline",
        "graphics_pipeline",
        "command_pool",
        "fence"
    };
    std::array<int, names.size()> live{};
    const char* fail_operation = "";
    int fail_countdown{};
    bool partial_graphics{};
    bool hold_evidence{};
    unsigned failures{};

    int index(std::string_view name)
    {
        for (int i = 0; i < int(names.size()); ++i)
        {
            if (names[i] == name)
            {
                return i;
            }
        }
        return -1;
    }

    template <class F> void fails(const char* operation, F&& create, int occurrence = 1)
    {
        const auto before = live;
        fail_operation = operation;
        fail_countdown = occurrence;
        auto candidate = create();
        CHECK(!candidate && fail_countdown == 0);
        CHECK(candidate.error().type == kNativeFailure);
        CHECK(candidate.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_HOST_MEMORY));
        fail_operation = "";
        CHECK(before == live);
        ++failures;
    }

    template <class F> void moves(F&& create)
    {
        const auto before = live;
        {
            auto first = take(create());
            auto second = std::move(first);
            CHECK(!first.native() && second.native());
            auto third = take(create());
            third = std::move(second);
            CHECK(!second.native() && third.native());
        }
        CHECK(before == live);
    }
} // namespace

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (hold_evidence && std::string_view(operation) == "fence_status")
        {
            return VK_NOT_READY;
        }
        if (std::string_view(operation) == fail_operation && --fail_countdown == 0)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        const auto i = index(operation);
        if (result == VK_SUCCESS && i >= 0)
        {
            ++live[i];
        }
        if (partial_graphics && std::string_view(operation) == "graphics_pipeline" && result == VK_SUCCESS)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        const auto i = index(operation);
        CHECK(i >= 0 && live[i] > 0);
        --live[i];
        if (std::string_view(operation) == "device")
        {
            for (std::size_t j = 3; j < live.size(); ++j)
            {
                CHECK(live[j] == 0);
            }
        }
    }
} // namespace lux::render::vulkan::test

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
        auto queue = take(SubmissionQueue::create(device, 3));
        const std::array schemas{PassSchema<F3SharedA>::contract(), PassSchema<F3SharedB>::contract()};
        const std::array owners{
            take(declareOwnerShape("a.scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas)),
            take(declareOwnerShape("b.feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas))
        };
        auto active =
            std::make_unique<NativeShaderProgram>(take(program<F3SharedA>(device, binaries, sources, owners)));
        const auto original_handle = active->native();
        const auto create = [&] { return program<F3SharedB>(device, binaries, sources, owners); };
        for (const char* operation : {"descriptor_layout", "pipeline_layout", "shader", "graphics_pipeline"})
        {
            fails(operation, create);
            CHECK(active->native() == original_handle);
        }
        fails("shader", create, 2);
        {
            const auto before = live;
            partial_graphics = true;
            CHECK(!create());
            partial_graphics = false;
            CHECK(live == before);
            ++failures;
        }
        auto image = take(Image::create(allocator, {2, 2}, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT));
        fails("image_view", [&] { return ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT); });
        fails("sampler", [&] { return Sampler::create(device); });
        moves([&] { return ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT); });
        moves([&] { return Sampler::create(device); });
        {
            const auto vertex = take(ShaderModule::create(device, words(binaries / "F3Raster.vert.spv")));
            const auto fragment = take(ShaderModule::create(device, words(binaries / "F3Raster.frag.spv")));
            const std::array ranges{
                VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 16, 4},
                VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16}
            };
            auto layout = take(PipelineLayout::create(device, {}, ranges));
            GraphicsDescription description;
            description.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
            description.blends = {ColorBlend{}};
            moves([&] { return GraphicsPipeline::create(device, vertex, fragment, layout, description); });
            description.stencil_format = VK_FORMAT_D32_SFLOAT;
            CHECK(!GraphicsPipeline::create(device, vertex, fragment, layout, description));
        }
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
        fails("descriptor_pool", [&] { return BoundDescriptorSets::create(device, *active, values); });
        fails("descriptor_set", [&] { return BoundDescriptorSets::create(device, *active, values); });
        auto active_sets =
            std::make_unique<BoundDescriptorSets>(take(BoundDescriptorSets::create(device, *active, values)));
        auto after_failure = submitSharedDraw(allocator, *queue, *active, *active_sets);
        checkSharedDraw(*queue, after_failure, {11, 22, 33, 44}, evidence / "last-good.bin");
        auto candidate = std::make_unique<NativeShaderProgram>(take(create()));
        auto candidate_sets =
            std::make_unique<BoundDescriptorSets>(take(BoundDescriptorSets::create(device, *candidate, values)));
        auto old_draw = submitSharedDraw(allocator, *queue, *active, *active_sets);
        auto old = std::move(active);
        auto old_sets = std::move(active_sets);
        active = std::move(candidate);
        active_sets = std::move(candidate_sets);
        // The private seam delays only CPU fence observation; actual native work and vkWaitForFences stay real.
        hold_evidence = true;
        auto new_draw = submitSharedDraw(allocator, *queue, *active, *active_sets);
        CHECK(take(queue->poll()) < old_draw.ticket.serial());
        CHECK(live[index("graphics_pipeline")] == 2);
        CHECK(old->native() == original_handle && active->native() != original_handle);
        hold_evidence = false;
        checkSharedDraw(*queue, old_draw, {11, 22, 33, 44}, evidence / "old-in-flight.bin");
        CHECK(queue->completed() >= old_draw.ticket.serial());
        old_sets.reset();
        old.reset();
        CHECK(live[index("graphics_pipeline")] == 1);
        checkSharedDraw(*queue, new_draw, {110, 220, 330, 440}, evidence / "new-shader.bin");
        std::printf(
            "G07 rollback=%u actual old/new submissions=%llu/%llu completed=%llu; old destroyed after fence evidence\n",
            failures,
            old_draw.ticket.serial(),
            new_draw.ticket.serial(),
            queue->completed()
        );
    }
    for (int value : live)
    {
        CHECK(value == 0);
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
