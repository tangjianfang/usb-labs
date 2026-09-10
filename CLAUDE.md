# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库定位与语言约定

USB 全栈实战实验室（商业级教学仓库）：7 个实验室覆盖 HID / CDC+DFU / MSC / UVC+UAC / PD / BLE 双模 / 扩展坞，从硬件设计、固件、主机工具到产线测试。概念性"为什么"沉淀在配套知识库 [USBTree](https://github.com/tangjianfang/USBTree)（另一仓库），本仓库只讲"怎么做"——解释协议概念时链接 USBTree，不重复展开。

- **文档与提交信息用中文**；提交走 conventional 前缀 + 中文摘要（如 `fix: …`、`docs: …`）。
- 仓库有强"诚实声明"文化（见根 README）：`captures/` 是按规范重构的教学样本而非真实抓包；**未经真机验证的东西不得写成已验证**。

## 常用命令

Python 侧无 pytest，全部测试以脚本方式直接运行（CI 口径见 `.github/workflows/usbtest-mock.yml`）：

```bash
pip install -r tools/usbtest/requirements.txt   # mock 模式仅需 pyyaml

# 七实验室产测计划 mock 回归（须在 tools/ 目录下运行，-m usbtest 依赖 CWD）
cd tools
for plan in ../labs/*/host/autotest.yaml; do python -m usbtest --plan "$plan" --mock --report-dir /tmp/reports; done

# 离线自测（无硬件，任意 CWD）
python tools/usbtest/tests/test_msc.py          # MSC 容量/CDB 内核假件 + usbsim 端到端
python tools/usbtest/tests/test_backends.py     # cdc/hid/uvc/pd/ble/dock/uac 后端派发冒烟
python tools/usbtest/tests/test_logbase.py      # 日志格式
python simulator/tests/test_sim.py              # usbsim 协议仿真器全链路

# 单测过滤（unittest 靶支持标准参数）
python tools/usbtest/tests/test_msc.py -v TestCapacityKernel.test_sentinel_upgrades_to_rc16

# Markdown 链接/围栏校验（CI 必跑）
bash tools/validate.sh
```

真机产测（需 DUT/工装）：`python -m usbtest --plan <yaml> --dut-sn <SN> --station STN-01`，退出码 0/1 供 MES 集成；哪些处理器缺什么工装见 `tools/usbtest/STATUS.md` 工装清单。

Windows 原生上位机（VS2022 v143、C++20、x64，零第三方依赖除 vendored spdlog）：

```bat
cmake -S apps/win/USBTestStudio -B apps/win/build -G "Visual Studio 17 2022" -A x64
cmake --build apps/win/build --config Release
:: 五靶 = 主程序 + 四个离线自测（CI 的 msvc-build job 同口径）
apps\win\build\Release\channel_selftest.exe
apps\win\build\Release\discovery_selftest.exe
apps\win\build\Release\session_selftest.exe
apps\win\build\Release\parser_selftest.exe
```

固件（labs/labN/firmware/）依赖外部 SDK（pico-sdk / STM32CubeF1 / Nordic NCS），CI 不编译；构建步骤以各 lab 的 `firmware/README.md` 为准。

## 架构

四套代码 + 教学文档，核心是**同一套产测契约的三端同构**：

```
labs/lab1..7/     每实验室固定五件套：README + hardware/ + firmware/ + host/ + capture/（另 experience.md）
tools/usbtest/    Python 产测框架（参考实现）
simulator/usbsim/ 纯软件 USB/BLE/PD 协议仿真器（CRC 实算、状态机、错误注入）
apps/win|mac/     USBTestStudio 原生上位机（Win32/C++20、Swift/AppKit）
captures|docs|experience/  教学文档（重构样本 + 方法论 + 坑集）
```

### usbtest 派发约定（扩展后端时的核心模式）

`core.run_plan` 按 `device.backend` 动态导入 `usbtest/<backend>_test.py`，取模块级 `HANDLERS = {<step_type>: callable(ctx, params) -> StepResult}`；模块可带可选的 `open_device(device)`（无设备句柄概念的后端如 ble/dock 不带，run_plan 回退 None）。`--mock` 整体换用 `mock_test.py`（确定性通过）——CI 借此在无硬件环境验证计划语法与报告链路。新步骤类型必须注册进对应后端的 `HANDLERS`，否则 run_plan 记 FAIL 继续（步骤级失败不中断整站）。

### 三端冻结契约（改动前必读）

- **报告 JSON**：`{plan, station, dut_sn, verdict, started, steps[{name, pass, measured, note}]}` 三端（Python/Win/macOS）字段级一致，是 MES 集成点。
- **测试计划字段**：YAML（Python）与 JSON（原生应用）同名同义，同一份计划可互换。
- **PD 遥测契约**（`labs/lab5-pd-charger/firmware/src/telemetry_contract.h` + `pd_test.py`）：DUT 串口输出 `key=value` 行（`cc_state/contract_v/contract_i/vbus_v`）。**只加字段不改既有字段**。
- **MSC CDB 内核口径**：Python `msc_test.py` 与 C++ `msc_scsi` 刻意保持同口径（>2TB RC10 哨兵→RC16、READ/WRITE(16) 高 LBA 选路、31 字节 CBW），两侧都有假件自测钉住；改一侧必须同步另一侧。

### Windows 应用分层

`src/framework`（RAII/自研 JSON）→ `usb/`（设备访问：SetupDi/HID/串口/SCSI 直通/WinUSB）→ `channel|discovery|session|parser`（EP-4 通信控制台，纯逻辑、模板化可注假件）→ `engine/`（计划执行）→ `ui/`。UI 线程零阻塞 I/O：引擎在 `std::jthread`，经 `PostMessage(WM_APP+1..3)` + 堆载 payload 回 UI。日志统一走 `src/app/log.h` 门面（spdlog），Python 侧 `logbase.py` 对齐同一格式；级别开关环境变量 `USBTS_LOG_LEVEL`。版本单一事实源：`src/app/version.h`。

### 仿真器与产测的关系

usbsim 的 mock 后端升级路径：`usbtest` 步骤 → 驱动 usbsim 虚拟设备 → 虚拟总线真协议行为断言。MSC 已端到端接通（`test_msc.py` 里的 UsbSimBotDev），其余后端 mock 仍是确定性占位。

## 状态记账（改动后要落的账）

- **`tools/usbtest/STATUS.md`**：处理器 × 验证状态矩阵。真机跑通某处理器后把该行改 ✅ 并注明设备名+日期，git 提交。
- **`docs/ROADMAP.md`**：唯一计划任务源（evolve 已退役）。完成一项就勾掉并写 evidence（提交号）；标注"阻塞：需真机/工装"的项不要在纯代码会话里闭账。
- 破坏性红线：`msc_write_verify` 只允许空白盘/授权测试介质。
