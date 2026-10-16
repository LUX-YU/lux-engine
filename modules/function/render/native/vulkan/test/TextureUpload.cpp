#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include <fstream>

// Historical CTest name is retained. The real consumer is now StorageTraits through render::upload.
int main()
{
    Validation validation;
    CpuFixture worker;
    std::ofstream csv("texture-chunks.csv");
    csv << "multiple,dedicated,bytes,mips,bc,chunks,prepare_ns,owner_ns,wall_ns,staging_bytes,pending_peak,gpu_ns,gpu_samples\n";
    {
        auto bootstrap = instance(validation);
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            DeviceOptions options;
            options.multiple_queues = mode != 0;
            options.prefer_dedicated_queues = mode == 2;
            options.texture_descriptors = true;
            options.texture_compression_bc = true;
            options.host_query_reset = true;
            auto session = take(DeviceSession::create(bootstrap, options));
            CHECK(session->configureUploads({1024 * 1024, 1, 128 * 1024 * 1024, true}, worker.cpu()));
            CHECK(configureTextures(*session, 16, 256 * 1024 * 1024));
            auto& storage = *textures(*session);
            for (unsigned sample = 0; sample < 5; ++sample)
            {
                const bool bc = sample == 4;
                auto info = information(sample == 0 ? 512 : sample == 1 ? 2048 : sample == 2 ? 4096 : 64,
                    sample >= 3 ? 4 : 1, bc);
                const auto& last = info.mip_ranges[info.mip_count - 1];
                auto source = std::make_shared<std::vector<std::byte>>(last.offset + last.size);
                for (std::size_t i = 0; i < source->size(); ++i) (*source)[i] = std::byte(i % 251);
                SetTexture::Request request{{}, 0, info, payload(source)};
                auto malformed = request;
                malformed.info.mip_ranges[0].size = UINT64_MAX;
                CHECK(!lux::render::upload(storage, malformed));
                malformed = request;
                malformed.info.color_space = rdesc::ETextureColorSpace::UNKNOWN;
                CHECK(!lux::render::upload(storage, malformed));
                const auto before = session->uploadStats();
                const auto start = Clock::now();
                auto candidate = take(lux::render::upload(storage, request));
                CHECK(!lux::render::upload(storage, request)); // page capacity; no dedicated fallback
                CHECK(!storage.find(candidate.first.texture));
                complete(candidate.second, worker, *session);
                candidate.second.reset();
                const auto* backing = storage.find(candidate.first.texture);
                CHECK(backing && std::ranges::equal(read(*session, *backing), *source));
                CHECK(storage.release(candidate.first.texture, candidate.first.revision));
                const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                const auto after = session->uploadStats();
                CHECK(after.pending_bytes == 0 && after.staging_bytes == 1024 * 1024);
                csv << options.multiple_queues << ',' << options.prefer_dedicated_queues << ',' << source->size() << ','
                    << info.mip_count << ',' << bc << ',' << after.chunks - before.chunks << ','
                    << after.prepare_ns - before.prepare_ns << ',' << after.owner_ns - before.owner_ns << ','
                    << wall << ',' << after.staging_bytes << ',' << after.pending_peak << ','
                    << after.gpu_ns - before.gpu_ns << ',' << after.gpu_samples - before.gpu_samples << '\n';
            }
        }
    }
    CHECK(validation.errors == 0);
    std::printf("texture chunks 1/16/64MiB + mip/BC exact readback; validation_errors=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
}
