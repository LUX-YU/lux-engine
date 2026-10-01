from pathlib import Path

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
def edit(path,old,new):
 p=repo/path;s=p.read_text();assert old in s,path;p.write_text(s.replace(old,new),newline='\n')
edit('engine/domain/world/storage/include/lux/engine/world/WorldStorageCodec.hpp',
 '#include <lux/cxx/compile_time/expected.hpp>', '#include <lux/cxx/compile_time/expected.hpp>\n#include <lux/cxx/memory/SharedBytes.hpp>')
edit('engine/domain/world/storage/include/lux/engine/world/WorldStorageCodec.hpp',
 '        ALLOCATION_FAILURE,','        ALLOCATION_FAILURE,\n        INVALID_PARTITION,\n        INVALID_VOLUME,')
edit('engine/domain/world/storage/include/lux/engine/world/WorldStorageCodec.hpp',
 '} // namespace lux::world','''    // Borrow already-owned immutable volumes; this path never schedules work or performs IO.
    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC lux::cxx::expected<WorldPartitionData, WorldStorageCodecFailure>
    decodeWorldStoragePartition(
        const WorldDescription& world,
        std::span<const lux::cxx::SharedBytes<>> volumes,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop = {}
    ) noexcept;
} // namespace lux::world''')
edit('engine/domain/world/storage/CMakeLists.txt','COMPONENTS algorithm','COMPONENTS algorithm memory')
edit('engine/domain/world/storage/CMakeLists.txt','${CMAKE_CURRENT_SOURCE_DIR}/src/WorldStorageCodec.cpp','${CMAKE_CURRENT_SOURCE_DIR}/src/WorldStorageCodec.cpp\n        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartitionDecoder.cpp')
edit('engine/domain/world/storage/CMakeLists.txt','lux::cxx::algorithm','lux::cxx::algorithm lux::cxx::memory')
p=repo/'engine/process/world_loading/src/WorldPartitionLoad.cpp';s=p.read_text()
s=s.replace('world/storage/detail/WorldStorageCodec.hpp','world/storage/detail/WorldPartitionDecoder.hpp')
s=s.replace('#include <new>','#include <optional>')
a=s.index('        enum class EStage');b=s.index('        Impl(',a);s=s[:a]+s[b:]
s=s.replace('            case Input::BUNDLE_MISMATCH:', '''            case Input::INVALID_PARTITION:
                code = EWorldStorageRuntimeError::INVALID_PARTITION;
                break;
            case Input::INVALID_VOLUME:
                code = EWorldStorageRuntimeError::INVALID_VOLUME;
                break;
            case Input::BUNDLE_MISMATCH:''')
a=s.index('        void beginChunk(');b=s.index('        WorldStorageSource source;',a)
s=s[:a]+'''        void advance(lux::world::detail::WorldPartitionDecoder::Next next) noexcept
        {
            if (!next)
            {
                finishCodecFailure(next.error());
                return;
            }
            if (*next)
            {
                current_range = **next;
                submit(current_range.volume, current_range.offset, current_range.size);
                return;
            }
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_value(receiver, std::move(*decoder).takeResult());
        }

        void complete(Outcome&& outcome) noexcept
        {
            if (finished.load(std::memory_order_acquire))
                return;
            if (stop.stop_requested())
            {
                finishStopped();
                return;
            }
            if (!outcome)
            {
                if (outcome.error().isRuntime())
                    finishError({EWorldStorageRuntimeError::IO_FAILURE, current_range.volume});
                else
                    finishError(outcome.error().domainError());
                return;
            }
            advance(decoder->accept(outcome->view()));
        }

        void start() noexcept
        {
            if (!source)
            {
                finishError({max_bytes == 0U ? EWorldStorageRuntimeError::LIMIT_EXCEEDED
                                            : EWorldStorageRuntimeError::INVALID_PARTITION});
                return;
            }
            decoder.emplace(source.world(), partition, max_bytes, stop);
            advance(decoder->start());
        }

'''+s[b:]
a=s.index('        EStage stage');b=s.index('\n    };',a)
s=s[:a]+'''        std::optional<lux::world::detail::WorldPartitionDecoder> decoder;
        lux::world::detail::WorldStorageReadRange current_range;
'''+s[b:]
p.write_text(s,newline='\n')
p=repo/'engine/scene/asset/src/ScenePackage.cpp';s=p.read_text()
s=s.replace('#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>\n','').replace('#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>\n','')
s=s.replace('        namespace loading = lux::process::world_loading;\n\n','')
a=s.index('        struct Cancelled');b=s.index('        template <class T>',a);s=s[:a]+s[b:]
a=s.index('        auto source = loading::');b=s.index('        const auto count',a);s=s[:a]+s[b:]
a=s.index('            VPartitionResult loaded');b=s.index('\n        }\n        result.package',a)
s=s[:a]+'''            auto loaded = lux::world::decodeWorldStoragePartition(
                result.world->data(), result.volumes, lux::partition::PartitionOrdinal{index}, remaining, stop
            );
            if (!loaded)
            {
                if (loaded.error().code == lux::world::EWorldStorageCodecError::CANCELLED)
                    return failed(EScenePackageError::CANCELLED);
                return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::STORAGE, {}, index, loaded.error()});
            }
            if (loaded->retainedBytes() > remaining)
                return failed(EScenePackageError::LIMIT);
            remaining -= loaded->retainedBytes();
            result.partitions.push_back(std::make_shared<const lux::world::WorldPartitionData>(std::move(*loaded)));'''+s[b:]
p.write_text(s,newline='\n')
edit('engine/scene/asset/include/lux/engine/scene/ScenePackage.hpp', '#include <lux/engine/process/world_loading/WorldStorageSource.hpp>\n','')
edit('engine/scene/asset/include/lux/engine/scene/ScenePackage.hpp', '            lux::process::world_loading::WorldStorageRuntimeFailure,\n','')
edit('engine/scene/asset/CMakeLists.txt','lux::engine::process::world_loading','lux::engine::world::storage')
edit('engine/scene/asset/CMakeLists.txt','lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading','lux-engine-world-storage REQUIRED COMPONENTS world_storage')
