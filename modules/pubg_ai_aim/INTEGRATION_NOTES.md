# PUBGAssistant-cpp 宿主接入说明

`pubg_ai_aim` 已作为独立 C++20 静态库接入根项目。模块核心不依赖 Qt、宿主 JSON 或
宿主输入实现；宿主边界集中在以下组件：

1. `AiAimSettings` 是宿主唯一的 JSON 解析器，启动时生成类型化配置快照；
2. `App` 把现有 `WeaponDetector`、`EquipmentDetector`、姿势和按键状态转换成
   `CombatContext` / `ActivationState`，没有复制武器识别算法；
3. `AiAimCoordinator` 管理手动开关、异步初始化、代际取消、状态发布和鼠标安全门；
4. `AiAimCoordinatorProduction` 只负责把类型化配置和资源路径装配成 DXGI/ORT 后端；
5. `AiAimRuntimePreflight` 在加载原生 DLL 或模型前执行有界的 PE、大小和 SHA-256 检查；
6. AI 与压枪都只向同一个 `MouseMotionDispatcher` 提交位移，最终输入后端仍只有一份。

Coordinator 状态回调可以请求开关、停止或替换自身，但沿用 `AimModule` sink 的所有权
约定：回调返回前不得销毁 Coordinator 或释放它的最后一个拥有者。

运行时目录由根项目构建同步到 exe 旁的 `ai_aim_runtime/`。当前模型、ORT 和 DirectML
摘要同时记录在 `runtime/SHA256SUMS.txt` 和宿主编译期资产契约中；升级任一资产时必须同步
更新两处并运行 `ai_aim_runtime_preflight_tests`。相邻 manifest 只用于打包一致性检查，不能
自行改变宿主接受的资产身份。

宿主始终保持这些不变量：每次进程启动 AI 默认关闭、只输出鼠标位移而不自动开火、游戏
失焦或背包/地图/菜单打开时关闭运动门、未识别到可用武器时暂停。DirectML 的实际结果由
后端快照显示为 `DML`/`CPU`；允许 fallback 时的降级原因通过 Coordinator 状态告警传给 HUD。
