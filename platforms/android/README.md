# Android 外壳与 bring-up harness

把"我们能为 arm64 交叉编译出 21 个 `.so`"变成"引擎真的在手机上跑起来"。

```bash
python bootstrap/setup.py build --platform android
cmake --build ../build/Android/lux-engine --target luxsmoke -- -j 4
```
```bash
pwsh platforms/android/package.ps1 -Run
```

游戏壳使用同一个 `LUX_BUILD_PROFILE=PLAYER`，不是另一种 profile：

```bash
pwsh platforms/android/package.ps1 -Target game \
  -GamePak <game.luxpak> \
  -RuntimeManifest <game.luxruntime.toml> \
  -EnginePak <engine.luxpak>
```

Runtime manifest 必须由共享的 Resource deployment writer 生成，并为 APK 使用
`game.luxpak`/`engine.luxpak` 相对名。当前 Android adapter 尚未 stage 动态 extension，manifest
含 extension 时会硬失败。

> `platforms/` 不是 `modules/platform/`。后者是引擎**链接**的平台抽象层
> (window / gapi / dynamic_library);这里放的是**每个目标平台的 port** —— manifest、
> 打包、原生入口点 —— 引擎里没有任何东西链接它。Godot 的 `platform/` 划的是同一条线。
> iOS(Xcode 工程 + Info.plist + 打包脚本)将来落 `platforms/ios/`。

## 为什么零 Java

`android.app.NativeActivity` 由平台提供,入口 `ANativeActivity_onCreate` 由 NDK 的
`native_app_glue` 提供 —— 所以 APK 里没有 `.java`/`.kt`,也没有 dex
(`android:hasCode="false"`)。整个 app 就是 `android.app.lib_name` 指名的那个 `.so`。

这不是"图省事",是 bring-up 阶段的正确形状:`native_app_glue` **随 NDK 分发**
(`$NDK/sources/android/native_app_glue`,两个文件),而 GameActivity 要走
Gradle/Maven 拉 AndroidX 的 AAR。起步成本差一个数量级,而这一阶段要验的东西
(linker、反射、Vulkan、surface)一个都不需要 Java。

**什么时候会被迫上 Java 外壳**:软键盘 / IME、运行时权限对话框、SAF 文件选择器、
商店内购 SDK、Android 12+ 的 SplashScreen API。到那时换 GameActivity —— 它提供
`native_app_glue` 兼容层,`android_main` 的生命周期循环基本原样可迁,**只有输入要重写**
(`GameActivityMotionEvent` 取代 `AInputEvent`)。所以现在按 `native_app_glue` 写的
循环不是一次性投入。

## 为什么不用 Gradle 打包

`hasCode=false` 的 NativeActivity APK 只有两样东西:编译过的 manifest,和
`lib/<abi>/*.so`。这用 build-tools 里已有的三个工具就够了(aapt2 / zipalign /
apksigner),不需要 JDK、Gradle、AGP、Maven。`package.ps1` 干的就是这个。

**什么时候必须换成生成式(Gradle)**:出现多个 APK(各自的包名)、出现能往 manifest
塞片段的插件系统、权限随工程内容变、或者出现编译期可选且硬件要求超出 Vulkan 1.3
底线的 feature(那会改变 `<uses-feature>` 的最低设备档)。这四条现在一条都不成立,
所以 manifest 是一个静态文件,`package.ps1` 是打包步骤而不是代码生成器。

**原生库不在这里编译。** `bootstrap/lux_build.android.json` 中的
`LUX_BUILD_PROFILE=PLAYER`、vcpkg overlay 三元组、NDK chainload 与
`LUX_HOST_TOOLS_PREFIX` 指定的宿主工具
共同定义唯一构建图；`cmake --build <android-build-dir>` 只执行该图，
`package.ps1` 只按 CMake 为 `luxgame`/`luxsmoke` 生成的 `.runtime-files` 收集真实 target
闭包；它不会再把 build/bin 中历史残留的所有 `.so` 打进 APK。让 AGP 的
`externalNativeBuild` 去驱动 CMake 会
制造第二套真相源,而且要它复现上面那一整套配置是自找麻烦。

## harness 的形状:顺序 gate,不是 runtime player

每一步打一条判决,前一步过了才走下一步。**故意不是 runtime player** —— player 会把所有
未知数同时点燃,一个黑屏你分不清是哪一层。

| | 检查 | 状态 |
|---|---|---|
| 1 | `.so` 加载 + 进 `android_main` | ✅ |
| 2 | `ComponentTypeRegistry` 非空 | ✅ 25 个组件类型 |
| 3 | Vulkan 实例 + 设备限制 | ✅ |
| 4 | `ANativeWindow` → `initFromNative` → `VkSurfaceKHR` | 待做 |
| 5 | swapchain 清屏成纯色 | 待做 |
| 6 | 装 19 个 feature,看谁被拒 | 待做 |

## 首跑实测(2026-07-31)

设备 `haotian`,arm64-v8a,SDK 36。

```
loader instance version: 1.4.0
device[0] 'Adreno (TM) 830' api=1.3.284
  maxBoundDescriptorSets             = 7
  maxPerStageDescriptorUniformBuffers= 16777216
  maxPerStageDescriptorStorageBuffers= 16777216
  maxPerStageDescriptorSampledImages = 16777216
  maxPerStageDescriptorStorageImages = 16777216
  maxPerStageResources               = 50331648
```

- **Vulkan 1.3 成立** —— `ANDROID_PLATFORM=android-33` 这个假设(API 29 的 libvulkan
  不导出 1.2/1.3 入口)在真机上得到确认。
- **`maxBoundDescriptorSets = 7`**,不是 4。这是 Adreno,不是 Mali。引擎的最宽管线占
  4 套(见 `LayoutPlan.hpp` 的实测记录),所以这台机器上还有余量。设计仍然按 **4** 收敛
  —— 那是 Vulkan 的**规范最低保证**,同时覆盖 Mali;按本机 7 放宽就会重蹈"拿本机上限
  当预算"的覆辙。
- **per-stage 限制在这颗芯片上等于无限**(16M),所以合并 set 抬高 per-stage 计数这件事
  在 Adreno 上不构成约束。Mali 上未必,门禁仍按 Vulkan 最低保证判。

## gate [2] 第一次跑就抓到的问题

sidecar `.so` 加载了(`/proc/<pid>/maps` 确认四个 `*_meta.so` 都在),`registerType`
的引用也在,`.init_array` 也在 —— 但注册表是空的。

原因是 `MetaModuleRegistrar` 的构造函数**只把 init 函数挂进一条待处理链表**,自己不注册
任何东西;链表由 `lux::meta::meta_module_init()` 排空,每个宿主必须在所有库加载完之后
调一次(`LuxEditor.cpp:154` 就是这么做的)。harness 漏了这一步。

这个错法是**静默**的而不是响亮的:目录为空时 `Scene::load` 对每个组件都走
"类型未知就跳过"的分支 —— 文件加载成功,实体数量正确,而每个实体身上什么都没有。
