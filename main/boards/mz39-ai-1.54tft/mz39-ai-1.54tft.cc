#include "wifi_board.h"
#include "codecs/vb6824_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>

#include "vb6824.h"
#include "assets/lang_config.h"

#define TAG "Mz39Ai154Tft"

/**
 * MZ39-AI v1.3 board (esp32c3).
 *
 * 1.54" 240x240 ST7789 LCD, VB6824 audio chip over UART1 (hardware wake word),
 * boot button on GPIO9, volume up/down on GPIO7/GPIO8 (right/left).
 */
class Mz39Ai154TftBoard : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    VbAudioCodec audio_codec_;
    LcdDisplay* display_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = DISPLAY_CS_PIN,
            .dc_gpio_num = DISPLAY_DC_PIN,
            .spi_mode = 3,
            .pclk_hz = 80 * 1000 * 1000,
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        // This panel needs a double reset before init (vendor sequence).
        esp_lcd_panel_reset(panel);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    Mz39Ai154TftBoard() :
            boot_button_(BOOT_BUTTON_GPIO, false),
            volume_up_button_(VOLUME_UP_BUTTON_GPIO),
            volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
            audio_codec_(CODEC_TX_GPIO, CODEC_RX_GPIO) {

        // Wake word ("你好小智") is detected by the VB6824 chip and reported
        // here as a voice-command string from the UART.
        audio_codec_.OnWakeUp([this](const std::string& command) {
            if (command == std::string(vb6824_get_wakeup_word())) {
                Application::GetInstance().Schedule([]() {
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() != kDeviceStateListening) {
                        app.WakeWordInvoke("你好小智");
                    }
                });
            } else if (command == "开始配网") {
                Application::GetInstance().Schedule([this]() {
                    EnterWifiConfigMode();
                });
            }
        });

        InitializeButtons();
        InitializeSpi();
        InitializeLcdDisplay();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec_;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(Mz39Ai154TftBoard);
