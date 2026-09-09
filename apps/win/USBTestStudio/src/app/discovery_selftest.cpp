// discovery_selftest.cpp — EP-4 S2 设备发现离线自测（无 USB 设备即可跑）：
// 即时过滤核心（device_catalog：匹配域/大小写/多关键词 AND/kind 掩码/保序过滤）、
// COM 口枚举（serial_enum：真注册表 SERIALCOMM，本机无串口时为空表同样绿）、
// MSC 盘行（msc_enum：真 PhysicalDrive 属性查询，无 U 盘机器上空表同样绿）、
// 目录组装与会话工厂（catalog_build：DeviceInfo/MSC/COM 合流 + 四通道创建口径）。
// UI 表格与双击开会话的窗口层按切片表另行（真机）验收。
#include "app/log.h"
#include "discovery/catalog_build.h"
#include "discovery/device_catalog.h"
#include "discovery/msc_enum.h"
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

// MSC 扫描假件（与 MscScsi 同名成员子集，evolve #73）：按盘号注入各分支——
// 0~4/9 正常入列、5 容量探测失败/超时、6 非 USB 静默跳过、7 打开失败静默跳过、
// 8 属性查询失败。记录探测超时秒数供断言（kScanTimeoutS 有界钉，对抗复核 2a）
static unsigned g_cap_timeout_s = 0, g_inq_timeout_s = 0;
class MockScanPort {
public:
    bool open_physical_drive(unsigned index, bool, std::wstring*) {
        m_index = index;
        return index != 7;                       // 7 号盘：打开失败
    }
    bool bus_is_usb(bool* is_usb, std::wstring* err) {
        if (m_index == 8) {                      // 8 号盘：属性查询失败
            if (err) *err = L"模拟属性查询失败";
            return false;
        }
        *is_usb = m_index != 6;                  // 6 号盘：系统盘（非 USB）
        return true;
    }
    bool read_capacity(unsigned long long* total, unsigned* blk, std::wstring* err,
                       unsigned timeout_s = 0) {
        g_cap_timeout_s = timeout_s;
        if (m_index == 5) {                      // 5 号盘：探测失败/超时（可见性 2c）
            if (err) *err = L"模拟探测超时";
            return false;
        }
        *total = 15667200;
        *blk = 512;
        return true;
    }
    bool scsi_inquiry(std::string* v, std::string* p, std::string* r, unsigned char*,
                      std::wstring*, unsigned timeout_s = 0) {
        g_inq_timeout_s = timeout_s;
        *v = "MOCKLAB";
        *p = "UDISK-S";
        *r = "1.0";
        return true;
    }

private:
    unsigned m_index = 0;
};

int wmain() {
    ustlog::init(/*also_stdout=*/true, L"usts-discovery");   // 日志规范见 app/log.h

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

    // ---- MSC 盘行（msc_enum，S5 后半） ----
    ConsoleDevice md = msc_enum::make_device(2, L"Generic Flash Disk");
    check(md.kind == DeviceKind::msc && md.path == L"\\\\.\\PhysicalDrive2",
          "MSC make_device 字段");
    check(md.name == L"Generic Flash Disk（PhysicalDrive2）", "MSC 具名行");
    ConsoleDevice mu = msc_enum::make_device(5, L"");
    check(mu.name == L"USB 大容量磁盘 5", "MSC 无名行回退");
    check(md.kind_text() == L"MSC", "MSC kind_text");
    check(device_matches(md, L"scsi"), "MSC 同义词 scsi 命中");
    check(device_matches(md, L"usbstor"), "MSC 同义词 usbstor 命中");
    check(device_matches(md, L"physicaldrive2"), "MSC 路径命中");
    check(!device_matches(md, L"", kind_bits(DeviceKind::hid))
              && device_matches(md, L"", kind_bits(DeviceKind::msc)), "MSC 掩码过滤");

    // ---- 真 PhysicalDrive 扫描（无 U 盘机器上空表，同样必须绿；只验行形态） ----
    auto msc_rows = msc_enum::scan(&err);
    bool msc_form = true;
    for (const auto& m : msc_rows)
        if (m.kind != DeviceKind::msc
            || m.path.rfind(L"\\\\.\\PhysicalDrive", 0) != 0)
            msc_form = false;
    check(msc_form, "MSC 扫描行均具形态");

    // ---- MSC 扫描内核（假件注入：探测有界 + 失败可见性，evolve #73） ----
    err.clear();
    auto mock_rows = msc_enum::scan_impl<MockScanPort>(&err);
    check(mock_rows.size() == 6, "MSC 假件扫描 10 盘收敛 6 行（5/6/7/8 号各按因跳过）");
    bool mock_form = true;
    for (const auto& m : mock_rows)
        if (m.kind != DeviceKind::msc
            || m.path.rfind(L"\\\\.\\PhysicalDrive", 0) != 0)
            mock_form = false;
    check(mock_form && mock_rows.back().path == L"\\\\.\\PhysicalDrive9",
          "MSC 假件行具形态且含 9 号尾盘");
    check(mock_rows[0].name.find(L"MOCKLAB UDISK-S") != std::wstring::npos,
          "MSC 假件行带 INQUIRY 身份");
    check(g_cap_timeout_s == 3 && g_inq_timeout_s == 3,
          "MSC 扫描容量/INQUIRY 探测同界 3s 有界（不回落端口 10s）");
    check(err.find(L"PhysicalDrive5") != std::wstring::npos
              && err.find(L"超时") != std::wstring::npos,
          "MSC 探测失败/超时留痕于 err（慢盘消失可溯，不无声少行）");

    // ---- DeviceInfo → 目录条目（catalog_build，S2 后半） ----
    DeviceInfo di_hid;
    di_hid.class_name = L"HID";
    di_hid.vid = 0x1234;
    di_hid.pid = 0x0002;
    di_hid.path = L"\\\\?\\hid#vid_1234&pid_0002#8&2c9d0e1f&0&0000";
    di_hid.name = L"USB-Labs Keyboard";
    ConsoleDevice h = catalog_build::from_device_info(di_hid);
    check(h.kind == DeviceKind::hid && h.path == di_hid.path, "HID 收集 → hid 条目");
    check(h.vid == 0x1234 && h.vidpid_text() == L"1234:0002", "VID:PID 透传");
    check(h.name == L"USB-Labs Keyboard", "名称透传");

    DeviceInfo di;
    di.class_name = L"USB";
    di.vid = 0x1234;
    di.pid = 0x0002;
    di.path = di_hid.path;
    ConsoleDevice u = catalog_build::from_device_info(di);
    check(u.kind == DeviceKind::usb && u.name == L"1234:0002", "USB 节点 → usb，空名回退 VID:PID");
    di.vid = 0;
    di.pid = 0;
    ConsoleDevice n = catalog_build::from_device_info(di);
    check(n.kind == DeviceKind::usb && n.name == L"USB 设备", "无名无 VID 回退协议名");

    // ---- 目录组装（USB/HID 枚举 + MSC + COM 合流） ----
    DeviceInfo usb_disk;
    usb_disk.class_name = L"USB";
    usb_disk.vid = 0x0C76;
    usb_disk.pid = 0x4002;
    usb_disk.path = L"\\\\?\\usb#vid_0c76&pid_4002#7&11e0f3c2&0&1";
    usb_disk.name = L"USB 大容量磁盘";
    const auto cat = catalog_build::build_catalog({di, di_hid, usb_disk}, {md},
                                                 {L"COM7", L"COM3"});
    check(cat.size() == 6, "目录 = USB/HID 枚举 + MSC + COM 合流");
    check(cat[0].name == L"USB 设备" && cat[1].kind == DeviceKind::hid
              && cat[2].kind == DeviceKind::usb, "USB/HID 枚举原序保持");
    check(cat[3].kind == DeviceKind::msc && cat[3].path == L"\\\\.\\PhysicalDrive2",
          "MSC 原序接续");
    check(cat[4].path == L"COM7" && cat[5].path == L"COM3", "COM 原序接续");
    check(cat[5].kind == DeviceKind::serial && cat[5].name == L"COM3", "COM 条目字段");
    auto hit = filter_devices(cat, L"COM3");
    check(hit.size() == 1 && hit[0].kind == DeviceKind::serial, "组装目录过滤命中 COM3");
    hit = filter_devices(cat, L"0c76 大容量");
    check(hit.size() == 1 && hit[0].name == L"USB 大容量磁盘", "组装目录过滤命中 VID+名称");
    hit = filter_devices(cat, L"scsi");
    check(hit.size() == 1 && hit[0].kind == DeviceKind::msc, "组装目录过滤命中 MSC");

    // ---- 会话工厂（双击开会话的通道口径；真机打开在切片验收） ----
    check(catalog_build::make_channel(h) != nullptr, "HID 条目 → 会话通道");
    check(catalog_build::make_channel(cat[4]) != nullptr, "串口条目 → 会话通道");
    check(catalog_build::make_channel(u) != nullptr, "通用 USB 条目 → WinUSB 通道（S5）");
    check(catalog_build::make_channel(md) != nullptr, "MSC 条目 → SCSI 直通通道（S5）");

    printf("discovery_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
