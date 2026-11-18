#pragma once
#include <cstdint>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/commands/JobCommands.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render::detail
{
    struct JobReceipt final
    {
        RenderJobId job_id;
        std::uint64_t frame_index = 0;
        std::uint32_t slot_index = 0;
    };

    // A value member of RenderJob. No graph/Target/Device owning pointers here.
    // All calls and callbacks belong to the single RenderServer control thread.
    class JobControl final
    {
    public:
        class Start;
        class Run;

        struct Request final
        {
            using Complete = void (*)(void*, RenderResult<JobReceipt>) noexcept;
            void* receiver;
            Complete complete;
            const FrameData* data;

            explicit Request(void* object, Complete done, const FrameData* packet = nullptr) noexcept
                : receiver(object), complete(done), data(packet)
            {
            }

            ~Request() noexcept;
            Request(const Request&) = delete;
            Request& operator=(const Request&) = delete;

        private:
            friend class JobControl;
            JobControl* owner_ = nullptr;
        };

        explicit JobControl(EJobCadence cadence) noexcept;
        void setId(RenderJobId) noexcept;
        ~JobControl() noexcept;
        JobControl(const JobControl&) = delete;
        JobControl& operator=(const JobControl&) = delete;

        // External Scene/Target device compatibility is checked once before this call.
        [[nodiscard]] RenderResult<void> beginStart(TargetId target, bool rebind, Start& output) noexcept;
        void stop() noexcept; // closes admission NOW; already admitted Run may finish
        void rejectUnadmitted(RenderError) noexcept;
        [[nodiscard]] RenderResult<void> request(Request& request) noexcept;
        [[nodiscard]] bool tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index, Run& output) noexcept;

        [[nodiscard]] bool running() const noexcept
        {
            return running_;
        }

        [[nodiscard]] Start* pendingStart() const noexcept { return starting_; }
        [[nodiscard]] bool hasRun() const noexcept { return admitted_ != nullptr; }
        [[nodiscard]] bool hasRequest() const noexcept { return requested_ != nullptr; }
        [[nodiscard]] const FrameData* takeRequestedData() noexcept;
        [[nodiscard]] bool eligible() const noexcept;
        [[nodiscard]] bool canRemove() const noexcept;

        [[nodiscard]] RenderJobId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] EJobCadence cadence() const noexcept
        {
            return cadence_;
        }

        [[nodiscard]] RenderResult<void> setCadence(EJobCadence cadence) noexcept;

        class Start final
        {
        public:
            Start() noexcept = default;
            ~Start() noexcept;
            Start(const Start&) = delete;
            Start& operator=(const Start&) = delete;
            // Call only after candidate graph/pipeline construction has succeeded.
            // The Server validates ownership of the actual Target reservation before publication.
            [[nodiscard]] RenderResult<void> publish() noexcept;

            [[nodiscard]] bool active() const noexcept
            {
                return job_ != nullptr;
            }

            [[nodiscard]] TargetId target() const noexcept { return target_; }
            [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

        private:
            friend class JobControl;
            JobControl* job_ = nullptr;
            TargetId target_{};
            bool cancelled_ = false;
        };

        class Run final
        {
        public:
            Run() noexcept = default;
            ~Run() noexcept;
            Run(const Run&) = delete;
            Run& operator=(const Run&) = delete;
            // Complete after actual native acceptance, not after enqueueing a call.
            void finish(RenderResult<void> submitted) noexcept;

            [[nodiscard]] JobReceipt receipt() const noexcept
            {
                return receipt_;
            }

            [[nodiscard]] bool active() const noexcept
            {
                return job_ != nullptr;
            }

        private:
            friend class JobControl;
            JobControl* job_ = nullptr;
            Request* request_ = nullptr;
            JobReceipt receipt_{};
        };

    private:
        RenderJobId id_;
        EJobCadence cadence_;
        bool running_ = false;
        Start* starting_ = nullptr; // addresses of real connected operations
        Run* admitted_ = nullptr;
        Request* requested_ = nullptr; // one exact request credit per Job; never silently coalesced
    };
} // namespace lux::render::detail
