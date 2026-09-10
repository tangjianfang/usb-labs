// vd_templates.h — 虚拟设备模板（设计 §3.2 模板▾ / 计划 MS2-T2）
// 模板=数据非代码：描述符模型 + 行为参数（key_seq=键盘扫描码序列）。
// MS2 两模板（HID 键盘/HID 复合键鼠）；MSC/PD/BLE 模板归 MS3 顺带（注记）。
#pragma once

#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::vd {

struct VdTemplate {
    std::string id;
    std::wstring name;
    desc::DescModel model;
    std::vector<uint8_t> key_seq;   // 扫描码序列（报告第 3 字节起；0=空帧）
};

// 键盘报告生成器：8B（modifier+reserved+6 键槽），序列步进循环
// （VirtualDevice::set_report_source 的现成数据源）
struct KeyboardReportSource {
    const std::vector<uint8_t>* seq = nullptr;
    size_t pos = 0;
    std::vector<uint8_t> operator()() {
        std::vector<uint8_t> r(8, 0);
        if (!seq || seq->empty()) return r;
        const uint8_t k = (*seq)[pos];
        pos = (pos + 1) % seq->size();
        r[2] = k;
        return r;
    }
};

// 内置模板（模型与 desc_selftest 黄金同源——Lab1 HID 复合口径）
inline const std::vector<VdTemplate>& builtin_templates() {
    static std::vector<VdTemplate> t = [] {
        auto golden = [] {
            static const uint8_t dev[18] = {
                0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
                0x41, 0x23, 0x02, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
            };
            static const uint8_t cfg[59] = {
                0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32,
                0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
                0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
                0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
                0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
                0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
                0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A,
            };
            desc::DescModel m;
            std::string err;
            std::vector<desc::UnknownBlock> unk;
            desc::parse_device_desc(dev, 18, m.device, err);
            m.configs.push_back({});
            desc::parse_config_blob(cfg, 59, m.configs[0], unk, err);
            m.strings.table[L"0x0409"] = {L"USB-Lab", L"KM", L"SN001"};
            return m;
        };
        std::vector<VdTemplate> v;
        {
            VdTemplate t1;
            t1.id = "hid-keyboard";
            t1.name = L"HID 键盘";
            t1.model = golden();
            t1.key_seq = {0x04, 0x05, 0x06, 0x00};   // a b c 空
            v.push_back(std::move(t1));
        }
        {
            VdTemplate t2;
            t2.id = "hid-composite";
            t2.name = L"HID 复合键鼠（Lab1）";
            t2.model = golden();
            t2.key_seq = {0x04, 0x2C, 0x00};          // a 空格 空
            v.push_back(std::move(t2));
        }
        return v;
    }();
    return t;
}

} // namespace usts::shell::vd
