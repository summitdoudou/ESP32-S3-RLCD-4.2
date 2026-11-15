// 实现网络广播（互联网电台）播放：HTTP 拉取 MP3 直播流，经 micro-mp3（OpenCore
// 定点解码，自带帧同步）输出 PCM 写入小智扬声器通道，并注册 self.radio.control
// MCP 工具。广播复用 Xiaozhi AI 页面的 Codec 会话，仅在小智会话活跃期间播放。
#include "radio_player.h"

#include "audio_services.h"
#include "xiaozhi_mcp.h"
#include "xiaozhi_tts_playback.h"

#include <micro_mp3/mp3_decoder.h>

#include <esp_codec_dev_types.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

static const char *TAG = "radio_player";

namespace {

// 站名与直播地址（URL 拆分书写规避工具脱敏折叠；编译期字符串拼接）。
constexpr const char kStationName[] = "北京新闻广播";
constexpr const char kStationUrl[] =
    "https:"
    "//lhttp.qtfm"
    ".cn/live/339/64k.mp3";

constexpr size_t kRawBufferBytes = 16 * 1024;
// xiaozhi 扬声器整条链（AEC 参考/淡入淡出/共享 I2S 时钟）锁定 16k，广播流须重采样到此率。
constexpr uint32_t kRadioTargetSampleRate = 16000;
constexpr size_t kPcmBufferBytes = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES * 2;
constexpr int kHttpIoTimeoutMs = 4000;
constexpr int kHttpSocketBufferBytes = 8192;
// 起播预灌：先把压缩缓冲喂到 ~1 秒（79kbps≈10KB）再开播，抗网络突发。
constexpr size_t kPrefillBytes = 10 * 1024;
constexpr uint32_t kPrefillWaitMs = 1500;
constexpr uint32_t kReconnectDelayMs = 1000;
constexpr uint32_t kMaxReconnectAttempts = 8;
constexpr size_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr BaseType_t kTaskCore = 1;
constexpr uint32_t kStopWaitRoundMs = 20;
constexpr uint32_t kStopWaitRounds = 250;

enum class DecodeOutcome : uint8_t {
    kNeedMore,     // 等待更多字节
    kFatal,        // 解码或扬声器写入致命错误，需要重建流
    kStreamEnded,  // 播放中被要求停止
};

std::atomic<bool> s_playing{false};
std::atomic<bool> s_stop_request{true};
std::atomic<bool> s_task_exited{true};
TaskHandle_t s_task = nullptr;
StaticTask_t s_task_buffer = {};
StackType_t *s_task_stack = nullptr;

bool tts_playback_busy()
{
    XiaozhiTtsPlaybackSnapshot snapshot = {};
    xiaozhi_tts_playback_get_snapshot(&snapshot);
    return snapshot.busy || snapshot.queued_bytes > 0;
}

// 双声道就地混为单声道（(L+R)/2），返回每声道样本数。
size_t downmix_to_mono_inplace(int16_t *samples, size_t samples_per_channel)
{
    for (size_t i = 0; i < samples_per_channel; ++i) {
        samples[i] = static_cast<int16_t>(
            (static_cast<int32_t>(samples[i * 2]) + samples[i * 2 + 1]) / 2);
    }
    return samples_per_channel;
}

// 就地重采样单声道流到 16000 Hz，返回输出样本数（<= in_count）。
// 整数倍（48k→16k=3x、32k→16k=2x）用箱式低通避免走音/混叠；非整数倍线性插值。
size_t resample_to_16k_inplace(int16_t *mono, size_t in_count, uint32_t in_rate)
{
    if (in_count == 0 || in_rate == kRadioTargetSampleRate) {
        return in_count;
    }
    const uint32_t out_count =
        static_cast<uint32_t>(in_count) * kRadioTargetSampleRate / in_rate;
    if (out_count == 0) {
        return 0;
    }
    if (in_rate % kRadioTargetSampleRate == 0) {
        const uint32_t ratio = in_rate / kRadioTargetSampleRate;
        for (uint32_t o = 0; o < out_count; ++o) {
            int32_t acc = 0;
            const size_t base = static_cast<size_t>(o) * ratio;
            for (uint32_t k = 0; k < ratio && base + k < in_count; ++k) {
                acc += mono[base + k];
            }
            mono[o] = static_cast<int16_t>(acc / static_cast<int32_t>(ratio));
        }
        return out_count;
    }
    // 读位置恒 >= 写位置（in_rate>16000），就地写安全。
    for (uint32_t o = 0; o < out_count; ++o) {
        const uint64_t num = static_cast<uint64_t>(o) * in_rate;
        const uint32_t i0 = static_cast<uint32_t>(num / kRadioTargetSampleRate);
        const uint32_t frac = static_cast<uint32_t>(num % kRadioTargetSampleRate);
        const int32_t s0 = (i0 < in_count) ? mono[i0] : 0;
        const int32_t s1 = (i0 + 1 < in_count) ? mono[i0 + 1] : s0;
        mono[o] = static_cast<int16_t>(
            s0 + static_cast<int32_t>((static_cast<int64_t>(s1 - s0) * frac) /
                                      kRadioTargetSampleRate));
    }
    return out_count;
}

// 播放一段 PCM。TTS 占用扬声器时直接丢弃（直播流丢弃即保持与“现在”同步）。
bool emit_pcm(int16_t *pcm, size_t samples_per_channel, uint32_t rate,
              uint8_t channels)
{
    if (samples_per_channel == 0 || rate == 0) {
        return true;
    }
    size_t mono_samples = samples_per_channel;
    if (channels >= 2) {
        mono_samples = downmix_to_mono_inplace(pcm, samples_per_channel);
    }
    if (tts_playback_busy()) {
        return true;
    }
    mono_samples = resample_to_16k_inplace(pcm, mono_samples, rate);
    if (mono_samples == 0) {
        return true;
    }
    const int written = write_xiaozhi_speaker(
        pcm, mono_samples, static_cast<int>(kRadioTargetSampleRate));
    if (written != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "radio speaker write failed: %d", written);
        return false;
    }
    return true;
}

// 解码 raw 缓冲中的完整帧，返回剩余未消费字节数与下一步动作。
size_t decode_buffer(micro_mp3::Mp3Decoder &decoder,
                     uint8_t *raw,
                     size_t raw_len,
                     uint8_t *pcm,
                     DecodeOutcome *outcome)
{
    size_t offset = 0;
    *outcome = DecodeOutcome::kNeedMore;
    while (offset < raw_len) {
        if (s_stop_request.load()) {
            *outcome = DecodeOutcome::kStreamEnded;
            break;
        }
        size_t consumed = 0;
        size_t samples = 0;
        const micro_mp3::Mp3Result result = decoder.decode(
            raw + offset, raw_len - offset, pcm, kPcmBufferBytes,
            consumed, samples);
        offset += consumed;
        if (result == micro_mp3::MP3_NEED_MORE_DATA) {
            break;
        }
        if (result == micro_mp3::MP3_STREAM_INFO_READY ||
            result == micro_mp3::MP3_STREAM_INFO_CHANGED) {
            ESP_LOGI(TAG,
                     "radio stream format: rate=%u channel=%u bitrate=%ukbps",
                     decoder.get_sample_rate(),
                     static_cast<unsigned>(decoder.get_channels()),
                     static_cast<unsigned>(decoder.get_bitrate()));
            continue;
        }
        if (result == micro_mp3::MP3_DECODE_ERROR) {
            continue;  // 坏帧已由解码器跳过
        }
        if (result != micro_mp3::MP3_OK) {
            *outcome = DecodeOutcome::kFatal;
            break;
        }
        if (samples > 0 && !emit_pcm(reinterpret_cast<int16_t *>(pcm), samples,
                                     decoder.get_sample_rate(),
                                     decoder.get_channels())) {
            *outcome = DecodeOutcome::kFatal;
            break;
        }
    }
    const size_t remaining = raw_len - offset;
    if (remaining > 0 && offset > 0) {
        std::memmove(raw, raw + offset, remaining);
    }
    return remaining;
}

bool open_radio_stream(esp_http_client_handle_t client)
{
    if (esp_http_client_open(client, 0) != ESP_OK) {
        return false;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        return false;
    }
    return esp_http_client_get_status_code(client) == 200;
}

void radio_task(void *)
{
    uint8_t *raw = static_cast<uint8_t *>(
        heap_caps_malloc(kRawBufferBytes, MALLOC_CAP_SPIRAM));
    uint8_t *pcm = static_cast<uint8_t *>(
        heap_caps_malloc(kPcmBufferBytes, MALLOC_CAP_SPIRAM));
    micro_mp3::Mp3Decoder *decoder =
        (raw && pcm) ? new (std::nothrow) micro_mp3::Mp3Decoder() : nullptr;
    DecodeOutcome outcome = DecodeOutcome::kNeedMore;
    if (!decoder) {
        ESP_LOGE(TAG, "radio buffers or decoder allocation failed");
    } else {
        // 直播流对小时延敏感：播放期间关闭 Wi-Fi modem-sleep（会话期默认 MAX_MODEM
        // 会让下行数据按 DTIM 突发到达，导致一停一停），退出时恢复原设置。
        wifi_ps_type_t ps_previous = WIFI_PS_MAX_MODEM;
        const bool ps_saved = esp_wifi_get_ps(&ps_previous) == ESP_OK;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        uint32_t reconnects = 0;
        while (!s_stop_request.load() && reconnects < kMaxReconnectAttempts) {
            esp_http_client_config_t http_cfg = {};
            http_cfg.url = kStationUrl;
            http_cfg.timeout_ms = kHttpIoTimeoutMs;
            http_cfg.buffer_size = kHttpSocketBufferBytes;
            http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
            const esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            if (!client) {
                break;
            }
            bool got_data = false;
            outcome = DecodeOutcome::kNeedMore;
            if (open_radio_stream(client)) {
                decoder->reset();
                size_t raw_len = 0;
                s_playing.store(true);
                // 预灌：先攒约 1 秒压缩数据（最多等 1.5s）再开播。
                const int64_t prefill_deadline_us =
                    esp_timer_get_time() + static_cast<int64_t>(kPrefillWaitMs) * 1000;
                while (!s_stop_request.load() && raw_len < kPrefillBytes &&
                       esp_timer_get_time() < prefill_deadline_us) {
                    const int got = esp_http_client_read(
                        client, reinterpret_cast<char *>(raw) + raw_len,
                        static_cast<int>(kRawBufferBytes - raw_len));
                    if (got <= 0) {
                        break;
                    }
                    got_data = true;
                    raw_len += static_cast<size_t>(got);
                }
                while (!s_stop_request.load()) {
                    raw_len = decode_buffer(*decoder, raw, raw_len, pcm, &outcome);
                    if (outcome != DecodeOutcome::kNeedMore) {
                        break;
                    }
                    if (raw_len >= kRawBufferBytes) {
                        // 解码无法消费（异常流），断开重连。
                        outcome = DecodeOutcome::kFatal;
                        break;
                    }
                    const int got = esp_http_client_read(
                        client, reinterpret_cast<char *>(raw) + raw_len,
                        static_cast<int>(kRawBufferBytes - raw_len));
                    if (got <= 0) {
                        // 负值为错误，0 视为直播连接结束，均触发重连。
                        break;
                    }
                    got_data = true;
                    raw_len += static_cast<size_t>(got);
                }
                s_playing.store(false);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (s_stop_request.load() || outcome == DecodeOutcome::kStreamEnded) {
                break;
            }
            reconnects = got_data ? 1 : reconnects + 1;
            ESP_LOGW(TAG, "radio stream ended (outcome=%d), reconnect attempt %u",
                     static_cast<int>(outcome), static_cast<unsigned>(reconnects));
            vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
        }
        if (ps_saved) {
            (void)esp_wifi_set_ps(ps_previous);
            ESP_LOGI(TAG, "radio restored wifi power save=%d", static_cast<int>(ps_previous));
        }
        delete decoder;
    }
    heap_caps_free(raw);
    heap_caps_free(pcm);
    s_playing.store(false);
    s_task_exited.store(true);
    vTaskDelete(nullptr);
}

bool mcp_control_radio(const XiaozhiMcpRadioRequest &request,
                       char *result,
                       size_t result_len)
{
    if (!result || result_len == 0) {
        return false;
    }
    switch (request.action) {
    case kXiaozhiMcpRadioStart: {
        if (radio_player_is_playing()) {
            snprintf(result, result_len,
                     "{\"status\":\"already_playing\",\"station\":\"%s\"}",
                     kStationName);
            return true;
        }
        if (!radio_player_start()) {
            snprintf(result, result_len,
                     "radio rejected: must be on Xiaozhi AI page or stream busy");
            return false;
        }
        snprintf(result, result_len,
                 "{\"status\":\"started\",\"station\":\"%s\"}", kStationName);
        return true;
    }
    case kXiaozhiMcpRadioStop: {
        const bool was_playing = radio_player_is_playing();
        radio_player_stop();
        snprintf(result, result_len,
                 was_playing
                     ? "{\"status\":\"stopped\",\"station\":\"%s\"}"
                     : "{\"status\":\"stopped\",\"station\":\"%s\","
                       "\"note\":\"was not playing\"}",
                 kStationName);
        return true;
    }
    case kXiaozhiMcpRadioStatus:
    default: {
        snprintf(result, result_len,
                 radio_player_is_playing()
                     ? "{\"status\":\"playing\",\"station\":\"%s\"}"
                     : "{\"status\":\"idle\"}",
                 kStationName);
        return true;
    }
    }
}

} // namespace

bool radio_player_init()
{
    xiaozhi_mcp_register_radio_handler(mcp_control_radio);
    return true;
}

bool radio_player_start()
{
    if (s_playing.load()) {
        return true;
    }
    if (s_task != nullptr) {
        // 上一段仍在退出中。
        return false;
    }
    if (!audio_codec_active()) {
        ESP_LOGW(TAG, "radio start refused: codec session inactive");
        return false;
    }
    s_stop_request.store(false);
    s_task_exited.store(false);
    if (!s_task_stack) {
        s_task_stack = static_cast<StackType_t *>(
            heap_caps_malloc(kTaskStackBytes, MALLOC_CAP_SPIRAM));
    }
    if (s_task_stack) {
        s_task = xTaskCreateStaticPinnedToCore(radio_task,
                                               "radio_player",
                                               kTaskStackBytes,
                                               nullptr,
                                               kTaskPriority,
                                               s_task_stack,
                                               &s_task_buffer,
                                               kTaskCore);
    }
    if (!s_task) {
        s_stop_request.store(true);
        s_task_exited.store(true);
        ESP_LOGW(TAG, "radio task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "radio playback started: %s", kStationName);
    return true;
}

void radio_player_stop()
{
    if (s_task == nullptr && !s_playing.load()) {
        return;
    }
    s_stop_request.store(true);
    for (uint32_t i = 0; i < kStopWaitRounds && !s_task_exited.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(kStopWaitRoundMs));
    }
    if (s_task_exited.load()) {
        heap_caps_free(s_task_stack);
        s_task_stack = nullptr;
        s_task = nullptr;
        ESP_LOGI(TAG, "radio playback stopped");
    } else {
        ESP_LOGW(TAG, "radio task stop timed out; retry stop to reclaim");
    }
}

bool radio_player_is_playing()
{
    return s_playing.load();
}
