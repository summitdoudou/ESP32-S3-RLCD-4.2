#!/usr/bin/env python3
# Host 逻辑测试统一入口（v1.6.31 P1-6 引入）。
# 164 个 *_host_test.cpp 中当前 46 个带 host_stubs 可直接编译+运行；
# 门限读 host_tests_gate 文件（防绿色测试悄悄消失）。其余 118 个待补
# 伴生源/stub 依赖后逐步纳入（compile_pending 计数不阻断）。
# 任何 RUNFAIL 直接失败。
# 写法注意：赋值一律用 `x = 0` 空格形态——无空格 KEY=数字 形态会被脱敏通道改写。
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GATE = int(open(os.path.join(HERE, 'host_tests_gate')).read().split()[0])
os.chdir(ROOT)
OUT = 'build/hosttest'
os.makedirs(OUT, exist_ok=True)

incs = ['-Itests/host_stubs', '-I.', '-Itests']
for base, dirs, files in os.walk('main'):
    if base.count(os.sep) - 1 <= 1:
        incs.append('-I' + base)

green = 0
compile_pending = 0
run_failures = []
for name in sorted(os.listdir('tests')):
    if not name.endswith('_host_test.cpp'):
        continue
    exe = os.path.join(OUT, name[:-4])
    compile_cmd = (['g++', '-std=c++17'] + incs
                   + [os.path.join('tests', name), '-o', exe])
    with open(exe + '.err', 'w') as errf:
        if subprocess.run(compile_cmd, stdout=subprocess.DEVNULL,
                          stderr=errf).returncode != 0:
            compile_pending += 1
            continue
    with open(exe + '.out', 'w') as outf:
        result = subprocess.run([exe], stdout=outf, stderr=subprocess.STDOUT)
    if result.returncode == 0:
        green += 1
    else:
        run_failures.append(name[:-10])

print('host tests: total=' + str(green + compile_pending + len(run_failures))
      + ' green=' + str(green)
      + ' compile_pending=' + str(compile_pending)
      + ' run_fail=' + str(len(run_failures)))
rc = 0
if run_failures:
    print('RUN FAILURES: ' + ' '.join(run_failures))
    rc = 1
if green < GATE:
    print('REGRESSION: green ' + str(green) + ' < gate ' + str(GATE))
    rc = 1
sys.exit(rc)
