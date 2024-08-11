#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "transport.h"
#include "mic.h"
#include "utils.h"
#include "led.h"
#include "config.h"
#include "audio.h"
#include "codec.h"
#include "storage.h"
#include <stdbool.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static void codec_handler(uint8_t *data, size_t len)
{
    int err = broadcast_audio_packets(data, len);
    if (err) {
        LOG_ERR("Failed to broadcast audio packets: %d", err);
    }

    // err = save_audio_to_storage(data, len);
    // if (err) {
    //     LOG_ERR("Failed to save audio to storage: %d", err);
    // }
}

static void mic_handler(int16_t *buffer)
{
    int err = codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES);
    if (err) {
        LOG_ERR("Failed to process PCM data: %d", err);
    }
}

void bt_ctlr_assert_handle(char *name, int type)
{
    LOG_ERR("Bluetooth assert: %s (type %d)", name ? name : "NULL", type);
}

bool is_connected = false;
bool is_charging = false;

static void set_led_state(bool is_connected, bool is_charging)
{
	// Recording and connected state - BLUE
	if (is_connected)
	{
		set_led_red(false);
		set_led_green(false);
		set_led_blue(true);
		return;
	}
	// Recording but lost connection - RED
	if (!is_connected)
	{
		set_led_red(true);
		set_led_green(false);
		set_led_blue(false);
		return;
	}
	// Not recording, but charging - WHITE
	if (is_charging)
	{
		set_led_red(true);
		set_led_green(true);
		set_led_blue(true);
		return;
	}
	// Not recording - OFF
	set_led_red(false);
	set_led_green(false);
	set_led_blue(false);
}

void test_sd_card(void) {
    char test_data[] = "Hello, SD card!";
    int ret = create_file("test.txt");
    if (ret) {
        LOG_ERR("Failed to create test file: %d", ret);
        return;
    }

    ret = write_file((uint8_t *)test_data, strlen(test_data), false, true);
    if (ret) {
        LOG_ERR("Failed to write test data: %d", ret);
        return;
    }

    LOG_INF("Successfully wrote test data to SD card");
}

int main(void)
{
    int err;

    LOG_INF("Omi firmware starting...");

    err = led_start();
    if (err) {
        LOG_ERR("Failed to initialize LEDs: %d", err);
        return err;
    }
    set_led_blue(true);

    LOG_INF("Initializing storage...");
    err = init_storage();
    if (err) {
        LOG_ERR("Failed to initialize storage: %d", err);
		// Don't return here, continue with other initializations
        // return err;
    }else{
		LOG_INF("Storage initialized successfully");
		test_sd_card();
	}

    err = transport_start();
    if (err) {
        LOG_ERR("Failed to start transport: %d", err);
        return err;
    }

    set_codec_callback(codec_handler);
    err = codec_start();
    if (err) {
        LOG_ERR("Failed to start codec: %d", err);
        return err;
    }

    set_mic_callback(mic_handler);
    err = mic_start();
    if (err) {
        LOG_ERR("Failed to start microphone: %d", err);
        return err;
    }

    LOG_INF("Omi firmware initialized successfully");

    while (1) {
        // Simplified main loop for initial testing
        set_led_state(is_connected, is_charging);
        k_msleep(500);
    }

    return 0;
}
