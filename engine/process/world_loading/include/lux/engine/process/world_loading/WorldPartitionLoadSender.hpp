#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/core/execution/Task.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <exec/task.hpp>
#include <exec/variant_sender.hpp>

namespace lux::process::world_loading
{
    namespace detail
    {
        using PartitionLoadResult = lux::cxx::expected<world::WorldPartitionData, WorldStorageRuntimeFailure>;
        using PartitionLoadTask = exec::task<PartitionLoadResult>;

        [[nodiscard]] LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC PartitionLoadTask decodeWorldPartition(
            WorldStorageSource source,
            partition::PartitionOrdinal partition,
            std::size_t max_bytes,
            std::stop_token stop,
            execution::TaskReporter reporter
        ) noexcept;
    } // namespace detail

    // Finite read/decode transaction. All decoder steps run on cpu; provider callbacks only deliver bytes.
    // Source and temporary bytes live in the task until the final value, error or stopped completion.
    [[nodiscard]] inline auto loadWorldPartition(
        WorldStorageSource source,
        execution::CpuScheduler cpu,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop = {}
    ) noexcept
    {
        auto operation = stdexec::let_value(
            stdexec::read_env(execution::detail::getTaskReporter),
            [source = std::move(source), cpu, partition, max_bytes, stop](execution::TaskReporter reporter
            ) mutable noexcept
            {
                auto decoded = stdexec::starts_on(
                    cpu,
                    detail::decodeWorldPartition(std::move(source), partition, max_bytes, stop, reporter)
                );
                return stdexec::let_value(
                    std::move(decoded),
                    [](detail::PartitionLoadResult& result) noexcept
                    {
                        using VResult = exec::variant_sender<
                            decltype(stdexec::just(world::WorldPartitionData{})),
                            decltype(stdexec::just_error(WorldStorageRuntimeFailure{}))>;
                        if (!result)
                        {
                            return VResult{stdexec::just_error(result.error())};
                        }
                        return VResult{stdexec::just(std::move(*result))};
                    }
                );
            }
        );
        return stdexec::let_error(
            std::move(operation),
            [](auto failure) noexcept -> decltype(stdexec::just_error(WorldStorageRuntimeFailure{}))
            {
                if constexpr (std::same_as<decltype(failure), std::exception_ptr>)
                {
                    std::terminate();
                }
                else
                {
                    return stdexec::just_error(std::move(failure));
                }
            }
        );
    }
} // namespace lux::process::world_loading
