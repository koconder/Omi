#include <zephyr/kernel.h>
#include "lib/sdcard/sdcard.h"
#include <zephyr/logging/log.h>
#include "transport.h"
#include "storage.h"
#include "mic.h"
#include "utils.h"
#include "led.h"
#include "config.h"
#include "audio.h"
#include "codec.h"

static void codec_handler(uint8_t *data, size_t len)
{
	broadcast_audio_packets(data, len); // Errors are logged inside
}

static void mic_handler(int16_t *buffer)
{
	codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES); // Errors are logged inside
}

void bt_ctlr_assert_handle(char *name, int type)
{
	if (name != NULL)
	{
		printk("Bt assert-> %s", name);
	}
}




// Main loop
int main(void)
{
	// Led start
	ASSERT_OK(led_start());
	set_led_blue(true);

	// Transport start
	ASSERT_OK(transport_start());

	// Codec start
	set_codec_callback(codec_handler);
	ASSERT_OK(codec_start());

	// Mic start
	set_mic_callback(mic_handler);
	ASSERT_OK(mic_start());

	// Storage start
	init_storage();

	// Blink LED
	bool is_on = true;
	//set_led_blue(false);
	set_led_red(is_on);

	//ReadInfo info = get_current_file();
	//printf("name: %u, ret: %d\n",info.name,info.ret);	

	while (1)
	{
		read_audio_in_storage();
		is_on = !is_on;
		set_led_red(is_on);
		k_msleep(500);

	}

	// Unreachable
	return 0;
}