"""usbsim.pd —— PD 消息层仿真：Source/Sink 策略引擎在虚拟 CC 上协商。
层边界: 模拟消息层（Header/数据对象/MessageID/GoodCRC 时序），不含 BMC 波形。
规范依据: PD 3.2 Table 6.4/6.5（消息类型）、Table 6.8/6.10（Fixed PDO）、Table 6.19（RDO）。
"""
from __future__ import annotations

TYPE_GOODCRC, TYPE_ACCEPT, TYPE_REJECT, TYPE_PS_RDY = 1, 3, 4, 6
TYPE_GET_SRC_CAP, TYPE_GET_SNK_CAP, TYPE_DR_SWAP, TYPE_PR_SWAP, TYPE_VCONN_SWAP = 7, 8, 9, 10, 11
TYPE_SOFT_RESET, TYPE_NOT_SUPPORTED = 13, 16
DATA_SOURCE_CAP, DATA_REQUEST, DATA_BIST, DATA_SINK_CAP, DATA_VDM = 1, 2, 3, 4, 15


def header(msg_type: int, n_objects: int, msg_id: int, power_role: str = "SRC",
           spec_rev: int = 3) -> int:
    """16 位消息头（PD 3.2 Table 6.2 口径，简化为常用字段）。"""
    h = (msg_type & 0x1F)
    h |= (1 << 5)              # 端口数据角色: DFP
    h |= (2 << 6)              # 规格版本（PD3 口径占位）
    h |= (0 if power_role == "SRC" else 1) << 8
    h |= (spec_rev & 0x3) << 6
    h |= ((n_objects & 0x7) << 12)
    h |= ((msg_id & 0x7) << 9)
    return h


def fixed_pdo(v: int, max_i: int, dual_role_power=False, usb_comm=True, unconstrained=False):
    pdo = (max_i // 10) & 0x3FF
    if v > 5:
        pdo |= (((v * 1000 // 50) & 0x3FF) << 10)
    if dual_role_power: pdo |= (1 << 29)
    if usb_comm: pdo |= (1 << 28)
    if unconstrained: pdo |= (1 << 27)
    return pdo & 0xFFFFFFFF


def rdo_fixed(obj_pos: int, op_ma: int, max_ma: int, no_usb_suspend=False, cap_mismatch=False):
    rdo = (obj_pos & 0x7) << 28
    rdo |= ((max_ma // 10) & 0x3FF)
    rdo |= ((op_ma // 10) & 0x3FF) << 10
    if no_usb_suspend: rdo |= (1 << 24)
    if cap_mismatch: rdo |= (1 << 26)
    return rdo & 0xFFFFFFFF


class Message:
    def __init__(self, msg_type: int, objects: list[int], msg_id: int, role: str = "SRC"):
        self.msg_type, self.objects, self.msg_id, self.role = msg_type, objects, msg_id, role
        self.header = header(msg_type, len(objects), msg_id, "SRC" if role == "SRC" else "SNK")

    def hex(self):
        out = self.header.to_bytes(2, "little")
        for o in self.objects:
            out += o.to_bytes(4, "little")
        return out

    def describe(self):
        names = {DATA_SOURCE_CAP: "Source_Capabilities", DATA_REQUEST: "Request",
                 TYPE_ACCEPT: "Accept", TYPE_PS_RDY: "PS_RDY", TYPE_GOODCRC: "GoodCRC",
                 DATA_SINK_CAP: "Sink_Capabilities"}
        return names.get(self.msg_type, f"类型 {self.msg_type}")


class Source:
    def __init__(self, pdos: list[int] | None = None):
        self.pdos = pdos or [fixed_pdo(5, 3000), fixed_pdo(9, 3000), fixed_pdo(20, 2250)]
        self.msg_id, self.contract = 0, None

    def send_caps(self) -> Message:
        m = Message(DATA_SOURCE_CAP, self.pdos, self.msg_id & 7, "SRC")
        self.msg_id += 1
        return m


class Sink:
    def __init__(self, want_v: int, want_i: int = 2000):
        self.want_v, self.want_i = want_v, want_i
        self.rdo = None

    def evaluate(self, caps: Message) -> Message | None:
        """选最优 Fixed PDO 并构造 RDO（策略: 电压≤want 中取最高；统一毫伏口径）。"""
        best, best_pos = None, 0
        for pos, pdo in enumerate(caps.objects, 1):
            if (pdo >> 30) != 0: continue            # 非 Fixed
            v_mv = 5000 if ((pdo >> 10) & 0x3FF) == 0 else ((pdo >> 10) & 0x3FF) * 50
            i_max = (pdo & 0x3FF) * 10
            if v_mv <= self.want_v * 1000 and (best is None or v_mv > best[0]):
                best, best_pos = (v_mv, i_max), pos
        if best is None:
            return None
        op = min(self.want_i, best[1])
        self.rdo = rdo_fixed(best_pos, op, best[1])
        return Message(DATA_REQUEST, [self.rdo], 0, "SNK")


def negotiate(source: Source, sink: Sink, log: list | None = None) -> bool:
    """完整协商: Source_Capabilities → Request → Accept → PS_RDY。返回是否达成合同。"""
    cap = source.send_caps()
    if log is not None:
        log.append({"dir": "SRC→SNK", "hex": cap.hex(), "desc": cap.describe() + f"（{len(cap.objects)} 个 PDO）"})
    req = sink.evaluate(cap)
    if log is not None:
        log.append({"dir": "SNK→SRC", "hex": req.hex(), "desc": f"Request（RDO=0x{sink.rdo:08X}）"} if req
                   else {"dir": "SNK→SRC", "hex": "", "desc": "无合适 PDO"})
    if req is None:
        return False
    acc = Message(TYPE_ACCEPT, [], 1, "SRC")
    if log is not None:
        log.append({"dir": "SRC→SNK", "hex": acc.hex(), "desc": "Accept"})
    rdy = Message(TYPE_PS_RDY, [], 2, "SRC")
    if log is not None:
        log.append({"dir": "SRC→SNK", "hex": rdy.hex(), "desc": "PS_RDY（电源就绪）"})
    source.contract = sink.rdo
    return True
