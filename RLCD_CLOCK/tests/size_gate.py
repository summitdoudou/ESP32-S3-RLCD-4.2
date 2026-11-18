#!/usr/bin/env python3
# 体积门禁（v1.6.31 P1-6）：App bin 超过 OTA 槽百分比阈值即失败并打印占用。
# 用法: size_gate.py <app.bin> <ota槽字节,支持0x前缀> <告警阈值百分比>
import os
import sys

app = os.path.getsize(sys.argv[1])
slot = int(sys.argv[2], 0)
limit = float(sys.argv[3])
pct = app * 100.0 / slot
print(f"size-gate: app={app}B slot={slot}B usage={pct:.2f}% limit={limit:.2f}%")
if pct > limit:
    print("FAIL: app 超出 OTA 槽预算，裁剪或扩容后重试", file=sys.stderr)
    sys.exit(1)
