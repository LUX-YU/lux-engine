# 施工接力、证据与最终交付格式

## 1. 唯一账本，不复制状态

`.internal/editor-redesign/` 保持施工权威。附带JSON模板只表示所需字段，可合入原账本，不必为每一类另建一个管理工具。阶段末冻结至 `dev_log/P10Q-structure/`，冻结之后不再把它作为活动工作目录。

原P10Q的PARTIAL保留。新增scope amendment写明：用户已取消旧50k样本补足和当前Linux实测硬阻塞；Windows、适用GPU、依赖和原行为仍要求实际验证。不是把原receipt.json中的PARTIAL改PASS。

## 2. 每批最小交接消息

```text
P10Q-structure / Lx
输入：<完整SHA>
实现：<完整SHA或明确尚未提交>
已闭合：<具体责任与文件/target>
删除：<旧定义/源/target；没有则写无>
暂留：<真实消费者、P11/P12责任>
验证：<实际命令及结果；未跑项目>
用户修改：<原路径/新映射；原bytes状态>
下一入口：<唯一下一L批次>
```

不能只写“重构完成，测试全绿”，也不需要每个小函数都附一份重复报告。

## 3. L6最终冻结目录

```text
dev_log/P10Q-structure/
  README.md                    # 实际结果、范围、唯一下一入口
  receipt.json                 # input/implementation/evidence与命令索引
  scope-amendment.json          # 新用户范围，原历史不改
  file-plan.json               # 全量文件和split符号去向
  target-map.json              # 真实层/角色/闭包和二进制决定
  abstraction-map.json         # 静态/动态/保留/删除理由
  behavior-map.json            # 原行为→当前实际测试
  retained-product.json        # 原旧岛consumer与P11/P12退出条件
  logs/                        # 命令、退出码、stdout/stderr
  evidence/                    # 实际File API/编译/安装/模式结果
```

可按原工具保留等价命名，不需要重复同一JSON内容。禁止把新报告塞生产include/src或安装SDK。

## 4. SHA与哈希的含义

- input_sha固定原参考与实际工作前置。
- implementation_sha只含此次被验收tracked代码，不含用户未授权diff。
- evidence_commit在实现之后单独提交；不在其自身文件里要求填写自己的Git SHA，避免自引用。
- file blob来自对应Git对象；raw SHA256用于运行产物/用户原bytes，两者不能混用。
- 哈希一致说明文件身份，不说明测试正确或运行过。
- 原absolute机器路径只为溯源，checker实际读取相对归档路径和Git对象。

## 5. 最终对用户的摘要模板

```text
P10Q-structure 已完成／PARTIAL，停在P10Q。
实现：<sha>；验收：<sha>。
已完成：五层正式归属、真实闭包、TaskMonitor分离、输入交付concept、旧路径退出。
保留：<必要动态接口/共享DSO边界/原owner>。
实际验证：Windows ...；SDK ...；GPU各模式 ...；依赖/编译负例 ...。
范围：Linux NOT_RUN不作为本轮阻塞；系统IME ...；旧长测不补跑。
原C01/C03/C04：<原判定与责任>。
用户修改：<实际保全状态>；main未改。
仍需复审/下一入口：<不自动进入P11>。
```

## 6. 失败和中断

正常编码/目录迁移/构建失败保留首个原始记录。只有旧判断错误才改测试，必须解释原测试错误与生产修复的区别；不能删掉第一次失败然后把重跑记为首次通过。

若一次LLM任务只能完成部分：提交可构建的已闭合单元，明确剩余计划和未通过项，停在当前L编号。不要临时压缩后续任务为“移动完成”；不要在未验证代码上发最终PASS。

复工只读真实HEAD、账本、上次结果和用户改动；不从长对话的印象推断已完成，也不重新跑已确认无关的旧长测。
