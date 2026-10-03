# EC2 R1 配方与采用契约

公开接口（同一 material_preview target / 安装包）：

```cpp
struct MaterialPreviewRecipe final {
    asset::AssetId mesh;
    cxx::SharedBytes<> mesh_image;
};
MaterialPreviewResult<MaterialPreviewRecipe> makeSphereMaterialPreviewRecipe();
MaterialPreviewResult<PreviewAdoptionKey> MaterialPreview::setDesired(
    MaterialCompileInputKey, std::optional<MaterialPreviewRecipe> recipe = {}
) noexcept;
```

配方是有不可变编码字节 owner 的值，不能仅填虚构 revision。显式配方必须有非空 MeshAsset id 和图像。
同 id 的不同字节是新配方；相同 id 和逐字节相同内容幂等。显式比较只发生在调用者送入配方时，
普通每帧输入同步不扫描网格字节。未传配方保留现有选择；首次调用由公共球体构造函数提供默认输入。
本轮网格配方不增加灯光/相机/Feature 的任意编辑语言；仍复用原固定场景装配与 navigate。

| 事实/资源 | 唯一责任 |
|---|---|
| 所选配方及期望采用身份 | MaterialPreview；配方改变推进 recipe 和 adoption 代次 |
| 不可变编译产物 | 原 CompiledMaterial；编译任务控制仍在原 operation/service |
| 准备/待提交/成功采用 | 原 MaterialPreview 内各阶段记录，分别拥有固定配方和编译结果 |
| CPU 资产读取/上传 | 原 Overlay、Process、RenderAssets、RenderResources |
| 实例与退休 | 原 Runtime lease/retirement；未新增 Runtime 或帧 owner |
| 作者数据、历史、保存基线 | 原模型/History/SessionState；不因换配方而改变 |

接收旧 key 只结清输入，不把结果改标为新配方。调用者可为新配方再次交付同一个匹配编译输入的
不可变结果，无需重新编译。资源准入/解码失败保留已成功资源；回退同时恢复读取源和 Mesh3D 描述，
不手工管理 GPU 资源寿命。场景内描述写入仍使用 emplace_or_replace，进入既有响应式维护。

删除/替换：原私有 MaterialPreviewRecipe 装配类型改为准确的 MaterialPreviewScene；旧物理头删除，
不留 alias；配方独立为公开 MaterialPreview.hpp 中紧凑值。原球体专用 mesh/mesh_image 成员删除，
由固定阶段配方提供。几何对象改称 surface。没有第二套编译、发布、缓存或退休状态机。

消费范围：原 MaterialView 的 setDesired(input) 保持默认球体及后续当前选择；公开 SDK 可显式传入
既有 MeshAsset 编码。第二网格复用原 scene_views 四边形夹具；GPU 测试不写预览内部 Registry，
不 include 其私有头。原有 SceneView 四边形回归复用同一夹具函数，原断言保留。
