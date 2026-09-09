// session_selftest.cpp — EP-4 S3 会话台前半·离线自测（无真机即可跑）：
// session_codec（发送框智能识别 auto/hex/ascii 锁定、奇数位补前导 0、UTF-8
// 编解码双视图、时间戳与行格式）与 session_core（周期节拍钳制/到期、发送
// 历史游标语义、帧日志环形与清账、SessionCore 时间基准）的纯逻辑行为面。
// S3 后半 UI 接线（session_view 渲染游标在此覆盖；session_pane/console_window
// 为纯 Win32 接线层，随整片真机验收）。
#include "app/log.h"
#include "session/session_codec.h"
#include "session/session_core.h"
#include "session/session_view.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace session_codec;
using session_core::FrameJournal;
using session_core::PeriodicSender;
using session_core::SessionCore;
using session_core::TxHistory;

static int g_pass = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s (passed %d before failure)\n", what, g_pass);
        exit(1);
    }
    ++g_pass;
}
static bool bytes_eq(const std::vector<uint8_t>& v, std::initializer_list<uint8_t> want) {
    return std::vector<uint8_t>(v) == std::vector<uint8_t>(want);
}
static ChannelFrame mk_frame(bool out, std::initializer_list<uint8_t> b,
                             unsigned long long t) {
    ChannelFrame f;
    f.out = out;
    f.t_ms = t;
    f.bytes.assign(b);
    return f;
}

int main() {
    ustlog::init(/*also_stdout=*/true, L"usts-session");   // 日志规范见 app/log.h

    // —— 发送框 auto 判定（§4.3） ——
    {   // 纯十六进制 + 空白 → Hex
        auto r = parse_send_text(L"01 02", SendEncoding::auto_detect);
        check(r.hex_mode && r.error.empty() && bytes_eq(r.bytes, {0x01, 0x02}),
              "auto: 01 02 → Hex 01 02");
    }
    {
        auto r = parse_send_text(L"aF", SendEncoding::auto_detect);
        check(r.hex_mode && bytes_eq(r.bytes, {0xAF}), "auto: aF → AF（大小写混合）");
    }
    {   // 奇数个十六进制位 → 整体补前导 0（"1 23" → 0123 → 01 23）
        auto r = parse_send_text(L"1 23", SendEncoding::auto_detect);
        check(r.hex_mode && bytes_eq(r.bytes, {0x01, 0x23}), "auto: 1 23 → 01 23");
    }
    {
        auto r = parse_send_text(L"12345", SendEncoding::auto_detect);
        check(r.hex_mode && bytes_eq(r.bytes, {0x01, 0x23, 0x45}), "auto: 12345 → 01 23 45");
    }
    {   // 含非十六进制字符 → ASCII（UTF-8）
        auto r = parse_send_text(L"AT\r\n", SendEncoding::auto_detect);
        check(!r.hex_mode && bytes_eq(r.bytes, {0x41, 0x54, 0x0D, 0x0A}), "auto: AT\\r\\n → ASCII");
    }
    {
        auto r = parse_send_text(L"", SendEncoding::auto_detect);
        check(!r.hex_mode && r.error.empty() && r.bytes.empty(), "auto: 空串 → 空帧不报错");
    }
    {
        auto r = parse_send_text(L" \t ", SendEncoding::auto_detect);
        check(!r.hex_mode && r.error.empty() && bytes_eq(r.bytes, {0x20, 0x09, 0x20}),
              "auto: 纯空白按 ASCII 原文三字节（空帧不可发仅对空串）");
    }
    {
        auto r = parse_send_text(L"你好", SendEncoding::auto_detect);
        check(!r.hex_mode && bytes_eq(r.bytes, {0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD}),
              "auto: 中文 → UTF-8 六字节");
    }
    {
        auto r = parse_send_text(L"01 G2", SendEncoding::auto_detect);
        check(!r.hex_mode && bytes_eq(r.bytes, {0x30, 0x31, 0x20, 0x47, 0x32}),
              "auto: 01 G2 → ASCII 原文五字节");
    }
    {   // 制表/换行视为 Hex 分隔空白
        auto r = parse_send_text(L"\t01 02\n", SendEncoding::auto_detect);
        check(r.hex_mode && bytes_eq(r.bytes, {0x01, 0x02}), "auto: 前后空白不干扰 Hex");
    }

    // —— 锁定模式 ——
    {
        auto r = parse_send_text(L"0G", SendEncoding::hex_lock);
        check(r.hex_mode && !r.error.empty() && r.bytes.empty(), "hex 锁定: 非法字符报错");
    }
    {
        auto r = parse_send_text(L"1 2", SendEncoding::hex_lock);
        check(r.hex_mode && r.error.empty() && bytes_eq(r.bytes, {0x12}),
              "hex 锁定: 空白仅分隔，半字节配对 1 2 → 12");
    }
    {
        auto r = parse_send_text(L"abc", SendEncoding::hex_lock);
        check(r.hex_mode && bytes_eq(r.bytes, {0x0A, 0xBC}), "hex 锁定: abc → 0A BC");
    }
    {
        auto r = parse_send_text(L"", SendEncoding::hex_lock);
        check(r.hex_mode && r.error.empty() && r.bytes.empty(), "hex 锁定: 空串不报错");
    }
    {   // ascii 锁定：十六进制样貌也按原文发
        auto r = parse_send_text(L"01 02", SendEncoding::ascii_lock);
        check(!r.hex_mode && bytes_eq(r.bytes, {0x30, 0x31, 0x20, 0x30, 0x32}),
              "ascii 锁定: 01 02 按原文发");
    }
    {
        auto r = parse_send_text(L"\r\n", SendEncoding::ascii_lock);
        check(bytes_eq(r.bytes, {0x0D, 0x0A}), "ascii 锁定: CR LF");
    }
    {   // 代理对 → 4 字节 UTF-8（U+1F600 → F0 9F 98 80）
        const wchar_t pair[] = {0xD83D, 0xDE00, 0};
        auto r = parse_send_text(pair, SendEncoding::ascii_lock);
        check(bytes_eq(r.bytes, {0xF0, 0x9F, 0x98, 0x80}), "ascii: 代理对 → 4 字节序列");
    }
    {   // 孤立代理 → U+FFFD（EF BF BD），保证可逆
        const wchar_t lone[] = {0xD800, 0};
        auto r = parse_send_text(lone, SendEncoding::ascii_lock);
        check(bytes_eq(r.bytes, {0xEF, 0xBF, 0xBD}), "ascii: 孤立代理 → 替换符");
    }

    // —— 接收区双视图（§4.6） ——
    check(to_hex_view({0x00, 0x01, 0xAB, 0xFF}) == L"00 01 AB FF", "Hex 视图: 大写空格分组");
    check(to_hex_view(std::vector<uint8_t>{}) == L"", "Hex 视图: 空帧为空串");
    check(to_ascii_view({0x48, 0x65, 0x6C, 0x6C, 0x6F}) == L"Hello", "ASCII 视图: 可打印直出");
    check(to_ascii_view({0x00, 0x48, 0x01, 0x7F}) == L".H..", "ASCII 视图: 控制/DEL → 点");
    check(to_ascii_view({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD}) == L"你好", "ASCII 视图: UTF-8 保真");
    check(to_ascii_view({0xFF, 0x41}) == L".A", "ASCII 视图: 非法前导 → 点后重同步");
    check(to_ascii_view({0xE4, 0xBD}) == L"..", "ASCII 视图: 截断序列 → 两点");
    {   // 4 字节序列还原为代理对（与发送编码往返一致）
        const wchar_t emoji[] = {0xD83D, 0xDE00, 0};
        check(to_ascii_view({0xF0, 0x9F, 0x98, 0x80}) == std::wstring(emoji),
              "ASCII 视图: 4 字节 → 代理对往返");
    }
    check(to_ascii_view({0x20, 0x7E}) == L" ~", "ASCII 视图: 可打印边界 0x20/0x7E");

    // —— 时间戳（§4.6 相对/绝对） ——
    check(format_time_of_day(0) == L"00:00:00.000", "绝对: 零点");
    check(format_time_of_day(3661234) == L"01:01:01.234", "绝对: 01:01:01.234");
    check(format_time_of_day(86399999) == L"23:59:59.999", "绝对: 日内最后一毫秒");
    check(format_time_of_day(86400000ULL + 500) == L"00:00:00.500", "绝对: 一天回绕取模");
    check(format_relative(0) == L"0ms", "相对: 0ms");
    check(format_relative(999) == L"999ms", "相对: 999ms");
    check(format_relative(1234) == L"1.234s", "相对: 1.234s");
    check(format_relative(60000) == L"60.000s", "相对: 60.000s");

    // —— 接收区一行（设计 §2 样例同构） ——
    check(format_frame_line(mk_frame(true, {0x01, 0xAB}, 0), true, L"12:00:01.234") ==
          L"12:00:01.234 OUT 01 AB", "行格式: OUT + Hex 视图");
    check(format_frame_line(mk_frame(false, {0x4F, 0x4B}, 0), false, L"500ms") ==
          L"500ms IN  OK", "行格式: IN + ASCII 视图（四字符对齐）");

    // —— 周期发送节拍（§4.4: 100ms~60s 钳制 + 到期判定） ——
    {
        PeriodicSender p;
        p.set_interval(50);
        check(p.interval() == PeriodicSender::kMinMs, "周期: 下限钳到 100ms");
        p.set_interval(70000);
        check(p.interval() == PeriodicSender::kMaxMs, "周期: 上限钳到 60s");
        p.set_interval(500);
        check(p.interval() == 500, "周期: 区间内原样保留");
        check(!p.due(1000000), "周期: 未武装不到期");
        p.set_interval(100);
        p.arm(0);
        check(!p.due(99) && p.due(100), "周期: 满 100ms 到期");
        check(!p.due(199) && p.due(200), "周期: 到期后相位重排");
        p.arm(1000);
        check(!p.due(1099) && p.due(1100), "周期: 重新武装重置相位");
        p.disarm();
        check(!p.due(999999), "周期: 解除后不再到期");
    }

    // —— 发送历史（§4.5: 最近 50 条 + 上下键游标） ——
    {
        TxHistory h;
        check(h.up().empty(), "历史: 空历史上键返回空");
        h.push(L"A");
        h.push(L"B");
        h.push(L"C");                       // front=C
        check(h.size() == 3, "历史: 三条入册");
        check(h.up() == L"C", "历史: 上键从最新开始");
        check(h.up() == L"B", "历史: 再上键更旧一档");
        check(h.up() == L"A", "历史: 到最旧");
        check(h.up() == L"A", "历史: 到顶粘住");
        check(h.down() == L"B", "历史: 下键回新一档");
        check(h.down() == L"C", "历史: 下键回最新");
        check(h.down().empty(), "历史: 最新处再下键交还草稿");
        check(h.down().empty(), "历史: 草稿态下键仍空");
    }
    {
        TxHistory h;
        h.push(L"A");
        h.push(L"A");
        check(h.size() == 1, "历史: 连续重复折叠");
        h.push(L"B");
        h.push(L"A");
        check(h.size() == 3, "历史: 非连续重复保留");
    }
    {   // 容量 50：最旧出册、最新在首
        TxHistory h;
        for (int i = 0; i < 55; ++i) h.push(L"cmd" + std::to_wstring(i));
        check(h.size() == TxHistory::kCap, "历史: 容量封顶 50");
        check(h.up() == L"cmd54", "历史: 首位为最新");
        std::wstring w;
        for (int k = 0; k < 49; ++k) w = h.up();
        check(w == L"cmd5", "历史: cmd0~cmd4 已出册");
        check(h.up() == L"cmd5", "历史: 容量边界同样粘顶");
        h.up();                              // 游历中
        h.push(L"X");
        check(h.up() == L"X", "历史: 游历中新发送重置到最新");
    }

    // —— 帧日志（§4.6: 环形保留 + 计数不封顶 + 清屏不清账） ——
    {
        FrameJournal j(8);
        const uint8_t a[] = {0x01};
        const uint8_t b[] = {0x02, 0x03};
        j.record(true, a, sizeof a, 100);
        j.record(false, b, sizeof b, 200);
        j.record(true, a, sizeof a, 300);
        check(j.frames().size() == 3, "日志: 三帧入账");
        check(j.frames()[0].out && !j.frames()[1].out && j.frames()[2].out, "日志: 方向按序");
        check(j.frames()[1].t_ms == 200 && j.frames()[1].bytes.size() == 2, "日志: 时间与字节入账");
        check(j.tx_frames() == 2 && j.rx_frames() == 1, "日志: 计数分向");
        j.clear();
        check(j.frames().empty(), "日志: 清屏清显示");
        check(j.tx_frames() == 2 && j.rx_frames() == 1, "日志: 清屏不清账");
    }
    {
        FrameJournal j(4);
        const uint8_t x[] = {0x00};
        for (int i = 0; i < 6; ++i) j.record(true, x, 1, unsigned(i));
        check(j.frames().size() == 4, "日志: 环形只留最近 4 帧");
        check(j.frames().front().t_ms == 2 && j.frames().back().t_ms == 5, "日志: 淘汰最旧");
        check(j.tx_frames() == 6, "日志: 计数不随环形丢");
    }

    // —— SessionCore 聚合（时间基准 + 收口行格式） ——
    {
        SessionCore c;
        c.set_time_base(1000, 43200000ULL);          // t0=1000；锚定"纪元=12:00:00"
        check(c.ts_absolute(2500) == L"12:00:02.500", "聚合: 绝对时刻 (t+锚)%一天");
        check(c.ts_relative(2500) == L"1.500s", "聚合: 相对时刻 t-t0");
        check(c.ts_relative(500) == L"0ms", "聚合: t<t0 钳零不回绕");
        const uint8_t d[] = {0x01, 0xAB};
        c.record_tx(d, 2, 2500);
        c.record_rx(d, 2, 2600);
        check(c.journal.tx_frames() == 1 && c.journal.rx_frames() == 1, "聚合: 收发入账");
        check(c.frame_line(c.journal.frames()[0], true, true) == L"12:00:02.500 OUT 01 AB",
              "聚合: 行格式·绝对+Hex");
        check(c.frame_line(c.journal.frames()[0], true, false) == L"1.500s OUT 01 AB",
              "聚合: 行格式·相对+Hex");
        check(c.frame_line(c.journal.frames()[1], true, false) == L"1.600s IN  01 AB",
              "聚合: 行格式·相对+IN 对齐");
    }

    // —— RenderCursor 渲染游标（S3 后半：账面-显示解耦） ——
    {   // 空账 poll → 空且游标不动
        session_core::SessionCore c;
        session_view::RenderCursor v;
        check(v.poll(c).empty() && v.rendered() == 0, "游标: 空账 poll 空");
    }
    {   // 增量渲染：三帧 → 三行按序，重复 poll 无重复
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        session_view::RenderCursor v;
        const uint8_t a[] = {0x01}, b[] = {0x42};
        c.record_tx(a, 1, 10);
        c.record_rx(b, 1, 20);
        c.record_tx(a, 1, 30);
        auto lines = v.poll(c);
        check(lines.size() == 3 && v.rendered() == 3, "游标: 三帧一次出三行");
        check(lines[0].rfind(L"10ms OUT ", 0) == 0 && lines[0].find(L"01") != std::wstring::npos,
              "游标: 行=相对时间戳+OUT+Hex");
        check(lines[1].find(L"IN  42") != std::wstring::npos, "游标: 顺序与方向保真");
        check(v.poll(c).empty(), "游标: 已渲染不重发");
    }
    {   // 暂停游标停走，恢复一次补齐
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        session_view::RenderCursor v;
        const uint8_t a[] = {0x00};
        c.record_rx(a, 1, 5);
        check(v.poll(c).size() == 1, "游标: 先渲染一帧");
        v.set_paused(true);
        c.record_rx(a, 1, 50);
        c.record_rx(a, 1, 60);
        check(v.poll(c).empty() && v.rendered() == 1, "游标: 暂停不渲染不推进");
        v.set_paused(false);
        auto lines = v.poll(c);
        check(lines.size() == 2 && v.rendered() == 3, "游标: 恢复一次补齐两帧");
    }
    {   // 环形淘汰：未渲染帧在暂停期间滑出账面，恢复只补存活帧，计数仍在
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        session_view::RenderCursor v;
        const uint8_t x[] = {0xEE};
        c.journal = session_core::FrameJournal(4);   // 小账面逼出淘汰
        for (int i = 0; i < 3; ++i) c.record_tx(x, 1, unsigned(i));
        v.set_paused(true);                          // 暂停在渲染之前，游标=0
        for (int i = 3; i < 6; ++i) c.record_tx(x, 1, unsigned(i));   // 总 6 存活 4
        v.set_paused(false);
        auto lines = v.poll(c);
        check(lines.size() == 4 && v.rendered() == 6,
              "游标: 最老 2 帧被淘汰不可见，恢复只补存活 4 帧而游标=总账");
        check(c.journal.tx_frames() == 6, "游标: 淘汰帧计数不丢");
    }
    {   // journal 清屏（清显示不清账）后 poll 无可渲染，新帧照常出
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        session_view::RenderCursor v;
        const uint8_t a[] = {0x01};
        c.record_rx(a, 1, 1);
        c.record_rx(a, 1, 2);
        v.poll(c);
        c.journal.clear();
        check(v.poll(c).empty(), "游标: 账面显示存储清空后无增量");
        c.record_rx(a, 1, 3);
        auto lines = v.poll(c);
        check(lines.size() == 1, "游标: 清屏后新帧照常渲染");
    }
    {   // rebuild：口径切换全量重渲染，游标推到最新，此后 poll 无新增
        session_core::SessionCore c;
        c.set_time_base(0, 43200000ULL);              // 锚定 12:00:00
        session_view::RenderCursor v;              // 默认 Hex+相对
        const uint8_t d[] = {'A', 0x01};
        c.record_rx(d, 2, 1500);
        v.poll(c);
        v.hex_view = false;                           // 切 ASCII 视图
        auto lines = v.rebuild(c);
        check(lines.size() == 1 && lines[0].find(L"A.") != std::wstring::npos,
              "游标: rebuild 按 ASCII 口径（0x01 不可打印→'.'）");
        check(v.rendered() == 1 && v.poll(c).empty(), "游标: rebuild 后无重复增量");
        v.absolute_ts = true;                         // 再切绝对时间戳
        lines = v.rebuild(c);
        check(lines.size() == 1 && lines[0].rfind(L"12:00:01.500", 0) == 0,
              "游标: rebuild 按绝对时刻口径");
    }
    {   // 暂停中 rebuild：全量返回且游标推进，恢复后 poll 空
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        session_view::RenderCursor v;
        const uint8_t a[] = {0x7F};
        c.record_rx(a, 1, 1);
        v.set_paused(true);
        check(v.rebuild(c).size() == 1 && v.rendered() == 1, "游标: 暂停中 rebuild 照常全量");
        v.set_paused(false);
        check(v.poll(c).empty(), "游标: rebuild 已推进，恢复无补齐");
    }

    printf("session_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
