#include <lux/engine/function/script/ScriptHost.hpp>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::script
{
    namespace
    {
        thread_local std::string g_last_error;

        std::string toLowerExt(const std::filesystem::path& path)
        {
            auto ext = path.extension().string();
            if (!ext.empty() && ext.front() == '.')
                ext.erase(ext.begin());
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    }

    class ScriptHostImpl
    {
    public:
        struct LoadedModule
        {
            ModuleHandle    handle;
            std::string     backend_id;
            ScriptModulePtr module;
        };

        std::vector<std::unique_ptr<IScriptBackend>>    backends;
        std::unordered_map<std::string, IScriptBackend*> backend_by_ext;

        std::unordered_map<ModuleHandle, LoadedModule>  modules;
        std::unordered_map<std::string, ModuleHandle>   modules_by_name;

        ModuleHandle next_handle = 1;
        mutable std::mutex mtx;
    };

    ScriptHost::ScriptHost() : impl_(std::make_unique<ScriptHostImpl>()) {}
    ScriptHost::~ScriptHost() = default;

    void ScriptHost::registerBackend(std::unique_ptr<IScriptBackend> backend)
    {
        if (!backend) return;
        std::lock_guard lock(impl_->mtx);
        for (const auto& ext : backend->handledExtensions())
        {
            std::string lower(ext);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            impl_->backend_by_ext[lower] = backend.get();
        }
        impl_->backends.push_back(std::move(backend));
    }

    IScriptBackend* ScriptHost::findBackend(std::string_view id) const
    {
        std::lock_guard lock(impl_->mtx);
        for (auto& b : impl_->backends)
            if (b->backendId() == id)
                return b.get();
        return nullptr;
    }

    ModuleHandle ScriptHost::loadModule(const std::filesystem::path& path)
    {
        IScriptBackend* backend = nullptr;
        {
            std::lock_guard lock(impl_->mtx);
            const auto ext = toLowerExt(path);
            auto it = impl_->backend_by_ext.find(ext);
            if (it == impl_->backend_by_ext.end())
            {
                g_last_error = "no script backend for extension '" + ext + "'";
                return kInvalidModule;
            }
            backend = it->second;
        }
        return loadModule(backend->backendId(), path);
    }

    namespace
    {
        ModuleHandle insertLoadedModule(ScriptHostImpl& impl,
                                        std::string_view backend_id,
                                        ScriptModulePtr mod)
        {
            if (!mod) return kInvalidModule;
            std::lock_guard lock(impl.mtx);
            const ModuleHandle handle = impl.next_handle++;
            std::string mod_name(mod->name());
            ScriptHostImpl::LoadedModule entry{handle, std::string(backend_id), std::move(mod)};
            impl.modules.emplace(handle, std::move(entry));
            if (!mod_name.empty())
                impl.modules_by_name[mod_name] = handle;
            return handle;
        }
    }

    ModuleHandle ScriptHost::loadModule(std::string_view backend_id,
                                        const std::filesystem::path& path)
    {
        IScriptBackend* backend = findBackend(backend_id);
        if (!backend)
        {
            g_last_error = "unknown script backend id";
            return kInvalidModule;
        }

        ScriptModulePtr mod = backend->loadModule(path);
        if (!mod)
        {
            if (g_last_error.empty())
                g_last_error = "backend loadModule failed";
            return kInvalidModule;
        }
        return insertLoadedModule(*impl_, backend_id, std::move(mod));
    }

    ModuleHandle ScriptHost::loadModuleFromMemory(std::string_view           backend_id,
                                                  std::span<const std::byte> payload,
                                                  std::string_view           module_name)
    {
        IScriptBackend* backend = findBackend(backend_id);
        if (!backend)
        {
            g_last_error = "unknown script backend id";
            return kInvalidModule;
        }
        ScriptModulePtr mod = backend->loadFromMemory(payload, module_name);
        if (!mod)
        {
            if (g_last_error.empty())
                g_last_error = "backend loadFromMemory failed or unsupported";
            return kInvalidModule;
        }
        return insertLoadedModule(*impl_, backend_id, std::move(mod));
    }

    void ScriptHost::unloadModule(ModuleHandle handle)
    {
        if (handle == kInvalidModule) return;
        std::lock_guard lock(impl_->mtx);
        auto it = impl_->modules.find(handle);
        if (it == impl_->modules.end()) return;
        const std::string mod_name(it->second.module ? it->second.module->name() : std::string_view{});
        impl_->modules.erase(it);
        if (!mod_name.empty())
        {
            auto nit = impl_->modules_by_name.find(mod_name);
            if (nit != impl_->modules_by_name.end() && nit->second == handle)
                impl_->modules_by_name.erase(nit);
        }
    }

    const IScriptModule* ScriptHost::module(ModuleHandle handle) const
    {
        std::lock_guard lock(impl_->mtx);
        auto it = impl_->modules.find(handle);
        return it == impl_->modules.end() ? nullptr : it->second.module.get();
    }

    ScriptFunctionHandle ScriptHost::findFunction(ModuleHandle handle,
                                                  std::string_view fn_name) const
    {
        const IScriptModule* mod = module(handle);
        if (!mod) return {};
        const ScriptFunction* fn = mod->findFunction(fn_name);
        if (!fn) return {};
        return ScriptFunctionHandle{handle, fn};
    }

    ScriptFunctionHandle ScriptHost::findFunction(std::string_view module_name,
                                                  std::string_view fn_name) const
    {
        ModuleHandle handle = kInvalidModule;
        {
            std::lock_guard lock(impl_->mtx);
            auto it = impl_->modules_by_name.find(std::string(module_name));
            if (it != impl_->modules_by_name.end()) handle = it->second;
        }
        if (handle == kInvalidModule) return {};
        return findFunction(handle, fn_name);
    }

    bool ScriptHost::invoke(const ScriptFunctionHandle& fn, CallFrame& frame) const
    {
        if (!fn) { g_last_error = "invalid script function handle"; return false; }
        return fn.fn->invoke(frame);
    }

    std::string ScriptHost::lastError() const
    {
        return g_last_error;
    }
}
