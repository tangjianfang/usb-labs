// discovery_selftest.cpp — EP-4 S2 设备发现离线自测（无 USB 设备即可跑）：
// 即时过滤核心（device_catalog：匹配域/大小写/多关键词 AND/kind 掩码/保序过滤）
// 与 COM 口枚举（serial_enum：真注册表 SERIALCOMM，本机无串口时为空表同样绿）。
// UI 表格与双击开会话（S2 后半）按切片表另行验收。
#include "discovery/device_catalog.h"
#include "discovery/serial_enum.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s (passed %d before failure)\n", what, g_pass);
        exit(1);
    }
    ++g_pass;
}

// 目录样例：三种 kind，覆盖设计 §2 表格中的行形态（HID 键盘/U 盘/串口）
static std::vector<ConsoleDevice> fixture() {
    return {
        {DeviceKind::hid, L"USB-Labs Keyboard", 0x1234, 0x0002,
         L"\\\\?\\hid#vid_1234&pid_0002#8&2c9d0e1f&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"},
        {DeviceKind::usb, L"USB 大容量磁盘", 0x0C76, 0x4002,
         L"\\\\?\\usb#vid_0c76&pid_4002#7&11e0f3c2&0&1#{a5dcbf10-6530-11d2-901f-00c04fb951ed}"},
        {DeviceKind::serial, L"COM7", 0, 0, L"COM7"},
    };
}

int wmain() {
    const auto devs = fixture();

    // ---- 展示文本 ----
    check(devs[0].vidpid_text() == L"1234:0002", "vidpid_text 格式");
    check(devs[1].vidpid_text() == L"0C76:4002", "vidpid_text %04X 口径");
    check(devs[2].vidpid_text().empty(), "无 VID 输出空");
    check(devs[0].kind_text() == L"HID" && devs[1].kind_text() == L"USB"
              && devs[2].kind_text() == L"串口", "kind_text 三类");

    // ---- 单关键词命中各匹配域 ----
    check(device_matches(devs[0], L""), "空 query 全命中");
    check(device_matches(devs[0], L"key"), "名称子串");
    check(device_matches(devs[0], L"KEY"), "不区分大小写");
    check(device_matches(devs[0], L"1234"), "VID 子串");
    check(device_matches(devs[0], L"0002"), "PID 子串");
    check(device_matches(devs[1], L"0c76"), "小写十六进制");
    check(device_matches(devs[1], L"0C76"), "大写十六进制折叠");
    check(device_matches(devs[0], L"HID"), "协议标签");
    check(device_matches(devs[2], L"cdc"), "串口英文同义词");
    check(device_matches(devs[0], L"vid_1234"), "路径子串");
    check(device_matches(devs[1], L"大容量"), "中文名称子串");
    check(!device_matches(devs[2], L"1234"), "无 VID 不误命中");

    // ---- 多关键词 AND（设计 §4.1） ----
    check(device_matches(devs[0], L"1234 key"), "多关键词 AND 命中");
    check(!device_matches(devs[0], L"1234 mouse"), "AND 一票否决");
    check(device_matches(devs[0], L"  key   1234 "), "多空格容忍");
    check(device_matches(devs[2], L"COM7 串口"), "中英混合关键词");

    // ---- kind 掩码（协议复选框二次过滤） ----
    check(device_matches(devs[0], L"", kind_bits(DeviceKind::hid)), "掩码命中 HID");
    check(!device_matches(devs[0], L"", DeviceKind::serial | DeviceKind::usb), "掩码排除");
    check(!device_matches(devs[0], L"key", kind_bits(DeviceKind::usb)), "掩码与关键词叠加");
    check(device_matches(devs[2], L"", kind_bits(DeviceKind::serial)), "掩码命中串口");

    // ---- filter_devices ----
    auto r = filter_devices(devs, L"1234");
    check(r.size() == 1 && r[0].name == L"USB-Labs Keyboard", "过滤收敛 1 行");
    r = filter_devices(devs, L"");
    check(r.size() == devs.size(), "空 query 全量");
    r = filter_devices(devs, L"nomatch");
    check(r.empty(), "无命中空表");
    r = filter_devices(devs, L"", kind_bits(DeviceKind::usb));
    check(r.size() == 1 && r[0].kind == DeviceKind::usb, "掩码过滤");

    // ---- COM 排序与形态 ----
    check(serial_enum::com_name_less(L"COM2", L"COM10"), "数字序 COM2<COM10");
    check(serial_enum::com_name_less(L"COM10", L"COM11"), "数字序 COM10<COM11");
    check(!serial_enum::com_name_less(L"COM7", L"COM7"), "相等不小于");
    check(serial_enum::com_number(L"XYZ") == 0, "非 COM 形态识别");

    // ---- 真注册表 COM 枚举（无串口机器上空表，同样必须绿） ----
    std::wstring err;
    auto coms = serial_enum::scan(&err);
    bool all_com = true;
    for (const auto& c : coms)
        if (serial_enum::com_number(c) == 0) all_com = false;
    check(all_com, "枚举结果均为 COM 形态");
    check(std::is_sorted(coms.begin(), coms.end(), serial_enum::com_name_less), "枚举已排序");

    // ---- COM → 目录条目 ----
    ConsoleDevice d = serial_enum::make_device(L"COM7");
    check(d.kind == DeviceKind::serial && d.path == L"COM7", "make_device 字段");
    check(device_matches(d, L"com7 cdc"), "条目可被过滤命中");

    printf("discovery_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
