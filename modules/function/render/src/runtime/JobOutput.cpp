#include <lux/engine/render/runtime/JobOutput.hpp>
#include <utility>

namespace lux::render::detail
{
    OutputImage::OutputImage(
        TargetId target,
        std::uint64_t epoch,
        std::uint32_t index,
        nri::Texture* image,
        nri::Descriptor* view,
        const void* domain,
        std::size_t queues
    ) noexcept
        : content_{target, index, epoch, kNoOutputFrame}, image_(image), view_(view), last_use_(domain, queues)
    {
        const bool is_invalid_identity = target.isNull() || epoch == 0 || index == UINT32_MAX;
        if (is_invalid_identity)
        {
            std::terminate();
        }
    }

    OutputImage::~OutputImage() noexcept
    {
        if (borrowed())
        {
            std::terminate();
        }
    }

    OutputImage::OutputImage(OutputImage&& other) noexcept
    {
        if (other.borrowed())
        {
            std::terminate();
        }
        content_ = std::exchange(other.content_, {});
        image_ = std::exchange(other.image_, nullptr);
        view_ = std::exchange(other.view_, nullptr);
        last_use_ = std::move(other.last_use_);
        last_write_frame_ = std::exchange(other.last_write_frame_, kNoOutputFrame);
    }

    bool OutputImage::matches(const JobOutput& output) const noexcept
    {
        return output.published() && content_ == output;
    }

    bool OutputImage::reusable(const SubmissionFrontiers& completed) const noexcept
    {
        return !borrowed() && last_use_.reachedBy(completed);
    }

    OutputRead::~OutputRead() noexcept
    {
        reset();
    }

    OutputRead::OutputRead(OutputRead&& other) noexcept
        : image_(std::exchange(other.image_, nullptr)), expected_(other.expected_)
    {
    }

    OutputRead& OutputRead::operator=(OutputRead&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            image_ = std::exchange(other.image_, nullptr);
            expected_ = other.expected_;
        }
        return *this;
    }

    bool OutputRead::resolve(const JobOutput& output, OutputImage& image) noexcept
    {
        const bool is_unavailable = image_ || image.writer_ || !image.matches(output);
        if (is_unavailable)
        {
            return false;
        }
        if (image.readers_ == UINT32_MAX)
        {
            std::terminate();
        }
        image_ = &image;
        expected_ = output;
        ++image.readers_;
        return true;
    }

    bool OutputRead::ready() const noexcept
    {
        return image_ && image_->matches(expected_);
    }

    void OutputRead::accept(SubmissionPoint accepted) noexcept
    {
        if (!ready())
        {
            std::terminate();
        }
        image_->last_use_.accept(accepted);
    }

    const SubmissionFrontiers& OutputRead::producerAndPriorUses() const noexcept
    {
        if (!image_)
        {
            std::terminate();
        }
        return image_->last_use_;
    }

    void OutputRead::reset() noexcept
    {
        if (!image_)
        {
            return;
        }
        if (image_->readers_ == 0)
        {
            std::terminate();
        }
        --image_->readers_;
        image_ = nullptr;
        expected_.clear();
    }

    OutputWrite::~OutputWrite() noexcept
    {
        reset();
    }

    bool OutputWrite::reserve(OutputImage& image, std::uint64_t frame_index) noexcept
    {
        const bool is_repeated_frame =
            image.last_write_frame_ != kNoOutputFrame && frame_index <= image.last_write_frame_;
        const bool is_invalid_frame = frame_index == kNoOutputFrame || is_repeated_frame;
        const bool is_reserved = image_ || image.writer_;
        const bool is_invalid_request = is_invalid_frame || is_reserved || image.content_.target.isNull();
        if (is_invalid_request)
        {
            return false;
        }
        image_ = &image;
        identity_ = image.content_;
        identity_.frame_index = frame_index;
        image.writer_ = this;
        phase_ = EPhase::WAITING;
        accepted_ = false;
        return true;
    }

    bool OutputWrite::tryBegin(const SubmissionFrontiers& completed) noexcept
    {
        const bool is_waiting = image_ && phase_ == EPhase::WAITING;
        if (!is_waiting)
        {
            return false;
        }
        const bool is_busy = image_->readers_ != 0 || !image_->last_use_.reachedBy(completed);
        if (is_busy)
        {
            return false;
        }
        if (image_->writer_ != this)
        {
            std::terminate();
        }
        phase_ = EPhase::WRITING;
        image_->last_write_frame_ = identity_.frame_index;
        image_->content_.frame_index = kNoOutputFrame;
        image_->last_use_.clear();
        return true;
    }

    bool OutputWrite::borrowPlanned(OutputRead& read) noexcept
    {
        const bool is_writing = image_ && phase_ == EPhase::WRITING;
        const bool can_borrow = is_writing && !read.image_;
        if (!can_borrow)
        {
            return false;
        }
        if (image_->readers_ == UINT32_MAX)
        {
            std::terminate();
        }
        read.image_ = image_;
        read.expected_ = identity_;
        ++image_->readers_;
        return true;
    }

    void OutputWrite::accept(SubmissionPoint accepted) noexcept
    {
        const bool is_writing = image_ && phase_ == EPhase::WRITING;
        if (!is_writing)
        {
            std::terminate();
        }
        image_->last_use_.accept(accepted);
        accepted_ |= accepted.serial != 0;
    }

    void OutputWrite::publish(JobOutput& output) noexcept
    {
        const bool can_publish = image_ && phase_ == EPhase::WRITING && accepted_;
        if (!can_publish)
        {
            std::terminate();
        }
        image_->content_ = identity_;
        output = identity_;
        phase_ = EPhase::PUBLISHED;
    }

    void OutputWrite::reset() noexcept
    {
        if (!image_)
        {
            return;
        }
        if (image_->writer_ != this)
        {
            std::terminate();
        }
        image_->writer_ = nullptr;
        image_ = nullptr;
        identity_.clear();
        phase_ = EPhase::WAITING;
        accepted_ = false;
    }
} // namespace lux::render::detail
