#pragma once

#include <limits>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>

namespace nri
{
    struct Texture;
    struct Descriptor;
} // namespace nri

namespace lux::render::detail
{
    inline constexpr auto kNoOutputFrame = std::numeric_limits<std::uint64_t>::max();

    // RenderJob's last publication. Owns no native object and does not keep a Target alive.
    struct JobOutput final
    {
        TargetId target;
        std::uint32_t image_index = UINT32_MAX;
        std::uint64_t target_epoch = 0;
        std::uint64_t frame_index = kNoOutputFrame;

        void clear() noexcept
        {
            *this = {};
        }

        [[nodiscard]] bool published() const noexcept
        {
            return !target.isNull() && image_index != UINT32_MAX && target_epoch != 0 && frame_index != kNoOutputFrame;
        }

        bool operator==(const JobOutput&) const noexcept = default;
    };

    class OutputRead;
    class OutputWrite;

    // Metadata embedded in an existing physical Target image entry, never another Image owner
    // or a last-valid selector. Epoch changes on replacement even if native pointer values repeat.
    class OutputImage final
    {
    public:
        OutputImage(
            TargetId target,
            std::uint64_t epoch,
            std::uint32_t index,
            nri::Texture* image,
            nri::Descriptor* view,
            const void* domain,
            std::size_t queues
        ) noexcept;
        ~OutputImage() noexcept;
        OutputImage(const OutputImage&) = delete;
        OutputImage& operator=(const OutputImage&) = delete;
        OutputImage(OutputImage&& other) noexcept;
        OutputImage& operator=(OutputImage&&) = delete;
        [[nodiscard]] bool matches(const JobOutput& output) const noexcept;
        [[nodiscard]] bool reusable(const SubmissionFrontiers& completed) const noexcept;

        [[nodiscard]] bool borrowed() const noexcept
        {
            return writer_ || readers_ != 0;
        }

        [[nodiscard]] const SubmissionFrontiers& lastUse() const noexcept
        {
            return last_use_;
        }

    private:
        friend class OutputRead;
        friend class OutputWrite;
        JobOutput content_;
        nri::Texture* image_ = nullptr;
        nri::Descriptor* view_ = nullptr;
        SubmissionFrontiers last_use_;
        // Prevent a failed/retried write from reusing a formerly published content stamp.
        std::uint64_t last_write_frame_ = kNoOutputFrame;
        std::uint32_t readers_ = 0;
        OutputWrite* writer_ = nullptr;
    };

    class OutputRead final
    {
    public:
        OutputRead() noexcept = default;
        ~OutputRead() noexcept;
        OutputRead(const OutputRead&) = delete;
        OutputRead& operator=(const OutputRead&) = delete;
        OutputRead(OutputRead&& other) noexcept;
        OutputRead& operator=(OutputRead&& other) noexcept;
        // Target lookup already validated generation; this validates exact image/epoch/content too.
        [[nodiscard]] bool resolve(const JobOutput& output, OutputImage& image) noexcept;
        // Planned reads cannot submit until the producer publishes its actual accepted result.
        [[nodiscard]] bool ready() const noexcept;
        void accept(SubmissionPoint accepted) noexcept;
        void reset() noexcept;

        [[nodiscard]] nri::Texture* image() const noexcept
        {
            return image_ ? image_->image_ : nullptr;
        }

        [[nodiscard]] nri::Descriptor* view() const noexcept
        {
            return image_ ? image_->view_ : nullptr;
        }

        [[nodiscard]] const SubmissionFrontiers& producerAndPriorUses() const noexcept;

    private:
        friend class OutputWrite;
        OutputImage* image_ = nullptr;
        JobOutput expected_;
    };

    class OutputWrite final
    {
    public:
        OutputWrite() noexcept = default;
        ~OutputWrite() noexcept;
        OutputWrite(const OutputWrite&) = delete;
        OutputWrite& operator=(const OutputWrite&) = delete;
        // Select first, then drain existing readers. New latest readers cannot starve this writer.
        [[nodiscard]] bool reserve(OutputImage& image, std::uint64_t frame_index) noexcept;
        [[nodiscard]] bool tryBegin(const SubmissionFrontiers& completed) noexcept;
        [[nodiscard]] bool borrowPlanned(OutputRead& read) noexcept;
        void accept(SubmissionPoint accepted) noexcept;
        // Caller has adopted ALL required producing submissions. Stop does not cancel this publication.
        void publish(JobOutput& output) noexcept;
        void reset() noexcept;

    private:
        enum class EPhase : std::uint8_t
        {
            WAITING,
            WRITING,
            PUBLISHED
        };
        OutputImage* image_ = nullptr;
        JobOutput identity_;
        EPhase phase_ = EPhase::WAITING;
        bool accepted_ = false;
    };
} // namespace lux::render::detail
