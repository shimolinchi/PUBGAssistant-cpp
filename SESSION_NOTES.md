# PUBGAssistant-cpp — 会话记忆 / 项目上下文

> 用途：下次进入 CLI 时把本文件发给 Claude，用于恢复上下文。
> 最后更新：2026-06-15

---

## 一、项目定位

- 项目：`/Users/wangrui/WorkSpace/PUBGAssistant-cpp`，基于 **OpenCV 截图识别**的 PUBG 辅助程序（C++17 + Qt + OpenCV，目标平台 Windows）。**不读内存**，靠截图分析。
- 已确认"雷达"实为：识别**玩家/队友自己在地图上手动标的点**并测距，不检测敌人位置 → 属于基于自身已知信息的计算，可接受协助显示相关部分。
- 迫击炮强制开启是**有意为之**（显示层不冲突），不要改。

---

## 二、已完成的工作（三轮）

### 轮次 1：修 bug —— LargeMapRadar 线程修复
- **问题**：`onMouseClick`（运行在全局鼠标钩子线程）里 `worker_.join()` 阻塞输入线程导致鼠标卡顿；`worker_` 跨线程重新赋值有数据竞争。
- **修法**：改为**常驻工作线程 + 条件变量**。点击只在锁内写 `job_x_/job_y_/job_pending_` 然后 `cv_.notify_one()`；析构置 `stop_` 并唤醒、join 干净退出。
- **改动文件**：`include/LargeMapRadar.hpp`（加 `<condition_variable>`、`cv_`/`stop_`/`job_*` 成员、`workerLoop()` 声明）、`src/LargeMapRadar.cpp`（构造启线程、析构停线程、`onMouseClick` 改投递、新增 `workerLoop`）。

### 轮次 2：修显示文本小瑕疵 —— ThrowablesAssistant
- **问题**：`render()` 里 `std::to_string(cook).substr(0,4)` 小数位数不稳定。
- **修法**：新增匿名命名空间的 `oneDecimal()` 助手（固定一位小数），替换该处文本拼接。加了 `<iomanip>`、`<sstream>`。
- **未动任何投掷逻辑**。

### 轮次 3：编译期开关 PUBG_ENABLE_INPUT_CONTROL（核心交付）
- **新增** `include/BuildConfig.hpp`：仅一行 `#define PUBG_ENABLE_INPUT_CONTROL 1`。
  - `1`=完整功能（默认，与改动前逐字节等价）；`0`=编译期排除所有主动控制键鼠的行为。
- **硬性约束（用户要求）**：绝不删代码，只用 `#if PUBG_ENABLE_INPUT_CONTROL ... #endif` 包裹；置1必须与原逻辑逐字节等价；置0只需能编译。
- **力度**：保留 RecoilControl 类与所有 UI 传参，**只禁行为**（实例仍构造，但置0不启 worker、不发输入）。
- **改动文件**（每个 .cpp 在首个 include 后加 `#include "BuildConfig.hpp"`）：
  - `src/RecoilControl.cpp`：构造里 worker 启动、析构 join+keyUp、`setEnabled`/`updateCurrentWeapon` 的 keyUp、`workerLoop` 整函数体、`applySrBreathControl` 整函数体（+`#else (void)capture;`）
  - `src/ThrowablesAssistant.cpp`：`onThrowKey` 捏雷4行+pull_key、`executeThrow`（+`#else (void)jump_throw;`）；显示与 `cooking_/throw_at_` 状态机保留
  - `src/StatusHud.cpp`：状态栏"压枪"项绘制 + 其后 `x += 35.0`
  - `src/App.cpp`：`toggle_recoil`（F3）热键注册
- **安全结论**：置0时 RecoilControl 的 `worker_` 默认构造 `joinable()==false`，析构不会 join 崩溃；`setEnabled` 变空操作，App 对它的调用安全，故 App 的 `setRecoilEnabled/toggleRecoil/recoil_enabled_` 无需 `#if`。
- **UI 未改**：0 构建里"开启辅助压枪/调试压枪参数"按钮仍存在但点击无效（符合所选力度）。如需隐藏可再加 `#if`。

---

## 三、待办 / 下一步

1. **用户需在 Windows + VS 实测验收**（本机无法编译）：
   - 默认（=1）编译运行，确认与改动前行为一致。
   - 改 `BuildConfig.hpp` 为 `0` 重编译，确认能编过、不崩；压枪不动鼠标、投掷不自动捏雷、状态栏无"压枪"、F3 无效；测距/弹道/标点/迫击炮显示正常。
2. 可选后续：0 构建里隐藏压枪相关 UI 按钮（再加一层 `#if`）。

---

## 四、注意事项

- 该项目**不是 git 仓库**，无法 diff。用户会**频繁手动改文件**——每次动手前务必**重新 Read 当前内容**，不要基于旧版本编辑。
- 计划文件存于：`/Users/wangrui/.claude/plans/tidy-greeting-fountain.md`
