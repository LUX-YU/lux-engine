from pathlib import Path
import json, subprocess, re
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
w=Path(__file__).resolve().parent
ledger=w.parent/'migration-ledger.json'
d=json.loads(ledger.read_text(encoding='utf-8'))
d['ec2']['batches']['R3']={'status':'IMPLEMENTED_DEVELOPMENT_VALIDATED','sha':subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip(),'evidence':['r3-normalized-target-build','r3-no-work','r3-native-regressions'],'note':'First test compile failures and canonical-path fault injection timeout retained. Final clean-SHA SDK/GPU qualification pending.'}
d['ec2']['batches']['R4']='IN_PROGRESS'; d['ec2']['status']='R4_IN_PROGRESS'
ledger.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
files=subprocess.check_output(['git','grep','-l','-E','MaterialCompileKey|MaterialPreviewStore','--',':!dev_log',':!docs'],cwd=s,text=True).splitlines()
for name in files:
 p=s/name; t=p.read_text(encoding='utf-8'); t=t.replace('MaterialCompileKey','MaterialCompileInputKey').replace('MaterialPreviewStore','MaterialPreview');p.write_text(t,encoding='utf-8')
base=s/'editor/activities/material'
for a,b in [('include/lux/engine/editor/material/MaterialPreviewStore.hpp','include/lux/engine/editor/material/MaterialPreview.hpp'),('src/MaterialPreviewStore.cpp','src/MaterialPreview.cpp')]:
 (base/a).rename(base/b)
# Encoded result associations are private; only the implementation-local compiler can assemble them.
for folder,header,name,key,source,artifact in [
 ('material','MaterialCompilation.hpp','Material','MaterialCompileInputKey','lux::material::MaterialSource','asset::MaterialAsset'),
 ('flow','FlowCompilationService.hpp','Flow','FlowCompileKey','lux::flowforge::FlowSource','lux::script::ScriptArtifactAsset')]:
 ns='material' if folder=='material' else 'flowforge'
 p=s/f'editor/activities/{folder}/include/lux/engine/editor/{ns}/{header}'
 t=p.read_text(); start=t.index(f'    struct Compiled{name} final');end=t.index('\n    };',start)+7
 t=t[:start]+f'''    namespace detail {{ struct {name}Compilation; }}
    class Compiled{name} final
    {{
    public:
        [[nodiscard]] const {key}& key() const noexcept {{ return key_; }}
        [[nodiscard]] const std::shared_ptr<const {source}>& source() const noexcept {{ return source_; }}
        [[nodiscard]] const std::shared_ptr<const {artifact}>& artifact() const noexcept {{ return artifact_; }}
        [[nodiscard]] const lux::cxx::SharedBytes<>& bytes() const noexcept {{ return bytes_; }}

    private:
        friend struct detail::{name}Compilation;
        Compiled{name}({key} key, std::shared_ptr<const {source}> source,
            std::shared_ptr<const {artifact}> artifact, lux::cxx::SharedBytes<> bytes)
            : key_(key), source_(std::move(source)), artifact_(std::move(artifact)), bytes_(std::move(bytes)) {{}}
        {key} key_;
        std::shared_ptr<const {source}> source_;
        std::shared_ptr<const {artifact}> artifact_;
        lux::cxx::SharedBytes<> bytes_;
    }};'''+t[end:]
 if name=='Material':
  a=t.index('    struct MaterialPreviewFailure'); b=t.index('    using VMaterialCompileFailure',a);t=t[:a]+t[b:]
  t=t.replace('#include <any>\n','').replace('process::EExecutionError,\n        MaterialPreviewFailure>','process::EExecutionError>')
  t=t.replace('environment{1}, target{1}','environment{1}').replace('std::uint64_t environment = 1,\n            std::uint64_t target = 1','std::uint64_t environment = 1')
 p.write_text(t)
 p=s/f'editor/activities/{folder}/src/{header[:-4]}.cpp'; t=p.read_text()
 # The factory is private to this source file; no public assembly from unrelated fields.
 at=t.index('\n    namespace\n')
 t=t[:at]+f'''
    struct detail::{name}Compilation final
    {{
        static std::shared_ptr<const Compiled{name}> finish({key} key,
            std::shared_ptr<const {source}> source, std::shared_ptr<const {artifact}> artifact,
            lux::cxx::SharedBytes<> bytes)
        {{
            return std::shared_ptr<const Compiled{name}>(
                new Compiled{name}(key, std::move(source), std::move(artifact), std::move(bytes))
            );
        }}
    }};
'''+t[at:]
 t=t.replace(f'std::make_shared<const Compiled{name}>(Compiled{name}{{',f'detail::{name}Compilation::finish(')
 # Exact end at SharedBytes final parameter.
 t=re.sub(r'(lux::cxx::SharedBytes<>::fromOwner\(bytes, \*bytes\))\n(\s*)\}\)',r'\1\n\2)',t)
 if name=='Material':
  t=t.replace('std::uint64_t environment,\n        std::uint64_t target','std::uint64_t environment')
  t=t.replace('settings, environment, target','settings, environment').replace('!key.target || !key.environment','!key.environment').replace('is_invalid_target','is_invalid_environment')
 p.write_text(t)
# Compilation control no longer knows a preview destination.
for name in ['include/lux/engine/editor/material/MaterialCompilationService.hpp','src/MaterialCompilationService.cpp']:
 p=base/name;t=p.read_text().replace('std::uint64_t environment = 1,\n            std::uint64_t target = 1','std::uint64_t environment = 1').replace('std::uint64_t environment,\n        std::uint64_t target','std::uint64_t environment').replace('settings, environment, target','settings, environment');p.write_text(t)
# Existing consumers of immutable compiler associations (only named compiled-result variables).
for name in subprocess.check_output(['git','ls-files','editor','cmake'],cwd=s,text=True).splitlines():
 p=s/name
 if not p.is_file() or p.suffix not in {'.cpp','.hpp'}: continue
 t=p.read_text(encoding='utf-8')
 old=t
 for var in ['compiled','compiled_','artifact']:
  # artifact is a CompiledFlow only in this integration file; avoid ordinary assets elsewhere.
  if var=='artifact' and name!='editor/tests/integration/material_activity/compilation.cpp': continue
  t=re.sub(rf'\b{var}->(key|source|artifact|bytes)\b(?!\()',rf'{var}->\1()',t)
 if t!=old:p.write_text(t,encoding='utf-8')
print('R4 result identities and private associations migrated')
