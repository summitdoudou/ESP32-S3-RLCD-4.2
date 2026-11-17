// 网络广播（互联网电台）后台播放服务接口声明。
// 服务由小智 MCP 工具 self.radio.control 触发，具备两种出声模式：
// 1) 小智页在场：借用小智音频会话（对话回合自动暂停，说完续播）；
// 2) 离开小智页：持有独立音频会话 + 网络唤醒锁，后台持续播放。
// 仅语音"关闭广播"或低电量会终止服务；进小智页瞬间需调用
// radio_player_pause_for_xiaozhi() 让其交还独立会话。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 注册广播 MCP 工具 handler，在功能初始化表中调用。
bool radio_player_init();

// 启动后台播放服务（意图式：小智页在场时借道播放，离页后独立播放）。
bool radio_player_start();

// 停止播放服务并等待任务退出（有界等待）。可安全重复调用。
void radio_player_stop();

// 小智页激活瞬间调用：让广播交还独立音频会话（随后自动借道小智会话）。
void radio_player_pause_for_xiaozhi();
// 小智会话释放后调用：允许广播恢复独立会话播放。
void radio_player_resume_after_xiaozhi();

// 当前是否正在出声。
bool radio_player_is_playing();
// 播放服务是否存续（后台意图在位，无论当前是否在出声）。v1.6.27 按键强停用。
bool radio_player_service_active();
// 当前是否有广播 PCM 正在推给功放（借道或独立模式均覆盖）。v1.6.28 语音输入隔离用。
bool radio_player_output_active();
// 有界等待广播交还独立音频会话；小智启动前调用，返回 true 表示已安静。
bool radio_player_wait_quiet(uint32_t timeout_ms);
