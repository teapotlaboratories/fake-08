/* ESP32Host.cpp — fake-08's Host binding for the ESP32-S3 (pico-e32).
 *
 * This is the platform/esp32 port. It implements fake-08's Host interface (source/host.h) against the
 * board's board_lcd_* display contract (fake08_board.h). source/ stays byte-identical to upstream fake-08;
 * this file + the component CMakeLists are the additive port — the 1-to-1 rule (see
 * docs/runtime/pico-e32-fake08-port.md in the pico-e32 repo).
 *
 * The shared source/hostCommonFunctions.cpp already defines the filesystem / palette / settings Host
 * methods; this file defines the platform complement: display, timing, and the input/audio seams.
 *
 * Draw-only milestone: input + audio are stubs (parts-blocked); the panel renders a flash-embedded cart.
 * Only the default draw mode is handled — the board owns the panel's Y-flip (offset_rotation), so drawFrame
 * writes pixels in natural order.
 */
#include <stdint.h>
#include <string.h>
#include <vector>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "host.h"
#include "hostVmShared.h"
#include "nibblehelpers.h"
#include "fake08_board.h"

static const char *TAG = "esp32host";

/* PICO-8 is 128x128; the panel is 320x480 portrait. Integer 2x -> 256x256, centred (letterboxed). */
#define PICO_W 128
#define PICO_H 128
#define SCALE  2
#define DST_W  (PICO_W * SCALE)     /* 256 */
#define DST_H  (PICO_H * SCALE)     /* 256 */
#define OX     ((320 - DST_W) / 2)  /* 32  */
#define OY     ((480 - DST_H) / 2)  /* 112 */

static uint16_t  s_lut[144];        /* PICO-8 colour index -> RGB565 (board byte order) */
static uint16_t *s_fb = nullptr;    /* 256x256 scaled RGB565 frame; one blit per frame  */

/* Frame pacing (esp_timer). */
static int64_t s_frame_period_us = 1000000 / 30;
static int64_t s_next_frame_us   = 0;

Host::Host(int windowWidth, int windowHeight) {
    (void)windowWidth;
    (void)windowHeight;
    _cartDirectory = "";
    _logFilePrefix = "";
}

void Host::oneTimeSetup(Audio *audio) {
    (void)audio;
    /* Build the RGB565 palette LUT from the base colours. setUpPaletteColors() must have run first. */
    for (int i = 0; i < 144; i++) {
        s_lut[i] = board_lcd_rgb565(_paletteColors[i].Red, _paletteColors[i].Green, _paletteColors[i].Blue);
    }
    /* Scaled framebuffer: prefer internal DMA-capable SRAM for blit speed, fall back to PSRAM. */
    s_fb = (uint16_t *)heap_caps_malloc(DST_W * DST_H * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_fb) {
        s_fb = (uint16_t *)heap_caps_malloc(DST_W * DST_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG, "scaled fb fell back to PSRAM");
    }
    board_lcd_fill(board_lcd_rgb565(0, 0, 0)); /* clear the letterbox once */
    ESP_LOGI(TAG, "oneTimeSetup done (fb=%p, %dx%d @ %d,%d)", s_fb, DST_W, DST_H, OX, OY);
}

void Host::oneTimeCleanup() {
    if (s_fb) {
        heap_caps_free(s_fb);
        s_fb = nullptr;
    }
}

void Host::setTargetFps(int targetFps) {
    if (targetFps <= 0) targetFps = 30;
    s_frame_period_us = 1000000 / targetFps;
    s_next_frame_us = esp_timer_get_time();
}

void Host::waitForTargetFps() {
    int64_t now = esp_timer_get_time();
    if (s_next_frame_us == 0) s_next_frame_us = now;
    s_next_frame_us += s_frame_period_us;
    int64_t wait_us = s_next_frame_us - now;
    if (wait_us > 1000) {
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000)); /* yield: feeds the idle task + task watchdog */
    } else {
        vTaskDelay(1);                             /* always yield >=1 tick */
        if (wait_us < -s_frame_period_us) s_next_frame_us = now; /* fell behind: resync, no catch-up burst */
    }
}

void Host::drawFrame(uint8_t *picoFb, uint8_t *screenPaletteMap, uint8_t drawMode) {
    (void)drawMode; /* draw-only milestone: only the default draw mode */
    if (!s_fb) return;
    /* The board's display path presents PICO-8 content rotated 90deg (verified on the bench: the L-pattern
     * cart's top+left bars render as an on-panel "L", and text reads sideways). Pre-rotate 90deg CW here so
     * carts render upright: pico (x,y) -> scaled buffer (row = 2x, col = 255 - 2y). One blit/frame. */
    for (int y = 0; y < PICO_H; y++) {
        int col_base = (DST_W - 1) - y * SCALE; /* 255 - 2y; fills cols col_base and col_base-1 */
        for (int x = 0; x < PICO_W; x++) {
            uint8_t c = getPixelNibble(x, y, picoFb);
            uint16_t col = s_lut[screenPaletteMap[c] & 0x8f];
            uint16_t *r0 = s_fb + (x * SCALE) * DST_W + col_base;
            uint16_t *r1 = r0 + DST_W;
            r0[0] = col; r0[-1] = col;
            r1[0] = col; r1[-1] = col;
        }
    }
    board_lcd_blit(OX, OY, DST_W, DST_H, s_fb);
}

InputState_t Host::scanInput() {
    InputState_t s = {}; /* no buttons: KDown=KHeld=0, no mouse, no keyboard -> pause menu never opens */
    return s;
}

bool Host::shouldRunMainLoop() { return true; }
bool Host::shouldQuit()        { return false; }
void Host::changeStretch()     { }
void Host::forceStretch(StretchOption newStretch) { (void)newStretch; }

bool   Host::shouldFillAudioBuff()   { return false; }
void  *Host::getAudioBufferPointer() { return nullptr; }
size_t Host::getAudioBufferSize()    { return 0; }
void   Host::playFilledAudioBuffer() { }

double Host::deltaTMs() { return (double)s_frame_period_us / 1000.0; }

std::vector<std::string> Host::listcarts() { return {}; }
std::vector<std::string> Host::listdirs()  { return {}; }
std::string Host::getCartDirectory()       { return ""; }
const char *Host::logFilePrefix()          { return ""; }
std::string Host::customBiosLua()          { return ""; }
void Host::overrideLogFilePrefix(const char *newPrefix) { (void)newPrefix; }

void Host::setPlatformParams(int, int, uint32_t, uint32_t, uint32_t,
                             std::string, std::string, std::string) { }
