#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class RenderGraphInstance;
    struct FeatureBinding;

    class LUX_RENDER_PUBLIC FrameExecution final
    {
        struct WriteWaiter final
        {
            WriteWaiter* next = nullptr;
            void* operation = nullptr;
            bool (*stopping)(void*) noexcept = nullptr;
            void (*complete)(void*) noexcept = nullptr;
        };

    public:
        class WriteAdmissionSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;

            WriteAdmissionSender(FrameExecution& frames, std::size_t bytes) noexcept : frames_(frames), bytes_(bytes) {}

            template <class Receiver> struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;
                FrameExecution& frames;
                Receiver receiver;
                WriteWaiter node;
                std::size_t bytes;
                std::size_t reserved_bytes = 0;

                ~Operation() noexcept
                {
                    frames.uploads_->releaseSourceBytes(reserved_bytes);
                }

                void start() & noexcept
                {
                    if (!frames.uploads_->reserveSourceBytes(bytes))
                    {
                        stdexec::set_value(
                            std::move(receiver),
                            RenderResult<void>{cxx::unexpected(RenderError{kDeviceCapacity})}
                        );
                        return;
                    }
                    reserved_bytes = bytes;
                    node.operation = this;
                    node.stopping = [](void* value) noexcept
                    {
                        auto& self = *static_cast<Operation*>(value);
                        return stdexec::get_stop_token(stdexec::get_env(self.receiver)).stop_requested();
                    };
                    node.complete = [](void* value) noexcept
                    {
                        auto& self = *static_cast<Operation*>(value);
                        const bool stopped = self.node.stopping(value);
                        auto result = stopped ? RenderResult<void>{cxx::unexpected(RenderError{kUploadStopped})}
                                              : RenderResult<void>{};
                        stdexec::set_value(std::move(self.receiver), std::move(result));
                    };
                    frames.enqueueWrite(node);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{frames_, std::move(receiver), {}, bytes_};
            }

        private:
            FrameExecution& frames_;
            std::size_t bytes_;
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<FrameExecution>> create(
            DeviceSession&,
            ResourceUploader*,
            std::uint32_t frame_count
        ) noexcept;
        ~FrameExecution() noexcept;
        FrameExecution(const FrameExecution&) = delete;
        FrameExecution& operator=(const FrameExecution&) = delete;

        [[nodiscard]] std::uint32_t frameCount() const noexcept
        {
            return frame_count_;
        }

        [[nodiscard]] bool tryBeginWrite(std::uint32_t& slot) noexcept;
        void finishWrite(std::uint32_t slot, const SubmissionFrontiers&) noexcept;

        // Reserve a reusable physical slice before applying a sealed frame's visible mutations.
        // No waiting or GPU submission. Standalone writers can still acquire a slice themselves.
        [[nodiscard]] bool beginFrame(std::uint32_t& slot) noexcept;
        // Query the slot acquired for this input, before it is reused. True ends the CPU input
        // borrow. An empty frame need not submit GPU work; accepted GPU work may still be in flight.
        [[nodiscard]] bool inputConsumed(std::uint32_t slot) const noexcept;
        [[nodiscard]] std::uint32_t submittedBatches(std::uint32_t slot) const noexcept;

        [[nodiscard]] WriteAdmissionSender whenWritable(std::size_t source_bytes) noexcept
        {
            return WriteAdmissionSender{*this, source_bytes};
        }

        void dispatchWrites() noexcept;
        // Register one preparation-submission result. Success means its copies were accepted;
        // failure reports the preparation error. Neither means GPU completion.
        void onPreparationSubmitted(
            void* receiver,
            void (*complete)(void*, std::uint32_t slot, RenderResult<void>) noexcept
        ) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> render(
            std::span<RenderGraphInstance* const>,
            std::span<const StorageFrameUse>,
            std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features = {}
        ) noexcept;
        [[nodiscard]] bool dumpSubmissions(DumpSink) const noexcept;
        [[nodiscard]] RenderResult<bool> waitForProgress(std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] bool submitting(const RenderGraphInstance&) const noexcept;
        // Cold structural boundary: a partially prepared frame must finish before resources can change.
        [[nodiscard]] bool canChangeResources(std::span<RenderGraphInstance* const>) const noexcept;
        [[nodiscard]] SubmissionFrontiers acceptedUses(std::span<RenderGraphInstance* const>) const noexcept;
        [[nodiscard]] bool submitting() const noexcept;
        void cancelSubmissions(RenderError) noexcept;

    private:
        enum class EFramePhase : std::uint8_t
        {
            FREE,
            COLLECTING,
            PREPARING,
            SUBMITTING,
            SUBMITTED
        };

        struct InstanceExecution final
        {
            RenderGraphInstance* instance = nullptr;
            std::uint64_t binding_revision = 0;
            std::uint32_t first_batch = 0;
            std::uint32_t remaining_batches = 0;
        };

        struct BatchExecution final
        {
            std::uint32_t instance = 0, local_batch = 0, queue = 0;
            std::uint32_t predecessor_count = 0, pending_predecessors = 0;
            std::uint32_t first_successor = 0, successor_count = 0;
            SubmissionFrontiers waits;
            std::array<nri::StageBits, 3> stages{};
            std::uint64_t serial = 0;
        };

        struct BatchDependency final
        {
            std::uint32_t producer = 0, consumer = 0;
            nri::StageBits stages = nri::StageBits::ALL;
        };

        struct ImportedUse final
        {
            const void* physical = nullptr;
            VGraphRange range;
            std::uint32_t instance = 0;
            std::uint32_t batch = 0, release_batch = 0;
            bool write = false;
        };

        struct FrameSlot final
        {
            struct PreparedStorage final
            {
                StorageId identity;
                const void* storage = nullptr;
                std::uint64_t revision = 0;
            };

            EFramePhase phase = EFramePhase::FREE;
            SubmissionFrontiers accepted;
            std::array<SubmissionCallback, 3> completions;
            std::uint32_t pending_writes = 0;
            std::uint32_t submitted_batches = 0;
            std::uint8_t pending_queues = 0;
            std::uint32_t recorded_instances = 0, recorded_passes = 0;
            std::uint64_t graphics_serial = 0;
            std::uint64_t preparation_serial = 0;
            std::vector<InstanceExecution> instances;
            std::vector<BatchExecution> batches;
            std::vector<BatchDependency> dependencies;
            std::array<std::vector<std::uint32_t>, 3> ready_batches;
            std::vector<ImportedUse> imported_uses;
            std::vector<std::uint32_t> recording_batches;
            std::vector<nri::FenceSubmitDesc> wait_fences, signal_fences;
            std::vector<ResourceUploader::BufferCopy> feature_copies;
            std::vector<PreparedStorage> prepared_storages;
            std::uint32_t remaining_batches = 0;
            std::uint64_t dependency_rebuilds = 0, imported_comparisons = 0;
        };

        FrameExecution(DeviceSession&, ResourceUploader*, std::uint32_t) noexcept;
        void enqueueWrite(WriteWaiter&) noexcept;
        void sealFrame(FrameSlot&) noexcept;
        void updateUploadAdmission() noexcept;
        void acceptSubmission(std::uint32_t slot, const SubmissionQueue&) noexcept;
        void finishPreparation(RenderResult<void>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareStorages(std::span<const StorageFrameUse>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFeatures(
            std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>>,
            bool& has_copies
        ) noexcept;
        [[nodiscard]] RenderResult<bool> prepareInstances(std::span<RenderGraphInstance* const>) noexcept;
        void collectFeatureCopies(std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>>) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> submitPreparationCopies(
            std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>>,
            bool include_features
        ) noexcept;
        void prepareSubmissions(std::span<RenderGraphInstance* const>) noexcept;
        void rebuildDependencies(std::span<RenderGraphInstance* const>) noexcept;
        void appendImportedDependencies(std::span<RenderGraphInstance* const>) noexcept;
        void enqueueReadyBatch(FrameSlot&, std::uint32_t batch) noexcept;
        void selectReadyBatches(FrameSlot&, std::uint32_t queue) noexcept;
        void acceptBatch(FrameSlot&, std::uint32_t batch, std::uint64_t serial) noexcept;
        [[nodiscard]] static bool laterBatch(const FrameSlot&, std::uint32_t, std::uint32_t) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> advanceSubmissions() noexcept;
        [[nodiscard]] std::size_t queueIndex(const SubmissionQueue&) const noexcept;

        DeviceSession& device_;
        ResourceUploader* uploads_;
        std::array<FrameSlot, 3> frames_;
        std::uint32_t frame_count_;
        std::uint32_t next_frame_ = 0;
        std::uint8_t submit_queue_cursor_ = 0;
        std::uint8_t wait_queue_cursor_ = 0;
        using PreparationCallback = void (*)(void*, std::uint32_t, RenderResult<void>) noexcept;
        std::vector<std::pair<void*, PreparationCallback>> preparation_receivers_;
        WriteWaiter* frame_waiters_ = nullptr;
        WriteWaiter* frame_waiters_tail_ = nullptr;
    };
} // namespace lux::render::detail
