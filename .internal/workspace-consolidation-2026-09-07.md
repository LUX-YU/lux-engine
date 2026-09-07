# lux-engine 工作树合并与清理（2026-09-07）

用户授权将分散的工程工作树收拢回原始 `E:/SyncForder/CodeRepos/lux-engine` 并删除副本。

- 原 main：`a577c49409e2519029693fbe780fe8ce3ab2dc1e`。
- 生产线快进至已验证 SR-4 `22304336806c70b209ae90e585cb5cb0c085bade`。
- 独立文档分支 `docs/implementation-v3@db2291471bcaec38084a1c88afa17fa19e8abd53` 合并，新增17份文档；合并提交 `65a3a9ff5a0eafe20fe1f5398d1ad369bf20d260`。
- 旧 frozen-runtime 性能对照的6项专用提交保留在 `codex/s6-v3-reference-driver@8ce2a0ff5210834c2a574fb41828f37891f3ce39`；不把旧生成/安装配置覆盖回当前生产实现。
- 删除26个同级工程目录及 qualification 下2个注册工作树；Git 最终只登记原始 lux-engine。
- `lux-engine-test`、`lux-engine-test-2d` 没有 Git，是独立场景资产，保留原位置。

## 未提交资料

原 main 的5个修改和 deep-optimization 的3个修改/未跟踪文件全部保全。可适用的本地改动保持未提交并移回main；
CppStaticScriptBridge 的排版针对已经删除的旧声明，故只保留原文件/patch，不恢复淘汰的接口。
忽略的说明、配置、旧下载证据与缓存也在删除前备份校验。完整本地备份位于
`.internal/workspace-consolidation-2026-09-07/`，其中 preserved.json 记录原始字节哈希，reapply.json 记录恢复方式。
本地备份和用户修改不纳入本次提交。每个副本 HEAD 在原仓库的
`refs/archive/workspace-consolidation-20260907/<原目录名>` 保留，原有分支不删除。

## 验证与边界

所有删除目标事先解析绝对路径、核对范围/HEAD/工作区、拒绝链接目录，并比对原文件与备份哈希。
删除后再次核对28项目录不存在、所有保存引用可读、全部本地备份匹配，git fsck 连通性检查通过。
合并提交相对SR-4只有文档新增，生产源码/构建规则没有新增差异；恢复的三份现存公共头仅有原本的空白排版变化。
本次是仓库整理，没有重新运行构建或以旧结果冒充新验证；SR-4验证身份及原始证据保持不变。

独立build/install目录未删除。历史构建缓存仍记录当时的独立源码路径，后续在原目录构建须重新配置；不改写旧资格证据身份。

## 已清理目录

| 原目录 | 保留HEAD |
|---|---|
| `lux-engine-deep-optimization` | `22304336806c70b209ae90e585cb5cb0c085bade` |
| `lux-engine-hook-b1` | `da5e29b65f293f68c6c42231ce576cfa85618d2f` |
| `lux-engine-hook-closure` | `a577c49409e2519029693fbe780fe8ce3ab2dc1e` |
| `lux-engine-hook-qualified` | `5f03e9b156421583ae81857025ec6156ad0e0f05` |
| `lux-engine-s3-qualification` | `2ef5d62a6dea4fd7102c47d09ac9ee2e2addde1c` |
| `lux-engine-s3h-qualification` | `97e68dc9c3f222c2f09be3ebefe513b697fd128d` |
| `lux-engine-s4-qualification` | `658004517267e2712d95cc91e167282727a44fed` |
| `lux-engine-s4p-qualification` | `b42e976dbcd3bc1536337ab04c217b67852aede9` |
| `lux-engine-s5-qualification` | `feb19bfb7e3558e8efd85609e83bc143f33a460c` |
| `lux-engine-s6` | `8354ae10e5d247cbc69746ae1f79c97ddfdd5ab9` |
| `lux-engine-s6-docs` | `db2291471bcaec38084a1c88afa17fa19e8abd53` |
| `lux-engine-s6-v3-bcompare` | `8ce2a0ff5210834c2a574fb41828f37891f3ce39` |
| `lux-engine-sr2-baseline` | `54de8af521887c4e5aaf0041b45422a817694005` |
| `lux-engine-sr2-candidate` | `feaa7550bd819b0bcb928d477fb45e40ff9282b1` |
| `lux-engine-sr3-candidate` | `0cf053b9a48bcc62bf415522e930650352ccb06c` |
| `lux-engine-sr3-closeout` | `9406f72c380cec6e119e50499f392db38d92eaca` |
| `lux-engine-sr3-cost-final` | `99c1d095fad6a728c3d808ff2d853ce84b59bfea` |
| `lux-engine-sr3-event` | `8a6e6ef468f7eb60265678ab25116719b42a01a6` |
| `lux-engine-sr3-final` | `3e2bcbd29580d2e744744b64149b7d8fedcc8950` |
| `lux-engine-sr3-reference` | `8145598c18421d03da1dac21251200af3290e27d` |
| `lux-engine-sr3-remote-check` | `07488c1eb8491e57bdfe9a0973e1a9682bac1de8` |
| `lux-engine-sr4-candidate` | `1750ce854967a382ee89accf3a3534a0628396cc` |
| `lux-engine-u-c-f5d6551e` | `2caaa6f7b35ad759d32e9f9763a67dcb559b8860` |
| `lux-engine-u-c-final-d7448965` | `9c952ed4544cf08cfc4e669d7196d47efe759bcf` |
| `lux-engine-v3q` | `babbd2d220aba2d957c5628781db6157258d5f8e` |
| `lux-engine-v3r` | `9d40ec68489c17b8f4c91091974cb6b54263d0b1` |
| `qualification/lux-engine-s15-3a221986` | `3a2219865bc2283f5414c2cac331d04d75937e23` |
| `qualification/lux-engine-s15-final-485f8737` | `485f8737cc8a848274fbfc43cfe5b8d616729e21` |
