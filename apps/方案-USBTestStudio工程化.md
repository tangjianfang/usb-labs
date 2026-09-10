# 方案 · USBTestStudio 工程化（总纲）

> 状态: **approved（三节均经用户确认）** · 2026-09-10 立账
> 定位跃迁：从"EP-4 通信控制台 + 产测 CLI"的 demo 形态 → **统一桌面工程软件**（设备目录 + 产测引擎 + 解析控制台 + 报告中心）。
> 控件级详设见 [设计-USBTestStudio统一工程软件.md](设计-USBTestStudio统一工程软件.md)。
> 基线诚实声明：当前代码在"完整工程化软件"标尺下视为**地基**（≈1% 完成度的工作基线），本文定义 100% 的样子。

## 一、四个已拍板的定向决策

| 决策点 | 结论 |
|---|---|
| 产品形态 | 统一桌面工程软件（对标 Docklight+USBTreeView+nRF Connect 整合体） |
| 平台 | Windows 先行（C++20/Win32），macOS/Linux 后续平移 |
| UI 技术栈 | **纯 Win32 零第三方依赖**（延续现有 483 例自测资产与仓库原则） |
| 节奏 | 三期：V1.0 单机商用级 → V1.5 数据/批产 → V2.0 插件/脚本 |
| 进程模型 | **混合容器**（用户修订）：GUI 主进程 + 功能 DLL 按需加载 + 引擎子进程 + 外联子进程 |

## 二、功能大全（113 项 · 12 协议域全覆盖）

覆盖标准：调研发现的每项对标能力必须三选一落位——**自建**(P0/P1/P2) / **🔗外联**(调度现有工具) / **⛔硬件边界**(明示不自建)。
标记：P0=V1.0（41 项）· P1=V1.5 · P2=V2.0。逐控件规格见设计文档。

### M1 设备目录（对标 USBTreeView/USBDeview/nRF Connect）
热插拔实时刷新 **P0** · 六源目录(USB/HID/COM/MSC/音频 P1/BLE P1) · 即时过滤+类型掩码 P0 · 设备详情页(描述符/驱动/电源/速率/口路径) P0 · 枚举快照导出 P0 · WinUSB 绑定检测+引导 P1 · 设备前后 diff/置顶 P1 · 设备启停用(devcon 级) P1 · BLE 扫描 RSSI 图 P1

### M2 产测引擎（对标 TestStand/OpenTAP）
GUI 计划编辑器(步骤树+参数表单+拖拽) P0 · YAML/JSON 双向兼容(契约冻结) P0 · 步骤库(工装要求 badge) P0 · 执行视图(实时测量值/单步重跑) P0 · 限值编辑器 P0 · SN 扫码互锁 P0 · 声光+大屏判定 P0 · --auto 无头+退出码 P0 · 接线引导图 P1 · 批产模式(自动待机) P1 · 重试策略/失败分支 P1 · 金机校准 P2 · MES 全量对接 P1 · 操作员权限 P1 · 并行工位 P2

### M3 通信控制台（对标 Docklight/nRF Connect；EP-4 全量继承）
四通道会话(HID/串口/WinUSB/MSC-CDB) P0 · 智能发送框+历史+周期 P0 · 双视图+时间戳+暂停/回滚 P0 · 会话日志保存/回放 P0 · 发送序列脚本(等响应/校验/分支) P1 · 多会话并排 P1 · 自定义帧模板 P2

### M4 解析与测量（对标 Wireshark dissector/MouseTester/REW/POWER-Z/STM32CubeProgrammer）
HID 报告实时解码 P0 · **描述符全树解码(逐字段+源字节) P0** · HID 回报率波形+直方图+抖动 P0 · **DFU 面板(镜像/下载/校验/版本回读) P0** · **UVC 实时预览+快照 P0** · MSC 容量/扇区查看器 P0 · PD 遥测曲线面板 P0 · 键盘全键扫描(N-RO) P1 · HID 事件流导出 P1 · 串口吞吐压力 P1 · 线路状态/流控面板 P1 · MSC 性能基准 P1 · 全盘填充验证(进度/断点) P1 · UVC 丢帧统计 P1 · UAC 电平表+扫频/THD P1 · 双工回环 P1 · BLE GATT 树/广播解码/配对管理 P1 · BLE-DFU P2 · UVC 扩展控件 P2 · 录像 P2 · 输入延迟测量 P2 · pcap 文件浏览器+过滤器 P2 · FUSB302 PD 嗅探 P2

### M5 报告中心
统一 JSON 报告(契约冻结) P0 · 报告列表/过滤 P0 · 两份对比 diff P0 · 导出 CSV/HTML/打印 P1 · 产线标签页(SN 二维码) P1 · 签核流 P2

### M6 数据管理（V1.5 主体）
SQLite 历史库(计划/运行/步骤三级) P1 · 良率/Pareto P1 · SN 全历史追溯 P1 · 测量分布 SPC 入门 P1 · 数据导出 P1

### M7 扩展系统（V2.0 主体）
步骤插件 DLL(C ABI) P2 · 面板插件 SDK P2 · 脚本自动化 P2

### M8 工程化基建（"像个软件"的地基）
安装包+签名 P0 · minidump 崩溃收集+提交引导 P0 · 设置中心 P0 · 性能红线(10 万行日志/500 设备) P0 · 多显示器 DPI 记忆 P0 · 帮助 F1+内置手册 P1 · i18n P1 · 自动更新 P1 · 无障碍 P1 · 关于/许可 P0

### 🔗外联调度（统一页+各面板内嵌）
dfu-util · USBPcap · **USB3CV/xHSETT(设备章 9 合规,纯软件可跑)** · f3 · UsbTreeView —— spawn+stdout 回收+超时+取消；USB3CV 带接管 xHCI 红色警示

### ⛔硬件边界（明示不自建，文档写明需何硬件）
电气合规(SigTest/USBET20=示波器) · USB4/PD 物理层合规(USB4CV/QuadraMAX=分析仪平台) · 空口抓包(Ellisys/nRF Sniffer) · APx 级音频分析 · PD 独立测量(建议 ¥300 级 POWER-Z 类采样硬件做 🔗半独立通道 P1)

## 三、混合容器架构（方案 A 修订版）

```
USBTestStudio.exe ── GUI 主进程(小而稳,零依赖)
 ├ usts_core.lib   纯逻辑内核(静态链接;现有 framework/usb/engine/parser/report 平移,483 例自测保留)
 ├ 面板框架         预设布局+面板注册表+主题/设置
 └ 功能 DLL(按需加载,缺失=入口灰+指引): uvc_preview / uac_measure / ble_panel / pcap_browser …(=V2.0 插件 ABI)
usts_runner.exe ── 产测引擎子进程(跑完即退;复用 --auto 入口;链接同一 core)
 IPC=命名管道 JSON-lines(消息=现行 WM_APP 载荷直译) · 设备租赁表=每设备路径一把命名互斥量
外联子进程 ── dfu-util/USBPcap/USB3CV/f3/UsbTreeView
```

分治判据与对标先例：GUI 单进程(USBTreeView/Docklight) · DLL 隔离重依赖(nRF Connect app 化；缺 DLL 优雅降级=sounddevice 哲学) · runner 独立(Wireshark dumpcap/TestStand out-of-process；MES 无头本就需独立进程) · 外联子进程(IDE external tools)。**不做每面板一进程**：USB 句柄进程私有，4 类容器已覆盖隔离需求。

## 四、可行性评估（摘要）

| 风险 | 等级 | 缓解 |
|---|---|---|
| Win32 自绘面板框架 | **高(唯一)** | 预设布局(不做自由 docking,砍掉最大成本坑)+3 面板竖切先行;退路=主窗口分栏 |
| USB3CV 接管 xHCI | 中 | 强警示+专用测试机建议 |
| MediaFoundation 预览 | 中 | V1.0 只承诺 YUY2/MJPEG,余回退采帧管线 |
| 代码签名 | 低/预算 | EV 证书 ~¥2-3k/年,待拍板 |
| WinRT 蓝牙 | 中 | 已隔离 DLL+P1 |

工作量：V1.0 ≈105 人日(AI 辅助 ≈45-55) · V1.5 ≈55 · V2.0 ≈55。
技术债红线：新功能只进 C++ 主线(Python usbtest 冻结为契约对照物)；22 项不确定 API 清单在 V1.0 真机联调集中清账。

## 五、与现有资产的关系

| 现有 | 处置 |
|---|---|
| Win EP-4 控制台(console/session/discovery/channel) | **全量平移**进新面板框架（M3 就是它） |
| 产测引擎+计划契约+报告 JSON | 冻结不动,内核平移,GUI 化包装(M2) |
| Python usbtest | 冻结为契约对照物+CI 参考 |
| usbsim | 不变(独立仿真器,内核测试的假件源) |
| macOS Swift 端 | 挂起,待 Windows V1.0 定型后按同设计平移 |
| 22 项不确定 API 清单 | V1.0 真机联调集中清账 |
