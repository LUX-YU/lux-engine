#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/context/render/RenderAssetPipeline.hpp>
#include <lux/engine/context/render/RenderExecution.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>

#include <map>

namespace lux::engine::render
{
    // One Main-thread directory of actual asset representations. A table is identified by its
    // pipeline and destination; neither Scene nor individual entities own duplicate caches.
    class RenderAssets final
    {
    public:
        class AvailabilityConnection final
        {
        public:
            AvailabilityConnection(
                RenderAssets&,
                void*,
                void (*)(void*, RenderAssetVersion) noexcept,
                void (*capacity_available)(void*) noexcept
            ) noexcept;
            ~AvailabilityConnection() noexcept;
            AvailabilityConnection(const AvailabilityConnection&) = delete;
            AvailabilityConnection& operator=(const AvailabilityConnection&) = delete;

            // Observe only an acquired representation. Completed values notify synchronously on Main.
            template <RenderAssetPipeline P>
            void observe(RenderAssetVersion key, lux::render::CapabilityInput destination = {}) noexcept;
            void whenCapacityAvailable() noexcept;

        private:
            friend class RenderAssets;
            RenderAssets& assets_;
            AvailabilityConnection* next_{};
            AvailabilityConnection** previous_{};
            void* receiver_;
            void (*completed_)(void*, RenderAssetVersion) noexcept;
            void (*capacity_available_)(void*) noexcept;
            std::vector<std::vector<AvailabilityConnection*>*> versions_;
            void disconnectCapacity() noexcept;
            void subscribe(std::vector<AvailabilityConnection*>&, RenderAssetVersion, bool loading) noexcept;
        };

        using CreateResult = lux::render::RenderResult<std::unique_ptr<RenderAssets>>;

        // Creates an empty directory. The application installs only pipelines required by its bindings.
        [[nodiscard]] static CreateResult create(
            RenderExecution&,
            process::asset_loading::AssetReadPort,
            lux::render::CapabilityQuery = {},
            RenderAssetLimits = {}
        ) noexcept;
        ~RenderAssets() noexcept;
        RenderAssets(const RenderAssets&) = delete;
        RenderAssets& operator=(const RenderAssets&) = delete;

        // Cold registration. Equal pipeline/destination reuses the existing typed table.
        template <RenderAssetPipeline P>
        void install(P pipeline, lux::render::CapabilityInput destination = {}) noexcept;

        template <RenderAssetPipeline P>
        [[nodiscard]] lux::render::RenderResult<void> acquire(
            RenderAssetVersion,
            process::asset_loading::AssetReadPort = {},
            asset::AssetVfsView = {},
            lux::render::CapabilityInput destination = {}
        ) noexcept;
        template <RenderAssetPipeline P>
        void release(RenderAssetVersion, lux::render::CapabilityInput destination = {}) noexcept;
        template <RenderAssetPipeline P>
        [[nodiscard]] const typename P::Value* ready(RenderAssetVersion, lux::render::CapabilityInput destination = {})
            const noexcept;
        template <RenderAssetPipeline P>
        [[nodiscard]] lux::render::RenderError failure(
            RenderAssetVersion,
            lux::render::CapabilityInput destination = {}
        ) const noexcept;

        // Public resource inputs used by dependent capability installation, never Storage identities.
        void addInputs(std::span<const lux::render::CapabilitiesInstalled::Input>) noexcept;
        [[nodiscard]] std::span<const lux::render::CapabilitiesInstalled::Input> inputs() const noexcept;
        [[nodiscard]] RenderExecution& executionContext() const noexcept;
        [[nodiscard]] const lux::render::CapabilityQuery& resourceCapability() const noexcept;

        // Scene references end before close. Keep driving Main until settled, then destroy this
        // directory before RenderExecution and the file provider. Failed releases remain diagnosable.
        void close() noexcept;
        void stopLoads() noexcept;
        [[nodiscard]] bool loadsSettled() const noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] lux::render::RenderError releaseFailure() const noexcept;

    private:
        struct Table
        {
            cxx::TypeToken type;
            lux::render::CapabilityInput destination;
            virtual ~Table() = default;
            virtual void close() noexcept = 0;
            [[nodiscard]] virtual bool settled() const noexcept = 0;
        };

        template <RenderAssetPipeline P> struct TypedTable final : Table
        {
            struct Record
            {
                std::optional<typename P::Value> value;
                lux::render::RenderError error;
                std::size_t references = 1;
                bool loading = true;
                bool releasing = false;
                std::vector<AvailabilityConnection*> observers;
            };

            RenderAssets& assets;
            P pipeline;
            std::map<RenderAssetVersion, Record> records;

            TypedTable(RenderAssets& assets, P pipeline, lux::render::CapabilityInput destination) noexcept
                : assets(assets), pipeline(std::move(pipeline))
            {
                this->type = cxx::typeToken<P>();
                this->destination = destination;
            }

            void releaseRecord(RenderAssetVersion key) noexcept
            {
                auto found = records.find(key);
                auto& record = found->second;
                if (!record.value || assets.rendering_.mainClient().serverStopped())
                {
                    records.erase(found);
                    return;
                }
                record.releasing = true;
                auto work = execution::taskResult(
                    stdexec::starts_on(assets.rendering_.execution().mainScheduler(), pipeline.release(*record.value))
                );
                const auto accepted = assets.rendering_.execution().spawn(stdexec::then(
                    std::move(work),
                    [this, key](auto result) noexcept
                    {
                        const auto error = resultError(result);
                        if (error.type != 0)
                        {
                            records.find(key)->second.error = error;
                            assets.recordReleaseFailure(error, P::name);
                        }
                        else
                        {
                            records.erase(key);
                        }
                        assets.notifyCapacity();
                    }
                ));
                if (!accepted)
                {
                    std::terminate();
                }
            }

            void close() noexcept override
            {
                for (auto entry = records.begin(); entry != records.end();)
                {
                    auto& [key, record] = *entry++;
                    if (std::exchange(record.references, 0) != 0 && !record.loading)
                    {
                        releaseRecord(key);
                    }
                }
            }

            bool settled() const noexcept override
            {
                return std::ranges::all_of(
                    records,
                    [](const auto& entry) noexcept
                    {
                        const auto& record = entry.second;
                        return !record.loading && record.references == 0 && record.releasing && record.error.type != 0;
                    }
                );
            }
        };

        template <class T>
        static lux::render::RenderError resultError(const execution::TTaskResult<T, lux::render::RenderError>& result
        ) noexcept
        {
            if (result)
            {
                return {};
            }
            if (const auto* domain = result.error().domainFailure())
            {
                return *domain;
            }
            return {result.error().isCancelled() ? lux::render::kCommandCancelled : lux::render::kRuntimeState};
        }

        template <RenderAssetPipeline P> TypedTable<P>* find(lux::render::CapabilityInput destination) const noexcept
        {
            TypedTable<P>* selected = nullptr;
            for (const auto& table : tables_)
            {
                if (table->type != cxx::typeToken<P>())
                {
                    continue;
                }
                if (table->destination == destination)
                {
                    return static_cast<TypedTable<P>*>(table.get());
                }
                if (destination.implementation.isNull())
                {
                    if (selected)
                    {
                        return nullptr; // Multiple representations require an explicit destination.
                    }
                    selected = static_cast<TypedTable<P>*>(table.get());
                }
            }
            return selected;
        }

        RenderAssets(
            RenderExecution&,
            process::asset_loading::AssetReadPort,
            lux::render::CapabilityQuery,
            RenderAssetLimits
        ) noexcept;
        static void notifyVersion(std::vector<AvailabilityConnection*>&, RenderAssetVersion) noexcept;
        void notifyCapacity() noexcept;
        void awaitTaskCapacity() noexcept;
        void recordReleaseFailure(lux::render::RenderError, std::string_view) noexcept;

        RenderExecution& rendering_;
        process::asset_loading::AssetReadPort source_;
        lux::render::CapabilityQuery resource_capability_;
        RenderAssetLimits limits_;
        execution::TaskScope tasks_;
        std::stop_source stop_;
        std::vector<std::unique_ptr<Table>> tables_;
        std::vector<lux::render::CapabilitiesInstalled::Input> inputs_;
        std::size_t in_flight_ = 0;
        lux::render::RenderError release_failure_;
        AvailabilityConnection* capacity_waiters_{};
        std::size_t connection_count_ = 0;
        bool capacity_notification_pending_ = false;
        bool closing_ = false;
    };

    template <RenderAssetPipeline P>
    void RenderAssets::install(P pipeline, lux::render::CapabilityInput destination) noexcept
    {
        if (!find<P>(destination))
        {
            tables_.push_back(std::make_unique<TypedTable<P>>(*this, std::move(pipeline), destination));
        }
    }

    template <RenderAssetPipeline P>
    lux::render::RenderResult<void> RenderAssets::acquire(
        RenderAssetVersion key,
        process::asset_loading::AssetReadPort source,
        asset::AssetVfsView snapshot,
        lux::render::CapabilityInput destination
    ) noexcept
    {
        auto* table = find<P>(destination);
        if (closing_ || key.asset.isNull() || !table)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeState});
        }
        auto found = table->records.find(key);
        if (found != table->records.end())
        {
            if (found->second.releasing)
            {
                const auto error = found->second.error.type != 0
                                       ? found->second.error
                                       : lux::render::RenderError{lux::render::kRuntimeCapacity};
                return cxx::unexpected(error);
            }
            ++found->second.references;
            return {};
        }
        if (in_flight_ == limits_.in_flight)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeCapacity});
        }
        auto [entry, inserted] = table->records.try_emplace(key);
        ++in_flight_;
        RenderAssetLoad
            load{key, source ? std::move(source) : source_, std::move(snapshot), limits_.decode, stop_.get_token()};
        auto task = tasks_.submit(
            {.name = std::string(P::name), .category = "render.asset"},
            [this, table, key, load = std::move(load)](execution::TaskReporter) mutable noexcept
            {
                auto result = execution::taskResult(
                    stdexec::starts_on(rendering_.execution().mainScheduler(), table->pipeline.load(std::move(load)))
                );
                return stdexec::then(
                    stdexec::continues_on(std::move(result), rendering_.execution().mainScheduler()),
                    [this, table, key](auto result) noexcept
                    {
                        --in_flight_;
                        auto& record = table->records.find(key)->second;
                        record.loading = false;
                        record.error = resultError(result);
                        if (result)
                        {
                            record.value.emplace(std::move(*result));
                        }
                        notifyVersion(record.observers, key);
                        // Connections only record intent; Registry edits and acquire/release run at a later safe point.
                        if (record.references == 0)
                        {
                            table->releaseRecord(key);
                        }
                        notifyCapacity();
                        return execution::taskState(result);
                    }
                );
            }
        );
        if (!task)
        {
            --in_flight_;
            table->records.erase(entry);
            awaitTaskCapacity();
            return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeCapacity});
        }
        return {};
    }

    template <RenderAssetPipeline P>
    void RenderAssets::release(RenderAssetVersion key, lux::render::CapabilityInput destination) noexcept
    {
        auto& table = *find<P>(destination);
        auto& record = table.records.find(key)->second;
        if (--record.references == 0 && !record.loading)
        {
            table.releaseRecord(key);
        }
    }

    template <RenderAssetPipeline P>
    const typename P::Value* RenderAssets::ready(RenderAssetVersion key, lux::render::CapabilityInput destination)
        const noexcept
    {
        const auto* table = find<P>(destination);
        if (!table)
        {
            return nullptr;
        }
        const auto found = table->records.find(key);
        return found != table->records.end() && found->second.value ? &*found->second.value : nullptr;
    }

    template <RenderAssetPipeline P>
    lux::render::RenderError RenderAssets::failure(RenderAssetVersion key, lux::render::CapabilityInput destination)
        const noexcept
    {
        const auto* table = find<P>(destination);
        if (!table)
        {
            return {lux::render::kRuntimeUnsupported};
        }
        const auto found = table->records.find(key);
        return found != table->records.end() ? found->second.error : lux::render::RenderError{};
    }

    template <RenderAssetPipeline P>
    void RenderAssets::AvailabilityConnection::observe(
        RenderAssetVersion key,
        lux::render::CapabilityInput destination
    ) noexcept
    {
        auto& record = assets_.find<P>(destination)->records.find(key)->second;
        subscribe(record.observers, key, record.loading);
    }
} // namespace lux::engine::render
