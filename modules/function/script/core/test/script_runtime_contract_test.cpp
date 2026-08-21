#include <lux/engine/function/script/ScriptRuntime.hpp>

#include <cassert>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    struct InvokeGate
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool release = false;
    };

    class FakeFunction final : public lux::script::ScriptFunction
    {
    public:
        explicit FakeFunction(
            std::string name,
            std::shared_ptr<InvokeGate> gate = {}
        )
            : gate_(std::move(gate))
        {
            signature_.name = std::move(name);
        }

        const lux::script::FunctionSignature& signature() const noexcept override
        {
            return signature_;
        }

        lux::script::ScriptResult<void> invoke(
            lux::script::CallFrame& frame
        ) const override
        {
            if (!frame.raw())
            {
                return lux::cxx::unexpected(lux::script::scriptFailure(
                    lux::script::EScriptError::INVOKE_FAILED,
                    "fake frame is empty"
                ));
            }
            if (gate_)
            {
                std::unique_lock lock(gate_->mutex);
                gate_->entered = true;
                gate_->condition.notify_all();
                gate_->condition.wait(lock, [this] { return gate_->release; });
            }
            ++invoke_count_;
            return {};
        }

        [[nodiscard]] int invokeCount() const noexcept { return invoke_count_; }

    private:
        lux::script::FunctionSignature signature_;
        std::shared_ptr<InvokeGate> gate_;
        mutable int invoke_count_ = 0;
    };

    class FakeModule final : public lux::script::IScriptModule
    {
    public:
        explicit FakeModule(
            std::string name,
            std::shared_ptr<InvokeGate> gate = {}
        )
            : name_(std::move(name)), main_("main", std::move(gate))
        {
        }

        std::string_view name() const noexcept override { return name_; }

        const lux::script::ScriptFunction* findFunction(
            std::string_view name
        ) const override
        {
            return name == "main" ? &main_ : nullptr;
        }

    private:
        std::string  name_;
        FakeFunction main_;
    };

    class FakeBackend final : public lux::script::IScriptBackend
    {
    public:
        FakeBackend(
            std::string id,
            std::vector<std::string> extensions,
            std::shared_ptr<InvokeGate> gate = {}
        )
            : id_(std::move(id))
            , extensions_(std::move(extensions))
            , gate_(std::move(gate))
        {
        }

        std::string_view backendId() const noexcept override { return id_; }
        std::vector<std::string> handledExtensions() const override
        {
            return extensions_;
        }

        lux::script::ScriptResult<lux::script::ScriptModulePtr> loadModule(
            const std::filesystem::path& path
        ) override
        {
            if (path.stem() == "bad")
            {
                return lux::cxx::unexpected(lux::script::scriptFailure(
                    lux::script::EScriptError::LOAD_FAILED,
                    "fake load failure"
                ));
            }
            return lux::script::ScriptModulePtr(
                std::make_unique<FakeModule>(path.stem().string(), gate_)
            );
        }

        lux::script::ScriptResult<lux::script::ScriptModulePtr> loadFromMemory(
            std::span<const std::byte>,
            std::string_view module_name
        ) override
        {
            return lux::script::ScriptModulePtr(
                std::make_unique<FakeModule>(std::string(module_name), gate_)
            );
        }

    private:
        std::string id_;
        std::vector<std::string> extensions_;
        std::shared_ptr<InvokeGate> gate_;
    };
}

int main()
{
    using lux::script::EScriptError;

    lux::script::ScriptRuntime runtime;
    auto null_backend = runtime.registerBackend({});
    assert(!null_backend);
    assert(null_backend.error().code == EScriptError::INVALID_ARGUMENT);

    auto registered = runtime.registerBackend(
        std::make_unique<FakeBackend>("fake", std::vector<std::string>{"fake"})
    );
    assert(registered);

    auto duplicate_id = runtime.registerBackend(
        std::make_unique<FakeBackend>("fake", std::vector<std::string>{"other"})
    );
    assert(!duplicate_id);
    assert(duplicate_id.error().code == EScriptError::DUPLICATE_BACKEND);

    auto duplicate_extension = runtime.registerBackend(
        std::make_unique<FakeBackend>("other", std::vector<std::string>{".FAKE"})
    );
    assert(!duplicate_extension);
    assert(duplicate_extension.error().code == EScriptError::DUPLICATE_EXTENSION);

    auto unsupported = runtime.loadModule("module.unknown");
    assert(!unsupported);
    assert(unsupported.error().code == EScriptError::UNSUPPORTED_EXTENSION);

    auto failed = runtime.loadModule("bad.fake");
    assert(!failed);
    assert(failed.error().code == EScriptError::LOAD_FAILED);
    assert(failed.error().detail == "fake load failure");

    auto loaded = runtime.loadModule("module.fake");
    assert(loaded);

    auto missing = runtime.findFunction(loaded.value(), "missing");
    assert(!missing);
    assert(missing.error().code == EScriptError::FUNCTION_NOT_FOUND);

    auto function = runtime.findFunction(loaded.value(), "main");
    assert(function);
    assert(function.value().module() == loaded.value());
    assert(function.value().name() == "main");

    auto signature = runtime.functionSignature(function.value());
    assert(signature);
    assert(signature.value().name == "main");

    lux_script_call_frame raw{};
    lux::script::CallFrame frame(&raw);
    assert(runtime.invoke(function.value(), frame));

    auto duplicate_module = runtime.loadModule("module.fake");
    assert(!duplicate_module);
    assert(duplicate_module.error().code == EScriptError::DUPLICATE_MODULE_NAME);

    assert(runtime.unloadModule(loaded.value()));
    auto stale = runtime.invoke(function.value(), frame);
    assert(!stale);
    assert(stale.error().code == EScriptError::STALE_HANDLE);

    auto unloaded_twice = runtime.unloadModule(loaded.value());
    assert(!unloaded_twice);
    assert(unloaded_twice.error().code == EScriptError::MODULE_NOT_FOUND);

    const std::byte payload[]{std::byte{0x01}};
    auto memory = runtime.loadModuleFromMemory("fake", payload, "memory");
    assert(memory);
    assert(runtime.unloadModule(memory.value()));

    auto empty_memory = runtime.loadModuleFromMemory("fake", {}, "empty");
    assert(!empty_memory);
    assert(empty_memory.error().code == EScriptError::INVALID_ARGUMENT);

    auto gate = std::make_shared<InvokeGate>();
    lux::script::ScriptRuntime concurrent_runtime;
    assert(concurrent_runtime.registerBackend(
        std::make_unique<FakeBackend>(
            "blocking",
            std::vector<std::string>{"block"},
            gate
        )
    ));
    auto blocking_module = concurrent_runtime.loadModule("blocking.block");
    assert(blocking_module);
    auto blocking_function = concurrent_runtime.findFunction(
        blocking_module.value(),
        "main"
    );
    assert(blocking_function);

    bool invocation_succeeded = false;
    std::thread invocation(
        [&]
        {
            invocation_succeeded = static_cast<bool>(
                concurrent_runtime.invoke(blocking_function.value(), frame)
            );
        }
    );
    {
        std::unique_lock lock(gate->mutex);
        gate->condition.wait(lock, [&] { return gate->entered; });
    }
    assert(concurrent_runtime.unloadModule(blocking_module.value()));
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->condition.notify_all();
    invocation.join();
    assert(invocation_succeeded);
    auto concurrent_stale = concurrent_runtime.invoke(
        blocking_function.value(),
        frame
    );
    assert(!concurrent_stale);
    assert(concurrent_stale.error().code == EScriptError::STALE_HANDLE);

    return 0;
}
