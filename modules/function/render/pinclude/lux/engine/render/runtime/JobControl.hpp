#pragma once
#include <cstdint>
#include <optional>
#include <utility>
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

        struct Request final
        {
            using Complete = void (*)(void*, RenderResult<JobReceipt>) noexcept;
            void* receiver = nullptr;
            Complete complete = nullptr;

            Request() noexcept = default;
            explicit Request(void* object, Complete done) noexcept : receiver(object), complete(done)
            {
            }

            ~Request() noexcept;
            Request(Request&& other) noexcept;
            Request(const Request&) = delete;
            Request& operator=(const Request&) = delete;

        private:
            friend class JobControl;
            JobControl* owner_ = nullptr;
        };

        struct Completion final
        {
            void* receiver;
            Request::Complete complete;
            RenderResult<JobReceipt> result;

            void deliver() && noexcept { complete(receiver, std::move(result)); }
        };

        explicit JobControl(EJobCadence cadence) noexcept;
        void setId(RenderJobId) noexcept;
        ~JobControl() noexcept;
        JobControl(const JobControl&) = delete;
        JobControl& operator=(const JobControl&) = delete;

        [[nodiscard]] RenderResult<std::uint64_t> reserveGeneration(bool rebind) noexcept;
        void publishGeneration(std::uint64_t generation) noexcept;
        void stop() noexcept; // closes admission NOW; already admitted Run may finish
        void rejectUnadmitted(RenderError) noexcept;
        [[nodiscard]] RenderResult<void> request(Request& request) noexcept;
        [[nodiscard]] bool tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index) noexcept;
        [[nodiscard]] std::optional<Completion>
        finishRun(std::uint64_t generation, RenderResult<void> submitted) noexcept;

        [[nodiscard]] bool running() const noexcept
        {
            return running_;
        }

        [[nodiscard]] std::uint64_t executionGeneration() const noexcept { return execution_generation_; }
        [[nodiscard]] bool hasRun() const noexcept { return admitted_.has_value(); }
        [[nodiscard]] bool hasRequest() const noexcept { return requested_ != nullptr; }

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

    private:
        struct AdmittedRun final
        {
            JobReceipt receipt;
            Request* request;
        };

        RenderJobId id_;
        EJobCadence cadence_;
        bool running_ = false;
        std::uint64_t last_generation_ = 0;
        std::uint64_t execution_generation_ = 0;
        std::optional<AdmittedRun> admitted_;
        Request* requested_ = nullptr; // one exact request credit per Job; never silently coalesced
    };
} // namespace lux::render::detail
