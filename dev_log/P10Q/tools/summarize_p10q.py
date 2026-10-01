"""Summarize recorded measurements, without changing raw evidence or inferring missing runs."""
from pathlib import Path
import json, subprocess, re

repo=Path(__file__).resolve().parents[2]; work=repo/'.internal/editor-redesign'
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
final=work/'P10Q-final'/sha
before=work/'P10Q-before'; after=final/'performance'

def lines(path):
    return path.read_text(errors='replace').splitlines() if path.exists() else []

def aggregates(path):
    return [x for x in lines(path) if any(k in x for k in ['p50_us','requested_bytes','allocation','shared_buffers',
        'distinct_buffers','copied_rows','buffer_changes','aliases','payload_bytes','identity','maps='])
        and not re.search(r'\bsample=\d',x)]

text=['# P10Q 性能记录','',f'最终实现：`{sha}`。基线实现为 P10 R1；原始提交和依赖见 before/baseline.json。',
      '', 'RelWithDebInfo、同机、固定依赖；微基准预热 10 次、测量 100 次。真实画布长期回归另记 10k 轮（10 次预热、9990 次计时）。全部分位数来自实际原始输出，不按比例推算。',
      '分配计数另跑计数段，范围为当前线程可执行文件／静态 archive 的 C++ new；不覆盖 DLL CRT、malloc、GPU。',
      '长基线进程与构建存在时间重叠，因此墙钟受负载影响；不据此承诺固定提速百分比。','',
      '## BQ1：全图层级验证','',
      '旧算法对每个对象重复走祖先链；新算法在一次身份／父索引后做迭代三色遍历。正常引用的访问次数由 N² 深链变为 O(N+E)，调用栈深度恒定。',
      '时间来自原 SDK；parent 读取计数单独使用固定 Git 原体重编的测试插桩，仅在实际读取前累加一次。插桩差异和原体哈希保留，不作为安装消费者资格。新索引使用 O(N) 额外内存；不声称分配减少。',
      '', '| 输入 | 修复前 p50／p95／p99／max（µs） | 修复后 p50／p95／p99／max（µs） |',
      '|---|---|---|']

def hierarchy(path):
    for line in lines(path):
        if line.startswith('{'):
            try:
                row=json.loads(line)
                if 'p50_us' in row:return ' / '.join(f"{row[k]:.3f}" for k in ['p50_us','p95_us','p99_us','max_us'])
            except ValueError: pass
    return 'NOT_RUN／未完成'

for n in [1000,10000,50000]:
    for shape in ['chain','wide','roots','cycle']:
        name=f'bq1-{n}-{shape}.log'
        old=before/'extended'/name
        if not old.exists():old=before/name
        if (n,shape)==(50000,'chain'):old=before/'bq1-50000-chain-02.log'
        old_result=hierarchy(old)
        if (n,shape)==(50000,'chain') and old_result=='NOT_RUN／未完成':
            completed=[]
            for line in lines(before/'bq1-50000-chain-samples.log'):
                found=re.fullmatch(r'sample=(\d+) warmup=0 duration_us=([\d.]+)',line.strip())
                if found:completed.append(float(found[2]))
            if completed:
                completed.sort();size=len(completed)
                measured=' / '.join(f'{completed[min(i,size-1)]:.3f}' for i in [size//2,size*95//100,size*99//100,size-1])
                old_result=f'PARTIAL {size}/100：{measured}'
        text.append(f'| {n} {shape} | {old_result} | {hierarchy(after/name)} |')

text+=['','旧版 50k 深链按用户明确指示结束，已有完整迭代的原始 stderr 保留；停止中的一轮不计入。未达到 100 个计时样本，因此性能项／XQ24 为 PARTIAL。上表若列部分分位数，只描述已完成样本，不能当作原计划的完整统计。']

text+=['','实际 parent 读取计数（单次计数段，不混入计时）：','']
for directory,title in [(before/'query-counts','修复前'),(final/'query-counts','修复后')]:
    text+=['### '+title,'','```text']
    records=[line for path in sorted(directory.glob('parent-*.log')) for line in lines(path) if line.startswith('parent_reads ')]
    text+=records or ['NOT_RUN／未完成']
    text+=['```','']
text+=['','分配计数（单次计数段，未混入计时样本）：','']
for directory,title in [(before/'extended','修复前'),(after,'修复后')]:
    text+=['### '+title,'','```text']
    for path in sorted(directory.glob('bq1-alloc*.log')):text+=aggregates(path)
    text+=['```','']

for heading,patterns,explanation in [
    ('BQ2：目录与任务',['catalog-*.log','tasks-*.log'],
     '每个时间样本为 10 次 Root update，另有 1000 次稳定 update 分配计数。shared_buffers 表示与首个视图共用数组的消费者个数，不是物理数组个数。旧目录已无稳定帧全量读取；新目录的收益是多个消费者共享版本数组。旧 tasks-revision 路径已经避免稳定复制，不能与未提供 revision 的 tasks 路径混淆。'),
    ('BQ3：画布身份',['canvas-ids.log','canvas.log'],
     '隔离身份映射微基准与真实 GraphCanvas 10k churn 分开。真实回归覆盖 Root 路由鼠标、在途输入、作者 ID、不复活旧 UI ID、首帧及后续 pan/zoom/selection。后端重建仅在安全点；没有重编号作者身份。'),
    ('BQ4：冻结字节',['bytes.log'],
     '实际 Material 编译结果被用于冻结传输资格，扩展 payload 只测运输开销，不声称扩展字节是合法的 Material 编码。记录物理字节 owner 和重复复制；这不是整个进程 RSS。真实 IO、取消与 Unknown 由原保存回归另行验证。'),
    ('BQ5 与候选路径',['records.log','candidate-paths.log'],
     'WriteCoordinator 保留小容量线性扫描。Flow 选择的整图冻结已由实测证明重复，改为复用选择来源戳，仍逐次通过原 gate；没有更换容器。Outliner、冻结投影保留实际实现；未证明替换方案能抵消维护和身份成本时，不改为新容器。StableSlotMap 地址稳定不等于回调期间可删除，既有 gate 必须保留。')]:
    text+=['','## '+heading,'',explanation,'']
    for directory,title in [(before/'extended','修复前'),(after,'修复后')]:
        text+=['### '+title,'','```text']
        found=False
        for pattern in patterns:
            for path in sorted(directory.glob(pattern)):
                data=aggregates(path)
                if data:text += [path.name,*data];found=True
        if not found:text+=['NOT_RUN／未完成，不能从其他规模推算。']
        text+=['```','']
text+=['## 保留方案与覆盖范围','',
    '- SessionStore／RunStore／ViewHost 的唯一 owner、generation 和执行保护未替换；没有因为容器名更新就采用新算法。',
    '- SmallVector／StableSlotMap 仅完成真实依赖资格与容量测量；不把 UUID 用作稠密 key，不把 SBO 当全局零分配保证。',
    '- 全快照投影的内容变化成本仍存在；本阶段未引入组件级增量投影框架。',
    '- Raw samples、命令、失败尝试、依赖和机器信息随归档保存。缺少的实际测量必须在 receipt 标为 PARTIAL，不能用源码推导冒充实测。','']
(work/'P10Q-performance.md').write_text('\n'.join(text),encoding='utf-8')

old=json.loads((before/'file-api-inventory.json').read_text())
new_path=final/'extra/file-api-inventory.json'
if new_path.exists():
    new=json.loads(new_path.read_text())
    def editors(data):return {x['name']:x for x in data['targets'] if x.get('paths',{}).get('source','').startswith('editor/')}
    a,b=editors(old),editors(new)
    def counts(data):
        values=editors(data).values()
        return {kind:sum(item['type']==kind for item in values) for kind in ['STATIC_LIBRARY','SHARED_LIBRARY','EXECUTABLE']}
    old_counts,new_counts=counts(old),counts(new)
    report=['# P10Q 实际链接边界','',f'实现：`{sha}`。统计来自 CMake File API，而非目录数量。','',
        '统计范围：CMake source 位于 editor/ 的全部 target，包含明示的测试程序及测试 domain/GUI DLL。它不是产品启动装载数。',
        '', '| 实际构建类型 | 基线 | 最终 |','|---|---|---|',
        *[f'| {kind} | {old_counts[kind]} | {new_counts[kind]} |' for kind in old_counts], '',
        '| Target | 原 TYPE | 现 TYPE |','|---|---|---|']
    for key in sorted(set(a)|set(b)):
        left=a.get(key,{}).get('type','—');right=b.get(key,{}).get('type','—')
        if left!=right:report.append(f'| {key} | {left} | {right} |')
    report+=['','## 保留的 Editor 动态边界','',
             '| Target | 消费者／共享状态理由 |','|---|---|']
    reasons={'edit_history':'全局 History 身份；exe、工具、动态 SDK GUI 共同使用',
             'edit_sessions':'Session 身份及唯一 Store 访问边界；多个动态消费者',
             'editor_metadata':'共享反射／组件／命令登记；插件与产品消费者',
             'render_feature_meta':'Feature 元信息动态共享状态；编辑器与插件',
             'scene_render_meta':'Scene Render 元信息共享登记；编辑器与插件',
             'editor_ui':'仍有独立 SDK／GUI DLL 消费者；P12 旧壳期限保留',
             'editor_editing':'窄编辑协议仍供动态 GUI／旧工具消费；不复制其身份职责',
             'physics2d_editor':'真实动态插件，不能改成产品静态自注册',
             'consumer_domain':'测试专用领域 DLL，验证安装后边界与析构',
             'consumer_gui':'测试专用 GUI DLL，验证生成控件与跨 DLL 生命周期'}
    for key,item in sorted(b.items()):
        if item.get('type') in ['SHARED_LIBRARY','MODULE_LIBRARY']:
            report.append(f'| {key} | {reasons.get(key,"需逐项核对，尚无记录")} |')
    report+=['','产品内部实际减少五个 SHARED 边界；新增 STATIC target 不计作 DLL 减少。',
             '实际链接命令、dumpbin imports、cdb lmf 装载列表分别保留；调试器启动时装载列表不能替代业务期间动态插件验证。',
             '共享身份／反射状态未放入多个 DSO 的 archive；无 whole-archive、全 exe 符号导出。',
             'SDK 使用全新 prefix；外部依赖种子有固定清单，未复制 Engine 生成头或旧库。',
             'Windows CRT/PIC/导出在本轮配置与实际消费者核验；Linux PIC 实机验证仍 NOT_RUN。','']
    (work/'P10Q-link-audit.md').write_text('\n'.join(report),encoding='utf-8')
print('Updated summaries from existing evidence only')
