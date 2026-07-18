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

#include <dirent.h>
#include <cctype>

#include "host.h"
#include "hostVmShared.h"
#include "nibblehelpers.h"
#include "filehelpers.h"   /* isCartFile */
#include "fake08_board.h"
#include "input.h"         /* input_init/input_poll — the compile-time-selectable input backend */

static const char *TAG = "esp32host";

/* PICO-8 is 128x128; the panel is 320x480 portrait. Integer 2x -> 256x256, centred (letterboxed). */
#define PICO_W 128
#define PICO_H 128
#define SCALE  2
#define DST_W  (PICO_W * SCALE)     /* 256 */
#define DST_H  (PICO_H * SCALE)     /* 256 */
#define OX     ((320 - DST_W) / 2)  /* 32  */
#define OY     0                    /* game flush to the top; the touch control deck owns the bottom 224 px */

static uint16_t  s_lut[144];        /* PICO-8 colour index -> RGB565 (board byte order) */

/* Blit in strips from a small INTERNAL DMA buffer. A full 256x256 fb (128 KB) doesn't fit internal SRAM
 * and falls back to slow PSRAM (measured: 16.5 ms/frame — the dominant cost). A strip fits internal,
 * keeping the per-pixel writes AND the blit DMA in fast SRAM. */
#define STRIP_PR   16                 /* pico rows per strip */
#define STRIP_ROWS (STRIP_PR * SCALE) /* panel rows per strip (32) */
static uint16_t *s_strip = nullptr;   /* DST_W * STRIP_ROWS RGB565 (16 KB), internal DMA */

/* Frame pacing (esp_timer). fake-08's game loop (__z8_run_cart's glue coroutine) is designed to be
 * RESUMED AT 60 Hz: a _update60 cart runs one resume per drawn frame (60 fps), and a 30 fps cart
 * (_update, e.g. Celeste) burns an extra yield() so it runs one drawn frame per TWO resumes — the
 * coroutine self-divides 60 Hz down to 30 fps. Pacing this at 30 Hz (the old default) therefore ran
 * 30 fps carts at HALF speed (15 fps of motion). So the host resumes at 60 Hz; the cart's own loop
 * decides its logical rate. Work is ~6 ms/frame, well inside the 16.6 ms budget. */
static int64_t s_frame_period_us = 1000000 / 60;
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
    /* Strip buffer in internal DMA-capable SRAM (16 KB fits; the full 128 KB fb does not — see above). */
    s_strip = (uint16_t *)heap_caps_malloc(DST_W * STRIP_ROWS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_strip) {
        s_strip = (uint16_t *)heap_caps_malloc(DST_W * STRIP_ROWS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG, "strip buffer fell back to PSRAM");
    }
    board_lcd_fill(board_lcd_rgb565(0, 0, 0)); /* clear the letterbox once */
    ESP_LOGI(TAG, "oneTimeSetup done (strip=%p, %dx%d, %d strips @ %d,%d)",
             s_strip, DST_W, STRIP_ROWS, PICO_H / STRIP_PR, OX, OY);
}

void Host::oneTimeCleanup() {
    if (s_strip) {
        heap_caps_free(s_strip);
        s_strip = nullptr;
    }
}

void Host::setTargetFps(int targetFps) {
    if (targetFps <= 0) targetFps = 30;
    s_frame_period_us = 1000000 / targetFps;
    s_next_frame_us = esp_timer_get_time();
}

#ifdef SHOW_FPS
extern "C" void board_lcd_draw_fps(int fps);
#endif

void Host::waitForTargetFps() {
    int64_t now = esp_timer_get_time();
#ifdef SHOW_FPS
    /* On-screen FPS HUD: measure the actual loop (render) rate over ~30 frames and repaint it in the
     * right letterbox only when the integer value changes (the game blit never touches x>=288). */
    {
        static int64_t s_last = 0, s_acc = 0;
        static int s_cnt = 0, s_shown = -1;
        if (s_last) {
            s_acc += now - s_last;
            if (++s_cnt >= 30) {
                int fps = (int)(1e6 * s_cnt / (double)s_acc + 0.5);
                if (fps != s_shown) { board_lcd_draw_fps(fps); s_shown = fps; }
                s_acc = 0; s_cnt = 0;
            }
        }
        s_last = now;
    }
#endif
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
    if (!s_strip) return;
    /* Straight mapping: pico (x,y) -> panel (2x,2y), matching the HG display path (host_main.cpp); the
     * panel renders this UPRIGHT. Built + blitted in strips so the buffer stays in fast internal SRAM.
     * The nibble-unpack is inlined (read one byte -> two pixels) to avoid a per-pixel getPixelNibble call.
     * NOTE: the bench camera is mounted 90deg rotated (bench-rig-gotchas: "LEFT of frame = TOP of panel")
     * — a raw /capture looks rotated and must be turned 90deg CW before judging. Do not correct it here. */
    for (int sy = 0; sy < PICO_H; sy += STRIP_PR) {
        for (int ry = 0; ry < STRIP_PR; ry++) {
            const uint8_t *src = picoFb + ((sy + ry) << 6); /* pico row base: *64 (4bpp, 2 px/byte) */
            uint16_t *d0 = s_strip + (ry * SCALE) * DST_W;
            uint16_t *d1 = d0 + DST_W;
            for (int x = 0; x < PICO_W; x += 2) {
                uint8_t b = src[x >> 1];
                uint16_t c0 = s_lut[screenPaletteMap[b & 0x0f] & 0x8f]; /* even x: low nibble  */
                uint16_t c1 = s_lut[screenPaletteMap[b >> 4]   & 0x8f]; /* odd  x: high nibble */
                int dx = x * SCALE;
                d0[dx] = c0; d0[dx + 1] = c0; d0[dx + 2] = c1; d0[dx + 3] = c1;
                d1[dx] = c0; d1[dx + 1] = c0; d1[dx + 2] = c1; d1[dx + 3] = c1;
            }
        }
        board_lcd_blit(OX, OY + (sy * SCALE), DST_W, STRIP_ROWS, s_strip);
    }
}

InputState_t Host::scanInput() {
    static bool    s_ready = false;
    static uint8_t s_prevHeld = 0;
    if (!s_ready) {
        input_init();                            /* brings up the compiled backend once */
        ESP_LOGI(TAG, "input backend: %s", input_backend_name());
        s_ready = true;
    }
    uint8_t held = input_poll();                 /* held mask; INPUT_* bits == fake-08 P8_KEY_* order */
    InputState_t s = {};
    s.KHeld = held;
    s.KDown = (uint8_t)(held & ~s_prevHeld);     /* pressed-this-frame edge, shared across all backends */
    s_prevHeld = held;
    return s;                                     /* mouse/keyboard stay zeroed (devkit-mode only) */
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

/* Scan _cartDirectory (set by the app to the SD mount point) for .p8/.p8.png. opendir failing — no
 * mount, no card, empty path — returns an empty list, which is the graceful "no SD cart" path (the app
 * then falls back to the flash cart). Ported from platform/gcw0 (ODHost.cpp:642). */
std::vector<std::string> Host::listcarts() {
    std::vector<std::string> carts;
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(_cartDirectory.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            /* isCartFile's .p8/.png check is case-sensitive, but a card's names can be any case — with LFN
             * (enabled in the app's sdkconfig) readdir returns the real mixed case, and an 8.3 fallback
             * would be upper — so match on a lowercased copy. FAT open is itself case-insensitive, so keep
             * the original-case name in the path we return. */
            std::string lower(ent->d_name);
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            if (isCartFile(lower)) {
                carts.push_back(_cartDirectory + "/" + ent->d_name);
            }
        }
        closedir(dir);
    }
    return carts;
}
std::vector<std::string> Host::listdirs()  { return {}; } /* not needed for the first-cart milestone */
std::string Host::getCartDirectory()       { return _cartDirectory; }
const char *Host::logFilePrefix()          { return ""; }
std::string Host::customBiosLua()          { return ""; }
void Host::overrideLogFilePrefix(const char *newPrefix) { (void)newPrefix; }

void Host::setPlatformParams(int, int, uint32_t, uint32_t, uint32_t,
                             std::string, std::string, std::string) { }
