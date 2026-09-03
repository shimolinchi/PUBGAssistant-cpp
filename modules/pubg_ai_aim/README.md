# pubg_ai_aim

`pubg_ai_aim` 是根据 LOAI 静态反编译结果重建的独立 C++20 AI 瞄准库。

完整迁移使用说明见 [`USAGE.md`](USAGE.md)，未来接线边界见
[`INTEGRATION_NOTES.md`](INTEGRATION_NOTES.md)。

当前目录是一个完整、可单独配置和测试的 CMake project；公共 API 不暴露 Qt、OpenCV、
ONNX Runtime、DirectX 或 `PUBGAssistant-cpp` 的内部类型。

本目录不携带预编译的 `pubg_ai_aim.lib`。静态库始终由目标工程自己的 MSVC、C++20、
Windows SDK 和 CMake 环境从源码生成。模型是运行时数据；目标工程目前没有 ONNX Runtime
依赖，因此随模块保留一套私有、动态加载的 ORT/DirectML runtime，避免与目标工程的
OpenCV/vcpkg ABI 混用。

首次接入及公共头文件升级后必须对本模块使用全新构建目录，不能复用旧 `.obj/.lib`；公共
结构布局与陈旧调用方产物不一致时会造成内存访问异常。

## 当前状态

已实现：

- 纯 C++ YOLOv8 `[1,6,2100]` 解码；
- 原版兼容 NMS 和修正后的浮点、按类别 NMS；
- 目标类别过滤、候选评分、IoU 关联、粘滞和丢失恢复；
- 类别瞄准点、框形状横向修正、开火 Y 偏移；
- DMR + lead boost 的 body 类强制、ADS/倍镜灵敏度 PID 增益；
- 速度死区、目标跳变、短时保持、武器/倍镜/小目标/DMR/自动武器提前量；
- lead 平滑、逐帧限步、有效范围；
- FOV 换算、原版非标准 PID、最大移动量和 debounce；
- 获取初期 `.45/.65/.82/1` 渐入；
- 外部检测帧推送模式，容量为 1，拒绝过期/倒退的 sequence 与 timestamp；
- `IFrameSource`、`IObjectDetector`、`IMotionSink`、可选 `ITriggerSink`；
- DXGI Desktop Duplication 中心 ROI 截图；
- 动态加载 ONNX Runtime 1.15 C API，优先 DirectML，失败回退 CPU；
- 自动开火接口默认关闭；
- 独立单元测试、外部检测示例和 ORT 模型 smoke test。

仍需在真实游戏中验证：

- 目标运动可靠性状态的每个逐帧边界只能做到语义等价，原始枚举名与训练回放数据不存在；
- 当前内置预处理在输入不是 320x320 时使用最近邻缩放；DXGI 默认正好输出 320x320，
  因此对应当前模型路径不会发生缩放；
- 根项目已经复用现有武器/配件/姿态识别、热键和统一鼠标 Dispatcher 完成接线；仍需用
  实际 PUBG 画面验证目标跟踪、前台切换和菜单游标判定；
- 其他宿主接入时，仍必须把真实游戏前台、背包/地图/菜单状态写入 `ActivationState`。

## 模块边界

这个库位于：

```text
modules/pubg_ai_aim/
```

核心库仍不依赖根项目的 Qt、JSON、识别器或输入实现。根项目通过宿主适配器、Coordinator
和共享 Dispatcher 接入；具体边界及运行时资产契约见 [`INTEGRATION_NOTES.md`](INTEGRATION_NOTES.md)。

## 独立构建

在 `C:\Projects\PUBGAssistant-cpp` 下执行：

```powershell
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 && cmake -S modules\pubg_ai_aim -B .build\pubg_ai_aim-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPUBG_AI_AIM_BUILD_TESTS=ON -DPUBG_AI_AIM_BUILD_ORT_SMOKE_TEST=OFF && cmake --build .build\pubg_ai_aim-debug --parallel'
```

运行测试：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir .build\pubg_ai_aim-debug --output-on-failure
```

主要目标：

```text
pubg_ai_aim                         静态库
pubg_ai_aim::pubg_ai_aim            CMake alias
pubg_ai_aim_core_tests              纯算法与线程安全测试
pubg_ai_aim_ort_smoke               ORT/模型 smoke（需单独开启对应选项）
pubg_ai_aim_external_example        外部检测帧示例
```

## 外部检测结果模式

这个模式可直接接现有或未来的检测器，不要求库拥有截图/模型：

```cpp
#include <pubg_ai_aim/pubg_ai_aim.hpp>

namespace aim = pubg::ai_aim;

class ProjectMotionSink final : public aim::IMotionSink {
public:
    void submitAimMotion(const aim::MotionCommand& command) override {
        // 把高精度 dx/dy 提交给未来的 MouseMotionDispatcher。
        // 不建议从 AI 线程直接与 RecoilControl 并发 SendInput。
    }
};

auto config = aim::AimConfig::correctedDefaults();
auto sink = std::make_shared<ProjectMotionSink>();
aim::AimModule module(config, sink);

aim::ActivationState activation;
activation.enabled = true;
activation.game_foreground = true;
activation.aim_primary = true;
module.updateActivation(activation);
module.start();

aim::DetectionFrame frame;
frame.sequence = 1;
frame.timestamp = aim::Clock::now();
frame.roi_left = 800;
frame.roi_top = 380;
frame.roi_width = 320;
frame.roi_height = 320;
frame.screen_width = 1920;
frame.screen_height = 1080;
frame.screen_center_x = 960;
frame.screen_center_y = 540;
frame.detections.push_back({
    aim::Box::fromCenter(160, 160, 40, 80), 0.9F, 1
});
module.submitDetections(std::move(frame));
```

## DXGI + ORT/DirectML 模式

`runtime/` 中附带了与重建样本匹配的运行时和模型：

```cpp
aim::DxgiFrameSourceConfig capture_config;
capture_config.roi_width = 320;
capture_config.roi_height = 320;
capture_config.preferred_output = aim::DesktopRect{-1920, 0, 1920, 1080};

aim::OrtDmlDetectorConfig detector_config;
detector_config.runtime_library =
    R"(modules\pubg_ai_aim\runtime\onnxruntime.dll)";
detector_config.model_path =
    R"(modules\pubg_ai_aim\runtime\PUBGV8.onnx)";
detector_config.prefer_directml = true;
detector_config.allow_cpu_fallback = true;
detector_config.dml_device_index = 0;

auto capture = aim::makeDxgiFrameSource(capture_config);
auto detector = aim::makeOrtDmlDetector(detector_config);
aim::AimModule module(
    aim::AimConfig::correctedDefaults(),
    std::move(capture),
    std::move(detector),
    motion_sink);
```

`DirectML.dll` 必须与 `onnxruntime.dll` 放在同一目录。若 DirectML provider 初始化失败，
adapter 会继续用 ORT CPU provider 创建 Session。

`preferred_output` 使用 Windows 物理桌面坐标匹配 `DXGI_OUTPUT_DESC::DesktopCoordinates`；
先精确匹配矩形，再匹配包含矩形中心点的输出，最后才回退到 `adapter_index/output_index`。
构造 `AimModule` 后，经过 sanitize 的 `AimConfig::detector` 和 `compatibility` 是内置 ORT
解码器的权威配置，会在第一次推理前和每次 `updateConfig` 后同步。

示例相对路径按进程工作目录解析。正式接入时应通过目标项目的资源路径工具生成绝对路径，
或把 runtime 复制到可执行文件旁。`cmake --install` 只安装静态库、头文件和 CMake package，
不会自动分发模型/DLL。

## 武器识别如何接入

不要运行反编译出的 Python 模板识别器。`PUBGAssistant-cpp` 已有：

```text
EquipmentDetector  -> 两槽武器、倍镜、握把、枪口、枪托
WeaponDetector     -> 当前手持武器
GestureIdentifier  -> 姿态
```

调用者只需把结果转换为：

```cpp
aim::CombatContext context;
context.revision = revision;
context.weapon = current_weapon;
context.scope = slot.scope;
context.grip = slot.grip;
context.muzzle = slot.muzzle;
context.stock = slot.stock;
context.stance = stance;
context.scope_zoom = recognized_scope_zoom;
context.ads = ads;
context.firing = firing;
module.updateCombatContext(std::move(context));
```

库内置别名规范化，例如 `berry/beryl/m762 -> m762`、`scar/scarl -> scarl`。

## 安全门控和线程保证

只有以下条件全部成立才会抓帧、推理并调用 motion sink：

```text
enabled
game_foreground
aim_primary 或 aim_secondary
非 paused
非 blocking_ui_open
```

- 最新检测帧容量为 1；旧帧覆盖，不积压；
- 外部帧的 sequence 和 timestamp 必须非零并严格递增，且 timestamp 不得超前或超过
  `tracking.max_frame_age_ms`（默认 250 ms）；
- config/context/activation 通过短锁复制；
- 不持状态锁调用外部 sink；
- sink 回调可调用 `setEnabled`、`updateActivation` 或 `stop`，但不得在回调内销毁模块；
- 停用、暂停、失焦或 UI 阻塞时清除跟踪/预测/PID；
- `stop()` 返回后 worker 已 join，不会再回调 sink；
- 同步 ORT `Run` 已开始时，外部 `stop()` 最长需要等待当前推理返回；
- 持续抓帧/推理错误使用 20 ms 起、最高 1 s 的指数退避；成功帧会清除旧错误；
- auto fire 默认关闭，且没有 `ITriggerSink` 时不会点击。

`AimSnapshot::detector_backend` 报告实际 provider：内置 ORT 为 `DirectML` 或 `CPU`，
外部检测帧模式为 `external`，未覆写状态接口的自定义 detector 为 `custom`。

## Original 与 Corrected

`Original` 保留有输出意义的原版行为：

- YOLO 坐标 ties-to-even 整数化；
- NMS 错把中心当左上角并跨类别抑制；
- 每帧位移预测；
- 非标准 PID；
- 原版武器和倍镜经验倍率。

`Corrected` 默认使用：

- 浮点 xyxy；
- class-aware NMS；
- 实际帧间隔归一化速度；
- 以目标框中心估速，避免开火/瞄准点偏移产生虚假速度；
- 安全不可变帧所有权；
- DXGI timeout 不重复旧帧。

即使在 `Original`，也不会复刻数据竞争、COM 泄漏或旧帧重复推理。
`Original` 是已证实输出规则的兼容档，不代表未知的运动可靠性计数器已做到逐指令完全一致；
这一边界在“当前状态”中明确列出。

## 第三方与模型

见 `THIRD_PARTY_NOTICES.md` 和 `third_party/` 内随包许可证。

模型的 ONNX metadata 标记为 `AGPL-3.0`，但发行包没有提供权重来源、训练数据或完整授权链。
当前复制仅用于本地重建和验证；如果要发布或商用，必须先单独确认模型权重的实际许可。
