// 声明音频播放、提示音选择和音频电源管理接口。
#pragma once

#include <cstddef>
#include <cstdint>

using AudioStopRequestedCallback = bool (*)();

void hourly_chime_task(void *arg);
void setup_prompt_task(void *);
bool start_chime_playback(int source_slot);
bool play_chime_sound_blocking(int source_slot,
                               AudioStopRequestedCallback stop_requested);
bool play_chime_sound_repeated_blocking(int source_slot,
                                        int repeat_count,
                                        AudioStopRequestedCallback stop_requested);
bool start_setup_prompt_playback();
bool setup_prompt_playback_pending();
bool is_audio_playing();
bool wait_for_audio_playback_idle(uint32_t timeout_ms,
                                  uint32_t poll_interval_ms,
                                  AudioStopRequestedCallback stop_requested);
bool audio_codec_active();
void request_setup_prompt_once();
void request_settings_confirmation_chime();
void play_hourly_chime(int hour, bool enforce_quiet_hours = true);

// 小智页独占使用现有 CodecPort；调用者只能在成功开始会话后读写 PCM，
// 结束时必须调用 stop_xiaozhi_audio_session() 归还音频与轻睡眠锁。
bool start_xiaozhi_audio_session();
void stop_xiaozhi_audio_session();
// 后台广播独立音频会话：只开扬声器、不开麦克风，与小智会话共用播放互斥门。
// 小智页需要接管音频时，广播必须先 stop_radio_audio_session() 让位。
bool start_radio_audio_session();
void stop_radio_audio_session();
bool radio_audio_session_active();
// 小智页音频会话是否真正建立（含麦克风就绪）。广播借道播放只允许在此为真时进行，
// 不能用 audio_codec_active()——广播自己的独立会话也会让它为真（11/15 卡启动事故）。
bool xiaozhi_audio_session_active();
bool set_xiaozhi_audio_high_performance(bool enabled);
int read_xiaozhi_microphone(void *buffer, size_t bytes);
int write_xiaozhi_speaker(const int16_t *mono_samples, size_t sample_count, int sample_rate);
void apply_xiaozhi_speaker_volume(int volume_percent);
bool resume_xiaozhi_microphone_after_playback();
bool play_xiaozhi_wake_feedback();
void smooth_xiaozhi_speaker_segment_transition();
void abort_xiaozhi_speaker_playback();
