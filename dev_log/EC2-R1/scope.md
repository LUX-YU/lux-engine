# EC2 R1 验证范围

- 本轮只改 MaterialPreview 配方输入/采用、同主题私有场景构造及两份现有测试，更新准确 header provider。
- 默认球体和复用既有 quad 网格都经过公开 setDesired(input, recipe)。不增另一资源 owner，不改 Runtime/Process。
- 最终验证绑定 ead7e59514a5b9a5d0dc65307e7c9e2a792b69f4，独立 clean clone，全量 all、二次无工作。
- 定向 CTest 包含 compilation 三项、projection reset、scene_views_gpu、两项显式旧 GPU 模式、
  architecture_current、projection_compilation/desktop_view/ec2 实际依赖负例。其余未变路径引用原 EC2 记录，
  不把它们写成 R1 新运行结果。
- 新 SDK 验证 projection-compilation（含八项 operation 编译拒绝）、desktop-views（排除原生输入）、
  两项旧 GPU 消费者；公开 MaterialPreview.hpp 使用 clang-cl C++20 独立解析。
- 修前 SDK 公共配方编译失败证据保留；修后在新前缀对同一源编译。
- 原生输入继续延后；Linux、IME 不运行；不补旧性能长测。
