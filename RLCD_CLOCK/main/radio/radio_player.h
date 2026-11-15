// 网络广播（互联网电台）播放模块接口声明。
// 广播通过小智 MCP 工具 self.radio.control 触发，复用 Xiaozhi AI 页面的
// Codec 会话与扬声器通道；离开页面或会话结束时必须调用 radio_player_stop()。
#pragma once

#include <stddef.h>

// 注册广播 MCP 工具 handler，在功能初始化表中调用。
bool radio_player_init();

// 开始播放北京新闻广播直播流。仅在 Codec 会话活跃时可用；重复调用视为成功。
bool radio_player_start();

// 请求停止播放并等待播放任务退出（有界等待）。可安全重复调用。
void radio_player_stop();

// 当前是否正在播放。
bool radio_player_is_playing();
