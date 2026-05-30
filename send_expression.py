"""
Clawd Mochi 远程表情推送脚本

用法：
  python send_expression.py happy                  # 发送单次表情
  python send_expression.py happy --duration 3000  # 持续 3 秒后恢复
  python send_expression.py angry --loop           # 循环播放，直到收到下一条指令
  python send_expression.py --list                 # 列出所有支持的表情

依赖：pip install paho-mqtt
"""

import argparse
import json
import paho.mqtt.publish as publish

BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC = "clawd/clawd-001/expression"

EXPRESSIONS = {
    "idle":     "待机 - 橙色背景，普通眼睛",
    "happy":    "开心 - 眯眼动画",
    "sad":      "难过 - 蓝色背景",
    "sleep":    "睡眠 - 关闭屏幕背光",
    "wink":     "眨眼 - 单次眨眼",
    "blush":    "害羞 - 粉色背景",
    "awe":      "惊叹 - Logo 动画",
    "thinking": "思考 - Claude Code 视图",
    "angry":    "生气 - 红色背景",
    "proud":    "完成 - Logo 动画",
}


def send_expression(expr: str, duration: int = 0, loop: bool = False, device_id: str = "clawd-001"):
    topic = f"clawd/{device_id}/expression"
    payload = json.dumps({"expr": expr, "duration": duration, "loop": loop})
    publish.single(topic, payload, hostname=BROKER, port=PORT)
    print(f"已发送 → {topic}")
    print(f"内容: {payload}")


def main():
    parser = argparse.ArgumentParser(description="Clawd Mochi 远程表情推送")
    parser.add_argument("expr", nargs="?", help="表情名称")
    parser.add_argument("--duration", "-d", type=int, default=0, help="持续时间（毫秒），0=无限")
    parser.add_argument("--loop", "-l", action="store_true", help="循环播放")
    parser.add_argument("--device", default="clawd-001", help="设备 ID（默认 clawd-001）")
    parser.add_argument("--list", action="store_true", help="列出所有支持的表情")
    args = parser.parse_args()

    if args.list:
        print("支持的表情：\n")
        for name, desc in EXPRESSIONS.items():
            print(f"  {name:12s} {desc}")
        return

    if not args.expr:
        # 交互模式
        print("Clawd Mochi 表情推送工具\n")
        print("支持的表情：", ", ".join(EXPRESSIONS.keys()))
        expr = input("\n请输入表情名称: ").strip()
        if expr not in EXPRESSIONS:
            print(f"未知表情: {expr}")
            return
        dur = input("持续时间(ms, 回车=无限): ").strip()
        duration = int(dur) if dur else 0
        loop = input("循环? (y/n, 回车=n): ").strip().lower() == "y"
        send_expression(expr, duration, loop, args.device)
        return

    if args.expr not in EXPRESSIONS:
        print(f"未知表情: {args.expr}")
        print(f"支持的表情: {', '.join(EXPRESSIONS.keys())}")
        return

    send_expression(args.expr, args.duration, args.loop, args.device)


if __name__ == "__main__":
    main()
