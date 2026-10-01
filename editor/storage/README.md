# 项目存储

ProjectStorage 是已打开项目的清单、源/产物目录、写锁与发布状态 owner。
它借用 EngineContext 的 AssetVfs，拥有自己的挂载登记；不拥有 Renderer、线程或 Pane。
后台 readProjectOpenData 产生 ProjectOpenData，在 Main 安全点安装到 ProjectStorage。

createProject 接收 ProjectBuildConfig 和原生目标路径，复用 Process CPU/Blocking/Main：
编码与摘要 → 原子取得新目录创建权 → journal 发布源文件 → 最后提交根清单 → Main 采用结果。
完成槽保存持久提交事实，提交后的取消不能改写为“项目未创建”。成功结果释放写锁后再交付。

现阶段要求目标目录不存在。失败恢复只处理 journal 中摘要匹配的文件，不递归删除目录；
根、空目录与持久锁可能保留。项目打开仍扫描源摘要和产物，按需读取属于后续工作。

`editor_storage` 为 STATIC；依赖 project 描述与 Process，不依赖 assets 的导入实现。

# File publication

`FileArtifactStore` publishes one file from owned encoded bytes. Existing ProjectStorage journal
code and FileArtifactStore share `editor_file_publication`'s file read/write/flush/replace primitives.
The kernel is a STATIC target in the existing storage directory, with a pure persistence dependency;
the backend never links the old ProjectStorage/Editor/editing implementation.

Targets are root-contained canonical filesystem addresses, including relative `.`/`..` aliases and
existing symbolic-link resolution. Windows keys fold case using the invariant locale. Known hard-link
aliases are rejected. Changing a symlink concurrently, case-sensitive Windows directories, filesystem
mount changes, and concurrent external writes between the final check and rename are outside the
single-coordinator guarantee. This is optimistic external-change detection, not global CAS.

The writer creates its own staging directory alongside the target, writes and flushes its payload,
rechecks the expected digest, then replaces the target. A pre-existing staging name is a failure,
never permission to truncate/delete someone else's file. Cleanup removes only this operation's
payload/directory. It is not a journal for project-wide multi-file transactions.

Windows uses FlushFileBuffers and MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH); POSIX uses fsync and
rename. A successful replacement is Published, separately from power-loss durability. Failure in the
optional post-replace durability callback produces a Published receipt with an UNCONFIRMED warning.
No directory-fsync or global crash-atomic guarantee is claimed. Reconciliation verifies bytes after
this synchronous backend has returned, which proves there is no detached writer left in the backend.

SaveExecution uses existing TaskScope RAII and CPU/Blocking senders. Neither its completion callback
nor a worker applies a Session baseline; SaveService does that separately on the owner. A foreign store
exception is Unknown, never guessed to be NotPublished.
