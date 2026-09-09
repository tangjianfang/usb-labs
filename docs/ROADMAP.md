# ROADMAP · 全部计划任务（自定义开发时代）

> 2026-09-10 立账。evolve 协议与驱动已退役（用户删除插件），本文件取代 evolve 目标池成为唯一计划源。
> 状态图例：☐ 待办 · ◐ 进行中/半成品 · ☑ 完成 ·（提案）= 需用户拍板才排期。
> 完成一项就在本文件勾掉并写一行 evidence（提交号/验收记录）。

## P0 · 接续收尾（先做，清掉历史遗留）

- ◐ **T1 · #79 半成品收尾**：`tools/usbtest/msc_test.py` CDB 选路补"块数 ≤0xFFFF 才走 10 字节 CDB，否则升 READ/WRITE(16)"边界（16 位域溢出静默错值/晦涩 OverflowError）。工作树已有 66 行 diff（msc_test.py + test_msc.py），需：复核口径与 C++ msc_read_cdb 一致 → 跑 `tests/test_msc.py` 全绿 → 提交。evolve 时代最后一个未闭环项。
- ☐ **T2 · 仓库卫生**：`tools/reports/`（产测运行产物 JSON）加 .gitignore；`.auto-evolve.lock`、`docs/auto-evolve-driver.log` 归档或 ignore；USBTree 侧 `scripts/auto-evolve*.sh/.cmd` 驱动基建退役（归档到 docs/历史 或删除）。
- ☐ **T3 · 记账闭环**：USBTree `docs/evolve-log.md` 头部 status 改"已终止（用户删除 evolve，转自定义开发）"+ 终局 checkpoint（终止于 #78，指针 #79 作废），防止未来会话误读指针复活驱动。

## P1 · EP-4 工程师通信控制台（代码全齐 → 验收关闭）

- ☐ **T4 · Lab5 遥测契约固件**（解锁链最上游，~20 行）：在 PE_SNK Ready 状态与 VBUS ADC 采样处向调试口打印 `cc_state/contract_v/contract_i/vbus_v`（契约已冻结见 `tools/usbtest/STATUS.md` 第二节，只加不改）。
- ☐ **T5 · EP-4 真机验收**（人工，插真机）：按 `apps/验收-工程师通信控制台.md` A~D 逐项——A 关键词 3 秒定位设备、B 双会话收发、C HID 解析、D U 盘读扇区+WinUSB 收发。验收过 = EP-4 关闭。
- ☐ **T6 · S6 PD 解析面板**（依赖 T4）：pd key=value 遥测 → 面板可视化（CC 状态机迁移 + 合同电压/电流曲线）。
- ☐（提案）**T7 · macOS 端对齐**：发现/过滤/会话台/解析四件套目前 Windows 独有（Swift 端仍是旧产测形态），按 Win 版切片移植。

## P2 · usbtest 产测框架（mock 全绿 → 真机闭环）

- ☐ **T8 · 逐后端真机验证**：按 STATUS.md 工装清单逐项——HID 环回（Lab1 固件自环模式 TODO）、CDC TX-RX 短接治具、dfu-util+DFU 分区、UVC opencv 采帧、BLE 屏蔽箱+适配器、MSC 空白盘破坏性写验证（DESTRUCTIVE 红线）、hub port cycle。
- ☐ **T9 · UAC 后端实现**：`uac_record_level` 是唯一"仅 mock"处理器，需 sounddevice + OS 音频路由。
- ☐ **T10 · dock 后端 Windows 路径**：现依赖 Linux lsusb，Windows 接 UsbTreeView CLI。

## P3 · 工程化/商业化最后一公里（提案，需拍板优先级）

- ☐（提案）**T11 · Windows 安装包**：Inno Setup/MSI、应用图标、版本号方案、发布页。
- ☐（提案）**T12 · 崩溃与运行日志落地**：文件日志 + 崩溃转储（现排障全靠控制台输出）。
- ☐ **T13 · CI 增强**：MSVC 四靶构建进 GitHub Actions（现 CI 仅 Python mock+自测）；确认 usbsim 自测在 CI 里。
- ☐ **T14 · lab1~lab7 真机 bring-up**：按实验室顺序逐个真机跑通，experience 文档回填实战数据。

## P4 · USBTree 知识库

- ☐ **T15 · EP-2 材料解锁**（人工）：浏览器下载 GATT Specification Supplement PDF → 放 `80-参考资料/bluetooth/` → 回填标准 UUID 全表（50/04-ATT与GATT 扩叶）。
- ☐ **T16 · 池刷新**：COVERAGE 残余空白重核（清单自 2026-09-05 未动）+ 模块轮换抽查（90/02 速查表 → tools → graph）。
- ☐ **T17 · 技能包同步**：usb-spec-lookup 缓存映射与实际缓存目录核对（37 份）。

## 建议执行顺序

T1→T2→T3（半天内清完）→ T4（解锁链上游）→ T5（人工验收，可并行）→ T6 → T8 按工装到位顺序 → T13 随时可做 → T7/T11/T12 拍板后排 → T15 等材料 → T16/T17 穿插。
