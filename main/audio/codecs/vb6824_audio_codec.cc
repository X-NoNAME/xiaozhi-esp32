#include "vb6824_audio_codec.h"

#include <algorithm>
#include <cstring>

#include "esp_opus_dec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "settings.h"

#include "vb6824.h"

static const char* TAG = "VbAudioCodec";

// The VB6824 audio link always runs at 16 kHz.
static constexpr int kVbSampleRate = 16000;

void VbAudioCodec::WakeUp(std::string command) {
    if (on_wake_up_) {
        on_wake_up_(command);
    }
}

void VbAudioCodec::OnWakeUp(std::function<void(std::string)> callback) { on_wake_up_ = callback; }

VbAudioCodec::VbAudioCodec(gpio_num_t tx, gpio_num_t rx) {
    input_sample_rate_ = kVbSampleRate;
    output_sample_rate_ = kVbSampleRate;

    vb6824_init(tx, rx);

    vb6824_register_voice_command_cb(
        [](char* command, uint16_t len, void* arg) {
            auto self = static_cast<VbAudioCodec*>(arg);
            self->WakeUp(std::string(command, len));
        },
        this);

    // The chip's mic link is OPUS-encoded (16 kHz mono, 20 ms packets);
    // ESP-side decoder turns the packets into 16 kHz PCM for the app.
    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate = kVbSampleRate,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };
    auto ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open OPUS decoder for the VB6824 mic link, error=%d", ret);
    }

    // Decode the mic-link OPUS packets on a dedicated task with a large
    // stack: running esp_opus_dec_decode on the audio service input task
    // (2048 * 2 bytes) overflows with a stack protection fault on the first
    // decoded frame. Same pattern as the fork's "opus_codec" task.
    pcm_ready_ = xSemaphoreCreateBinary();
    if (pcm_ready_ == nullptr ||
        xTaskCreate(
            [](void* arg) {
                auto self = static_cast<VbAudioCodec*>(arg);
                self->OpusDecodeTask();
                vTaskDelete(NULL);
            },
            "vb_opus_dec", 2048 * 12, this, 2, &decode_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the VB6824 OPUS decode task");
        decode_task_handle_ = nullptr;
    }
}

VbAudioCodec::~VbAudioCodec() {
    if (decode_task_handle_ != nullptr) {
        vTaskDelete(decode_task_handle_);
        decode_task_handle_ = nullptr;
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    if (pcm_ready_ != nullptr) {
        vSemaphoreDelete(pcm_ready_);
        pcm_ready_ = nullptr;
    }
}

void VbAudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)",
                 output_volume_);
        output_volume_ = 10;
    }
    ESP_LOGI(TAG, "Audio codec started, output_volume=%d", output_volume_);

    EnableOutput(true);
    EnableInput(true);
}

void VbAudioCodec::SetOutputVolume(int volume) {
    vb6824_audio_set_output_volume(volume);
    AudioCodec::SetOutputVolume(volume);  // updates output_volume_ and persists it in NVS
}

void VbAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    vb6824_audio_enable_input(enable);
    AudioCodec::EnableInput(enable);
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void VbAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    vb6824_audio_enable_output(enable);
    AudioCodec::EnableOutput(enable);
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

int VbAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_) {
        return 0;
    }
    int delivered = 0;
    while (delivered < samples) {
        {
            std::lock_guard<std::mutex> lock(pcm_queue_mutex_);
            int copied = std::min<int>(static_cast<int>(pcm_queue_.size()), samples - delivered);
            if (copied > 0) {
                std::copy(pcm_queue_.begin(), pcm_queue_.begin() + copied, dest + delivered);
                pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + copied);
                delivered += copied;
                continue;
            }
        }
        if (opus_decoder_ == nullptr) {
            break;
        }
        // Queue is empty: wait briefly for the decode task, then report
        // whatever we have (possibly 0). Waiting instead of busy-spinning
        // keeps the usually higher priority input task useful while the
        // decode task is being starved.
        if (xSemaphoreTake(pcm_ready_, pdMS_TO_TICKS(20)) == pdFALSE) {
            break;
        }
    }
    return delivered;
}

void VbAudioCodec::OpusDecodeTask() {
    // vb6824_audio_read() returns exactly one OPUS packet per call (the
    // component uses an item ring buffer in the OPUS mic-link mode) and
    // blocks internally until a packet arrives or input gets disabled.
    uint8_t packet_buf[256];
    // One decoded 20 ms OPUS packet: 320 PCM samples @ 16 kHz.
    int16_t pcm_buf[320];
    ESP_LOGI(TAG, "OPUS decode task started");
    for (;;) {
        if (!input_enabled_ || opus_decoder_ == nullptr) {
            // vb6824_audio_read() returns 0 immediately when input is
            // disabled; idle here instead of hammering the component.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int packet_len = vb6824_audio_read(packet_buf, sizeof(packet_buf));
        if (packet_len <= 0) {
            // Input was disabled while waiting for a packet.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = packet_buf;
        raw.len = static_cast<uint32_t>(packet_len);
        esp_audio_dec_out_frame_t out = {};
        out.buffer = reinterpret_cast<uint8_t*>(pcm_buf);
        out.len = sizeof(pcm_buf);
        esp_audio_dec_info_t info = {};
        auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out, &info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to decode VB6824 OPUS packet (%d bytes), error=%d", packet_len,
                     ret);
            esp_opus_dec_reset(opus_decoder_);
            continue;
        }
        int decoded = out.decoded_size / sizeof(int16_t);
        if (decoded <= 0) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(pcm_queue_mutex_);
            if (pcm_queue_.size() + decoded > kMaxPcmQueueSamples) {
                // Keep the freshest audio; input latency must stay bounded.
                size_t drop = pcm_queue_.size() + decoded - kMaxPcmQueueSamples;
                pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + drop);
            }
            pcm_queue_.insert(pcm_queue_.end(), pcm_buf, pcm_buf + decoded);
        }
        xSemaphoreGive(pcm_ready_);
    }
}

int VbAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        if (first_volume_is_set_ == false) {
            first_volume_is_set_ = true;
            SetOutputVolume(output_volume_);
        }
        vb6824_audio_write(reinterpret_cast<uint8_t*>(const_cast<int16_t*>(data)), 2 * samples);
        return samples;
    }
    return 0;
}
