"""capture —— 仿真抓包流导出: JSONL 与缩进文本（格式与 captures/ 教学标注一致）。"""
import json


def export_jsonl(capture: list[dict], path: str):
    with open(path, "w", encoding="utf-8") as f:
        for e in capture:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")


def render_text(capture: list[dict]) -> str:
    lines = []
    for i, e in enumerate(capture, 1):
        if e["kind"] in ("RESET", "ENUM", "SOF") and not e.get("hex"):
            lines.append(f"[{i:>3}] fr{e['frame']:<4} {e['kind']:<10} {e['desc']}")
        else:
            lines.append(f"[{i:>3}] fr{e['frame']:<4} {e['kind']:<10} {e['hex']:<48} {e['desc']}")
    return "\n".join(lines)
