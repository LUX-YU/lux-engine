#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    lux::cxx::SharedBytes<> readImage(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            return {};
        const auto end = input.tellg();
        if (end <= 0)
            return {};
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return input
            ? lux::cxx::SharedBytes<>::copyOf(bytes)
            : lux::cxx::SharedBytes<>{};
    }
}

int main(int argc, char** argv)
{
    using namespace lux::extensions;

    static_assert(lux::extensions::kExtensionAbiV4 == 4u);
    static_assert(sizeof(lux::cxx::AbiFingerprint) == 32u);
    static_assert(
        lux::extensions::kEngineExtensionAbiFingerprint ==
        lux::cxx::AbiBuildInfo::fingerprint());
    static_assert(std::is_standard_layout_v<ExtensionDependencyView>);
    static_assert(std::is_standard_layout_v<ExtensionModuleDescriptorV4>);
    static_assert(std::is_standard_layout_v<ExtensionRegistrationResult>);
    static_assert(
        static_cast<std::uint8_t>(EExtensionModuleTarget::RUNTIME) == 0u);
    static_assert(
        static_cast<std::uint8_t>(EExtensionModuleTarget::EDITOR) == 1u);
    static_assert(
        static_cast<std::uint8_t>(EExtensionRegistrationError::NONE) == 0u);
    static_assert(
        static_cast<std::uint8_t>(
            EExtensionRegistrationError::INTERNAL_FAILURE) == 7u);
    static_assert(offsetof(ExtensionDependencyView, id) == 0u);
    static_assert(
        offsetof(ExtensionDependencyView, required_major) ==
        sizeof(lux::cxx::AbiStringView));
    static_assert(
        offsetof(ExtensionDependencyView, minimum_minor) ==
        sizeof(lux::cxx::AbiStringView) + sizeof(std::uint16_t));
    static_assert(offsetof(ExtensionModuleDescriptorV4, struct_size) == 0u);
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, extension_abi) ==
        sizeof(std::uint32_t));
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, engine_abi_fingerprint) ==
        sizeof(std::uint32_t) * 2u);
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, id) ==
        offsetof(ExtensionModuleDescriptorV4, engine_abi_fingerprint) +
            sizeof(lux::cxx::AbiFingerprint));
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, version) ==
        offsetof(ExtensionModuleDescriptorV4, id) +
            sizeof(lux::cxx::AbiStringView));
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, target) >=
        offsetof(ExtensionModuleDescriptorV4, version) +
            sizeof(ExtensionVersion));
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, dependencies) >=
        offsetof(ExtensionModuleDescriptorV4, target) +
            sizeof(EExtensionModuleTarget));
    static_assert(
        offsetof(ExtensionModuleDescriptorV4, dependency_count) >=
        offsetof(ExtensionModuleDescriptorV4, dependencies) +
            sizeof(const ExtensionDependencyView*));
#if defined(_WIN64)
    static_assert(sizeof(ExtensionDependencyView) == 24u);
    static_assert(alignof(ExtensionDependencyView) == 8u);
    static_assert(sizeof(ExtensionModuleDescriptorV4) == 80u);
    static_assert(alignof(ExtensionModuleDescriptorV4) == 8u);
    static_assert(sizeof(ExtensionRegistrationResult) == 1u);
    static_assert(alignof(ExtensionRegistrationResult) == 1u);
    static_assert(offsetof(ExtensionModuleDescriptorV4, target) == 62u);
    static_assert(offsetof(ExtensionModuleDescriptorV4, dependencies) == 64u);
    static_assert(offsetof(ExtensionModuleDescriptorV4, dependency_count) == 72u);
#endif

    int failures = 0;
    const auto check = [&failures](bool condition, const char* message)
    {
        if (condition)
            std::printf("[ ok ] %s\n", message);
        else
        {
            std::printf("[FAIL] %s\n", message);
            ++failures;
        }
    };

    check(
        std::string_view{kGetExtensionModuleV4Symbol} ==
            "luxGetExtensionModuleV4",
        "v4 module symbol remains compatible");
    check(
        std::string_view{kRegisterRuntimeContributionsV4Symbol} ==
            "luxRegisterRuntimeContributionsV4",
        "v4 runtime registration symbol remains compatible");
    check(
        std::string_view{kRegisterEditorContributionsV4Symbol} ==
            "luxRegisterEditorContributionsV4",
        "v4 editor registration symbol remains compatible");
    check(argc == 3, "test module paths supplied");
    if (argc != 3)
        return 1;

    const ExtensionModuleRequirement requirement =
        ExtensionModuleRequirement::fromPath(
        ExtensionId{"org.lux.test.minimal"},
        std::filesystem::path{argv[1]},
        EExtensionModuleTarget::RUNTIME,
        1u,
        1u);

    auto prepared = ExtensionModuleManager::prepare(requirement);
    check(prepared.has_value(), "load and validate ABI V4 extension DLL");
    if (!prepared)
        return 1;

    const auto image = readImage(argv[1]);
    check(!image.empty(), "retain an owning module image");
    const auto memory_requirement =
        ExtensionModuleRequirement::fromMemory(
            ExtensionId{"org.lux.test.minimal"},
            image,
            "lux_minimal_extension",
            EExtensionModuleTarget::RUNTIME,
            1u,
            1u);
    auto memory_prepared = ExtensionModuleManager::prepare(
        memory_requirement);
    check(memory_prepared.has_value(),
          "load and validate ABI V4 extension from owning memory image");
    if (memory_prepared)
    {
        ExtensionModuleManager memory_manager;
        std::vector<PreparedExtensionModule> memory_batch;
        memory_batch.push_back(std::move(*memory_prepared));
        auto memory_committed = memory_manager.commitBatch(
            std::move(memory_batch));
        check(memory_committed.has_value(),
              "commit memory-backed extension normally");
        const auto memory_snapshot = memory_manager.snapshot();
        check(memory_snapshot.size() == 1u &&
                  memory_snapshot.front().origin.kind ==
                      EExtensionModuleSource::MEMORY_IMAGE &&
                  memory_snapshot.front().origin.path.empty() &&
                  memory_snapshot.front().origin.hint ==
                      "lux_minimal_extension" &&
                  memory_snapshot.front().origin.image_bytes == image.size(),
              "memory origin is diagnostic data, not a fake path");
        const auto memory_id = extensionId("org.lux.test.minimal");
        check(memory_manager.beginRegistration(memory_id) &&
                  memory_manager.markReady(memory_id),
              "memory-backed module follows the same state machine");
        if (memory_committed)
            memory_committed->clear();
        check(memory_manager.close().has_value(),
              "memory-backed module unloads after leases are released");
    }

    ExtensionModuleManager manager;
    std::vector<PreparedExtensionModule> batch;
    batch.push_back(std::move(*prepared));
    auto committed = manager.commitBatch(std::move(batch));
    check(committed.has_value() && committed->size() == 1u,
          "commit prepared module atomically");

    const auto id = extensionId("org.lux.test.minimal");
    check(static_cast<bool>(manager.find(id)), "lookup uses hash and full name");

    const ExtensionModuleRequirement dependent_requirement =
        ExtensionModuleRequirement::fromPath(
        ExtensionId{"org.lux.test.dependent"},
        std::filesystem::path{argv[2]},
        EExtensionModuleTarget::RUNTIME,
        1u,
        0u);
    auto dependent_before_ready =
        ExtensionModuleManager::prepare(dependent_requirement);
    check(dependent_before_ready.has_value(),
          "prepare dependency consumer before provider is READY");
    if (dependent_before_ready)
    {
        std::vector<PreparedExtensionModule> dependent_batch;
        dependent_batch.push_back(std::move(*dependent_before_ready));
        auto rejected = manager.commitBatch(std::move(dependent_batch));
        check(!rejected &&
                  rejected.error().code ==
                      EExtensionModuleCommitError::DEPENDENCY_NOT_READY,
              "batch rejects a present but non-READY external dependency");
    }

    check(manager.beginRegistration(id), "module enters REGISTERING once");
    check(manager.markReady(id), "registration publishes READY");

    auto dependent_ready = ExtensionModuleManager::prepare(
        dependent_requirement);
    check(dependent_ready.has_value(),
          "prepare dependency consumer after provider is READY");
    std::vector<ModuleLease> dependent_committed;
    if (dependent_ready)
    {
        std::vector<PreparedExtensionModule> dependent_batch;
        dependent_batch.push_back(std::move(*dependent_ready));
        auto accepted = manager.commitBatch(std::move(dependent_batch));
        check(accepted.has_value() && accepted->size() == 1u,
              "batch accepts a READY external dependency");
        if (accepted)
            dependent_committed = std::move(*accepted);
    }

    const auto dependent_id = extensionId("org.lux.test.dependent");
    check(manager.beginRegistration(dependent_id),
          "dependent module enters REGISTERING");
    check(manager.markReady(dependent_id),
          "dependent module publishes READY");
    const auto state = manager.snapshot();
    check(state.size() == 2u &&
              state[0].state == EExtensionModuleState::READY &&
              state[1].state == EExtensionModuleState::READY,
          "snapshot exposes both committed module states");

    auto held = manager.find(id);
    auto close_in_use = manager.close();
    check(!close_in_use &&
              close_in_use.error() ==
                  EExtensionModuleCloseError::MODULE_IN_USE,
          "manager refuses close while an external module lease exists");
    held.reset();
    committed->clear();
    dependent_committed.clear();
    check(manager.close().has_value(),
          "manager unloads only after every external lease is released");

    std::printf(
        failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n",
        failures);
    return failures == 0 ? 0 : 1;
}
