# Lab4 固件：UVC + UAC 复合设备

> **定位声明（必读）**：`src/` 下的代码是**教学级骨架**——把 UVC/UAC 描述符、Probe/Commit 协商、MJPEG 分帧这些"协议难啃的部分"写成可读、可移植、可逐字段对照抓包的 C 代码；不含厂商 HAL、不追求完整商用特性。
> **完整商用的 UVC 固件请以 SoC 方案为准**（Linux UVC gadget 或厂商原生 UVC 栈，见 §2）。这不是偷懒：商业会议/采集产品的视频管线（ISP、H.264、多路流）本来就超出 MCU 的物理能力（见 [../hardware/设计要点.md §7](../hardware/设计要点.md) 的带宽账）。

---

## 1. MCU 方案（教学级）

### 1.1 目标与硬件

| 项 | 内容 |
|---|---|
| 平台 | STM32H743ZIT6（NUCLEO-H743ZI2 或自画板）+ USB3300 ULPI PHY |
| 视频 | OV5640 DVP 模组（JPEG 直出）→ DCMI → UVC 同步流 |
| 音频 | INMP441/ICS-43434 I2S MEMS 麦 → SAI → UAC1 同步流 |
| 设备形态 | IAD 复合：UVC（VC+VS）+ UAC1（AC+AS），bDeviceClass=0xEF/0x02/0x01 |
| 诚实预期 | 640×480@30 稳；720p 名义 30fps、实际 15–25fps（单事务同步端点 8.19 Mbit/s 上限）；麦克风 16k/48k 16bit 单声道 |

### 1.2 src/ 文件说明

| 文件 | 内容 | 学习重点 |
|---|---|---|
| [src/uvc_descriptors.c](src/uvc_descriptors.c) | FS/HS 两套配置描述符：IAD、VC 接口（Camera Terminal/Processing Unit/Output Terminal 实体图）、VS MJPEG 格式与帧描述符、同步端点；`uvc_probe_ctrl_t` Probe/Commit 结构体 | 描述符逐字节 ↔ `lsusb -v` 输出对照 |
| [src/uac_mic_descriptors.c](src/uac_mic_descriptors.c) | UAC1 麦克风：AC 接口（IT/Feature Unit/OT）、AS 接口 Alt0/Alt1、Type I 格式、类特定端点；Feature Unit 的 MUTE/VOLUME 请求处理 | UAC1 请求必答集（CUR/MIN/MAX/RES） |
| [src/mjpeg_frame_fill.c](src/mjpeg_frame_fill.c) | MJPEG 载荷填充伪代码：载荷头组装（FID/EOF/PTS/SCR）、按 `dwMaxPayloadTransferSize` 切片、丢帧策略 | FID 翻转与抓包分帧（[../capture/UVC流建立帧分析.md](../capture/UVC流建立帧分析.md)） |

### 1.3 构建与移植

代码刻意写成"协议层与 DCD 层分离"，移植只需实现 4 个钩子（任选 STM32CubeHAL、libopencm3 或寄存器）：

```c
// 需要平台层提供的最小接口（原型见各 src 文件头部注释）
void  dcd_init(void);                       // OTG_HS + ULPI 初始化，HS/FS 协商
void  dcd_ep0_send(const void*, uint32_t);  // EP0 IN（描述符/Probe 应答走这里）
void  dcd_iso_video_send(uint8_t ep, const void* buf, uint32_t len); // VS 同步 IN
void  dcd_iso_audio_send(uint8_t ep, const void* buf, uint32_t len); // AS 同步 IN
// 需要平台层回调上来的事件
//   SET_INTERFACE(VS, alt)  → uvc_vs_alt_changed(alt)
//   EP0 类请求 (bmRequestType 0x21/0xA1) → uvc_class_request() / uac_class_request()
```

构建（以 GCC+CMake 为例，CubeIDE 用户可直接把三个 .c 拖进工程）：

```bash
cd firmware/src
cmake -B build -DCMAKE_TOOLCHAIN_FILE=arm-none-eabi.cmake   # 骨架可先 host 端语法检查
cmake --build build
arm-none-eabi-objdump -d build/lab4.elf | less              # 烧录按平台常规 SWD 流程
```

### 1.4 验证步骤

1. 插 USB → `lsusb` 出现新设备；`lsusb -v -d 1234:5678`（示例 VID/PID）逐字段对照描述符源码；
2. Linux：`v4l2-ctl --list-formats-ext` 应列出 640×480 与 1280×720 MJPEG（命令见 [../host/预览与调参.md](../host/预览与调参.md)）；
3. 相机应用出图，`--stream-mmap` 计帧率验证教学预期；
4. `arecord -D hw:N -f S16_LE -r 48000 -c 1` 录音；`alsamixer` 里 MUTE/VOLUME 应可调（Feature Unit 生效）；
5. 若掉帧/花屏：先按 [../capture/UVC流建立帧分析.md](../capture/UVC流建立帧分析.md) 抓包核对 FID 与包长，再回头查 DMA 带宽。

---

## 2. SoC 方案（商业主流）：Linux UVC/UAC gadget

### 2.1 框架

Linux 把 USB device 控制器抽象为 UDC，gadget 功能（function）用 **configfs** 组装。我们的复合设备 = `uvc` function + `uac1` function：

```
┌ 用户态 ─────────────────────────────┐
│ 视频源(vivid/真实sensor via V4L2)    │   音频源(ALSA loopback/codec驱动)
│   └ uvc-gadget 用户态程序            │   └ ALSA gadget 内核直通
│        ↑ FunctionFS/uvc bus 端点桥   │
├ 内核态 ─────────────────────────────┤
│ usb_f_uvc.ko（描述符由 configfs 生成）│ usb_f_uac1.ko
│        UDC 驱动（musbfsh/dwc2/dwc3） │
└──────────────────────────────────────┘
```

两个关键事实（与 MCU 方案的本质差异）：

- **描述符由内核 configfs 生成**：你在 `/sys/kernel/config/usb_gadget/g1/functions/uvc.0/streaming/` 下用 `mkdir frame/... format/...` 声明格式与帧间隔，内核替你拼描述符——所以"手写描述符"的功力用在 MCU 方案里，在 SoC 方案里变成"读得懂 configfs 层级"；
- **用户态只管数据**：`uvc-gadget` 程序响应 UDC 事件（SET_INTERFACE、PROBE/COMMIT 转成的 ioctl），把视频源帧桥接到 gadget 端点——MJPEG 分帧/FID 逻辑与本 Lab `mjpeg_frame_fill.c` 同构，读通了教学代码就能读 uvc-gadget 源码。

### 2.2 快速跑通（树莓派/Luckfox/任意带 OTG 的 Linux）

```bash
# 内核需 CONFIG_USB_CONFIGFS_F_UVC / F_UAC1（官方文档 gadget-testing §UVC、§UAC1
# 提供了从 mkdir 到 UDC 绑定的完整 shell 示例，直接照抄即可）：
#   https://docs.kernel.org/usb/gadget-testing.html
# 用户态 uvc-gadget（freedesktop 官方，配合 libcamera/vivid）：
#   https://gitlab.freedesktop.org/camera/uvc-gadget
modprobe vivid           # 无真实 sensor 时用虚拟视频源
./uvc-gadget -s <gadget名> # 或 ./uvc-gadget -u /dev/videoX
# PC 端即出现 UVC 摄像头；UAC1 麦克风同理由 configfs 的 uac1 function 提供
```

### 2.3 平台备注

- **全志 V3s / 瑞芯微 RV1106**：社区主线/供应商内核都带 UDC + UVC gadget，生态资料多，适合自研与魔改；
- **海思 / 君正 T31**：SDK 内置原生 UVC 固件栈（RTOS 或精简 Linux），出货量大但生态封闭，按厂商 SDK 文档走；
- **ESP32-S2/S3**：若想要"介于 MCU 与 SoC 之间"的开源参照，Espressif 的 usb_device_uvc 组件（同步/批量双模式）与社区 esp32-usb-uvc-experiments 是可读性很好的商用级参考（链接见 [../README.md 参考资源](../README.md)）。

---

## 参考资源

- 知识库：[UVC 详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md) · [UVC 规范级-控制与格式全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/01-UVC规范级-控制与格式全表.md) · [UAC1.0 详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md)
- Linux 内核 gadget 测试文档（UVC/UAC1 configfs 完整示例）: https://docs.kernel.org/usb/gadget-testing.html
- uvc-gadget（freedesktop）: https://gitlab.freedesktop.org/camera/uvc-gadget
- Linux UVC driver & tools: https://www.ideasonboard.org/uvc/
- TinyUSB（开源设备栈，UAC 示例可直接跑）：https://github.com/hathach/tinyusb
- Espressif usb_device_uvc 组件：https://components.espressif.com/components/espressif/usb_device_uvc
