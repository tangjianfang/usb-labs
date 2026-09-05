# Lab4 经验库：UVC/UAC 实战踩坑

> 定位：**社区经验 = 公开资料的共识整理**（每条附真实链接），非本仓库私有实测数据；涉及价格/周期的均为区间量级，**以官方与实时行情为准**。协议细节以 USB-IF 规范原文为准。

---

## 1. MJPEG 无 Huffman 表（DHT 缺失）——"花屏但部分软件正常"的经典

**现象**：同一台设备，浏览器/微信里画面正常，OpenCV/某些播放器里灰屏或马赛克条带；采集棒类产品尤其高发。

**根因**：JPEG 标准的 Huffman 表（DHT 段）可以被省略，前提是解码器使用 Annex K 默认表。UVC MJPEG 设备为了压 CPU/带宽普遍**省略 DHT**——这符合规范，但解码端实现分成两派：内置默认表兜底的（浏览器、FFmpeg mjpeg 解码器）正常；严格按流内表解码的（部分 OpenCV 后端、简易播放器、某些嵌入式解码器）直接花屏。

**共识做法**：
- **解码端**兜底：检测无 DHT 时注入标准默认表（FFmpeg/libjpeg 系已内置）；
- **固件端**最稳妥：首帧头之后插入一次 DHT（几十字节，开销可忽略）——商固件常见的"兼容性补丁"就是这个；
- 排查口诀：花屏先换软件解码验证，再抓包看 JPEG 帧头是否以 `FF D8` 开头、有无 `FF C4`（DHT）段。

**参考**：
- UVC 规范对 MJPEG 载荷的约定（USB-IF 文档库下载 UVC 1.5 原文）：https://www.usb.org/documents
- Linux UVC（uvcvideo）项目站，含设备兼容性差异与 FAQ：https://www.ideasonboard.org/uvc/
- FFmpeg 内置 MJPEG 解码器（含默认 Huffman 表处理）：https://ffmpeg.org/ffmpeg-codecs.html#mjpeg
- 知识库：[UVC 详解 §5（DHT 缺失根因）](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md)

---

## 2. 多摄像头带宽规划——"单插正常，挂 Hub 就掉帧"

**现象**：一台 720p@30 摄像头直插正常；两三台接同一个 Hub（或笔记本的扩展坞）后，帧率腰斩甚至开不了流。

**根因（数学账，非玄学）**：USB 2.0 每微帧最多 80% 时间给周期性传输（同步+中断）≈ **6000 B/微帧**的全总线预算。一台高带宽端点摄像头（3072B/微帧）吃掉一半；**两台已逼近上限**；三台直接挤不下。Hub 不增加 USB 2.0 带宽，只把同一份预算共享给下游；FS 摄像头还要经 Hub 的 TT（事务转换器）串行化，更紧。

**共识做法**：
- 逐口预算制：每台设备的 `wMaxPacketSize × 8000` 相加 ≤ 6000B/微帧，留中断与控制余量；
- 多摄像头产品：直插主机根 Hub 不同端口；USB3 Hub 下行 USB2 段同样只有一份 480Mbit/s 预算，换 USB3 摄像头或 UASP 采集盒才是出路；
- 会议整机内部则常用专用上行链路（双 USB 控制器或 MIPI 直连），把摄像头与音频挂不同总线；
- 排查口诀：先换直插主机口复测（知识库"常见故障"第一条），再算预算。

**参考**：
- USB 2.0 规范（微帧预算/周期性传输 80% 限制，USB-IF 文档库）：https://www.usb.org/documents
- 知识库：[UVC 详解 §6（带宽被同步端点挤占的故障特征）](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md) · [四种传输类型](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)
- 本文配套计算：[hardware/设计要点.md §7](hardware/设计要点.md)

---

## 3. Windows 相机隐私开关——不是 UVC 描述符的事

**现象**：设备枚举正常（设备管理器/`lsusb` 都在），但相机应用黑屏或提示"无法启动相机"；同一台设备在另一台 Windows 机器上正常。

**根因**：Windows 的相机访问控制在 **OS 策略层**（设置 → 隐私和安全性 → 相机，内部由 CapabilityAccessManager/ConsentStore 管理），它直接停掉流（切回 Alt0 / 拒绝开流），**不通过任何 UVC 类请求询问设备**——所以 UVC 描述符里没有任何"隐私开关"字段，抓包也抓不到"拒绝"报文。设备侧与之相关的标准能力只有：UVC 1.5 后微软生态推广的**硬件隐私快门/禁用开关指示**（多为 MIPI/驱动层扩展与认证要求项），普通 UVC 设备不必实现。

**共识做法（OEM 视角）**：
- 排查顺序：隐私开关 → 相机被独占（另一个应用占用）→ 驱动层（Usbvideo.sys 事件日志）→ 固件；
- 支持文档里必须写清"无法预览请先检查系统相机权限"——这是消费类摄像头客诉第一大来源（社区共识）；
- 认证要求（Teams 等）包含工作指示灯/隐私状态指示等用户体验项，见 §5。

**参考**：
- Microsoft 支持：相机在 Windows 中无法工作（隐私设置官方排查）：https://support.microsoft.com/en-us/windows/camera-doesn-t-work-in-windows-32adb016-b29c-a928-0073-53d31da0dad5
- Microsoft Learn：USB Video Class Driver Overview（Windows UVC 驱动能力边界）：https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview

---

## 4. UAC 异步时钟——"宣传是异步，实测会漂"

**现象**：产品页写"异步时钟架构"，但长录音与真实世界时间对不上；或录音周期性"咔哒/断续"；两台设备级联时音质劣化。

**根因**：异步≠精准。设备用本地晶振（典型 ±20–50ppm）做采样时钟，异步的价值是**不受 USB 帧节拍抖动污染**，漂移本身必然存在，靠主机跟随/反馈补偿（机制见 [capture/UVC流建立帧分析.md §5](capture/UVC流建立帧分析.md)）。常见翻车三种：
1. **假异步**：描述符声称 asynchronous，实现却是自适应/同步模式（如无反馈端点的播放方向），主机补偿策略落空 → 断续；
2. **时钟分家**：I2S 麦时钟与 USB 时钟不同晶振，两域各漂各的 → 长录漂移叠加；
3. **宣传与实测差距**：±50ppm 晶振录 1 小时漂 ±180ms——听不出来，但对音视频对齐/拼接是硬伤。

**共识做法**：
- 实测漂移：`arecord -d 3600` 录满 1 小时对比文件时长（ppm = 偏差/3600s）；或抓反馈端点值看波动（§5.3）；
- 设计上让音频时钟与 USB 时钟同源（同晶振分频），或干脆声明与实现一致的同步/自适应模式；
- 验收标准写数字（如 ≤±50ppm @ 量产温区），不写"异步高品质"形容词。

**参考**：
- UAC 1.0 规范（端点与时钟模式定义，USB-IF 文档库）：https://www.usb.org/documents
- 知识库：[UAC 概述 §4 时钟同步三模式](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/00-UAC概述.md) · [UAC1.0 详解 §3](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md) · [02-UAC2与UAC3 §1.1 时钟实体三件套](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/02-UAC2与UAC3.md)

---

## 5. 会议设备 OEM 的 Teams/Zoom 认证——成本量级认知

**现象**：创业团队做完一台"参数很好"的会议摄像头/麦克风，问"怎么打上 Certified for Teams / Zoom 认证"。

**共识认知（公开资料整理，⚠ 具体费用与流程以官方计划页面为准，且政策随年度变化）**：
- 认证不是"提交固件等盖章"：微软 Teams 设备认证 = 第三方授权实验室测试 + 多轮官方审核，覆盖**音质指标（回声消除/本底噪声/频响/延迟）、视频体验、互操作场景（真机矩阵）、固件升级安全、隐私体验（指示灯）**等规格清单；
- **花费量级**：单产品线整体（预测试+正式测试+整改回归+官方费用）公开渠道共识约 **数万至十几万人民币（≈1–5 万美元量级）**，周期 **3–9 个月**；整改不通过回炉重测会显著放大数字；
- **前置成本**先于认证：USB-IF 合规/VID 授权（数千美元量级/年）、FCC/CE 电磁兼容（数万元级/型号）、音频实验室调 ECHO/降噪（人力大头）；
- Zoom 硬件认证同理（其官方硬件计划入口见参考链接），两家规格高度重叠，做法上先过 Teams 规格再平移是社区常见路线；
- 商业结论：认证投入决定了会议设备整机售价与渠道，"认证成本"应计入 BOM/定价模型（对照 [hardware/BOM.csv](hardware/BOM.csv) 的整机价格区间理解）。

**参考**：
- Microsoft Learn：Certification for Teams devices overview（官方计划入口与流程）：https://learn.microsoft.com/en-us/microsoftteams/devices/certification-overview
- Microsoft Learn：Certification specifications for Teams devices（测试规格清单）：https://learn.microsoft.com/en-us/microsoftteams/devices/certification-specifications
- Zoom Help Center（搜索 "hardware certification"，以官方页面为准）：https://support.zoom.com
- USB-IF 合规计划（VID/TID，以官方为准）：https://www.usb.org/documents
