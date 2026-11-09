# 作者场景加载

`WorldLoadingSystem` 是 Scene 拥有的前置系统，在 `BEFORE_SIMULATION` 执行。同步 `create` 只保存源 AssetId、几何索引、完整加载选择、容量和组件 Schema；Engine 接纳后，`initialize` 才通过 AssetRepository 读取 Scene。

Scene 根内包含流送目录，内容属于同一资产的相邻成员文件。系统持有根快照，直接借用其中的目录；范围读、文件定位和旧读者寿命由既有资产设施处理。运行中不自动切换根版本。

范围模式订阅 Observer、Transform 和生命周期数据的变化，按每个 Observer 的范围查询组并合并需求。一个 Observer 撤销不抵消其他 Observer。完整加载与 bootstrap 在初始化时采用。读取和解码使用共享 Execution；结果保存在原 GroupState 中，后续前置阶段再完整采用，不在 Worker 修改 Registry。

World 唯一拥有 Registry 和 UUID→Entity 索引。加载器只记录自己采用的实体及组归属；不为程序生成实体分配作者身份，不自动保存游戏运行状态。`LifecyclePolicyComponent::MANUAL` 会阻止自动卸载。已声明的外部 Entity 引用在实际卸载前保守检查；这次检查可能遍历 Registry，不是常量时间查询。没有在稳定帧中扫描全部实体。

Scene 退出时先由 Engine 清偿任务，再释放系统借用与源快照，World 最后销毁实体。旧 WorldResidency、作者编辑记录和显式 WorldStreamingDemand 控制面已移出当前生产路径。旧接口测试的迁移状态以外置施工清单为准，不能把保留的算法等同于旧测试已通过。
