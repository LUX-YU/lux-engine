#include "TextureFixture.hpp"
#include "TextureSceneFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <atomic>
#include <fstream>
#include <semaphore>

namespace
{
    SubmissionQueue* observed_graphics = nullptr;
    std::uint64_t observed_limit = UINT64_MAX;
    constexpr std::array names{"image", "image_view", "texture_sampler", "buffer"};
    std::array<unsigned, 4> created{}, destroyed{};
    std::array<long long, 4> last_destroyed_ns{};
    unsigned pipelines = 0;

    long long monotonicNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    struct Timeline final
    {
        struct Submit final { long long begin = 0, end = 0; VkResult result = VK_NOT_READY; };
        std::array<Submit, 8> submissions{};
        std::size_t count = 0;
        long long terminal_observed = 0, publish_returned = 0, staging_lease_released_observed = 0;
        long long source_destroyed = 0;
        unsigned source_destroys = 0;
    };

    Timeline* active_timeline = nullptr;

    void verify(bool condition, int line) noexcept
    {
        if (condition) return;
        std::fprintf(stderr, "FAILED LastUse assertion line %d\n", line);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

#undef CHECK
#define CHECK(condition) verify(static_cast<bool>(condition), __LINE__)

namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "submit" && active_timeline)
        {
            CHECK(active_timeline->count < active_timeline->submissions.size());
            active_timeline->submissions[active_timeline->count].begin = monotonicNs();
        }
        // During the controlled interval only Graphics has pending submissions. All other physical
        // queues are drained before arming this limit. The real Fence query still decides readiness.
        if (std::string_view(call) == "fence_status" && observed_graphics &&
            observed_graphics->completed() >= observed_limit) return VK_NOT_READY;
        return VK_SUCCESS;
    }

    VkResult after(const char* call, VkResult result) noexcept
    {
        if (std::string_view(call) == "cpuPrepare") lux::render::test::CpuFixture::runBeforeWork();
        if (std::string_view(call) == "submit" && active_timeline)
        {
            auto& submission = active_timeline->submissions[active_timeline->count++];
            submission.end = monotonicNs();
            submission.result = result;
        }
        if (result == VK_SUCCESS)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (std::string_view(call) == names[i]) ++created[i];
            if (std::string_view(call) == "pipeline") ++pipelines;
        }
        return result;
    }

    void destroyed(const char* call) noexcept
    {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (std::string_view(call) == names[i])
            {
                ++::destroyed[i];
                last_destroyed_ns[i] = monotonicNs(); // The real Native destruction call already returned.
            }
    }
}

namespace lux::render::vulkan
{
    struct NativeMemoryAudit final
    {
        // Existing test friendship: inspect the actual prepared binding's shared requirement value.
        // No mutation, extra lease, mirrored requirement or production diagnostic API is introduced.
        static SubmissionRequirements textureRequirements(const ExecutionContext& context, VkDescriptorSet descriptors)
        {
            for (const auto& lease : context.leases_)
                if (lease.binding.descriptors == descriptors && lease.binding.last_use) return *lease.binding.last_use;
            CHECK(false);
            return {};
        }
    };
}

namespace
{
    using NativeTexture = lux::render::vulkan::TextureStorage;

    void contains(const SubmissionRequirements& requirements, SubmissionTicket ticket)
    {
        unsigned matching = 0;
        for (const auto& use : requirements.uses())
            if (use.queue == &ticket.owner())
            {
                ++matching;
                CHECK(use.serial == ticket.serial());
            }
        CHECK(matching == 1);
    }

    void pixels(ExecutionContext& context, unsigned expected)
    {
        const auto bytes = take(context.readback());
        CHECK(bytes.size() == 16 * 16 * 4);
        for (const auto byte : bytes) CHECK(std::to_integer<unsigned>(byte) == expected);
    }

    void observe(DeviceSession& session, SubmissionTicket ticket)
    {
        observed_limit = ticket.serial();
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (take(session.queue().poll()) < ticket.serial())
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    TexturePublished createTexture(NativeTexture& storage, const SetTexture::Request& input,
        DeviceSession& session, CpuFixture& host)
    {
        auto prepared = take(lux::render::upload(storage, input));
        complete(prepared.second, host, session);
        prepared.second.reset();
        return prepared.first;
    }

    void report(std::ofstream& csv, unsigned topology, bool multiple_chunks, std::string_view stage,
        SubmissionQueue& graphics, const NativeTexture& storage, SubmissionTicket first, SubmissionTicket draw,
        const ImageMemoryBinding& memory, std::uint32_t descriptor, const Timeline& timeline, TransferStats stats)
    {
        csv << topology << ',' << multiple_chunks << ',' << stage << ',' << monotonicNs() << ','
            << graphics.nativeQueue().family << ',' << graphics.nativeQueue().index << ','
            << first.serial() << ',' << draw.serial() << ',' << graphics.completed() << ','
            << storage.residentVersions() << ',' << storage.residentBytes() << ',' << descriptor << ','
            << memory.memory << ',' << memory.offset << ',' << memory.size << ','
            << destroyed[0] << ',' << destroyed[1] << ',' << destroyed[2] << ','
            << timeline.submissions[0].begin << ',' << timeline.submissions[0].end << ','
            << timeline.submissions[1].begin << ',' << timeline.submissions[1].end << ','
            << timeline.terminal_observed << ',' << timeline.publish_returned << ','
            << timeline.staging_lease_released_observed << ',' << timeline.source_destroyed << ','
            << timeline.source_destroys << ',' << stats.pending_bytes << ',' << stats.staging_bytes << ','
            << last_destroyed_ns[0] << ',' << last_destroyed_ns[1] << ',' << last_destroyed_ns[2] << '\n';
        csv.flush();
    }

    struct SecondPreparation final
    {
        std::atomic<unsigned> calls{0};
        std::binary_semaphore entered{0}, released{0};

        static void before(void* owner) noexcept
        {
            auto& self = *static_cast<SecondPreparation*>(owner);
            if (self.calls.fetch_add(1, std::memory_order_acq_rel) == 1)
            {
                self.entered.release();
                self.released.acquire();
            }
        }
    };

    void interleave(const InstanceContext& bootstrap, CpuFixture& host, unsigned topology,
        bool multiple_chunks, std::ofstream& csv)
    {
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.texture_descriptors = true;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options, 8));
        const std::size_t chunk_bytes = multiple_chunks ? 256 : 1024;
        CHECK(session->configureUploads({chunk_bytes, 1, 65536}, host.cpu()));
        CHECK(configureTextures(*session, 16, 1024 * 1024));
        auto& storage = *textures(*session);
        auto& graphics = session->queue();
        auto& transfer = session->queue(EQueueRole::TRANSFER);
        if (topology == 0) CHECK(&graphics == &transfer);
        if (topology == 1) CHECK(&graphics != &transfer &&
            graphics.nativeQueue().family == transfer.nativeQueue().family);
        if (topology == 2) CHECK(&graphics != &transfer &&
            graphics.nativeQueue().family != transfer.nativeQueue().family);

        const auto original = std::make_shared<const std::vector<std::byte>>(1024, std::byte{128});
        const SetTexture::Request create{{storage.id(), {}}, 0, information(16, 1, false), payload(original)};
        std::vector<TexturePublished> fillers;
        if (!multiple_chunks)
            for (unsigned i = 0; i < 14; ++i) fillers.push_back(createTexture(storage, create, *session, host));
        const auto published = createTexture(storage, create, *session, host);
        const auto old_descriptor = storage.find(published.texture)->descriptorIndex();
        const auto old_memory = session->memoryBinding(storage.find(published.texture)->image());
        const auto old_image = storage.find(published.texture)->image().native();

        const texture_fixture::Config config{published.texture, 16, 16, 1,
            rdesc::ETextureFormat::RGBA8_UNORM, 0};
        auto assembly = take(texture_fixture::assemble(config));
        const auto pipelines_before = pipelines;
        auto graph = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings,
            texture_fixture::programs(config), texture_fixture::target(config))));
        CHECK(pipelines == pipelines_before + 1);
        auto* context = take(session->prepare(graph, texture_fixture::target(config), nullptr, nullptr,
            texture_fixture::arguments(config)));
        CHECK(session->drain());
        for (const auto role : {EQueueRole::GRAPHICS, EQueueRole::COMPUTE, EQueueRole::TRANSFER})
            CHECK(session->queue(role).completed() == session->queue(role).submitted());

        Timeline timeline;
        auto patch_bytes = std::shared_ptr<const std::vector<std::byte>>(
            new std::vector<std::byte>(1024, std::byte{96}), [&timeline](auto* bytes) noexcept {
                delete bytes;
                ++timeline.source_destroys;
                timeline.source_destroyed = monotonicNs();
            });
        SetTextureRegion::Request patch{published.texture, published.revision,
            {0, 0, 0, 16, 16, 64}, payload(patch_bytes)};
        patch_bytes.reset(); // The real request alone retains the source through all CPU/GPU use.
        auto replacement = take(lux::render::upload(storage, patch));
        CHECK(replacement.first.texture == published.texture && replacement.first.revision == 2);
        const auto destroyed_before = destroyed;
        const auto chunks_before = session->uploadStats().chunks;
        const auto submissions_before = graphics.submitted();
        observed_graphics = &graphics;
        observed_limit = graphics.completed();
        active_timeline = &timeline;
        SecondPreparation cpu_gate;
        if (multiple_chunks) host.setBeforeWork(&cpu_gate, SecondPreparation::before);
        WorkCompletion completion;
        completion.start(replacement.second, host);
        const auto first_deadline = Clock::now() + std::chrono::seconds(10);
        while (graphics.submitted() == submissions_before)
        {
            // Only CPU completion consumption is allowed here. No Queue completion is observed yet.
            (void)host.drain();
            CHECK(!completion.done && Clock::now() < first_deadline);
            std::this_thread::yield();
        }
        const auto first = *graphics.lastSubmitted();
        CHECK(session->uploadStats().chunks - chunks_before == 1);
        CHECK(first.serial() == submissions_before + 1);
        CHECK(take(context->execute()) == EExecutionOutcome::EXECUTED);
        const auto draw = *context->lastUse();
        CHECK(&draw.owner() == &first.owner() && draw.serial() == first.serial() + 1);
        CHECK(timeline.count == 2 && timeline.submissions[0].result == VK_SUCCESS &&
            timeline.submissions[1].result == VK_SUCCESS);
        CHECK(timeline.submissions[0].begin <= timeline.submissions[0].end &&
            timeline.submissions[0].end <= timeline.submissions[1].begin &&
            timeline.submissions[1].begin <= timeline.submissions[1].end);
        contains(NativeMemoryAudit::textureRequirements(*context, storage.descriptors()), draw);
        report(csv, topology, multiple_chunks, "T1-clone-T2-draw-unobserved", graphics, storage,
            first, draw, old_memory, old_descriptor, timeline, session->uploadStats());

        observe(*session, first);
        CHECK(graphics.completed() == first.serial() && graphics.completed() < draw.serial());
        CHECK(!completion.done); // poll alone cannot run the source completion callback.
        session->dispatchCompletions();
        if (completion.done) timeline.terminal_observed = monotonicNs();
        contains(NativeMemoryAudit::textureRequirements(*context, storage.descriptors()), draw);

        if (multiple_chunks)
        {
            CHECK(cpu_gate.entered.try_acquire_for(std::chrono::seconds(10)));
            CHECK(cpu_gate.calls.load(std::memory_order_acquire) == 2 && !completion.done);
            CHECK(graphics.submitted() == draw.serial());
            contains(NativeMemoryAudit::textureRequirements(*context, storage.descriptors()), draw);
            report(csv, topology, true, "T1-observed-CPU2-held-T2-preserved", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
            replacement.second.cancel();
            CHECK(!completion.done && graphics.submitted() == draw.serial());
            cpu_gate.released.release();
            finish(completion, host, *session);
            timeline.terminal_observed = monotonicNs();
            host.setBeforeWork(nullptr, nullptr);
            CHECK(!completion.result && completion.result.error().type == kWorkStopped);
            CHECK(!replacement.second.publish());
            CHECK(timeline.source_destroys == 0 && session->uploadStats().pending_bytes == 1024);
            report(csv, topology, true, "cancel-terminal-source-and-staging-still-retained", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
            replacement.second.reset();
            timeline.staging_lease_released_observed = monotonicNs();
            CHECK(session->uploadStats().pending_bytes == 0 && timeline.source_destroys == 0);
            patch.source = {};
            CHECK(timeline.source_destroys == 1);
            CHECK(graphics.submitted() == draw.serial()); // There must be no T3 hiding the stale T1 overwrite.
            CHECK(session->uploadStats().chunks - chunks_before == 1);
            CHECK(storage.find(published.texture)->revision() == 1);
            CHECK(storage.find(published.texture)->image().native() == old_image);
            CHECK(storage.find(published.texture)->descriptorIndex() == old_descriptor);
            contains(NativeMemoryAudit::textureRequirements(*context, storage.descriptors()), draw);
            // Only the rejected candidate has been destroyed; the original's real Draw still owns its use.
            CHECK(destroyed[0] == destroyed_before[0] + 1 && destroyed[1] == destroyed_before[1] + 1);
            CHECK(destroyed[2] == destroyed_before[2] + 1 && storage.residentVersions() == 1);
            CHECK(storage.release(published.texture, 1));
            CHECK(session->release(context));
            context = nullptr;
            CHECK(session->collect());
            CHECK(storage.residentVersions() == 1 && graphics.completed() < draw.serial());
            CHECK(destroyed[2] == destroyed_before[2] + 1);
            report(csv, topology, true, "cancel-no-T3-last-good-delete-held", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
            observe(*session, draw);
            CHECK(session->collect());
            CHECK(storage.residentVersions() == 0 && destroyed[2] == destroyed_before[2] + 2);
            report(csv, topology, true, "T2-observed-collect-returned-old-physically-destroyed", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
        }
        else
        {
            CHECK(completion.done && completion.result);
            CHECK(replacement.second.publish());
            timeline.publish_returned = monotonicNs();
            CHECK(!replacement.second.publish());
            CHECK(timeline.source_destroys == 0 && session->uploadStats().pending_bytes == 1024);
            report(csv, topology, false, "T1-terminal-published-source-and-staging-still-retained", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
            replacement.second.reset();
            timeline.staging_lease_released_observed = monotonicNs();
            CHECK(session->uploadStats().pending_bytes == 0 && timeline.source_destroys == 0);
            patch.source = {};
            CHECK(timeline.source_destroys == 1);
            CHECK(storage.find(published.texture)->revision() == 2);
            CHECK(storage.find(published.texture)->descriptorIndex() != old_descriptor);
            CHECK(storage.residentVersions() == 16 && session->collect());
            CHECK(destroyed == destroyed_before); // No manual old lease conceals a missing T2 requirement.
            const auto no_capacity = lux::render::upload(storage, create);
            CHECK(!no_capacity && no_capacity.error().type == kCapacity);
            CHECK(create.source.bytes().data() == original->data());
            CHECK(graphics.submitted() == draw.serial());
            CHECK(storage.release(published.texture, 2));
            CHECK(storage.residentVersions() == 16 && graphics.completed() < draw.serial());
            CHECK(destroyed == destroyed_before);
            report(csv, topology, false, "replace-delete-T2-held-no-old-lease", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());

            observe(*session, draw);
            pixels(*context, 128); // This was the real T2 Draw against the old binding.
            CHECK(session->collect());
            CHECK(storage.residentVersions() == 15);
            CHECK(destroyed[0] == destroyed_before[0] + 1 && destroyed[1] == destroyed_before[1] + 1);
            CHECK(destroyed[2] == destroyed_before[2] + 1);
            report(csv, topology, false, "T2-observed-collect-returned-old-physically-destroyed", graphics, storage,
                first, draw, old_memory, old_descriptor, timeline, session->uploadStats());
            auto capacity_recovered = take(lux::render::upload(storage, create));
            CHECK(graphics.submitted() == draw.serial()); // Cold reservation did not submit work.
            capacity_recovered.second.reset();
            observed_limit = UINT64_MAX;
            CHECK(take(context->execute()) == EExecutionOutcome::EXECUTED);
            CHECK(take(graphics.wait(*context->lastUse(), UINT64_MAX)));
            pixels(*context, 96); // The same compiled program now reads the published replacement.
            CHECK(session->release(context));
            context = nullptr;
            for (const auto filler : fillers) CHECK(storage.release(filler.texture, filler.revision));
            CHECK(session->collect() && storage.residentVersions() == 0);
        }
        observed_graphics = nullptr;
        active_timeline = nullptr;
        CHECK(timeline.source_destroys == 1 && timeline.terminal_observed >= timeline.submissions[0].end);
        CHECK(timeline.source_destroyed >= timeline.staging_lease_released_observed &&
            timeline.staging_lease_released_observed >= timeline.terminal_observed);
        CHECK(pipelines == pipelines_before + 1);
        CHECK(session->uploadStats().pending_bytes == 0);
        CHECK(session->drain());
        graph.reset();
        std::printf("LastUse topology=%u graphics=%u:%u transfer=%u:%u region_queue=graphics planned_chunks=%u "
            "accepted_copy_chunks=1 "
            "T1=%llu T2=%llu; old_descriptor=%u memory_offset=%llu bytes=%llu PASS\n", topology,
            graphics.nativeQueue().family, graphics.nativeQueue().index, transfer.nativeQueue().family,
            transfer.nativeQueue().index, multiple_chunks ? 4u : 1u,
            static_cast<unsigned long long>(first.serial()), static_cast<unsigned long long>(draw.serial()),
            old_descriptor, static_cast<unsigned long long>(old_memory.offset),
            static_cast<unsigned long long>(old_memory.size));
    }
}

int main()
{
    Validation validation;
    CpuFixture host;
    std::ofstream csv("last-use-interleavings.csv");
    CHECK(csv.good());
    csv << "topology,multichunk,stage,steady_ns,graphics_family,graphics_index,T1,T2,frontier,versions,"
        "resident_bytes,old_descriptor,memory,offset,allocation_size,image_destroys,view_destroys,sampler_destroys,"
        "T1_submit_cpu_begin_ns,T1_submit_cpu_end_ns,T2_submit_cpu_begin_ns,T2_submit_cpu_end_ns,"
        "terminal_observed_ns,publish_returned_ns,staging_lease_release_observed_ns,source_destroyed_ns,"
        "source_destroys,pending_source_bytes,allocated_staging_bytes,last_image_destroy_returned_ns,"
        "last_view_destroy_returned_ns,last_sampler_destroy_returned_ns\n";
    {
        auto bootstrap = instance(validation);
        for (unsigned topology = 0; topology < 3; ++topology)
        {
            interleave(bootstrap, host, topology, false, csv);
            interleave(bootstrap, host, topology, true, csv);
        }
    }
    CHECK(created == destroyed);
    CHECK(validation.errors == 0);
    std::printf("LastUse real clone/draw: 3 topologies x normal replacement + multichunk cancellation; "
        "Validation=0 warnings=%u; VMA ranges measured, no separate Arena implementation; GPU timestamp NOT_MEASURED; "
        "staging lease observation is after Work reset, allocated page remains resident; zero time means not yet observed\n",
        validation.warnings.load());
}
