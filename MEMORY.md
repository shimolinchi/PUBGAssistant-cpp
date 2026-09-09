# PUBGAssistant-cpp Project Memory

## 工作约定与构建

- 主目录 `C:/Projects/PUBGAssistant-cpp`。保留无关本地/未跟踪变更；临时、缓存、构建均放本目录。使用现有抽象，小步修改、针对测试，再按需要回归；用户要求源码批次时暂停编译直到明确允许，目前无编译禁令。
- 发布入口 `.build/package/release/PUBGAssistant-cpp.exe`，此项目不用顶层dist；DLL/platforms/assets/config/ai_aim_runtime必须同包。只用VS x64/MSVC和CMake `windows-x64-debug/release`及`-tests`，禁止引入/探测clang。普通编译只指定PUBGAssistant-cpp目标；Release ALL会触发删除重建原发布目录的打包步骤。
- 替换发布包前关闭助手、备份当前包，恢复并验hash用户配置/标定；不扰动游戏、不从旧备份恢复配置。运行时资源每次build同步到EXE旁。
- MSVC/Ninja曾因中文`/showIncludes`前缀AUTO解码损坏记录零头依赖，混用ABI。`cmake/msvc_ninja_dependencies.cmake`以ENCODING NONE探测字面前缀；保留修复。四套build已干净重建验证真实头依赖，旧增量不可信，VSLANG无效。
- 最新2026-09-04 fps版Debug/Release app与各19/19测试、preflight/diff通过；仅发布EXE变更，其余414文件（含7配置）hash相同。回滚`.tmp/package-before-fps-label-20260904-01/release`415文件。EXE SHA256=`FCE4DCB803DAFD939422DA2C6943BA0D92597135F63CF49F4AD3061A6A4C5255`；最新主程序/实战未启动验证。

## AI与界面不变量

- AI瞄准为第四独立开关，每次启动off，RMB按住激活；复用WeaponDetector/EquipmentDetector/GestureIdentifier/HotkeyManager/StatusHud/Config ResourcePaths/MouseMotionDispatcher及中立EquipmentSelection，不重复识别或接自动开火。
- 默认快捷键武器F1、距离F2、压枪F3、AI F4、地图F5。已有用户绑定优先；缺失动作默认键被占则留空显示未设置；冲突归一大小写/括号/空白。
- M762/RPD原生全自动`auto_fire:false`（该项意为生成连续点击）；RPD模板齐，用M249压枪曲线作可调基准。待重构散落的武器能力表，尤其O12分类。
- HUD固定文字`自瞄`：红关闭、黄开但未识别、绿已识别，状态表示识别而非可输出。MK47已识别，用户6x触发`disable_variable_high_zoom=true`门控（默认保留）；根据倍镜类型而非游戏缩放比例。
- 输出必须AI启用、武器/姿态支持、RMB、游戏前台、新鲜帧、无菜单；Escape菜单失败关闭，恢复需重复隐藏光标证据。不要以磁盘开关证明实时状态。
- 运行时验证x64 ORT/DirectML/model清单有界路径、PE、大小、SHA256；生产不静默CPU fallback，CPU仅显式smoke。
- `ai_aim_settings.json`仍schema1，可选键向后兼容；AiAimSettings专属序列化/模块sanitize，普通Config::save不改AI配置。参数窗4页63控件，参数帮助含含义/默认/范围/生效条件/JSON路径；公开控件不授权改算法或用户值，隐藏项/精度原样保留。
- 保存并应用：文档+存在性快照防外部改写，先原子落盘，再用同一已提交且校验的快照应用，不重新读盘；失败不应用，保留重试基线。draft/reset/reload不应用。updateConfig通过协调器配置/会话代际和锁隔离旧推理、重置跟踪/输出寿命；保留启用暂停状态，初始化中变更取消旧stop token并重启；换设备只重建AI runtime。
- `display.target_boxes`主机侧默认false，目标识别页开关。复用不可变AimSnapshot检测帧和StatusHud/OverlayWindow；真实ROI/桌面坐标，裁剪到ROI/显示器，丢弃畸形/未来/>150ms帧。1px、alpha85、红黄绿按置信，无文字；并非敌友识别。配置/暂停/错误/失焦/停止清框，代际屏蔽旧结果；不改瞄准压枪。
- HUD`当前`尾部显示`fps: xx.x`，非Hz；统计AI有效推理完成（含空目标），不等于游戏/捕获/武器识别/配置频率或20Hz轮询。模块RecognitionFrequency：无分配500ms平均、1s过期，启停/配置/错误清零；与框共用快照发布，无帧也到时刷新。高于轮询等核心/HUD测试已重复×5。
- 显示设置共用SliderBinding，字体10–22步长.1、16ms合并预览，release/save/close/hide刷新；析构只交内存不回调owner。读设置不写配置，不透明度不重建样式，字体保持double/GDI+。
- Popup中SpinBox/Slider/ComboBox鼠标滚轮不改值（含focus）；保留键盘/箭头/拖动，携带pixel/angle/phase/device转给最近滚动容器viewport；覆盖ChildAdded/Polished，不能只ignore事件。
- 协调器状态带generation/revision，单回调owner/drain且寿命覆盖调用；HUD按revision丢弃旧状态。关停Running→Stopping→Stopped，join生产者再销毁依赖，MainWindow保活，未静默停稳则fail-fast；Overlay只泵自己的窗口消息。

## 调参事实与待验收

- AI和压枪在同一dispatcher相加。导入开火身体偏移120叠加Y跟踪可能过补偿；开火暂停AI纵向仅清开火中的AI Y，不自动启压枪/协调算法，也不删范围检查里的开火偏移。诊断漂移先对比纯压枪/叠加，禁止默认改曲线。
- 速度：max_pixels只限幅；PIDgain=global_scale*pow(clamp(base/current,.3,3),1.36)/clamp(scope_zoom,1,10)，获得目标前几帧乘.45/.65/.82/1。P默认X=.881/Y=.456；I旧逐帧递推非时间积分，D未除dt；global_scale也影响FOV/range。UI P/D 0–10、I 0–.95；保留导入精度。
- 有效预测normal_predict_factor/deadzone；旧predict_factor/deadzone/location隐藏未生效。开火偏移非DMR按3/6、4/8倍镜共享缩放，正值向下；距离缩放来自框尺寸非米数；Y预测0不关闭Y跟踪。
- 手工待验收DXGI实战、AI键+RMB、菜单/软件光标门控、HUD恢复、实际跟随/缩镜感受。已做历史CPU/DirectML、原生设置预览、自定义热键/配置回归；构建失败由代理检查，用户只需反馈游戏/GUI现象。
- 导入瞄准模型标AGPL-3.0且溯源不完整，分发/商用需解决许可；声音研究真实背景/覆盖与授权另行验证。大研究ZIP本地忽略；此前清理commit5b1f9d3已推origin/master。

## 声音研究与 PUBGAudio 子模块

- 独立 public modules/PUBGAudio，https://github.com/shimolinchi/PUBGAudio，main分支。本机pubg Anaconda/torch2.8+cu128/RTX5070Ti16GB。2026-09-09用户授权最新版代码提交并同步：主仓库master与子模块main，先推子模块再更新主仓库引用；版本以git日志/远端引用为准。代码/配置/文档/小型登记报告纳入，音频/标签/权重/凭据仅本地；主outputs/exports（含聊天备份）忽略。108模块测试+15旧音频测试通过；C++/设备/游戏未改。
- F041最新：已把置信度规则接入v6.1 spec/configs/confidence_v1.json，生成时冻结二元真值/mask定义和验证整场景50/50校准计划（seed2026090811）。新版run_spatial_training.py对带规则的新数据，在主导出model_localization.pt后自动校准/带概率推理；旧冻结运行不追溯。71测试（训练/导出模拟）通过，源码/配置/清单哈希未改，无新数据/训练/实际概率拟合。见模块docs/confidence.md及outputs/reports/confidence_generation_policy_20260908.json。
- F040原外部模长/self sigmoid未校准；新每100ms每类分数和可选概率，未校准/样本不足概率null，位置可信度尚缺。0/1真值无需另标置信度，概率来自预测与标签的统计关系；校准绑定确切权重，验证整场景拟合/审计，不读测试，不代表真实游戏可靠性。23测试与既有120s新旧1200帧原值一致。
- F039仍为仅改脚本、稍后生成：configs/dataset_v6_1.json准备好但无新数据。混合/纯外脚步75/25%，120/240秒；纯1/2人65/35%、偏远60%+往返20%、有界曲线/频繁转头。混合脚步倍率.6→1，自身50→30%；按全段真实车功率筛整轨，本人车/强车冲突保留1%/2%，隐藏帧mask0。52测试，详表docs/external_footsteps_v6_1.md；720段期望30h不能报已生成。存在性loss/采样修正尚未实施。
- F037的v6 400轮已完成，outputs/train-spatial-v6-400epochs-20260908，W&B235fa5787d18；pipeline_status记录2026-09-08 14:44+08 completed，2026-09-09核验。定位371轮、检测360轮、loss367轮导出及120秒CPU1200帧验证通过；原冻结文件保留，置信校准/实战未做，勿重复启动。详见模块outputs/reports/spatial_v6_training_20260908.md。
- F038第50轮全验证诊断：外脚步阈值.5 TP0/FP37/FN2601、真分数中位.238；诊断阈值.2 recall67.3%却precision19.8%，未改正式门限。训练脚步正例3.9%，目标正常，presence MSE未平衡正负；是否主因待对照。报告模块outputs/reports/footstep_diagnosis_v6_20260908.md。置信校准不等于修复漏检；MAE须配检测/有效帧数，无样本null，W&B可能保留旧点。
- 当前datasets/spatial-v6-24h-20260908：720×120s=24h，train600/val60/test60=20/2/2h，19.312GiB，49枪组/8车型/5地面/682原声。known_assets_new_scenes，同原声可跨区但场景/轨迹/种子独立，不冒充独立录音或未见枪型测试；评价枪型角色均在训练实际可听、外部类四象限覆盖。全标签/几何/投影/3重放核验通过，报告spatial_v6_release_20260908.md；预检24min试听8770及GPU/CPU检查非精度结论。
- v6脚步RMS.025、枪.12、master.35，走/跑/冲刺35/45/50合成单位平滑截断，远70%；近似物理空气吸收，低油门6kHz；非游戏实测米数/HRTF。自动枪单点/短扫/持续20/30/50%，按弹匣/发间隔约束；DMR/栓狙逐发，M16/Mk47单发或3/2连发。
- 论文骨干106万参数，5秒非因果→100ms，未加空间分支/长上下文。可定位mask要求可听且SIR≥−10dB，分类仍保留难定位可听帧；ADPIT权重activity1/direction2/distance.2/self.5，以检测且角误差≤20°的本地多源宏F1验证选模（非官方DCASE）。400轮每轮7200随机5秒窗/450更新，总180000更新，50%镜像、无噪声/增益/变速；归一化仅训练数据。
- 旧400轮F034保留outputs/train-light-v5_3-gunvehicle-aug400-20260907及旧模型/报告；检测339轮test脚步/车/枪 .8851/.9310/.5591，loss229轮 .8594/.909/.68，末轮val枪.7827不是同指标。旧协议整枪型/车型留出，与v6不作同条件对比。旧方向车67.4°有远距/车型混杂，最差固定视角，不能仅归因转头/窗口短；报告direction_diagnosis_20260908.md。
- 旧25h先删净15.026GiB；随后旧2h/3.6h（包含关系）、缓存、scenario-small/smoke共6目录归档删除净12.375GiB，不重复计数；registry下v4/v5_retired_*存元数据，v6_replacement_cleanup_20260908.json存凭据。2880新文件和5旧模型hash保留一致，原声/试听保留。
- 本机源音只读提取/复制，不下载替代。先轨迹状态后渲染；同人固定地面，不混地面模拟左右脚。self按直接来源非距离：本人拉环self，离手爆炸燃烧不标投掷者。原10ms/32ms窗标签→100ms训练，跨5秒裁剪监督屏蔽；遮蔽正例mask0。
- 轻量版无天气/飞机/动作/投掷/损伤链。全声音试听仍outputs/body-action-preview-20260907-v5_3-final（8769，10段480s/815源），人工反馈未填，不混训练。仅水平转头无平移/俯仰；实时采集、实录标定、跨音量/声卡、完整RTPC/遮挡混响多普勒、因果长上下文未实现。WASAPI默认pre-volume不绕过全部上游增益/DSP。
