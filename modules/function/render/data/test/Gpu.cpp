#include "Golden.data.hpp"
#include "Support.hpp"
#include <filesystem>
#include <fstream>
#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/memory/Memory.hpp>
#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>
#include <lux/engine/toolchain/shader/DataValidation.hpp>
#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>
#include <vector>

using namespace foundation_test;
namespace rd = lux::rdesc;

std::vector<std::uint32_t> words(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    CHECK(stream);
    const auto size = static_cast<std::size_t>(stream.tellg());
    CHECK(size % 4 == 0);
    std::vector<std::uint32_t> result(size / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()), size);
    CHECK(stream);
    return result;
}

template <GpuData T> auto emit()
{
    const auto gpu = DataSchema<T>::gpuLayout();
    std::array<rd::PassResourceField, 2> fields{};
    fields[0].path = "input";
    fields[0].shader_name = "p_input";
    fields[0].role = gpu.storage == rd::EDataLayout::STD140 ? rd::EPassFieldRole::UNIFORM_READ
                                                            : rd::EPassFieldRole::READ_ONLY_STORAGE;
    fields[0].owner = rd::EFieldOwner::SCENE;
    fields[0].required = true;
    fields[0].array_count = 1;
    fields[0].stages = 4;
    fields[1] = fields[0];
    fields[1].path = "output";
    fields[1].shader_name = "p_output";
    fields[1].role = rd::EPassFieldRole::READ_WRITE_STORAGE;
    std::string declarations(gpu.declarations);
    declarations += gpu.storage == rd::EDataLayout::STD140 ? "\nlayout(std140) uniform Input { "
                                                           : "\nlayout(std430) readonly buffer Input { ";
    declarations += std::string(DataSchema<T>::shader_type) + " record; } p_input;\n";
    declarations += "layout(std430) buffer Output { float value; } p_output;\n";
    rd::PassShaderContract contract{"DataAbiFixture", fields, {}, declarations, 0, 1, {"", "", declarations}};
    std::string source = "#version 450\n//! lux-shader stage=compute entry=main\nlayout(local_size_x=1) in;\nvoid "
                         "main() { p_output.value = 0.0;\n";
    for (const auto& field : gpu.fields)
    {
        for (unsigned column = 0; column < field.columns; ++column)
        {
            for (unsigned row = 0; row < field.rows; ++row)
            {
                std::string access = "p_input.record." + std::string(field.path);
                if (field.columns > 1)
                {
                    access += "[" + std::to_string(column) + "]";
                }
                if (field.rows > 1)
                {
                    access += "[" + std::to_string(row) + "]";
                }
                source += "p_output.value += float(" + access + ");\n";
            }
        }
    }
    source += "}\n";
    auto result = lux::shadergen::lglsl::emitPassGlsl(contract, source);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", result.error().c_str());
    }
    CHECK(result);
    return *result;
}

template <CpuAuthoredGpuData T> void verify(const std::filesystem::path& path)
{
    auto spirv = words(path);
    const auto gpu = DataSchema<T>::gpuLayout();
    auto valid = lux::toolchain::validateDataSpirv(spirv, gpu, "p_input");
    if (!valid)
    {
        std::fprintf(stderr, "%s\n", valid.error().c_str());
    }
    CHECK(valid);
    auto fields = std::vector<rd::DataGpuField>(gpu.fields.begin(), gpu.fields.end());
    auto invalid = gpu;
    invalid.fields = fields;
    fields[0].offset += 4;
    CHECK(!lux::toolchain::validateDataSpirv(spirv, invalid, "p_input"));
    fields[0] = gpu.fields[0];
    fields[0].rows = fields[0].rows == 1 ? 2 : 1;
    CHECK(!lux::toolchain::validateDataSpirv(spirv, invalid, "p_input"));
    Validation validation;
    {
        auto host = instance(validation);
        auto device = take(VulkanDevice::create(host));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 2));
        auto emitted = emit<T>();
        CHECK(emitted.injected.size() == 2);
        CHECK(emitted.injected[0].set == emitted.injected[1].set);
        const auto input_type = gpu.storage == rd::EDataLayout::STD140 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        auto input = take(
            Buffer::create(
                allocator,
                gpu.size,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                EMemoryAccess::UPLOAD
            )
        );
        auto output = take(Buffer::create(allocator, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::READBACK));
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<float, sizeof(T) / 4> values{};
        for (unsigned i = 0; i < values.size(); ++i)
        {
            values[i] = float(i + 1);
        }
        T value{};
        float expected = float(values.size() * (values.size() + 1) / 2);
        if constexpr (std::same_as<T, IntegerGpu>)
        {
            value = {-7, 19, {-3, 5}};
            expected = 14;
        }
        else
        {
            std::memcpy(&value, values.data(), sizeof(T));
        }
        std::vector<std::byte> packed(gpu.size);
        CHECK(DataSchema<T>::pack(value, packed));
        CHECK(input.write(0, packed));
        const std::array bindings{
            VkDescriptorSetLayoutBinding{
                emitted.injected[0].binding,
                input_type,
                1,
                VK_SHADER_STAGE_COMPUTE_BIT,
                nullptr
            },
            VkDescriptorSetLayoutBinding{
                emitted.injected[1].binding,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_COMPUTE_BIT,
                nullptr
            }
        };
        auto descriptor_layout = take(DescriptorSetLayout::create(device, bindings));
        std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        if (input_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        {
            sizes.push_back({input_type, 1});
        }
        auto pool = take(DescriptorPool::create(device, 1, sizes));
        auto set = take(pool.allocate(descriptor_layout));
        CHECK(pool.writeBuffer(set, bindings[0].binding, 0, input_type, input, 0, gpu.size));
        CHECK(pool.writeStorageBuffer(set, bindings[1].binding, output, 0, 4));
        auto empty = take(DescriptorSetLayout::create(device, {}));
        std::vector<VkDescriptorSetLayout> layouts(emitted.injected[0].set + 1, empty.native());
        layouts.back() = descriptor_layout.native();
        auto layout = take(PipelineLayout::create(device, layouts));
        auto shader = take(ShaderModule::create(device, spirv));
        auto pipeline = take(ComputePipeline::create(device, shader, layout));
        auto batch = take(queue->begin());
        vkCmdBindPipeline(batch.native(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.native());
        vkCmdBindDescriptorSets(
            batch.native(),
            VK_PIPELINE_BIND_POINT_COMPUTE,
            layout.native(),
            emitted.injected[0].set,
            1,
            &set,
            0,
            nullptr
        );
        vkCmdDispatch(batch.native(), 1, 1, 1);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch.native(), &dependency);
        auto ticket = take(std::move(batch).submit());
        CHECK(take(queue->wait(ticket, 5'000'000'000)));
        float actual{};
        CHECK(output.read(0, std::as_writable_bytes(std::span{&actual, 1})));
        CHECK(actual == expected);
        std::printf("GPU ABI sum=%g expected=%g bytes=%u\n", actual, expected, gpu.size);
    }
    CHECK(validation.errors == 0);
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    const std::filesystem::path directory(argv[2]);
    if (std::string_view(argv[1]) == "emit")
    {
        std::ofstream(directory / "Light.comp") << emit<LightGpu>().glsl;
        std::ofstream(directory / "Sprite.comp") << emit<SpriteGpu>().glsl;
        std::ofstream(directory / "Complex.comp") << emit<ComplexGpu>().glsl;
        std::ofstream(directory / "Integer.comp") << emit<IntegerGpu>().glsl;
    }
    else
    {
        verify<LightGpu>(directory / "Light.spv");
        verify<SpriteGpu>(directory / "Sprite.spv");
        verify<ComplexGpu>(directory / "Complex.spv");
        verify<IntegerGpu>(directory / "Integer.spv");
    }
}
