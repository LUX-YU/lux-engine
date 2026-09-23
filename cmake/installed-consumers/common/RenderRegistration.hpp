#pragma once
#include <lux/engine/render/RenderRuntime.hpp>
#include <cassert>
#include <chrono>
#include <thread>

inline void registerRenderFeatures(lux::render::RenderRuntime &runtime,
                                  std::vector<lux::render::RenderFeatureRegistration> features)
{
    using namespace lux::render;
    assert(runtime.beginFeatureRegistration(std::move(features)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (runtime.featureRegistrationStatus().state == EFeatureRegistrationState::REGISTERING)
    {
        std::size_t controls = 2, programs = 1;
        assert(runtime.poll(8, controls, programs));
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(runtime.featureRegistrationStatus().state == EFeatureRegistrationState::READY);
    assert(runtime.commitFeatureRegistration());
}
