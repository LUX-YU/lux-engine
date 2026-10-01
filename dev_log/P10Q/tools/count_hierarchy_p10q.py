"""Count actual parent reads in fixed Git sources; instrumentation is test-only, not SDK qualification."""
from pathlib import Path
import subprocess, os, json, hashlib, difflib, sys

repo=Path(__file__).resolve().parents[2];work=repo/'.internal/editor-redesign';cluster=repo.parent
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
for before in [True,False]:
    source=cluster/'build'/('p05-r1-clean-f2b5a00c60e9' if before else 'p10q-clean-'+sha[:12])
    measured_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
    prefix=cluster/'install'/('RelWithDebInfo' if before else 'P10Q-'+sha[:12])
    out=work/('P10Q-before/query-counts' if before else 'P10Q-final/'+sha+'/query-counts')
    out.mkdir(parents=True,exist_ok=True)
    build=cluster/'build/RelWithDebInfo'/('p10q-parent-counts-'+('before' if before else sha[:12]))
    relative='editor/tools/scene/model/src/SceneSource.cpp'
    original=subprocess.check_output(['git','show',measured_sha+':'+relative],cwd=source).decode()
    assert (source/relative).read_text()==original
    begin=original.index('SceneEditResult<void> SceneSourceAccess::validate(')
    end=original.index('SceneEditResult<SceneSource> SceneSourceAccess::build(',begin)
    body=original[begin:end]
    marker='const auto* parent = source.registry.try_get<ecs::Parent>'
    assert body.count(marker)==1
    body=body.replace(marker,'++p10q_parent_reads; '+marker)
    instrumented='\n#include <cstdint>\nstd::uint64_t p10q_parent_reads{};\n'+original[:begin]+body+original[end:]
    (out/'SceneSource-counted.cpp').write_text(instrumented)
    (out/'instrumentation.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True),instrumented.splitlines(True),fromfile=relative,tofile='SceneSource-counted.cpp')))
    driver=(work/'P10Q-bench/hierarchy_alloc.cpp').read_text()
    driver=driver.replace('int main(int argc, char** argv)','extern std::uint64_t p10q_parent_reads;\nint main(int argc, char** argv)')
    driver=driver.replace('    allocation_probe::begin();','    p10q_parent_reads=0;\n    allocation_probe::begin();')
    driver=driver.replace('    allocation_probe::enabled=false;','    allocation_probe::enabled=false;\n    std::printf("parent_reads n=%zu shape=%s count=%llu\\n",std::size_t(count),shape.c_str(),static_cast<unsigned long long>(p10q_parent_reads));')
    (out/'main.cpp').write_text(driver)
    (out/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(p10q_parent_reads LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)
add_executable(parent_reads main.cpp SceneSource-counted.cpp)
target_include_directories(parent_reads PRIVATE "${SOURCE_PRIVATE}" "${BENCH_INCLUDE}")
target_link_libraries(parent_reads PRIVATE lux::engine::editor::scene_model)
target_compile_options(parent_reads PRIVATE /UNDEBUG /utf-8 /permissive- /Zc:__cplusplus)
''')
    (out/'source.json').write_text(json.dumps({'implementation_sha':measured_sha,'source':relative,
        'original_sha256':hashlib.sha256(original.encode()).hexdigest(),
        'instrumented_sha256':hashlib.sha256(instrumented.encode()).hexdigest(),
        'scope':'One counter immediately before each actual Parent read in validate. No algorithm replacement; count-only, not timing or independent installed SDK qualification.'},indent=2)+'\n')
    records=[]
    env=dict(os.environ);env['PATH']=str(prefix/'bin')+os.pathsep+'D:/Development/vcpkg/installed/x64-windows/bin'+os.pathsep+env['PATH']
    def run(name,args):
        args=list(map(str,args));log=out/(name+'.log')
        assert not log.exists(),'Preserve prior count evidence: '+str(log)
        with log.open('wb') as stream:r=subprocess.run(args,cwd=repo,env=env,stdout=stream,stderr=subprocess.STDOUT)
        records.append({'name':name,'argv':args,'exit_code':r.returncode,'log':log.name,
            'implementation_sha':measured_sha,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
        (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        print(('before' if before else 'after'),name,r.returncode,flush=True)
        if r.returncode:raise SystemExit(r.returncode)
    run('configure',['cmake','-S',out,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
        '-DCMAKE_PREFIX_PATH='+str(prefix),'-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
        '-DVCPKG_TARGET_TRIPLET=x64-windows','-DSOURCE_PRIVATE='+str(source/Path(relative).parent),
        '-DBENCH_INCLUDE='+str(work/'P10Q-bench')])
    run('build',['cmake','--build',build,'--target','all','-j','4','--','-k','0'])
    for n in [1000,10000,50000]:
        for shape in ['chain','wide','roots','cycle']:
            run(f'parent-{n}-{shape}',[build/'parent_reads.exe',n,shape,0])
