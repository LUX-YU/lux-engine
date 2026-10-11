# RD2-D0-C2 正文勘误与版本身份

修订标识：**RD2-D0-C2**，2026-10-11。只修订 RD2 候选；原 FINAL、输入 ZIP、原 CSV、历史验收与生产代码不动。用户四项裁决原文存于 work_orders/00_D0_用户歧义裁决_只读.md，SHA 见来源清单。

四项裁决直接反映在正文：A 保留 Core 身份域；B 恢复 C0–C13；C Composition 先解析 DataSet/Feature；D CPU/Wire/GPU 分层。另按批准计划同步 F6/F7 交付时间、F11 与 H 阶段边界、Device Lost 及明确示例字段。未扩大生产范围。

## 原包文件与新候选哈希

MANIFEST 自身的原哈希在来源 ZIP 中保存；新 MANIFEST 的哈希由外部资格证据记录，避免自引用。合订版由修订后的 00–20 机械生成；原包内容仍可按源 ZIP 精确恢复。

| 相对路径 | 原 SHA-256 | 候选 SHA-256 | 处理 |
|---|---|---|---|
| `00_README_权威总纲.md` | `91070f83f463cdea44204e9856fae5da56079b44485de9a8cad892f9f928038a` | `1bb515a5a90ad86716f5fc69d60772c6e694f4e80f5282ef53b5c5c43dd0bbd4` | 候选修订/机械生成 |
| `01_设计哲学与成熟引擎对照.md` | `3bbf69da8ae722ac5ff69cde960163de721d99a41c903a7386c2344dbd2036f8` | `3bbf69da8ae722ac5ff69cde960163de721d99a41c903a7386c2344dbd2036f8` | 原字节保留 |
| `02_模块分层与所有权.md` | `b07460304e9e3738c1872b4c6900890563212009a89ce7775709eaae0680d83c` | `3cbf6519b6d000d42d08120255bacc316d1de2921e13b19383db5f6f629b43f1` | 候选修订/机械生成 |
| `03_数据描述与类型身份.md` | `95c79365b58bbc347d0d4b81ecfe336feff8ae40ddd4602192783fcd253b0bed` | `a2d68cad057806ffacc0cb78cd319c7202d3bbdc05c1f2c210727c0a9cb1eb98` | 候选修订/机械生成 |
| `04_类型化存储与GPU布局.md` | `9eabf53b70ca8a6e7d9a91913c30ff04c6121da9612a446ae10b50078f86a5d2` | `746adb8e84fd1a6ce252e87cca219cc81949f420e359402aea94a84e22fb2550` | 候选修订/机械生成 |
| `05_通信与发布事务.md` | `7d60931ff46d8d30ccd6b3d7474ac092560c5c12ab5c04b66fe341ee33c0bd4c` | `8018401da1b857479041394f2492e193a3d7c4b5a948198eb8eda54a36181572` | 候选修订/机械生成 |
| `06_Feature组合与数据依赖.md` | `d9e19c54b1a148633a5aa77484aa5d0cbe3a1bdc03b1bfea78c2d3e09de8e9de` | `ec68b51852fd1092a78044f983337187ea0aeab0c18c7a2961c132f599fba593` | 候选修订/机械生成 |
| `07_RenderGraph推导与编译.md` | `b85b4b2ea2bba57b8f74ef204962bab1a63919e80a5f54fa787259685fc10cdf` | `1f3203962f305879d422def70f2c7de3997c670ebc98d767e1fa1e094bacd5f2` | 候选修订/机械生成 |
| `08_Shader与布局自动生成.md` | `520936fec44832156ca6ebc677bd07540d46e8d86f08956c4d09c72f180a51f3` | `368e9da40b35e62dd0d5bd6eca1907cf78db255072d64a45c91c6699fda5ee99` | 候选修订/机械生成 |
| `09_VulkanNative与执行.md` | `b02373b5ebbc3dfd7b78e8e87fe9b982132d95dd60301f586601542176773441` | `b02373b5ebbc3dfd7b78e8e87fe9b982132d95dd60301f586601542176773441` | 原字节保留 |
| `10_Runtime多Scene多ViewTarget.md` | `4dd4223f88ae41a514a6da2c90e58759f12a3af6f860baed88400477cdf20caf` | `c02687f35ff55222a4d23c2e2e0bfd48dc2b51999c6fd3e525c7786c4bd5f0c3` | 候选修订/机械生成 |
| `11_插件SDK与扩展边界.md` | `5fbbe1c40cbb688e767f8e6b04ebd7ead8020ded54f743d3f7815edfbbc81a10` | `5fbbe1c40cbb688e767f8e6b04ebd7ead8020ded54f743d3f7815edfbbc81a10` | 原字节保留 |
| `12_Legacy功能完整迁移.md` | `f9cacd241a16ce5ea8816c46924f6350712864436fae2b25432e0381a28d15d4` | `61c3b2610578ba3ea684a70661055453c35aec405af4eb96b54e82581d524404` | 候选修订/机械生成 |
| `13_先进渲染兼容性.md` | `2cd3379925eb5d2f2832d4291f197b71cef496046a8acd3986b3cd0b8e99b49e` | `2cd3379925eb5d2f2832d4291f197b71cef496046a8acd3986b3cd0b8e99b49e` | 原字节保留 |
| `14_Cpp20类型和编码纪律.md` | `6b514aaa6d52720d3fba4b20ef41df497382301e58942ccb9b1a5b385f12e837` | `8d48a5b2fce71f21a546616284fd1dbb1a960ade6f9077f80c38f14064d59095` | 候选修订/机械生成 |
| `15_端到端API示例.md` | `4b37ab4cc5d64fe5856d3da8e04b1f314f83fcfabd2e22f04d255119e51be975` | `fd6ffa1fa1a8a771eda1fa68acfdec12f99c1b8cf3f457ee554d09d10315d883` | 候选修订/机械生成 |
| `16_性能模型与可比实验.md` | `5fd73a18fd20ab2eb715f6accc799fb177b1dbedb201c318be37faef4176caa0` | `5fd73a18fd20ab2eb715f6accc799fb177b1dbedb201c318be37faef4176caa0` | 原字节保留 |
| `17_完整验收与正确性矩阵.md` | `0d60fd6946c7752d6695c7dd639abd2495f76a56d763c09f9015d518904f38fb` | `dfc885a2045768c72966434ca642bbddda633ba57a2773438eeda184c41c174c` | 候选修订/机械生成 |
| `18_阶段计划与交付门禁.md` | `400f0ec018108b0532918486fa78c86c5a2c099b0e8b0a058f06a107e304d795` | `911492bb0c2f026abb5830e46b48118f2cdd4434de49adcb36ef22b1579fecea` | 候选修订/机械生成 |
| `19_冻结设计决策RD2.md` | `c3c7e3667473ec892f35f8de7e09e9f9c79ee66172f6836e54679cbc49ef23db` | `59461a4f79b0f8874ec8b491823df35af6d96dbdbc5735dc0193f7735796140a` | 候选修订/机械生成 |
| `20_原设计迁移对照.md` | `18c3d823deb80345a3f971cf646946ce51762ba63345112cc40de6f6b460d1fe` | `28d191c332231ef1de3cb433c761bb373ddd28e947eb8e3a19a6102195d19fab` | 候选修订/机械生成 |
| `AUDIT_来源与一致性.md` | `4eafabe5ef9b9c0450b5d4664671b3f53bdbed9ecc88426a1a702f5313bc801c` | `87eada5a8e436a6985d1c5a43276ea78f63119b70f70401d6917b2e10a5b33a8` | 候选修订/机械生成 |
| `FINAL_RD2_全章合订版.md` | `cd549e912ffbe081ef37227c2fd276ddf7a9b2d76acb9a952e0aff6c036468f8` | `3dc47c4dbc214b1b85c16947233b369a9442805c936660aeda91a54c37e5dac6` | 候选修订/机械生成 |
| `README.md` | `afd767efb90eb0963e88d4b1d9e6aaca2598cd44e6f8e52a5b4a94781c93367b` | `be5d5144462198bd69428f21b47c9f995b66730dc82d29416d9056b0bbc471b4` | 候选修订/机械生成 |
| `appendix/GoldenWorkloads.csv` | `bc9fd980b4d6455bc981a2f458ba53e427da1fceccb7e44b63ffc5a9c0f31b42` | `bc9fd980b4d6455bc981a2f458ba53e427da1fceccb7e44b63ffc5a9c0f31b42` | 原字节保留 |
| `appendix/LegacyCapabilityMatrix.csv` | `ce3555c7eaf50a981a53ec22a9ae41f5bfbe35e626970a891623c8c4448451b6` | `ce3555c7eaf50a981a53ec22a9ae41f5bfbe35e626970a891623c8c4448451b6` | 原字节保留 |
| `appendix/RequirementsTrace.csv` | `9f04107bcf5d06a77c42637614f8a315abf98c211aad101bca3340bdcbb5b9a0` | `9f04107bcf5d06a77c42637614f8a315abf98c211aad101bca3340bdcbb5b9a0` | 原字节保留 |
| `appendix/TypeOwnership.csv` | `9a57c133d5bb7e1e4da6c57a34483be6364bfa6226195fcfe129892ed47952d4` | `9a57c133d5bb7e1e4da6c57a34483be6364bfa6226195fcfe129892ed47952d4` | 原字节保留 |
| `work_orders/01_D0_文档架构冻结工作单.md` | `512929c1e0ac09355638a0593216bd6f63e53ac38c067ffd55318a3068867d42` | `a1eb70b21d2988ba769a782d12a4563ea04c9db36d67fce5d7538a396ccbc82d` | 候选修订/机械生成 |
| `work_orders/02_F4_CLOSEOUT_实施工作单_待授权.md` | `f4cfc96ebe2e7423c450584c76cde0138d2b61aa178e7c1e3aa3c9d5a94582af` | `f4cfc96ebe2e7423c450584c76cde0138d2b61aa178e7c1e3aa3c9d5a94582af` | 原字节保留 |
| `work_orders/03_F5_RUNTIME_入场工作单_未授权.md` | `511b78ac5cee5c295560b69732d0e9848a50b50c7d02338f9898b4ea942626c4` | `511b78ac5cee5c295560b69732d0e9848a50b50c7d02338f9898b4ea942626c4` | 原字节保留 |
| `work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md` | `8283c193936f35e98bc19c9c0b5bd3181b66083688453ce034e0869bdd29c6c5` | `0700516537987bfb12afe8cb8b99e6a466c24d5a243698429bde968ba28cf74c` | 候选修订/机械生成 |
| `work_orders/05_F7_TypedStorage_GPUResident_预备范围.md` | `d0f9e1806cfff02d2b74d555d406dbfbfe593a0f04cc793e9d3905b58b7b10b5` | `0ecd57039d04c661ca2ec1f59449af659f29f8d6302bd68d9950469bce577bd6` | 候选修订/机械生成 |

## 逐项责任变化

以下列旧句与新句，引用旧句只为审计，不具有覆盖修订正文的效力。

首个 Implementation `264e285d6948fadf24feee56efc828eaa8bbb5e6` 的机械检查 4492/4492 通过，但独立语义复核发现第 04 章顺序图未与裁决同步及第 07/08 章旧术语残留，未给整体 PASS。当前 C2 在施工检出修正这些正文并重生成合订版；旧 clean clone 未修改，首轮原始证据保留。新的 Implementation 需重新独立资格，不由 V 修正文。

### E001 · 00_README_权威总纲.md

理由：候选身份与权威分离。

原文：

````text
版本：**RD2 — 2026-10-11，拟议的完整替代设计，尚待独立设计门禁批准**
````

修订：

````text
版本：**RD2-D0-C2 — 2026-10-11，已按用户歧义裁决修订的候选设计；仍待用户架构审查批准**
````

### E002 · 00_README_权威总纲.md

理由：文档入库不等于实现。

原文：

````text
不是已写入仓库的实施事实
````

修订：

````text
不是已实现的生产 API 或功能资格
````

### E003 · 00_README_权威总纲.md

理由：裁决 C：Composition 在 Graph 之前。

原文：

````text
Graph Composition 将 Feature 的数据需求解析为确切资源与 Pass 依赖
````

修订：

````text
冷态 Feature Composition 先解析数据需求，再降低为 typed Graph 资源和 Pass；唯一 Graph 从实际访问推导依赖
````

### E004 · 00_README_权威总纲.md

理由：表含十一行，去除错误计数。

原文：

````text
不能混为一谈的十个名字
````

修订：

````text
不能混为一谈的身份与类型
````

### E005 · 00_README_权威总纲.md

理由：裁决 A/D。

原文：

````text
数据的稳定语义、版本、字段、GPU ABI、存储约束
````

修订：

````text
既有稳定语义类型 ID；Schema 描述字段、版本、存储约束及可选的 GPU 表示
````

### E006 · 00_README_权威总纲.md

理由：两阶段规划。

原文：

````text
由 Schema + Device Caps + 实际访问声明决定具体存储计划/布局、Ready、Retirement
````

修订：

````text
先冻结逻辑 StorageInterfaceShape，再由实际访问与 Device Caps 在 C10 选择相容 backing、Ready、Retirement；不得晚改 Shader ABI
````

### E007 · 00_README_权威总纲.md

理由：完整性编号解释。

原文：

````text
36 F/30 I，防功能降级及业务迁移门禁
````

修订：

````text
36 F/44 I/6 H 完整追踪，其中原 Legacy 业务与基础设施为 36 F/30 I
````

### E008 · 02_模块分层与所有权.md

理由：裁决 A。

原文：

````text
canonical name + Schema 修订
````

修订：

````text
既有 renderDataTypeId(canonical_name)；版本/兼容性单独校验
````

### E009 · 02_模块分层与所有权.md

理由：裁决 A/D。

原文：

````text
可，使用稳定版本化 POD 身份
````

修订：

````text
稳定语义 ID 可编码；Wire 合同另行验证，不把 Schema 版本拼入现有 ID
````

### E010 · 02_模块分层与所有权.md

理由：保留现有 Core 全部身份域。

原文：

````text
行为合同 + version
````

修订：

````text
既有 sceneCapabilityId(canonical_name)；contract_version 独立验证
````

### E011 · 02_模块分层与所有权.md

理由：继承既有 Device Lost 终态。

原文：

````text
CPU authority 可以选择保留以供设备恢复，Native backing 必须按照 device-loss 合同终止/重建
````

修订：

````text
当前 Runtime 进入终态；宿主可保留合法 CPU authority 并另建新的 Runtime，Native backing 按所属 device-loss 合同终止，不在原 Runtime 内自动恢复
````

### E012 · 02_模块分层与所有权.md

理由：裁决 A。

原文：

````text
scope owner + key + generation
````

修订：

````text
scope owner + RenderDataTypeId + key + generation（Runtime-local）
````

### E013 · 03_数据描述与类型身份.md

理由：裁决 A/D。

原文：

````text
## 3.1 Data Type 首先是语义，不是内存分配申请

声明 `T` 只注册“数据是什么”与“可以怎样被合法存储/访问”；不意味着立即建立 GPU Buffer，更不意味着创建一个 Entity。实际可寻址的 DataSet 必须有 Scope 与 Key，CPU/GPU 物理实现按真实读写者决定。`RenderDataDescriptor` 已有稳定 Name/ID、Wire 与 Layout 版本、Size/Alignment；本章在它上面增加**同源生成的字段列表、GPU Layout Fingerprint、Storage Contract**，而不是平行造一套 `GpuTypeId`。Wire Schema 与 GPU ABI 是同一个作者结构的两种可验证表示，不能假设二进制布局必然相同。


````

修订：

````text
## 3.1 Data Type、CPU/Wire/GPU 表示各有合同

`RenderDataTypeId` 继续使用既有 `renderDataTypeId(canonical_name)`；canonical name 的精确字节及碰撞检查不变。Schema semantic version、Wire version、native value layout version 与可选 GPU layout fingerprint 分开验证。不得创建平行全局 `GpuTypeId`。

`DataSchema<T>` 描述语义身份、字段、允许 Scope、authority、变更模型与存储资格，不意味着分配 GPU Buffer，也不意味着创建 Entity。CPU-only T 不要求 standard-layout、trivially-copyable 或 std430；具体 CPU storage 策略只要求其真实构造、析构、移动/拷贝能力。

既有 `RenderDataDescriptor` 的 `wire_version/layout_version/size/alignment` 保持原语义，不能静默改为 GPU ABI fingerprint。后续可选 `GpuLayout<T>` 或显式 `GpuRepresentation<T>` 保存 Shader-visible 表示与布局身份；其正式 public shape 由 F6 授权与实测冻结。CPU struct、Wire bytes、GPU record 不必字节相同。


````

### E014 · 03_数据描述与类型身份.md

理由：裁决 A。

原文：

````text
- `DataSchema<T>` 编译期字段/版本/Scope/Authority 元数据；`RenderDataTypeId` 与 collisions 检查。
````

修订：

````text
- `DataSchema<T>` 编译期字段/版本/Scope/Authority 元数据；复用 `RenderDataTypeId` 与 canonical-name collision 检查。
````

### E015 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
- GPU 布局验证（`offsetof/sizeof/alignof/stride/matrix major/std430/std140/scalar`）、Shader 结构声明和实际 SPIR-V Reflection 一致性。
````

修订：

````text
- 仅 Shader-visible 表示生成可选 `GpuLayout<T>`/`GpuRepresentation<T>`：验证 offset/size/alignment/stride/matrix-major/std430/std140/scalar、Shader 声明与实际 SPIR-V reflection；必要时生成 CPU→GPU 打包转换。CPU-only 不生成虚假的 GPU ABI。
````

### E016 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
- 若跨 Transport，则单独的版本化 Wire Encoding/typed Operation；不得把带 padding 的 C++ struct 直接当可跨编译器 Wire ABI。
````

修订：

````text
- 仅跨 Transport/ABI 时生成版本化 Wire Encoding/typed Operation，遵循既有 PacketValue 与 owned attachment/codec；不得把任意 C++ struct 直接 memcpy 为 Wire ABI。Wire-only 不要求 GPU Layout。
````

### E017 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
- `SchemaFingerprint` 及 Layout Revision，区分语义兼容 vs 字节布局兼容 vs 物理 Buffer compatible。
````

修订：

````text
- 分别保存 Schema 兼容性、可选 Wire 编码身份、可选 GPU Layout Identity，区分语义兼容、编码兼容、Shader ABI 兼容和 physical backing compatibility。
````

### E018 · 03_数据描述与类型身份.md

理由：统一名称。

原文：

````text
TypeSchema/容量/数据视图
````

修订：

````text
DataSchema/容量/可选 GPU 数据视图
````

### E019 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
## 3.4 编译期模板约束

```cpp
template<class T>
concept RenderDataRecord =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> &&
    requires {
        { DataSchema<T>::descriptor() } noexcept;
        requires DataSchema<T>::validated_gpu_abi;
    };

template<RenderDataRecord T> struct RecordHandle;
template<RenderDataRecord T> class TypedStorage; // 只含最小真正必要算法
```

这里 `DataSchema<T>` 为**预生成 Traits**，不是运行时 RTTI，也不要求 public data type 继承 `IGpuData` 或 `IGpuRecord`。Generated `gpu_abi` 可能拒绝部分标准布局但 Shader 不支持的类型；`std::is_trivially_copyable_v<T>` 只是必要而非充分条件。


````

修订：

````text
## 3.4 分层的目标 C++20 约束

下例是概念性目标约束，尚非当前可编译 API；最终拼写和完整 Concepts 正负例属于 F6/F7。

```cpp
template<class T>
concept DescribedData = requires { DataSchema<T>::descriptor(); };

template<class T>
concept GpuRecord = DescribedData<T> && requires {
    requires GpuLayout<T>::validated;
};

template<DescribedData T> struct RecordHandle;
template<DescribedData T> class TypedStorage;
// 具体 CPU storage 策略另约束 T 的生命周期能力。
// GPU-facing typed view/encoding 才要求 GpuRecord 或显式 GpuRepresentation。
// Wire packet 继续由既有 PacketValue、生成 wire contract 与 owned codec 验证。
```

`GpuRecord` 不是 DataSchema 或 CPU TypedStorage 的前提。CPU-only 非平凡类型可合法存储；不能因此直接进入 Wire/GPU。GPU-authority 数据不暴露伪 CPU get/patch。显式 GPU representation 可不同于 CPU T，必须由同一现有 meta-generator 生成并核对布局与转换，不引入第二反射、序列化或 Shader parser。


````

### E020 · 03_数据描述与类型身份.md

理由：裁决 A。

原文：

````text
(OwnerScopeId, CanonicalDataTypeId, DataSetKey, generation)
````

修订：

````text
(OwnerScopeId, RenderDataTypeId, DataSetKey, generation)
````

### E021 · 03_数据描述与类型身份.md

理由：裁决 A。

原文：

````text
`DataSetKey` 是 Scope 内
````

修订：

````text
此 DataSet handle 为 Runtime-local，必须校验真实 owner 和 generation；不能跨 Runtime 直接按数值比较。`RecordHandle<T>` 必须携带 DataSet 身份或绑定到严格限定的 reader/writer，防止同 T 不同集合的 slot 碰撞。`DataSetKey` 是 Scope 内
````

### E022 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
Type 匹配需同时校验 name collision、schema semantic major/version、field type/stride/gpu ABI fingerprint、read/write contract
````

修订：

````text
Type 匹配校验 name collision、schema semantic major/version、field type 与 read/write contract；只有涉及 Shader-visible 表示时才另校验 stride/GPU ABI fingerprint
````

### E023 · 03_数据描述与类型身份.md

理由：裁决 D。

原文：

````text
`DataSchema<T>` 提供 T 及 GPU backing 的长期数据合同
````

修订：

````text
`DataSchema<T>` 提供 T 的长期语义合同，可选 `GpuLayout/GpuRepresentation` 提供 GPU 表示与 backing 约束
````

### E024 · 04_类型化存储与GPU布局.md

理由：裁决 D。

原文：

````text
DataSchema<T> + DataSet requested scope/count/budget + DeviceCaps
````

修订：

````text
DataSchema<T> + DataSet requested scope/count/budget + optional GPU profile/Caps
````

### E025 · 04_类型化存储与GPU布局.md

理由：裁决 C。

原文：

````text
Feature declarations + PassSchema accesses + RenderGraph logical C0–C8
````

修订：

````text
Cold Feature Composition resolves DataSet/Capability requirements
             ↓ typed logical views contribute PassSchema/Graph declarations
RenderGraph logical C0–C8 (no DataSet registry lookup)
````

### E026 · 04_类型化存储与GPU布局.md

理由：独立复核 S01：解析需求先于接口形状冻结，与用户裁决顺序严格一致。

原文：

````text
DataSchema<T> + DataSet requested scope/count/budget + optional GPU profile/Caps
             ↓ derive **logical StorageInterfaceShape** (GPU ABI/format,
               addressability/capacity guarantees) WITHOUT allocating VkBuffer
Cold Feature Composition resolves DataSet/Capability requirements
             ↓ typed logical views contribute PassSchema/Graph declarations
RenderGraph logical C0–C8 (no DataSet registry lookup)
````

修订：

````text
DataSchema/type registration + Scope declarations + requests/budget + optional GPU profile/Caps
             ↓ cold Feature Composition resolves DataSet/Capability/Writer requirements
             ↓ freeze neutral StorageInterfaceShape + structural BindingSlotKey
               (GPU ABI/format/addressability/capacity guarantees; no VkBuffer allocation)
             ↓ resolved typed logical views contribute PassSchema/Graph declarations
RenderGraph logical C0–C8 (no DataSet registry lookup)
````

### E027 · 04_类型化存储与GPU布局.md

理由：裁决 B。

原文：

````text
Graph C0–C8 计算实际访问后，再在 C9–C13 选择相容 Native 物理方案和分配 Backing
````

修订：

````text
Graph C0–C8 计算实际访问后，再在 C10 选择相容 Native 物理方案和分配 Backing；C9/C11/C12/C13 保持既有 Layout/执行配方/同步/装配职责
````

### E028 · 04_类型化存储与GPU布局.md

理由：裁决 D。

原文：

````text
`TypedStorage<T>` 的 CPU 端复用数据结构算法
````

修订：

````text
CPU-only `TypedStorage<T>` 无需 GPU ABI，其合法性由 CPU 存储策略的生命周期约束决定；需要 GPU view 才应用可选 GPU Layout/Representation 合同。`TypedStorage<T>` 的 CPU 端复用数据结构算法
````

### E029 · 04_类型化存储与GPU布局.md

理由：裁决 D：CPU-only 不被迫进入 Graph。

原文：

````text
## 4.3 `StoragePlan` 的冷态确定流程
````

修订：

````text
## 4.3 `StoragePlan` 的冷态确定流程

下述 Graph/Native 路径只适用于需要 GPU 表示的集合；纯 CPU-only DataSet 只选择合法 CPU 策略，不制造 GPU shape/backing 或伪 Pass。
````

### E030 · 04_类型化存储与GPU布局.md

理由：示例字段与 LightGpu 声明一致。

原文：

````text
value.intensity = 3.5f
````

修订：

````text
value.color_intensity[3] = 3.5f
````

### E031 · 05_通信与发布事务.md

理由：裁决 D。

原文：

````text
三个契约来自同一源类型和生成 metadata 但职责不同
````

修订：

````text
三个可选契约复用同一现有 generator 但职责不同：CPU-only 不要求 Wire/GPU；Wire-only 不要求 GPU；需要 Shader-visible 数据才生成并验证 GPU 表示
````

### E032 · 06_Feature组合与数据依赖.md

理由：裁决 C。

原文：

````text
Type-safe **logical** DataSet slots + initial compatible interface shape;
````

修订：

````text
Type-safe **logical** DataSet slots + frozen neutral StorageInterfaceShape;
````

### E033 · 06_Feature组合与数据依赖.md

理由：裁决 B/C。

原文：

````text
RenderGraphDefinition → compileLogicalGraph [C0–C8]
````

修订：

````text
RenderGraphDefinition → existing compileLogicalGraph [C0–C8]
       ↓ [C1 graph producers/semantics/import-export; C2 pass resource interfaces only]
````

### E034 · 06_Feature组合与数据依赖.md

理由：裁决 B。

原文：

````text
native GPU backing + shader/layout + resource/queue/sync/record [C9–C13]
````

修订：

````text
C9 Layout/Shader; C10 physical backing; C11 native pass recipes;
C12 queue/sync; C13 executable assembly [existing responsibilities]
````

### E035 · 06_Feature组合与数据依赖.md

理由：裁决 C。

原文：

````text
Generated PassSchema<T> 将 Shader Buffer/Image 字段映射成确切 DataSet 的 Buffer/Image range、read/write、stages 和 producers。
````

修订：

````text
Composition 用已解析 Typed DataView 与 Generated PassSchema 将字段降低成中立 Graph Buffer/Image range、read/write、stages 和 producers。Graph 不查 DataSet Registry；实际实例/backing 留到 Invocation 绑定。
````

### E036 · 07_RenderGraph推导与编译.md

理由：裁决 B/C：恢复 14 阶段，消除 Graph/Data 依赖倒置。

原文：

````text
## 7.2 必须保留 C0–C8 语义

| 阶段 | 输入/输出/证明 |
|---|---|
| C0 Canonicalize | 多 Feature 贡献的资源、pass、owner identity、namespace、range 规范化；不靠注册顺序生效 |
| C1 Data/Provider Resolve | 必需/可选 Type/Key/Scope/Schema/Capability 绑定；若有 GPU producer，具体版本确定 |
| C2 Validate | 生成的 PassSchema 角色与 DataSchema GPU ABI、shader interface、writer capability、fallback/type/range 完全一致 |
| C3 Build versions/hazards | Buffer byte ranges、Image aspect/mip/layer，RAW/WAW/WAR、LOAD/RESOLVE、条件与导入初始事实 |
| C4 Detect ambiguity/cycles | 必须同时检查生产者与执行图；不选择“最新注册的 Writer” |
| C5 Cull | 输出 Present/Readback/Export/external writes/feedback 等有真实副作用的根，保留其有用生产者 |
| C6 Lifetime/usage | 记录逻辑资源版本、life interval、实际 usage roles、alias 候选，**不是 GPU 完成** |
| C7 Scope-sharing proof | Scene/View/Target、Camera/History/Time/Revision/Side Effect 的共享合法性 |
| C8 Deterministic schedule/cache identity | 中立不可变 `LogicalGraphPlan`，完整 equality，不以冲突 hash 为最终证明 |

现有 F2 的大部分实现已经具备，RD2 的新增责任集中在 **C1 DataSet resolution, C2 Schema/owner reconciliation 与复用缓存键**，而不是重写版本图算法。

## 7.3 F4 C9–C13 继续唯一执行链

```text
C9 Shader/Layout compilation: DataSet full owner shape + PassSchema + SPIR-V identity
C10 Physical storage planning: resolve NativeStoragePlan against C0–C8 actual usages under
    pre-frozen logical StorageInterfaceShape; persistent DataSet backing vs transient allocations
C11 Barrier/state/alias planning: exact ranges, cross-frame/cross-queue, queue family,
    real memory alias/disjointness and external backing ready
C12 Queue schedule/indirect/record recipes: typed GPU operations, release/acquire,
    precompiled descriptor + draw/dispatch/copy/readback operations
C13 Executable candidate: immutable recipes + bounded mutable InvocationSlots,
    one native plan owner, atomic publish and completion-proven retirement
```

Compilation 的数据种类输出：`LogicalGraphIdentity`、`StorageBindingIdentity`、`LayoutIdentity`、`NativePipelineIdentity`、`ExecutableGraphPlan`。它们可以不同层共享不变量，但不能混成只靠 `GraphPlan.matches()` 判等的“通用 Cache Key”。`DataSet revision` 通常不改变 topology，`DataType ABI/layout revision` 与 Native backing 兼容形状可能使 cold key 失效。**具体 Scene/View 的 DataSetId 和 Native Buffer 句柄不写入共享 Logical/Shader Layout cache key**：编译使用结构性的 `BindingSlotKey/StorageCompatibilitySignature`，每次 Invocation 才将槽绑定到合法 Dataset/Backing。Camera 值、Frame Serial、当前 import native handle 通常是 invocation 数据，不能每次使 Shader 重编译。


````

修订：

````text
## 7.2 完整保留 C0–C8 编号和职责

DataSet/Feature Requirement/Capability 的解析在冷态 Feature Composition 完成；它输出已绑定 DataSlot/View 与中立 StorageInterfaceShape，再贡献 Graph 资源和 Pass。Graph 不依赖 DataStorage、Runtime registry 或业务类型。

| 阶段 | 输入/输出/证明 |
|---|---|
| C0 Canonicalize | Graph 作者声明 → owning IDs、typed resources/pass、normalized ranges |
| C1 Resolve | Graph 内 required/optional producer、semantic provider、import/export、fallback → 确切图资源；不查 DataSet Registry |
| C2 Validate Interfaces | PassSchema 与 Graph/Shader 资源访问接口契约一致；业务 authority 在 Composition 先验证 |
| C3 Build Versioned Hazard Index | exact buffer/image ranges、显式 producer/version/order → RAW/WAR/WAW 与初始化证明 |
| C4 Topological Schedule | 确定性拓扑；歧义/循环包含 pass/resource/path；不靠地址或注册顺序 |
| C5 Root/SideEffect Reachability | 从真实 present/readback/export/external effects 保留可达有用 Pass |
| C6 Lifetime/Usage | 子资源/版本逻辑 first/last use、有效 usage、alias 候选；不证明 GPU completion |
| C7 Scope & View Group Proof | Scene/View/Target 输入、revision/epoch/time/side effect 的合法共享证明 |
| C8 Emit LogicalPlan | 唯一中立、拥有型、不可变逻辑结果及诊断/完整缓存比较事实 |

RD2 不新增第二套调度器，不重排已验收 F2 阶段。DataSchema/GPU ABI 的冷态对账复用已有生成器与 LayoutPlan，Graph C2 接收中立接口事实，不访问业务 catalogue。

## 7.3 完整保留 C9–C13 编号和职责

| 阶段 | 输入/输出/证明 |
|---|---|
| C9 Layout/Shader | 完整结构性 owner shape、PassSchema、Shader 与 DeviceCaps → LayoutPlan、重定位且验证的 SPIR-V |
| C10 Physical resources | logical usage/lifetime 与预冻结 StorageInterfaceShape → 相容 native backing/placement、import/export/alias 物理约束；不晚改 Shader ABI |
| C11 Native pass execution | Shader variant 与 native pass contract → 完整 PSO、不可变录制/绑定配方；无 lazy creation |
| C12 Queue/sync | exact hazards、stage/access、queue/native overlap 与 external readiness → BarrierProgram、ownership/wait/signal 与提交调度 |
| C13 Executable assembly | 完整 C9–C12 候选 → ExecutableGraphPlan、native owners/code pins/bounded invocation 状态与退休合同 |

Compilation 各层 identity 保持其真实职责。Shader/Layout 结构性缓存键包含 BindingSlotKey、完整 Schema/GPU Layout/Storage compatibility、Shader/Device/Target 等必要事实；**不包含具体 DataSetId、普通 dirty revision、Camera 数值、FrameSerial 或当前 native handle**。完整字段比较最终确认相容性，hash 不能单独证明相等。

Invocation 才将结构槽绑定到实际 DataSet/NativeBackingGeneration，并验证 owner、generation、Ready、last-use。改变 GPU ABI、资源形状或设备能力时按既有 native key 产生新候选。跨 View 共用计划不等于跨 View 共用可变资源或无条件只执行一次 GPU 工作。


````

### E037 · 07_RenderGraph推导与编译.md

理由：裁决 C。

原文：

````text
及真正 Owner/Ready 引用
````

修订：

````text
及结构性 owner slot；真正 DataSet/backing/Ready 在 Invocation/Native Import 绑定
````

### E038 · 07_RenderGraph推导与编译.md

理由：独立复核 S02：保留唯一类型域。

原文：

````text
GPU DataTypeId 不进入 Graph 通用变体
````

修订：

````text
业务 RenderDataTypeId 不进入 Graph 通用变体；不创建平行 GPU Type ID
````

### E039 · 07_RenderGraph推导与编译.md

理由：独立复核 S02：明确在 Graph 之外降低。

原文：

````text
在 Graph Composition 期间
````

修订：

````text
在 Graph 编译之前的冷态 Feature Composition 期间
````

### E040 · 08_Shader与布局自动生成.md

理由：裁决 D。

原文：

````text
`DataSchema<T>`：稳定数据类型/字段/存储布局、Schema Fingerprint。适用于 CPU 数据、GPU Buffer Record、Chunk Metadata；不声明某个 Pass 的 access。
````

修订：

````text
`DataSchema<T>`：稳定数据类型/字段/CPU 存储约束、Schema 兼容性；不声明某个 Pass 的 access。GPU-facing 数据另由同源可选 GpuLayout/GpuRepresentation 描述 Shader ABI，CPU-only 不要求 GPU Layout。
````

### E041 · 08_Shader与布局自动生成.md

理由：与 F6.b 的正式责任一致。

原文：

````text
F6 前以两个独立的共享 Owner
````

修订：

````text
F6 内以两个独立的共享 Owner
````

### E042 · 08_Shader与布局自动生成.md

理由：独立复核 S03：更新频率类别不冒充 Scope owner。

原文：

````text
二者都是 `scope=feature`
````

修订：

````text
二者落入旧 `OwnerAssignment.feature` 类别（不是 DataSet 的实际 Scope）
````

### E043 · 08_Shader与布局自动生成.md

理由：裁决 D。

原文：

````text
同源生成的布局声明与函数
````

修订：

````text
同源生成的可选 GPU 表示、布局声明、必要打包转换与函数
````

### E044 · 10_Runtime多Scene多ViewTarget.md

理由：继承终态失败。

原文：

````text
Device Lost 触发明确终止/恢复政策
````

修订：

````text
Device Lost 令当前 Runtime 终态失败；宿主只能按后续授权另建 Runtime，不在此引入自动恢复状态机
````

### E045 · 12_Legacy功能完整迁移.md

理由：纠正 86 项与高级算法阶段边界。

原文：

````text
这些行的完整端到端闭环到 F11 产品切换为止
````

修订：

````text
F01–F36/I01–I44 的完整端到端闭环到 F11 产品切换为止；H01–H06 的算法分别由 F11 之后的 H1–H6 验收
````

### E046 · 12_Legacy功能完整迁移.md

理由：避免第二 Editor frame loop 歧义。

原文：

````text
独立 Editor frame
````

修订：

````text
Editor 独立于 Simulation 的渲染进度，仍使用唯一 FrameDriver
````

### E047 · 14_Cpp20类型和编码纪律.md

理由：与分层目标 Concept 一致。

原文：

````text
`template<RenderData T>`
````

修订：

````text
`template<DescribedData T>`
````

### E048 · 14_Cpp20类型和编码纪律.md

理由：裁决 D。

原文：

````text
Generated Shader Data structs 需满足真实布局，不可凭 `trivially_copyable` 断言可以直接跨 DLL/Shader memcpy。
````

修订：

````text
仅 Shader-visible representation 需满足经生成器/反射核对的 GPU 布局。CPU-only T 不要求平凡复制或 standard-layout；Wire-only 遵守既有 PacketValue/codec。不凭 trivially_copyable 断言可以直接跨 DLL/Shader memcpy。
````

### E049 · 14_Cpp20类型和编码纪律.md

理由：LightGpu 字段一致。

原文：

````text
x.intensity = 4
````

修订：

````text
x.color_intensity[3] = 4
````

### E050 · 14_Cpp20类型和编码纪律.md

理由：保留仓库 OOM 约定。

原文：

````text
- `RenderResult` 用于可预期的 invalid type/range/owner/DeviceCaps/Capacity/Busy/Ready error；
````

修订：

````text
- 普通 new/STL OOM 继续为 fatal，不新增可恢复 ALLOCATION_FAILURE 或热路径 catch。`RenderResult` 用于可预期的 invalid type/range/owner/DeviceCaps/Capacity/Busy/Ready error；
````

### E051 · 14_Cpp20类型和编码纪律.md

理由：不新增与仓库 fatal/OOM 相冲突的规则。

原文：

````text
`std::terminate` 只适用于内部已通过验证、理论不可到达的不变量破坏，且须有独立反证测试
````

修订：

````text
除普通堆 OOM 的既有 fatal 合同外，不可恢复的内部不变量破坏使用已有 fatal 出口（不依赖 release assert），并有独立反证测试
````

### E052 · 15_端到端API示例.md

理由：裁决 D。

原文：

````text
现有 Generator、MSVC C++20、安装 SDK
````

修订：

````text
现有 Generator、MSVC C++20、分离 CPU/Wire/GPU 合同、安装 SDK
````

### E053 · 15_端到端API示例.md

理由：裁决 D。

原文：

````text
// 原始 C++ sizeof(SpriteGpu)=48；真实 ABI 仍需 generator SPIR-V 对账。
````

修订：

````text
// 目标 Shader-visible representation 为 48 字节；实际 GPU ABI 需生成器/反射核对。
// 这不要求所有 CPU-only 类型具有此布局，也不将 CPU bytes 默认视为 Wire bytes。
````

### E054 · 15_端到端API示例.md

理由：避免把实例写入布局缓存。

原文：

````text
实际 Owner 依据 DataSetRef，而不是单个 feature_owner 枚举槽
````

修订：

````text
结构性 Owner 依据 Schema/semantic/scope/full shape；实际 DataSetRef 留到 Invocation
````

### E055 · 17_完整验收与正确性矩阵.md

理由：裁决 D。

原文：

````text
一份类型生成 C++/GLSL/Wire/Layout Fingerprint、Type Collision/ABI compile negatives
````

修订：

````text
一份源类型按实际用途生成 CPU Schema、可选 Wire/GLSL/GPU Layout；三类独立正负例与 Type Collision/ABI 检查
````

### E056 · 17_完整验收与正确性矩阵.md

理由：裁决 D。

原文：

````text
`LightGpu`/`SpriteGpu` 复杂 align/array/struct；生成 GLSL/SPIRV；错 schema/version/stride
````

修订：

````text
CPU-only 非平凡类型、Wire-only 显式编码、LightGpu/SpriteGpu GPU 表示分别验证；GPU align/array/struct 与 GLSL/SPIRV 对账；错 schema/version/stride
````

### E057 · 18_阶段计划与交付门禁.md

理由：权威状态。

原文：

````text
D0 文档设计门禁（doc only; 仅批准设计）
````

修订：

````text
D0 文档设计门禁（doc only; 候选文档资格不等于用户批准架构）
````

### E058 · 18_阶段计划与交付门禁.md

理由：与 F5 工作单两 Scene 共享资产一致。

原文：

````text
1 Scene×2 shared Asset
````

修订：

````text
2 Scenes×1 shared Asset
````

### E059 · 18_阶段计划与交付门禁.md

理由：裁决 D。

原文：

````text
portable wire/layout/ABI
````

修订：

````text
按用途可选的 wire/GPU layout 合同；CPU-only 不强制 GPU ABI
````

### E060 · 18_阶段计划与交付门禁.md

理由：阶段义务不提前关闭。

原文：

````text
F11 冻结全部 86 ID 原责任与 Editor/Player/Toolchain/Installed SDK product cutover；H1–H6
````

修订：

````text
F11 完成 F01–F36/I01–I44 及 Editor/Player/Toolchain/Installed SDK product cutover，保留全部 86 ID 追踪；H01–H06 不能在 F11 提前关闭，H1–H6
````

### E061 · 19_冻结设计决策RD2.md

理由：裁决 D。

原文：

````text
增加 DataSchema↔PassSchema 对账
````

修订：

````text
增加可选 GPU Layout/Representation↔PassSchema 对账，CPU-only/Wire-only 分别验证
````

### E062 · 19_冻结设计决策RD2.md

理由：裁决 D。

原文：

````text
C++20 `TypedStorage<T>`
````

修订：

````text
C++20 `TypedStorage<T>`（CPU-only 仅按真实生命周期策略约束，不要求 GPU ABI）
````

### E063 · 19_冻结设计决策RD2.md

理由：裁决 B/C。

原文：

````text
StoragePlan 冷态根据数据约束+真实 Pass Access+Caps 决策，非法形状明确拒绝
````

修订：

````text
Composition 先冻结中立 StorageInterfaceShape；原 C10 根据实际 Pass Access+Caps 决定相容 backing，不晚改 ABI，非法形状明确拒绝
````

### E064 · 19_冻结设计决策RD2.md

理由：裁决 C。

原文：

````text
Feature 先声明所需/产生的 Type/Key/Scope/Access/Capability，Graph 再由真实 PassSchema 使用建立依赖
````

修订：

````text
Feature Composition 先解析 Type/Key/Scope/Access/Capability，降低成 typed Graph 声明；原 C1 只解析图内关系，原 C2 只验证 Pass 接口，版本算法从真实访问建立依赖
````

### E065 · 19_冻结设计决策RD2.md

理由：裁决 D。

原文：

````text
Shader Structured Type 与 C++ DataSchema 同一 generator/ABI fingerprint
````

修订：

````text
CPU Schema、可选 Wire 编码和可选 Shader GPU representation 由同一现有 generator 生成；GPU 布局另验证 fingerprint，不改 Core descriptor 含义
````

### E066 · 19_冻结设计决策RD2.md

理由：裁决 D。

原文：

````text
DataSchema/Typed DataSet views
````

修订：

````text
DataSchema/Typed DataSet views 及按用途可选的 Wire/GPU 合同
````

### E067 · 19_冻结设计决策RD2.md

理由：裁决 A。

原文：

````text
另外 DataSetId 和 RecordHandle 是实例身份，不合并
````

修订：

````text
另外 DataSetId 为 Scope/Runtime-local 实例身份、RecordHandle 受 DataSet 约束；既有 renderDataTypeId(canonical_name) 与版本字段不改
````

### E068 · 19_冻结设计决策RD2.md

理由：裁决 B/C。

原文：

````text
DataSet 不重写 Graph 算法
````

修订：

````text
DataSet/Feature 解析在 Composition；不改原 C0–C8 职责或 Graph 算法
````

### E069 · 19_冻结设计决策RD2.md

理由：裁决 B。

原文：

````text
唯一 Native Compiler C9–C13，不冒称产品已可用
````

修订：

````text
唯一 Native Compiler C9–C13；C11=Native pass execution、C12=Queue/sync，保持 FINAL 分工，不冒称产品已可用
````

### E070 · 20_原设计迁移对照.md

理由：裁决 B/C。

原文：

````text
C1/C2 的输入解析扩展，不重写依赖算法
````

修订：

````text
Composition 先解析 DataSet/Feature，降低中立声明；原 C1/C2 职责与 C0–C13 编号保持，不重写依赖算法
````

### E071 · 20_原设计迁移对照.md

理由：裁决 D。

原文：

````text
增加 DataSchema GPU ABI
````

修订：

````text
增加可选 GPU Layout/Representation，不要求 CPU-only 满足 GPU ABI
````

### E072 · work_orders/01_D0_文档架构冻结工作单.md

理由：裁决 D：限定 GPU-facing，不误拒绝 CPU-only。

原文：

````text
DataSchema 可以不验证 GPU Shader ABI
````

修订：

````text
Shader-visible representation 可以不验证 GPU Shader ABI
````

### E073 · work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md

理由：裁决 D：直接修正文。

原文：

````text
GPU field ABI (`offsetof`, `stride`, `align`, type`, matrix major`)、C++ `DataSchema<T>` Traits、GLSL data representation，以及必要的 wire type schema/descriptor
````

修订：

````text
C++ DataSchema<T> Traits；CPU-only 按真实存储生命周期约束，跨线程/ABI 时才生成既有 PacketValue/codec 所需 Wire schema，Shader-visible 时才生成可选 GPU Layout/Representation（offset/stride/alignment/type/matrix major）、GLSL 与必要打包转换
````

### E074 · work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md

理由：裁决 D。

原文：

````text
TypeID 碰撞、同名 layout/version mismatch、invalid std430/struct, illegal pointer, unsupported arbitrary T 均 fail
````

修订：

````text
TypeID 碰撞和不兼容版本须 fail；invalid std430/struct 仅适用于 GPU-facing 表示；裸指针/任意 C++ T 不得绕过 Wire/GPU 合同，合法 CPU-only 非平凡类型不因此被拒绝
````

### E075 · work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md

理由：裁决 C。

原文：

````text
冷态 composite candidate transaction
````

修订：

````text
冷态 Composition 在 Graph 前解析 DataSet/Capability/权限，降低 typed Graph 声明，原 C1/C2 只解析图内关系和验证 Pass 接口；完整候选事务
````

### E076 · work_orders/05_F7_TypedStorage_GPUResident_预备范围.md

理由：裁决 D。

原文：

````text
template<RenderDataRecord T>
````

修订：

````text
template<DescribedData T>
````

### E077 · work_orders/05_F7_TypedStorage_GPUResident_预备范围.md

理由：裁决 D。

原文：

````text
小而可复用的 slot/dirty算法
````

修订：

````text
CPU-only T 仅受真实存储生命周期约束，GPU-facing view 另受同源 GPU Layout/Representation 验证；小而可复用的 slot/dirty算法
````

### E078 · README.md

理由：候选版本。

原文：

````text
**2026-10-11 · 设计候选
````

修订：

````text
**RD2-D0-C2 · 2026-10-11 · 已按用户裁决修订的设计候选
````

### E079 · README.md

理由：补充可审阅入口。

原文：

````text
## 读者入口
````

修订：

````text
## 本轮裁决与文档资格入口

- [逐条正文勘误与哈希](RD2_D0_ERRATA.md)
- [决策迁移与 C0–C13 对照](RD2_DECISION_MIGRATION.md)
- [一致性审阅与目标 API](RD2_CONSISTENCY_AUDIT.md)
- [逐项要求与类型追踪](RD2_TRACEABILITY.md)
- [阶段准入与未完成项](RD2_IMPLEMENTATION_GATE.md)
- [来源与保护清单](RD2_PROVENANCE.md)
- [用户歧义裁决原文](work_orders/00_D0_用户歧义裁决_只读.md)

D0 独立报告在 Verification 提交中新增；文档 PASS 不激活候选架构。

## 读者入口
````

### E080 · AUDIT_来源与一致性.md

理由：原包审计与当前资格分离。

原文：

````text
# RD2 原来源、机器校验与身份说明
````

修订：

````text
# RD2 原来源、机器校验与身份说明

候选修订 RD2-D0-C2。下列原包审计是来源声明，不是本轮独立 Verification；本轮裁决、来源与检查分别见 RD2_D0_ERRATA、RD2_PROVENANCE 和后续独立报告。
````

### E081 · AUDIT_来源与一致性.md

理由：裁决 D。

原文：

````text
`DataSchema`（类型/ABI）
````

修订：

````text
`DataSchema`（语义类型及按用途可选的 Wire/GPU 表示）
````

### E082 · AUDIT_来源与一致性.md

理由：裁决 B/C。

原文：

````text
先定义逻辑 StorageInterfaceShape，Feature 写 Graph → Graph C0–C8 → C9–C13 物理 StoragePlan/Native.
````

修订：

````text
冷态 Composition 先解析 DataSet/Feature 并冻结逻辑 StorageInterfaceShape，Feature 写 Graph → 原 C0–C8 → 原 C9–C13（C10 物理 backing；C11 native recipes；C12 queue/sync）。
````

### E083 · AUDIT_来源与一致性.md

理由：文档资格不冒充构建。

原文：

````text
准确的仓库 File API / frozen legacy 核对
````

修订：

````text
准确的 Git tree/文档来源/frozen legacy 核对；D0 不重新运行生产 CMake File API 或构建
````
