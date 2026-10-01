# P11 R1 单一施工节点

只补拒绝输入拥有作用域与复合命令注册保护；不进入 P12，不改变五层、包、三模型、History、Process、Runtime、保存版本链或 GPU 责任。

1. 原3bbc SDK：完整原 models.cpp、contributions.cpp 程序内增加 R11 用例，记录 before 源、测试差异、实际 SDK include/link、失败和正面对照。包内缩小 witness 不作为运行资格。
2. SaveService 输入进入现有 dispatch 后立刻转入局部拥有单元；拒绝析构仍受保护，最后代码 pin 在源之后释放。完成吸收不改变。
3. CommandRegistry::Batch 在命令 owner 内限定保护；readBatch 允许固定读取，preparePublication 检查容量并拥有一次无普通失败 commit。上层同时持有自身与命令 scope，候选/旧值清理和通知都在两者以内。下层不认识 application。
4. 复用原程序新增9项场景，保留213项。完整固定提交 P11+STRICT，全量/二次、CPU、PLAYER、24组SDK、真实DLL、GPU/输入、clang公共头和ABI指纹。原历史快照和用户差异不修改。
5. 实现与dev_log/P11-R1分别提交；正常推送原实施分支，等待复审。Linux/IME/sanitizer NOT_RUN；旧性能PARTIAL不改写。C03当前PASS保持，C01/C04仍P12。
