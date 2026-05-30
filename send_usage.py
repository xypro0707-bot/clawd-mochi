"""
Clawd Mochi 用量数据推送脚本

用法：
  python send_usage.py --tokens 12345 --cost 0.50 [--model deepseek-v4-pro] [--device clawd-001]

依赖：pip install paho-mqtt
"""

import argparse
import json
import paho.mqtt.publish as publish

BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC_BASE = "clawd"


def send_usage(tokens: str, cost: str, model: str = "", device_id: str = "clawd-001"):
    topic = f"{TOPIC_BASE}/{device_id}/usage"
    payload = json.dumps({
        "tokens": tokens,
        "cost": cost,
        "model": model,
    })
    publish.single(topic, payload, hostname=BROKER, port=PORT)
    print(f"已发送用量 → {topic}")
    print(f"内容: {payload}")


def main():
    parser = argparse.ArgumentParser(description="Clawd Mochi 用量数据推送")
    parser.add_argument("--tokens", "-t", type=str, required=True, help="Token 用量")
    parser.add_argument("--cost", "-c", type=str, required=True, help="费用（USD）")
    parser.add_argument("--model", "-m", type=str, default="", help="模型名称")
    parser.add_argument("--device", "-d", default="clawd-001", help="设备 ID（默认 clawd-001）")
    args = parser.parse_args()

    send_usage(args.tokens, args.cost, args.model, args.device)


if __name__ == "__main__":
    main()
