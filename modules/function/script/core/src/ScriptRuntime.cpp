#include <lux/engine/function/script/ScriptRuntime.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::script
{
    namespace
    {
        [[nodiscard]] std::string normalizeExtension(std::string_view extension)
        {
            if (!extension.empty() && extension.front() == '.')
                extension.remove_prefix(1);

            std::string normalized(extension);
            std::transform(
                normalized.begin(),
                normalized.end(),
                normalized.begin(),
                [](unsigned char value)
                {
                    return static_cast<char>(std::tolower(value));
                }
            );
            return normalized;
        }

        [[nodiscard]] std::string extensionOf(const std::filesystem::path& path)
        {
            return normalizeExtension(path.extension().string());
        }
    }

    class ScriptRuntimeState
    {
    public:
        struct LoadedModule
        {
            std::shared_ptr<IScriptModule> module;
            mutable std::mutex             invoke_mutex;
        };

        std::unordered_map<std::string, std::unique_ptr<IScriptBackend>> backends;
        std::unordered_map<std::string, IScriptBackend*> backend_by_extension;
        std::unordered_map<ModuleHandle, std::shared_ptr<LoadedModule>> modules;
        std::unordered_map<std::string, ModuleHandle> modules_by_name;
        ModuleHandle next_handle = 1;
        mutable std::mutex mutex;
    };

    namespace
    {
        [[nodiscard]] ScriptResult<IScriptBackend*> findBackend(
            ScriptRuntimeState& state,
            std::string_view backend_id
        )
        {
            std::lock_guard lock(state.mutex);
            const auto it = state.backends.find(std::string(backend_id));
            if (it == state.backends.end())
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::BACKEND_NOT_FOUND,
                    "unknown script backend '" + std::string(backend_id) + "'"
                ));
            }
            return it->second.get();
        }

        [[nodiscard]] ScriptResult<ModuleHandle> insertLoadedModule(
            ScriptRuntimeState& state,
            ScriptModulePtr module
        )
        {
            if (!module)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "script backend returned a null module"
                ));
            }

            std::lock_guard lock(state.mutex);
            const std::string module_name(module->name());
            if (!module_name.empty() && state.modules_by_name.contains(module_name))
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::DUPLICATE_MODULE_NAME,
                    "script module name is already loaded: '" + module_name + "'"
                ));
            }
            if (state.next_handle == 0
                || state.next_handle == std::numeric_limits<ModuleHandle>::max())
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::HANDLE_EXHAUSTED,
                    "script module handle space is exhausted"
                ));
            }

            const ModuleHandle handle = state.next_handle++;
            auto loaded = std::make_shared<ScriptRuntimeState::LoadedModule>();
            loaded->module = std::shared_ptr<IScriptModule>(std::move(module));
            state.modules.emplace(handle, std::move(loaded));
            if (!module_name.empty()) state.modules_by_name.emplace(module_name, handle);
            return handle;
        }

        [[nodiscard]] ScriptResult<std::shared_ptr<ScriptRuntimeState::LoadedModule>>
        loadedModule(
            const ScriptRuntimeState& state,
            ModuleHandle handle,
            EScriptError missing_error
        )
        {
            if (handle == 0)
            {
                return lux::cxx::unexpected(scriptFailure(
                    missing_error,
                    "script module handle is empty"
                ));
            }

            std::lock_guard lock(state.mutex);
            const auto it = state.modules.find(handle);
            if (it == state.modules.end())
            {
                return lux::cxx::unexpected(scriptFailure(
                    missing_error,
                    "script module is not loaded"
                ));
            }
            return it->second;
        }
    }

    ScriptRuntime::ScriptRuntime()
        : state_(std::make_unique<ScriptRuntimeState>())
    {
    }

    ScriptRuntime::~ScriptRuntime() = default;

    ScriptResult<void> ScriptRuntime::registerBackend(
        std::unique_ptr<IScriptBackend> backend
    )
    {
        if (!backend)
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::INVALID_ARGUMENT,
                "cannot register a null script backend"
            ));
        }

        const std::string backend_id(backend->backendId());
        if (backend_id.empty())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::INVALID_ARGUMENT,
                "script backend id cannot be empty"
            ));
        }

        std::vector<std::string> extensions;
        std::unordered_set<std::string> unique_extensions;
        for (const auto& extension : backend->handledExtensions())
        {
            std::string normalized = normalizeExtension(extension);
            if (normalized.empty())
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_ARGUMENT,
                    "script backend extension cannot be empty"
                ));
            }
            if (!unique_extensions.emplace(normalized).second)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::DUPLICATE_EXTENSION,
                    "script backend repeats extension '" + normalized + "'"
                ));
            }
            extensions.emplace_back(std::move(normalized));
        }

        std::lock_guard lock(state_->mutex);
        if (state_->backends.contains(backend_id))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::DUPLICATE_BACKEND,
                "script backend id is already registered: '" + backend_id + "'"
            ));
        }
        for (const auto& extension : extensions)
        {
            if (state_->backend_by_extension.contains(extension))
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::DUPLICATE_EXTENSION,
                    "script extension is already owned: '" + extension + "'"
                ));
            }
        }

        IScriptBackend* backend_ptr = backend.get();
        state_->backends.emplace(backend_id, std::move(backend));
        for (const auto& extension : extensions)
            state_->backend_by_extension.emplace(extension, backend_ptr);
        return {};
    }

    ScriptResult<ModuleHandle> ScriptRuntime::loadModule(
        const std::filesystem::path& path
    )
    {
        const std::string extension = extensionOf(path);
        if (extension.empty())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::UNSUPPORTED_EXTENSION,
                "script module path has no extension"
            ));
        }

        std::string backend_id;
        {
            std::lock_guard lock(state_->mutex);
            const auto it = state_->backend_by_extension.find(extension);
            if (it == state_->backend_by_extension.end())
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::UNSUPPORTED_EXTENSION,
                    "no script backend owns extension '" + extension + "'"
                ));
            }
            backend_id = std::string(it->second->backendId());
        }
        return loadModule(backend_id, path);
    }

    ScriptResult<ModuleHandle> ScriptRuntime::loadModule(
        std::string_view backend_id,
        const std::filesystem::path& path
    )
    {
        auto backend = findBackend(*state_, backend_id);
        if (!backend) return lux::cxx::unexpected(std::move(backend.error()));

        auto module = backend.value()->loadModule(path);
        if (!module) return lux::cxx::unexpected(std::move(module.error()));
        return insertLoadedModule(*state_, std::move(module.value()));
    }

    ScriptResult<ModuleHandle> ScriptRuntime::loadModuleFromMemory(
        std::string_view backend_id,
        std::span<const std::byte> payload,
        std::string_view module_name
    )
    {
        if (payload.empty())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::INVALID_ARGUMENT,
                "script module memory payload cannot be empty"
            ));
        }

        auto backend = findBackend(*state_, backend_id);
        if (!backend) return lux::cxx::unexpected(std::move(backend.error()));

        auto module = backend.value()->loadFromMemory(payload, module_name);
        if (!module) return lux::cxx::unexpected(std::move(module.error()));
        return insertLoadedModule(*state_, std::move(module.value()));
    }

    ScriptResult<void> ScriptRuntime::unloadModule(ModuleHandle handle)
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->modules.find(handle);
        if (it == state_->modules.end())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::MODULE_NOT_FOUND,
                "cannot unload a script module that is not loaded"
            ));
        }

        const std::string module_name(it->second->module->name());
        state_->modules.erase(it);
        if (!module_name.empty())
        {
            const auto name_it = state_->modules_by_name.find(module_name);
            if (name_it != state_->modules_by_name.end()
                && name_it->second == handle)
            {
                state_->modules_by_name.erase(name_it);
            }
        }
        return {};
    }

    ScriptResult<ScriptFunctionHandle> ScriptRuntime::findFunction(
        ModuleHandle handle,
        std::string_view function_name
    ) const
    {
        if (function_name.empty())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::INVALID_ARGUMENT,
                "script function name cannot be empty"
            ));
        }

        auto loaded = loadedModule(*state_, handle, EScriptError::MODULE_NOT_FOUND);
        if (!loaded) return lux::cxx::unexpected(std::move(loaded.error()));

        std::lock_guard lock(loaded.value()->invoke_mutex);
        if (!loaded.value()->module->findFunction(function_name))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::FUNCTION_NOT_FOUND,
                "script function is not exported: '" + std::string(function_name) + "'"
            ));
        }
        return ScriptFunctionHandle(handle, std::string(function_name));
    }

    ScriptResult<ScriptFunctionHandle> ScriptRuntime::findFunction(
        std::string_view module_name,
        std::string_view function_name
    ) const
    {
        ModuleHandle handle = 0;
        {
            std::lock_guard lock(state_->mutex);
            const auto it = state_->modules_by_name.find(std::string(module_name));
            if (it == state_->modules_by_name.end())
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::MODULE_NOT_FOUND,
                    "script module name is not loaded: '" + std::string(module_name) + "'"
                ));
            }
            handle = it->second;
        }
        return findFunction(handle, function_name);
    }

    ScriptResult<FunctionSignature> ScriptRuntime::functionSignature(
        const ScriptFunctionHandle& function
    ) const
    {
        auto loaded = loadedModule(*state_, function.module_, EScriptError::STALE_HANDLE);
        if (!loaded) return lux::cxx::unexpected(std::move(loaded.error()));

        std::lock_guard lock(loaded.value()->invoke_mutex);
        const ScriptFunction* resolved =
            loaded.value()->module->findFunction(function.function_name_);
        if (!resolved)
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::STALE_HANDLE,
                "script function handle no longer resolves"
            ));
        }
        return resolved->signature();
    }

    ScriptResult<void> ScriptRuntime::invoke(
        const ScriptFunctionHandle& function,
        CallFrame& frame
    ) const
    {
        auto loaded = loadedModule(*state_, function.module_, EScriptError::STALE_HANDLE);
        if (!loaded) return lux::cxx::unexpected(std::move(loaded.error()));

        std::lock_guard lock(loaded.value()->invoke_mutex);
        const ScriptFunction* resolved =
            loaded.value()->module->findFunction(function.function_name_);
        if (!resolved)
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::STALE_HANDLE,
                "script function handle no longer resolves"
            ));
        }
        return resolved->invoke(frame);
    }
}
