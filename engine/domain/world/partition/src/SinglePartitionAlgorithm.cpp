#include <lux/engine/world/SinglePartitionAlgorithm.hpp>

namespace lux::world
{
    PartitionBuildResult SinglePartitionAlgorithm::build(const Input& input) noexcept
    {
        WorldPartitionLayoutBuilder builder(input.objects);
        if (auto added = builder.addPartition(input.partition, input.objects); !added)
        {
            return cxx::unexpected(added.error());
        }
        auto layout = std::move(builder).build();
        if (!layout)
        {
            return cxx::unexpected(layout.error());
        }
        return WorldPartitionBuildProduct::build(identity(), std::move(*layout), {});
    }
} // namespace lux::world
