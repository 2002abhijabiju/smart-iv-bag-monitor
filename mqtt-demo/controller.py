"""Send control commands (threshold / reset) to the IV bag device.

Usage:
  python controller.py patient001 --threshold 25
  python controller.py patient001 --reset
"""
import argparse
import json

import paho.mqtt.publish as publish

parser = argparse.ArgumentParser()
parser.add_argument("patient_id")
parser.add_argument("--threshold", type=float, default=20)
parser.add_argument("--reset", action="store_true")
parser.add_argument("--host", default="localhost")
parser.add_argument("--user", default="team40")
parser.add_argument("--password", default=None)
args = parser.parse_args()

payload = json.dumps({"threshold": args.threshold, "reset": args.reset})
auth = {"username": args.user, "password": args.password} if args.password else None
publish.single(f"private/ctl/ivbag/{args.patient_id}", payload,
               hostname=args.host, auth=auth)
print("Sent:", payload)
