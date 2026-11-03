#ifndef _VB6824_AUDIO_CODEC_H
#define _VB6824_AUDIO_CODEC_H

#include <driver/gpio.h>
#include <esp_timer.h>
#include <functional>
#include <mutex>
#include <vector>
#include "audio_codec.h"
#include "vb6824.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

/**
 * Audio codec for boards with an Awinlan/VB6824 audio chip connected via UART.
 *
 * The VB6824 chip runs the vendor hybrid mode
 * (`CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K=y`): its microphone link sends
 * OPUS packets (16 kHz mono, 20 ms) over UART while playback expects plain
 * 16 kHz PCM. This codec decodes the mic packets with `esp_opus_dec` and
 * exchanges plain 16 kHz PCM with the application. Wake word detection is
 * performed on the VB6824 chip and reported through a voice-command callback.
 */
class VbAudioCodec : public AudioCodec {
private:
    void WakeUp(std::string command);
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

    std::function<void(std::string)> on_wake_up_;
    bool first_volume_is_set_ = false;

    void* opus_decoder_ = nullptr;
    // Mic-link OPUS packets are decoded on a dedicated task: the libopus SILK
    // decoder needs far more stack than the audio service input task that
    // calls Read() has (stack protection fault on the first decoded frame).
    // The decode task mirrors this fork's own "opus_codec" task (2048 * 12
    // bytes instead of 2048 * 2, lower priority).
    TaskHandle_t decode_task_handle_ = nullptr;
    SemaphoreHandle_t pcm_ready_ = nullptr;  // signals newly decoded PCM
    std::mutex pcm_queue_mutex_;
    // Decoded 16 kHz PCM samples waiting to be delivered by Read().
    std::vector<int16_t> pcm_queue_;
    // ~8 OPUS frames (160 ms @ 16 kHz) keeps input latency bounded.
    static constexpr size_t kMaxPcmQueueSamples = 320 * 8;
    void OpusDecodeTask();

public:
    VbAudioCodec(gpio_num_t tx, gpio_num_t rx);
    ~VbAudioCodec();
    void OnWakeUp(std::function<void(std::string)> callback);
    void SetOutputVolume(int volume) override;
    virtual void Start() override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif  // _VB6824_AUDIO_CODEC_H
