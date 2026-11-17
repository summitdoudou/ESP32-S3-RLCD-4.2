// 声明网络与音频电源管理锁的初始化、状态和嵌套所有权接口。
#pragma once

struct PowerLockDepthSnapshot {
    int network = 0;
    int audio = 0;
    int audio_wake = 0;
    int audio_cpu = 0;
};

// 网络唤醒锁身份（v1.6.28 锁分身份）：Keepalive=只要求"Wi-Fi 别睡"（后台流媒体/会话保活），
// 不独占信道；Exclusive=独占性网络操作（配网/OTA/短风暴同步），小智任务创建只等这一类。
enum class NetworkAwakeKind { Keepalive, Exclusive };

[[nodiscard]] bool acquire_network_awake_lock(
    NetworkAwakeKind kind = NetworkAwakeKind::Exclusive);
void release_network_awake_lock(
    NetworkAwakeKind kind = NetworkAwakeKind::Exclusive);
bool network_awake_lock_active();
// 仅 Exclusive 计数在位时为 true（互斥超时按 true 处理，fail-open 与 total 语义一致）
bool network_exclusive_lock_active();
bool get_power_lock_depth_snapshot(PowerLockDepthSnapshot *out);
[[nodiscard]] bool acquire_audio_awake_lock();
void release_audio_awake_lock();
