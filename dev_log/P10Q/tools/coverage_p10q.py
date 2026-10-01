"""Explicit XQ mapping. Run after qualifications; absent evidence is never PASS."""
from pathlib import Path
import json,subprocess
repo=Path(__file__).resolve().parents[2];work=repo/'.internal/editor-redesign'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip();final=work/'P10Q-final'/sha
mapping={
1:('P10 R1 来源保持', ['editor.draft_source_'], ['P10Q-behavior-map.md']),
2:('共享阶段，领域载荷和 gate 不变', ['editor.draft_source_','editor.material_interaction','editor.flowforge_interaction'], []),
3:('目录唯一 owner，多消费者共享', ['editor.project_views'], ['P10Q-performance.md']),
4:('目录失败／连接／FULL/CLOSED/resync', ['editor.project_views','object.queue'], []),
5:('双 TaskView、单 observer、稳定查询', ['editor.tasks.monitor'], ['P10Q-performance.md']),
6:('关闭视图不取消任务 owner', ['editor.tasks.monitor'], []),
7:('编译与发布准确结果', ['editor.compilation.actual_publish','editor.compilation.ownership_'], []),
8:('关闭重试分类', ['editor.view_host','editor.detached_views'], []),
9:('保存／产物／Workspace 真实发布', ['editor.persistence.','editor.workspace.'], []),
10:('共享 viewport 的真实双视口和 Material', ['editor.scene_views_gpu','editor.projection.highlight_backend','editor.quality.boundaries'], []),
11:('公共头、精确依赖正负例', ['editor.quality.boundaries','editor.scene_execution_boundaries','editor.interaction_view_boundaries'], ['P10Q-link-audit.md']),
12:('一次迁移、旧入口删除', ['editor.quality.boundaries'], ['P10Q-link-audit.md']),
13:('owner 特殊成员和 operation 八项负例', ['editor.compilation.ownership_material','editor.compilation.ownership_flow'], []),
14:('受控连续区间复用；未删除跨回调检查', ['editor.project_views','editor.tasks.monitor','editor.draft_source_'], ['P10Q-report.md','P10Q-performance.md']),
15:('回调修改／关闭／代码寿命', ['editor.material_model.reload-','editor.flowforge_model.reload-','editor.persistence.r1-','editor.interaction_reclaim.'], []),
16:('外部输入／真实文件冲突与容量', ['editor.persistence.conflict','editor.persistence.capacity','editor.workspace.reads','editor.workspace.aliases'], []),
17:('可靠完成／外层 dispatch', ['editor.persistence.r2-','editor.scene_execution.r1-','editor.compilation.ownership_'], []),
18:('线性深链、宽树、多根、环', ['editor.scene_model.hierarchy_','editor.scene_model.identity'], ['P10Q-performance.md']),
19:('跨域和 generation 资格', ['editor.quality.containers','editor.three_actual_sessions','editor.flowforge_model.ids-'], ['P10Q-performance.md']),
20:('StableSlotMap 回调／构造失败／稳定地址', ['editor.quality.containers'], ['P10Q-performance.md']),
21:('SmallVector SBO、move-only、allocator 清理', ['editor.quality.containers'], ['P10Q-performance.md']),
22:('真实画布 churn、首帧导航／旧 UI ID', ['editor.canvas.churn'], ['P10Q-performance.md']),
23:('SharedBytes 源释放／预算／取消／Unknown', ['editor.persistence.write_coordinator','editor.compilation.actual_publish'], ['P10Q-performance.md']),
24:('BQ1–BQ5 对照和未采用方案', [], ['P10Q-performance.md']),
25:('实际 target／链接／装载', [], ['P10Q-link-audit.md']),
26:('动态插件和 domain/GUI DLL', ['plugin.configuration','plugin.physics2d','editor.plugin_publication','editor.component_elements'], ['P10Q-link-audit.md']),
27:('新 prefix 安装闭包', [], ['P10Q-link-audit.md']),
28:('CPU native，不要求 GPU／linker 运行', [], []),
29:('Windows／Linux 实际工具链', ['editor.compilation.actual_publish'], []),
30:('路径、IO 与相对归档', ['editor.workspace.aliases','editor.workspace.reads','editor.persistence.real_files'], []),
31:('完全新 build tree 的生成顺序', [], ['P10Q-work.md']),
32:('测试分组与实机范围', ['editor.desktop_native_input','editor.scene_views_gpu'], []),
33:('原有行为、历史失败范围', [], ['P10Q-behavior-map.md']),
34:('正式 P10Q、证据缺失／篡改拒绝', ['editor.quality.evidence','editor.quality.boundaries'], [])}
tests=json.loads((final/'test-names.log').read_text())['tests'];names=[x['name'] for x in tests]
ctest=(final/'ctest.log').read_text();coverage=[]
dest={'P10Q-report.md':'README.md','P10Q-work.md':'development/P10Q-work.md',
      'P10Q-performance.md':'performance.md','P10Q-link-audit.md':'link-audit.md','P10Q-behavior-map.md':'behavior-map.md'}
extra={11:['logs/public-headers/commands.json'],
       12:['logs/extra/removed-symbols.log','logs/extra/migrated-consumers.log'],13:['logs/sdk/projection-compilation-ctest.log','logs/performance/owners.log'],
       18:['logs/query-counts/commands.json','before/query-counts/commands.json'],
       24:['logs/performance/commands.json','before/extended/commands.json'],
       25:['logs/extra/file-api-inventory.json','logs/imports.log','logs/extra/editor-loaded-modules.log'],
       26:['logs/sdk/editor-d2-ctest.log','logs/sdk/external-feature-ctest.log'],
       27:['logs/sdk/commands.json'],28:['logs/cpu-ctest.log','logs/cpu-test-names.log'],
       29:['logs/clang-ctest.log'],31:['logs/build.log','logs/no-work.log','logs/tracked-snapshot.log'],
       32:['logs/sdk/gpu-ui-ctest.log','logs/sdk/editor-scene-pane-ctest.log'],
       33:['logs/C01.log','logs/C03.log','logs/C04.log']}
for i,(title,prefixes,documents) in mapping.items():
    matching=[n for n in names if any(n.startswith(p) for p in prefixes)]
    evidence=['logs/ctest.log','logs/ctest-details.log'] if prefixes else []
    evidence += [dest[d] for d in documents]+extra.get(i,[])
    status='PASS';reason=title
    for p in prefixes:
        assert any(n.startswith(p) for n in matching),p
    if '100% tests passed' not in ctest and prefixes:status='PARTIAL'
    for item in evidence:
        path=final/item.removeprefix('logs/') if item.startswith('logs/') else work/item
        if item in dest.values():path=work/next(k for k,v in dest.items() if v==item)
        if item.startswith('before/'):path=work/'P10Q-before'/item.removeprefix('before/')
        if not path.exists():status='PARTIAL';reason+='; missing '+item
    if i in [29,30]:status='PARTIAL';reason+='; Linux mandatory NOT_RUN'
    if i==32:reason+='; system IME NOT_RUN (separate from automatic native input)'
    if i==24:
        complete=False
        for line in (work/'P10Q-before/bq1-50000-chain-02.log').read_text(errors='replace').splitlines():
            try:
                value=json.loads(line)
                complete = value.get('samples',0)>=100 and value.get('warmup',0)>=10
            except (ValueError,AttributeError):pass
        if not complete:status='PARTIAL';reason+='; user ended old 50k-chain before all 100 samples; completed raw iterations retained'
    coverage.append({'id':f'XQ{i:02}','status':status,'evidence':evidence,'tests':matching,'reason':reason})
(work/'P10Q-coverage.json').write_text(json.dumps(coverage,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('Generated concrete coverage mapping; review performance and SDK results before freezing')
