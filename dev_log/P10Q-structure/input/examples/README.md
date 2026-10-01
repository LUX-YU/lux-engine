# 独立说明性编译夹具

本目录只演示施工文档中的C++20约束和原同步阶段推进形状。Result是合成类型，不是lux-cxx；没有Session、Task、UI或插件。不得复制该Result到生产，也不得将运行结果写成SDK/引擎资格。

`InteractionDelivery.example.hpp`逐字提取自主文档代码块；正式实施仍需要当前真实lux-cxx的Result、MaterialView/FlowView以及基准行为回归。

NEGATIVE_CASE=0是正例；1–4分别为bool返回、错误Result域、仅右值可调用、非void Result。工具可用时的本地结果见evidence中的concept-illustration-selftest.json。
