# XEC2-12 勘误（不改写原 EC2 快照）

前置验收提交：644d953ac650157d68f87d0ca7b37ce0a8a12e9c；原实现：dd54847ee281f5e96adf4ebeba2f6c06bc858515。

原 `dev_log/EC2/coverage.json` 将 XEC2-12 标为 PASS，并声称实际 GPU 预览消费第二个网格配方。
复核真实源码发现：MaterialPreviewRecipe 是私有装配结构；setDesired 只有编译输入；recipe 字段固定为 1；
makeMaterialPreviewRecipe 始终编码球体。scene_views/views.cpp 的第二个 MaterialPreview 也是球体。
同文件另一个 meshViews 用例使用四边形 SceneView，不能充当 MaterialPreview 第二网格证据。

因此原证据能证明两目标共享编译结果及各自退休，不能证明公开配方、更换配方和第二网格采用。
本勘误撤回原 XEC2-12 的完整通过主张；旧原始记录和哈希原样保留。

修复前真实 SDK 资格：before/CMakeLists.txt 仅 find_package 已安装 material_preview，配置成功；
before/recipe.cpp 调用公开配方路径，实际编译报 C2039/C2065（无公开 MaterialPreviewRecipe）。
对应 logs/before-sdk-*。这是实际 SDK API 缺失证据，不宣称运行了不存在的配方接口，也不将它当作运行期失败。

修后同一公开 API 使用 sphere 和已有双视口四边形夹具的编码字节，真实 GPU 像素回读证明两者不同。
覆盖同输入切换、同 ID 不同字节、迟到完成、损坏字节失败保留上一成功画面、重试及原退休。
最终运行结果将在本目录报告中绑定实现 SHA；本段描述验证方法，不是预报 PASS。

原生输入继续 NOT_RUN_USER_DEFERRED；Linux/系统 IME 不执行，旧慢算法样本不补。
