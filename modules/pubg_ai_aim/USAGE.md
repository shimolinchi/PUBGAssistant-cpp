# pubg_ai_aim 使用说明

本文档随模块放置在：

```text
C:\Projects\PUBGAssistant-cpp\modules\pubg_ai_aim\USAGE.md
```

模块提供两种输入方式：调用者提交已有检测框，或由模块使用 DXGI 截图并运行
`PUBGV8.onnx`。两种方式共用目标选择、跟踪、预测、FOV/PID 和安全门控逻辑。
AI 瞄准只通过 `IMotionSink` 输出位移，不直接依赖或修改压枪模块。

## 1. 环境与文件

- C++ 标准：C++20；
- 构建器：使用 `PUBGAssistant-cpp` 当前的 MSVC、Windows SDK、CMake/Ninja；
- 公共接口：不暴露 Qt、OpenCV、ONNX Runtime 或 DirectX 类型；
- 模型：`runtime/PUBGV8.onnx`；
- 可选本机推理：`runtime/onnxruntime.dll` 与 `runtime/DirectML.dll`；
- 文件校验：`runtime/SHA256SUMS.txt`；
- 第三方许可：`THIRD_PARTY_NOTICES.md` 和 `third_party/`。

DirectML 需要可用的 D3D12 GPU/驱动；ORT/DirectML DLL 需要对应的 x64 VC++ runtime。
当前 DXGI adapter 明确拒绝旋转输出，竖屏/旋转显示器应由宿主提供自定义 `IFrameSource`。

本目录不会携带预编译的 `pubg_ai_aim.lib`。静态库必须在目标项目环境中从源码生成。
目标项目当前的 OpenCV 4.12 不包含 DNN，因此本模块不链接 OpenCV。ONNX Runtime
使用 C API 动态加载，不会给目标项目增加 ORT import library 或 C++ ABI 依赖。
`cmake --install` 只安装静态库、公共头文件和 CMake package，不会自动安装模型或 runtime DLL。

首次接入或升级模块公共头文件后，应删除该模块专用的旧构建目录并重新配置、全量编译；
不要混用旧 `.obj/.lib`。`AimConfig`、`Frame` 等公共结构发生布局变化时，陈旧增量产物会形成
ABI 不一致，表现可能是启动时 fail-fast 或内存访问异常。

## 2. 独立构建与测试

在 `C:\Projects\PUBGAssistant-cpp` 中执行：

```powershell
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 && cmake -S modules\pubg_ai_aim -B .build\pubg_ai_aim-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPUBG_AI_AIM_BUILD_TESTS=ON -DPUBG_AI_AIM_BUILD_ORT_SMOKE_TEST=OFF -DPUBG_AI_AIM_BUILD_EXAMPLES=ON && cmake --build .build\pubg_ai_aim-debug --parallel'

& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir .build\pubg_ai_aim-debug --output-on-failure
```

模型 smoke test：

```powershell
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 && cmake -S modules\pubg_ai_aim -B .build\pubg_ai_aim-smoke -G Ninja -DCMAKE_BUILD_TYPE=Release -DPUBG_AI_AIM_BUILD_TESTS=OFF -DPUBG_AI_AIM_BUILD_ORT_SMOKE_TEST=ON -DPUBG_AI_AIM_BUILD_EXAMPLES=OFF && cmake --build .build\pubg_ai_aim-smoke --target pubg_ai_aim_ort_smoke'

& .build\pubg_ai_aim-smoke\tests\pubg_ai_aim_ort_smoke.exe `
  modules\pubg_ai_aim\runtime\onnxruntime.dll `
  modules\pubg_ai_aim\runtime\PUBGV8.onnx
```

## 3. 模式 A：接收外部检测框

适合复用其他检测器。检测框必须使用 ROI 内局部坐标的浮点 `xyxy`：

```cpp
#include <pubg_ai_aim/pubg_ai_aim.hpp>

namespace aim = pubg::ai_aim;

class ProjectMotionSink final : public aim::IMotionSink {
public:
    void submitAimMotion(const aim::MotionCommand& command) override {
        // 提交给项目统一的鼠标位移 Dispatcher；保留 double 精度到最终量化。
        enqueueAiMotion(command.dx, command.dy, command.frame_sequence);
    }
};

auto sink = std::make_shared<ProjectMotionSink>();
aim::AimModule module(aim::AimConfig::correctedDefaults(), sink);

aim::ActivationState active;
active.enabled = true;
active.game_foreground = true;
active.aim_primary = true;
module.updateActivation(active);
module.start();

aim::DetectionFrame frame;
frame.sequence = 1;                 // 必须严格递增
frame.timestamp = aim::Clock::now();
frame.roi_left = 800;
frame.roi_top = 380;
frame.roi_width = 320;
frame.roi_height = 320;
frame.screen_width = 1920;          // FOV 使用完整游戏视口/输出尺寸
frame.screen_height = 1080;
frame.screen_center_x = 960;
frame.screen_center_y = 540;
frame.detections.push_back({
    aim::Box::fromCenter(160.0F, 160.0F, 40.0F, 80.0F),
    0.90F,
    1,
});

const bool accepted = module.submitDetections(std::move(frame));
```

`class_id` 的当前模型语义为 `0=head`、`1=body`。默认配置只启用 body。
队列容量为 1，新帧覆盖尚未处理的旧帧；sequence 和 timestamp 都必须非零并严格递增。
timestamp 不能晚于当前单调时钟，也不能比当前时间早超过
`AimConfig::tracking.max_frame_age_ms`（默认 250 ms）。模块未运行或任一安全门控关闭时，
`submitDetections` 返回 `false`，调用者应丢弃该帧而不是重试旧帧。

## 4. 模式 B：DXGI + ONNX Runtime/DirectML

```cpp
#include <pubg_ai_aim/pubg_ai_aim.hpp>

namespace aim = pubg::ai_aim;

aim::DxgiFrameSourceConfig capture;
capture.adapter_index = 0;
capture.output_index = 0;
capture.roi_width = 320;
capture.roi_height = 320;
// 优先用宿主已选显示器的物理桌面矩形；索引只作为回退。
capture.preferred_output = aim::DesktopRect{-1920, 0, 1920, 1080};

aim::OrtDmlDetectorConfig detector;
detector.runtime_library =
    LR"(modules\pubg_ai_aim\runtime\onnxruntime.dll)";
detector.model_path =
    LR"(modules\pubg_ai_aim\runtime\PUBGV8.onnx)";
detector.prefer_directml = true;
detector.allow_cpu_fallback = true;
detector.dml_device_index = 0;

auto module = std::make_unique<aim::AimModule>(
    aim::AimConfig::correctedDefaults(),
    aim::makeDxgiFrameSource(capture),
    aim::makeOrtDmlDetector(detector),
    motion_sink);
```

`DirectML.dll` 必须与 `onnxruntime.dll` 位于同一目录。DirectML provider 初始化失败时，
detector 会回退到 ORT CPU provider。建议从程序实际工作目录解析绝对路径，不要假定
Windows 进程工作目录就是源码根目录。

`preferred_output` 先精确匹配 DXGI 输出的物理桌面矩形，再尝试包含该矩形中心点的输出；
没有匹配时才使用 `adapter_index/output_index`。这可避免多显示器环境默认截取 0/0 输出。

`AimConfig::detector` 与 `AimConfig::compatibility` 是模块运行时的权威解码设置。即使创建
ORT detector 时传过 `OrtDmlDetectorConfig::decoder`，`AimModule` 也会在首次推理前以及
每次 `updateConfig` 后用 sanitize 后的模块配置同步它，避免预处理尺寸、输出布局和解码阈值
各用一套配置。

## 5. 生命周期和安全门控

推荐顺序：

1. 构造 sink、可选 capture/detector 和 `AimModule`；
2. 调用 `updateConfig`、`updateCombatContext` 和 `updateActivation`；
3. 调用 `start()`；
4. 运行期按识别结果更新 context，按窗口/热键状态更新 activation；
5. 退出或重建依赖前调用 `stop()`。

只有下列条件全部满足才会抓帧、执行检测并向 `IMotionSink` 输出：

```text
enabled
game_foreground
aim_primary 或 aim_secondary
paused == false
blocking_ui_open == false
```

失焦、暂停、打开背包/地图/菜单或关闭功能时，应立即更新 `ActivationState`。模块会清除
跟踪、预测和 PID 状态，并让内置抓帧/推理 worker 等待；总开关关闭时不会持续截图或推理。
已经进入同步 `capture`/ORT `Run` 的单次调用会正常返回，但其结果会被安全 epoch 丢弃。
`stop()` 返回后 worker 已 join，不会继续调用外部 sink。
自动开火默认关闭。只有显式启用配置并提供 `ITriggerSink` 才可能触发点击。

同步 ORT `Run` 已经开始时，外部 `stop()` 会等待当前推理返回。sink 回调可以调用
`setEnabled`、`updateActivation` 或 `stop`；不要在 sink 回调内销毁 `AimModule` 或释放它的
最后一个拥有者。

## 6. 接入武器、配件和姿态识别

复用目标项目已有识别结果，不要在模块里再启动另一套模板识别：

```cpp
aim::CombatContext context;
context.revision = recognition_revision;
context.weapon = current_weapon;
context.scope = current_slot.scope;
context.grip = current_slot.grip;
context.muzzle = current_slot.muzzle;
context.stock = current_slot.stock;
context.stance = current_stance;
context.scope_zoom = recognized_scope_zoom; // 独立数值，原版 PID 会除以它
context.ads = ads;
context.firing = firing;
context.crouched = crouched;
context.lean_left = lean_left;
context.lean_right = lean_right;
module.updateCombatContext(std::move(context));
```

模块负责武器/倍镜名称规范化和预测倍率，不负责识别图片。`EquipmentDetector`、
`WeaponDetector`、`GestureIdentifier` 仍是识别来源；“武器预测”是在武器已经识别后，
根据目标速度、武器类别、倍镜和当前跟踪可靠性计算提前量，而不是再次识别武器。
`scope_zoom` 不是预测用的 1/2/3/4 经验档；无法识别时传 `1.0F`。原版并不检查 ADS
按键才应用灵敏度增益，因此 `ads` 只是宿主状态元数据。

## 7. AI 与压枪保持独立

AI 模块只生成 `MotionCommand{dx,dy}`。不要让 AI worker 和 `RecoilControl` 分别并发
调用 `SendInput`；两者应将命令提交到同一个单线程 Dispatcher，由 Dispatcher 按固定顺序
合并或发送，并分别维护 AI 与压枪的小数残差。这样算法、开关和状态机仍相互独立，只有
最终鼠标输出被串行化。

## 8. 配置选择

```cpp
auto corrected = aim::AimConfig::correctedDefaults();
auto compatible = aim::AimConfig::legacyDefaults();
```

- `Corrected`：浮点 `xyxy`、按类别 NMS、使用实际帧间隔和目标框中心估算速度，
  开火/瞄准点偏移不会伪装成目标速度；推荐新接入使用。
- `Original`：保留原程序中会影响输出的舍入、NMS、每帧预测和非标准 PID 行为，便于回放
  对照，包括 legacy 6x→3x、8x→4x 灵敏度映射；不会复刻数据竞争、COM 泄漏或重复旧帧。
`Original` 只承诺已由反汇编和配置交叉确认的规则；运动可靠性计数器/迟滞的部分边界仍是
语义还原，不应当作逐指令 bit-exact 实现。

更新任意配置后调用 `updateConfig`；模块会复制并 sanitize 配置。JSON 示例位于
`config/ai_aim.example.json`，核心库刻意不依赖 JSON 库，字段解析由宿主项目负责。

## 9. 状态和错误诊断

```cpp
const aim::AimSnapshot state = module.snapshot();
```

重点字段：

- `running`：worker 是否运行；
- `model_ready`：检测器是否成功初始化/处理；
- `target_locked`、`target_class`、`confidence`：当前目标；
- `capture_ms`、`inference_ms`：耗时；
- `frame_sequence`：最近处理帧；
- `detector_backend`：实际 provider；内置 ORT 返回 `DirectML` 或 `CPU`，外部帧模式返回
  `external`，默认自定义 detector 返回 `custom`；
- `last_error`：初始化或运行错误。

连续抓帧或推理错误使用 20 ms 起、最高 1 s 的指数退避，避免永久错误形成 CPU 热循环；
恢复后的成功帧会清除 `last_error` 并重新设置 `model_ready`。

常见排查：

- `LoadLibraryExW` 失败：核对 ORT/DirectML 路径、x64 架构和 SHA-256；
- Session 创建失败：核对模型输入输出名 `images` / `output0`；
- 有目标但不输出：检查完整 `ActivationState`，尤其前台和阻塞 UI；
- 外部帧不接收：模块必须处于完整激活状态；sequence/timestamp 必须严格递增且帧龄不超过
  `tracking.max_frame_age_ms`；尺寸/中心坐标必须有效；
- 位置偏移：外部检测框是 ROI 局部 `xyxy`，`roi_left/top` 是桌面坐标；
- AI 与压枪互相抖动：检查是否绕开了统一 Dispatcher 直接调用鼠标输入。

## 10. 链接到宿主项目

当前根项目已经按下列方式链接本库：

```cmake
add_subdirectory(modules/pubg_ai_aim)
target_link_libraries(PUBGAssistant-cpp PRIVATE pubg_ai_aim::pubg_ai_aim)
```

作为子目录嵌入时，模块测试和示例默认关闭，不会污染主程序构建目标；需要时可显式设置
`PUBG_AI_AIM_BUILD_TESTS=ON` 或 `PUBG_AI_AIM_BUILD_EXAMPLES=ON`。

根项目没有复制本库的检测、跟踪或控制实现，也没有在本库中复制武器识别；宿主接线集中
在 `AiAimSettings`、`AiAimHostContext`、`AiAimCoordinator`、运行时预检和共享鼠标
Dispatcher。完整约定见 [`INTEGRATION_NOTES.md`](INTEGRATION_NOTES.md)。
