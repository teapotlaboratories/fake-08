/* fake08_board.h — the display contract the ESP32 Host draws through.
 *
 * Mirrors boards/<board>/board.h (extern "C"); the board (board.cpp, compiled into the pico-e32
 * firmware app) provides the definitions. Kept deliberately minimal — just the three calls drawFrame /
 * oneTimeSetup make — so the fake-08 component needn't pull the board's esp_err.h / IDF headers.
 * board_lcd_init() is intentionally absent: bringing up the bus/panel is the app's job (it owns the board).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Blit an RGB565 rectangle (panel pixels). Blocks until the transfer drains, so src is reusable on
 * return. Colours must come from board_lcd_rgb565 so the byte order matches the board's bus. */
void board_lcd_blit(int x, int y, int w, int h, const uint16_t *src);

/* Fill the whole panel with one RGB565 colour. */
void board_lcd_fill(uint16_t color);

/* RGB888 -> RGB565 in the board's bus byte order. Build the palette LUT with this. */
uint16_t board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* Audio output seam (optional). board_audio_init() returns 0 (ESP_OK) if this board has working audio
 * output; a board with none returns non-zero and the Host stays silent. board_audio_write() plays `frames`
 * stereo 16-bit frames, blocking until queued — which self-paces the caller to the audio clock. */
int  board_audio_init(void);
void board_audio_write(const int16_t *stereo, size_t frames);

#ifdef __cplusplus
}
#endif
