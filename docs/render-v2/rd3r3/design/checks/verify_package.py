#!/usr/bin/env python3
"""RD3-R3 deterministic *document* integrity, not production qualification."""
from __future__ import annotations
from pathlib import Path
import hashlib,csv,re,sys

def check(root:Path):
    total=0
    def req(cond,msg):
        nonlocal total
        total+=1
        if not cond:raise AssertionError(msg)
    ch=[]
    for i in range(33):
        files=list(root.glob(f'{i:02d}_*.md'))
        req(len(files)==1,f'exactly one chapter {i:02d}, got {files}')
        ch.extend(files)
    req(len(list((root/'work_orders').glob('*.md')))==9,'nine work orders')
    def rows(name):
        with (root/'appendix'/name).open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f))
    for name,count,field,last in [('CapabilityMatrix.csv',86,'Id','H06'),('GoldenWorkloads.csv',25,'WorkloadId','W25'),('RequirementsTrace.csv',51,'RequirementId','REQ51'),('LegacyGpuAlgorithms.csv',16,'LegacyMechanism',None),('PriorRD3R2TypeOwnership_ARCHIVE.csv',66,'TypeOrFunction',None),('TypeOwnership.csv',79,'TypeOrFunction',None),('ExecutionScenarios.csv',69,'Id','B54'),('DesignDecisions.csv',33,'DecisionId','R3-D33'),('LegacyCompilerMechanisms.csv',20,'Id','GC20'),('MaterialGraphContracts.csv',14,'Id','MAT14'),('UploadTransportCases.csv',16,'Id','UP16')]:
        a=rows(name)
        req(len(a)==count,f'{name}: row count {len(a)} != {count}')
        ids=[r[field] for r in a]
        req(len(set(ids))==len(ids),f'{name}: duplicate ID')
        if last:req(last in ids,f'{name}: absent {last}')
    a=rows('CapabilityMatrix.csv');req(sum(x['Id'].startswith('F') for x in a)==36,'F36')
    req(sum(x['Id'].startswith('I') for x in a)==44,'I44')
    req(sum(x['Id'].startswith('H') for x in a)==6,'H06')
    req(len(rows('DataProjectionExamples.csv'))>=8,'DataProjection examples')
    req(len(rows('ResourcePlanExamples.csv'))>=12,'ResourcePlan examples')
    req(len(rows('MigrationDecisions.csv'))>=20,'Migration decisions')
    for f in root.rglob('*.md'):
        text=f.read_text(encoding='utf-8')
        req(text.count('```')%2==0,f'{f.name}: unbalanced fences')
        req(not '\x00' in text,f'{f.name} embedded nul')
    req((root/'LAYOUT_目录与CMake模块边界.md').read_bytes()==(root/'28_目录与CMake模块边界.md').read_bytes(),'LAYOUT must mirror chapter 28')
    concat='# RD3-R3 · 完整架构章节合订版\n\n> 本文件机械生成，分章 00–32 是唯一编辑源。源文件位于本包根目录；本合订版不是额外一份设计权威。\n\n'
    for f in ch:concat+='\n---\n\n## Source: '+f.name+'\n\n'+f.read_text(encoding='utf-8').strip()+'\n'
    req((root/'FINAL_RD3R3_全章合订版.md').read_text(encoding='utf-8')==concat,'combined differs from chapters')
    decisions=rows('DesignDecisions.csv');chap=(root/'19_RD3R3统一架构裁决.md').read_text(encoding='utf-8')
    for d in decisions:req(d['DecisionId'] in chap,'decision not in normative chapter '+d['DecisionId'])
    norms={
      '00_架构总纲.md':['InstanceContext','ScopeKey','ResourcePlan','DataProjection','RenderEffect'],
      '04_作用域资源与RAII生命周期.md':['ResourceLease','Attachment','ScopeKey','getOrCreate'],
      '05_ResourcePlan与Attach编译事务.md':['ResourcePlan','PassSchema','attach','Executor'],
      '21_Vulkan三级Context与单DeviceSession.md':['Native bootstrap','InstanceContext','DeviceSession'],
      '24_ECS数据投影与插件扩展.md':['RenderSyncStage','DataProjection','prepare/commitPrepared/discardPrepared'],
      '27_H1至H6高级渲染设计输入.md':['H1','H2','H3','H4','H5','H6']
    }
    for name,terms in norms.items():
        txt=(root/name).read_text(encoding='utf-8')
        for term in terms:req(term in txt,f'contract missing {term} in {name}')
    for name, terms in {
        '29_Legacy_RenderGraph_Compiler源码审计与迁移合同.md': ['eliminateStarvedPasses','eliminateDeadPasses','LocalRead','BarrierProgram','QueueSubmitProgram','LAZILY_ALLOCATED'],
        '30_MaterialGraph_Shader变体与运行时集成.md': ['MaterialGraph','ShaderIR','MaterialAsset','MaterialNodeCatalog','GraphFamilyGPU','VariantBucketManager'],
        '31_大资源UPLOAD与GPUTransferPipeline.md': ['PROGRAM','CONTROL','UPLOAD','DEDICATED_QUEUE','RECORD_ONLY','SubmissionTicket','QFOT'],
        '32_GraphTransient与专业GPUStorage性能账本.md': ['Transient','VMA','Bindless','Texture','Mesh']
    }.items():
        source=(root/name).read_text(encoding='utf-8')
        for term in terms:req(term in source,f'new source contract {term} missing in {name}')
    for r in rows('ExecutionScenarios.csv'):
        req(r['RD3R3RequiredStage'] not in ('','N/A'),f'undefined scenario stage {r["Id"]}')
    # Verify manifest after it's generated; self excluded.
    lines=(root/'MANIFEST.sha256').read_text(encoding='utf-8').splitlines()
    man={}
    for line in lines:
        h,name=line.split('  ',1);req(name not in man,'duplicate manifest path')
        man[name]=h
    actual={x.relative_to(root).as_posix() for x in root.rglob('*') if x.is_file() and x.name!='MANIFEST.sha256'}
    req(set(man)==actual,f'manifest coverage missing={sorted(actual-set(man))}, stale={sorted(set(man)-actual)}')
    for name,h in man.items():
        req(hashlib.sha256((root/name).read_bytes()).hexdigest()==h,'sha mismatch '+name)
    return total

if __name__=='__main__':
    target=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
    print('RD3-R3 DOCUMENT CHECKS PASSED:',check(target),'checks', 'root=',target)
