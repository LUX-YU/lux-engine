# 本轮原始证据

实际资格源码 `e39535cfa9d2ebbcdeaeb165c00c7773d52db7cc`；封存 E 参照 `841320a36e864145eda43fd6b775a10a696d8e7f`。

[完整归档](cleanup-e39535cf.zip)：5321465 字节，562 项原始文件及一份内部索引。
SHA-256：`3c6687acd41da78e6c56959da3756032b2f31e81ebddd8f99b222d85ddfe0db5`。解压后的路径和逐项哈希见 [archive-files.json](archive-files.json)。

- `cleanup-20260912/`：构建、正确性、失败尝试、身份与机器码摘录。
- `cleanup-installed/`、`cleanup-value-incremental/`、`cleanup-new-incremental/`、`cleanup-relocated/`：真实 SDK、增量及迁址。
- `cleanup-costs/`：78 个有效进程的原始 CSV/log、命令、业务、配对分布。
- `cleanup-resources/`：独立资源计数；`cleanup-profiles/`：新软件 Hotspots 的原始 result 与导出。
- `cleanup-machine-code/`、`cleanup-class-layout/`：完整反汇编、类型布局及命令。静态指令计数不等于动态执行次数。

不含编译 DLL/PDB/EXE/OBJ；它们保存在本机封存镜像，身份见 [artifact-files.json](artifact-files.json)。
未采集的性能字段保持 null；历史原始文件不改写。

远端归档提交：`3eef07c7ed179c2a129cb4ce68346d6190bb2f49`。已从新的独立 bare 仓库回取并核对同一 SHA-256。

[固定提交下载](https://raw.githubusercontent.com/LUX-YU/lux-engine/3eef07c7ed179c2a129cb4ce68346d6190bb2f49/.internal/evidence/script/premerge-cleanup-20260912/cleanup-e39535cf.zip) · [回取记录](remote-check.json)。
