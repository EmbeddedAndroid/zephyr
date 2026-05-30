/*
 * LED matrix demo for the Arduino UNO Q (STM32U585) using the charlieplex
 * display driver via the standard Zephyr display API.
 *
 * Walks a single lit pixel across the 13x8 matrix, then flashes the whole
 * panel, using display_write() with a MONO01 buffer. A green-LED heartbeat
 * runs independently so firmware liveness is visible even if the matrix path
 * misbehaves.
 *
 * Verification when hardware is connected: the moving dot proves per-pixel
 * addressing + the framebuffer->pin-pair mapping; the full-panel flash proves
 * the scan/refresh covers all 104 LEDs.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(led_matrix_demo, LOG_LEVEL_INF);

#define W 13
#define H 8
#define STRIDE ((W + 7) / 8)          /* bytes per row, MONO01 bit-packed */

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static uint8_t buf[STRIDE * H];

static void set_px(uint8_t *b, int x, int y, bool on)
{
	if (x < 0 || x >= W || y < 0 || y >= H) {
		return;
	}
	uint8_t *byte = &b[y * STRIDE + (x / 8)];

	if (on) {
		*byte |= BIT(x % 8);
	} else {
		*byte &= ~BIT(x % 8);
	}
}

static void push(void)
{
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(buf),
		.width = W,
		.height = H,
		.pitch = STRIDE * 8,          /* pitch is in pixels */
		.frame_incomplete = false,
	};

	display_write(disp, 0, 0, &desc, buf);
}

static void heartbeat(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	while (1) {
		gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(500));
	}
}
K_THREAD_DEFINE(hb_tid, 512, heartbeat, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	if (!device_is_ready(disp)) {
		LOG_ERR("display not ready");
		return -1;
	}

	struct display_capabilities caps;

	display_get_capabilities(disp, &caps);
	LOG_INF("matrix %ux%u up", caps.x_resolution, caps.y_resolution);

	display_blanking_off(disp);

	while (1) {
		/* Walking dot across all 104 cells. */
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				memset(buf, 0, sizeof(buf));
				set_px(buf, x, y, true);
				push();
				k_sleep(K_MSEC(40));
			}
		}

		/* Whole panel on, then off. */
		memset(buf, 0xFF, sizeof(buf));
		push();
		k_sleep(K_MSEC(600));
		memset(buf, 0, sizeof(buf));
		push();
		k_sleep(K_MSEC(400));
	}

	return 0;
}
