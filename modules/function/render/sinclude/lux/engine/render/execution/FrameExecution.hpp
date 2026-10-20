#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class ExecutionContext;

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

        [[nodiscard]] WriteAdmissionSender whenWritable(std::size_t source_bytes) noexcept
        {
            return WriteAdmissionSender{*this, source_bytes};
        }

        void dispatchWrites() noexcept;
        void afterSubmit(
            void* receiver,
            void (*complete)(void*, std::uint32_t slot, RenderResult<void>) noexcept
        ) noexcept;
        static void collectStorageUses(std::span<ExecutionContext* const>, std::vector<StorageFrameUse>&) noexcept;
        [[nodiscard]] RenderResult<
            std::uint32_t> render(std::span<ExecutionContext* const>, std::span<const StorageFrameUse>) noexcept;
        [[nodiscard]] RenderResult<bool> waitForProgress(std::uint64_t timeout_ns) noexcept;

    private:
        enum class EFramePhase : std::uint8_t
        {
            FREE,
            COLLECTING,
            PREPARING,
            SUBMITTED
        };

        struct FrameSlot final
        {
            EFramePhase phase = EFramePhase::FREE;
            SubmissionFrontiers accepted;
            std::array<SubmissionCallback, 3> completions;
            std::uint32_t pending_writes = 0;
            std::uint8_t pending_queues = 0;
        };

        FrameExecution(DeviceSession&, ResourceUploader*, std::uint32_t) noexcept;
        void enqueueWrite(WriteWaiter&) noexcept;
        void sealFrame(FrameSlot&) noexcept;
        void updateUploadAdmission() noexcept;
        void acceptSubmission(std::uint32_t slot, const SubmissionQueue&) noexcept;
        void finishSubmission(RenderResult<void>) noexcept;

        DeviceSession& device_;
        ResourceUploader* uploads_;
        std::array<FrameSlot, 3> frames_;
        std::uint32_t frame_count_;
        std::uint32_t next_frame_ = 0;
        std::uint8_t wait_queue_cursor_ = 0;
        std::vector<std::pair<void*, void (*)(void*, std::uint32_t, RenderResult<void>) noexcept>> frame_receivers_;
        WriteWaiter* frame_waiters_ = nullptr;
        WriteWaiter* frame_waiters_tail_ = nullptr;
    };
} // namespace lux::render::detail
