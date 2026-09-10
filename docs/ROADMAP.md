# ROADMAP · 全部计划任务（自定义开发时代）

> 2026-09-10 立账。evolve 协议与驱动已退役（用户删除插件），本文件取代 evolve 目标池成为唯一计划源。
> 状态图例：☐ 待办 · ◐ 进行中/半成品 · ☑ 完成 ·（提案）= 需用户拍板才排期。
> 完成一项就在本文件勾掉并写一行 evidence（提交号/验收记录）。

## P0 · 接续收尾（先做，清掉历史遗留）

- ☑ **T1 · #79 半成品收尾** ✓ 71a770a（test_msc 39/39 绿）：`tools/usbtest/msc_test.py` CDB 选路补"块数 ≤0xFFFF 才走 10 字节 CDB，否则升 READ/WRITE(16)"边界（16 位域溢出静默错值/晦涩 OverflowError）。工作树已有 66 行 diff（msc_test.py + test_msc.py），需：复核口径与 C++ msc_read_cdb 一致 → 跑 `tests/test_msc.py` 全绿 → 提交。evolve 时代最后一个未闭环项。
- ☑ **T2 · 仓库卫生** ✓ 36f84b9 + USBTree eaabd03（驱动脚本归档 scripts/retired/）：`tools/reports/`（产测运行产物 JSON）加 .gitignore；`.auto-evolve.lock`、`docs/auto-evolve-driver.log` 归档或 ignore；USBTree 侧 `scripts/auto-evolve*.sh/.cmd` 驱动基建退役（归档到 docs/历史 或删除）。
- ☑ **T3 · 记账闭环** ✓ USBTree eaabd03（evolve-log status=terminated）：USBTree `docs/evolve-log.md` 头部 status 改"已终止（用户删除 evolve，转自定义开发）"+ 终局 checkpoint（终止于 #78，指针 #79 作废），防止未来会话误读指针复活驱动。

## P1 · EP-4 工程师通信控制台（代码全齐 → 验收关闭）

- ☑ **T4 · Lab5 遥测契约固件** ✓ 25d5555（telemetry.c 宿主自检四行输出实证；顺手修契约头三引号/PD_CTRL_WAIT 两存量错）（解锁链最上游，~20 行）：在 PE_SNK Ready 状态与 VBUS ADC 采样处向调试口打印 `cc_state/contract_v/contract_i/vbus_v`（契约已冻结见 `tools/usbtest/STATUS.md` 第二节，只加不改）。
- ◐ **T5 · EP-4 真机验收**（阻塞：需人工+真机）——验收表 apps/验收-工程师通信控制台.md 备好待执行：按 `apps/验收-工程师通信控制台.md` A~D 逐项——A 关键词 3 秒定位设备、B 双会话收发、C HID 解析、D U 盘读扇区+WinUSB 收发。验收过 = EP-4 关闭。
- ☑ **T6 · S6 PD 解析面板** ✓ 1ad0928（pd_telemetry_parser.h + parser_select 嗅探升级 + 面板接线；parser_selftest +10 例）：pd key=value 遥测 → 面板可视化（CC 状态机迁移 + 合同电压/电流曲线）。
- ◐ **T7 · macOS 端对齐**（首切片完成 ✓ eac5a37：UTSLog 文件日志+发现过滤；未编译验证需 macOS；会话台/解析面板移植留后续切片）：发现/过滤/会话台/解析四件套目前 Windows 独有（Swift 端仍是旧产测形态），按 Win 版切片移植。

## P2 · usbtest 产测框架（mock 全绿 → 真机闭环）

- ◐ **T8 · 逐后端真机验证**（阻塞：需工装/真机）——代码侧已就绪：uac 优雅失败/dock pnputil 主机侧实测 98KB 解析通过；其余按 STATUS.md 工装清单逐项：按 STATUS.md 工装清单逐项——HID 环回（Lab1 固件自环模式 TODO）、CDC TX-RX 短接治具、dfu-util+DFU 分区、UVC opencv 采帧、BLE 屏蔽箱+适配器、MSC 空白盘破坏性写验证（DESTRUCTIVE 红线）、hub port cycle。
- ☑ **T9 · UAC 后端实现** ✓ 68d11f3（sounddevice 可选依赖优雅降级；本机真机探针实测 -75.8dBFS 全链派发）：`uac_record_level` 是唯一"仅 mock"处理器，需 sounddevice + OS 音频路由。
- ☑ **T10 · dock 后端 Windows 路径** ✓ 68d11f3（UsbTreeView 优先/pnputil 回退；实修 zh-CN GBK 解码 bug）：现依赖 Linux lsusb，Windows 接 UsbTreeView CLI。

## P5 · USB DevStudio 全栈工作站（2026-09-11 立账，方案/详设见 apps/）

- ☑ **MS0 · IDE 壳** ✓ 35e3418（推送 origin）：四视角/命令面板/工程模型 .ustsproj/面板注册表/设置+布局持久化/崩溃 minidump/6 靶 512 例全绿/exe 更名 USBDevStudio v0.10.0；实施计划 docs/superpowers/plans/2026-09-11-devstudio-ms0.md T1~T12 全勾
- ☑ **MS1 · 描述符编译器** ✓（三向闭环+round-trip 幂等性质测试/Linter 25 规黄金样例实战首胜/字段 diff/W2 面板替换桩/第七靶 16 例/v0.11.0——证据见 git log feat(MS1)）
- ☑ **MS2 · 虚拟调试器** ✓（C++ vd_core 架构诚实账落账/枚举字节级=MS1 产物/断点五类/注入五型/ustssim 脚本/ustsvd 进程内协议/W3 面板真替换/第八靶 9 例/v0.12.0——证据见 git log feat(MS2)）
- ☐ **MS3 · 测试台+产线归位**（Test Explorer/双后端/Pipeline/EP-4 迁移）
- ☐ **MS4 · 协议追踪台**（DSL/统计/USBPcap/pcapng）
- ☐ **MS5+ · 滚动**（W1 诊断/插件 SDK/脚本台/知识服务扩展）

## P3 · 工程化/商业化最后一公里（提案，需拍板优先级）

- ☑ **T11 · Windows 安装包** ✓ 8a3d261（Inno Setup iss + zip 兜底打包实测产出；版本单源 src/app/version.h 0.9.0）：Inno Setup/MSI、应用图标、版本号方案、发布页。
- ☑ **T12 · 崩溃与运行日志落地** ✓ 1713f60/b2fa82f（spdlog 滚动文件 5MB×3 + OutputDebugString 双 sink；Python 侧 logbase 对齐 66944ab）：文件日志 + 崩溃转储（现排障全靠控制台输出）。
- ☑ **T13 · CI 增强** ✓ 8a3d261（msvc-build job：五靶编译+四自测+exe artifact 7 天）：MSVC 四靶构建进 GitHub Actions（现 CI 仅 Python mock+自测）；确认 usbsim 自测在 CI 里。
- ◐ **T14 · lab1~lab7 真机 bring-up**（阻塞：需真机逐个）——产测计划/mock 链全就绪，按实验室顺序插板即跑：按实验室顺序逐个真机跑通，experience 文档回填实战数据。

## P4 · USBTree 知识库

- ☑ **T15 · EP-2 材料解锁** ✓ USBTree 427649d/4b8e15e（GSS 现行版 Azure blob 直链入缓存——无需人工浏览器；UUID 全表改由 SIG YAML 仓库机生成 615 项入 BLE/16 篇）：浏览器下载 GATT Specification Supplement PDF → 放 `80-参考资料/bluetooth/` → 回填标准 UUID 全表（50/04-ATT与GATT 扩叶）。
- ☐ **T16 · 池刷新**：COVERAGE 残余空白重核（清单自 2026-09-05 未动）+ 模块轮换抽查（90/02 速查表 → tools → graph）。
- ☐ **T17 · 技能包同步**：usb-spec-lookup 缓存映射与实际缓存目录核对（37 份）。

## 执行状态（2026-09-10 一轮冲刺后）

T1~T4、T6、T9~T13、T15 全部完成（代码+离线自测全绿）；T7 完成首切片。
**余下三项均为硬件/人工阻塞**：T5 真机验收（验收表就绪，插机即测）、
T8 逐后端真机验证（工装清单见 tools/usbtest/STATUS.md）、T14 实验室 bring-up（插板即跑）。
T16/T17（USBTree 池刷新/技能包同步）未在本轮范围，池中保留。
