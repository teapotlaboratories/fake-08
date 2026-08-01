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

/* PICO-8 is 128x128. The integer upscale is chosen at RUNTIME from the panel width — the largest that
 * fits, clamped to [1, MAX_SCALE] — so each board fills more of its glass without ever exceeding the panel:
 * S3 320 -> 2x (256x256), P4 480 -> 3x (384x384). Panel geometry + offsets are likewise runtime (the app
 * passes the panel size to the Host ctor); defaults match the S3 so a Host(0,0) still behaves as before. */
#define PICO_W    128
#define PICO_H    128
#define STRIP_PR  16                  /* pico rows per strip */
#define MAX_SCALE 3                   /* bounds the strip buffer (P4 480-wide -> 3x is the largest today) */

static int s_scale      = 2;          /* PICO_W*s_scale = game width; set in oneTimeSetup */
static int s_dst_w      = PICO_W * 2; /* 256 by default */
static int s_dst_h      = PICO_H * 2;
static int s_strip_rows = STRIP_PR * 2;
static int s_panel_w    = 320;
static int s_panel_h    = 480;
static int s_ox = 0, s_oy = 0;

static uint16_t  s_lut[144];        /* PICO-8 colour index -> RGB565 (board byte order) */

/* Blit in strips from a small INTERNAL DMA buffer. A full frame buffer doesn't fit internal SRAM and
 * falls back to slow PSRAM; a strip (s_dst_w * s_strip_rows: 16 KB at 2x, 36 KB at 3x) stays in fast SRAM,
 * keeping the per-pixel writes AND the blit DMA there. */
static uint16_t *s_strip = nullptr;
static uint8_t  *s_fb_shadow = nullptr;   /* last-blitted pico framebuffer, to skip re-blitting an unchanged frame */

/* Audio runs on a task pinned to CORE 1, decoupled from the game loop (core 0). fake-08's per-sample synth
 * uses double math, which is soft-float on the P4's single-precision FPU (~10 ms per buffer); running it
 * inline in the game loop pushed the loop past its 16.6 ms/60 Hz budget and played everything ~10% slow.
 * The task generates a buffer then does a BLOCKING I2S write, which self-paces it to the 22050 Hz clock;
 * core 0 is then free to hold 60 Hz. Buffer is AUDIO_FRAMES stereo frames (uint32 each). */
#define AUDIO_FRAMES 368
static bool         s_audio_ok   = false;
static int16_t     *s_audio_buf  = nullptr;
static Audio       *s_audio      = nullptr;   /* fake-08's synth engine (passed to oneTimeSetup) */
static TaskHandle_t s_audio_task = nullptr;

/* LOCK-FREE by design: this task reads/advances the Audio engine on core 1 while the game loop mutates it
 * on core 0 (sfx()/music() from the cart). No mutex — PICO-8's audio state is fixed-size (4 sfx channels +
 * one music channel; pattern indices are always -1..63, no realloc/pointers), so a torn cross-core read is
 * at worst a one-buffer audio glitch, never an out-of-bounds. If artifacts ever appear under heavy
 * sfx/music churn, guard the FillAudioBuffer + sfx/music paths with a lightweight critical section. */
static void audio_task(void *arg) {
    (void)arg;
    while (true) {
        s_audio->FillAudioBuffer(s_audio_buf, 0, AUDIO_FRAMES);   /* synthesis (soft-float doubles) on core 1 */
        board_audio_write(s_audio_buf, AUDIO_FRAMES);             /* blocking I2S write paces to 22050 Hz */
    }
}

/* Largest integer scale that fits the panel width, clamped to [1, MAX_SCALE]. Flooring at 1 (not 2) means
 * the game never renders wider than the glass: a <256 px panel gets 1x rather than an oversized 256 px blit
 * that would overflow. The two real panels are >=320, so both still land on 2x/3x — this only guards the
 * degenerate narrow case. */
static int pico_scale(int w) { int s = w / PICO_W; if (s < 1) s = 1; if (s > MAX_SCALE) s = MAX_SCALE; return s; }

/* Frame pacing (esp_timer). fake-08's game loop (__z8_run_cart's glue coroutine) is designed to be
 * RESUMED AT 60 Hz: a _update60 cart runs one resume per drawn frame (60 fps), and a 30 fps cart
 * (_update, e.g. Celeste) burns an extra yield() so it runs one drawn frame per TWO resumes — the
 * coroutine self-divides 60 Hz down to 30 fps. Pacing this at 30 Hz (the old default) therefore ran
 * 30 fps carts at HALF speed (15 fps of motion). So the host resumes at 60 Hz; the cart's own loop
 * decides its logical rate. Work is ~6 ms/frame, well inside the 16.6 ms budget. */
static int64_t s_frame_period_us = 1000000 / 60;
static int64_t s_next_frame_us   = 0;

Host::Host(int windowWidth, int windowHeight) {
    /* The app passes the board's panel size here (BOARD_LCD_H_RES/V_RES); 0 keeps the S3 default. Used to
     * centre the game blit at runtime so one host serves any panel geometry. */
    if (windowWidth > 0)  s_panel_w = windowWidth;
    if (windowHeight > 0) s_panel_h = windowHeight;
    _cartDirectory = "";
    _logFilePrefix = "";
}

void Host::oneTimeSetup(Audio *audio) {
    s_audio = audio;   /* kept for the core-1 audio_task; see the audio bring-up at the end of this function */
    /* Build the RGB565 palette LUT from the base colours. setUpPaletteColors() must have run first. */
    for (int i = 0; i < 144; i++) {
        s_lut[i] = board_lcd_rgb565(_paletteColors[i].Red, _paletteColors[i].Green, _paletteColors[i].Blue);
    }
    /* Pick the integer upscale for this panel, then size the strip buffer for it. */
    s_scale = pico_scale(s_panel_w);
    s_dst_w = PICO_W * s_scale;
    s_dst_h = PICO_H * s_scale;
    s_strip_rows = STRIP_PR * s_scale;
    /* Strip buffer in internal DMA-capable SRAM (the full frame buffer does not fit — see above). */
    s_strip = (uint16_t *)heap_caps_malloc((size_t)s_dst_w * s_strip_rows * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_strip) {
        s_strip = (uint16_t *)heap_caps_malloc((size_t)s_dst_w * s_strip_rows * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG, "strip buffer fell back to PSRAM");
    }
    /* Horizontally centre the game on the panel (an integer upscale can be narrower than the glass), and
     * put it flush to the TOP — the control deck (if any) owns the band below. Always flush-top now. */
    s_ox = (s_panel_w - s_dst_w) / 2;
    if (s_ox < 0) s_ox = 0;
    s_oy = 0;
    /* Shadow of the last-blitted pico framebuffer. A 30 fps cart yields TWICE per game-frame (one bare yield
     * that draws nothing, then _draw()+flip()), but the host loop blits after every Step — so ~half the blits
     * re-present an unchanged framebuffer. drawFrame() skips those by memcmp against this shadow (~us) instead
     * of paying the ~6 ms upscale+blit. 0xFF-init forces the first frame to blit. */
    s_fb_shadow = (uint8_t *)heap_caps_malloc((size_t)PICO_W * PICO_H / 2, MALLOC_CAP_INTERNAL);
    if (s_fb_shadow) memset(s_fb_shadow, 0xff, (size_t)PICO_W * PICO_H / 2);
    else ESP_LOGW(TAG, "no fb shadow — every Step will blit (no unchanged-frame skip)");
    board_lcd_fill(board_lcd_rgb565(0x0f, 0x14, 0x1d)); /* fill the letterbox/deck once — subtle dark
                                                         * surface (mockup's deck colour), not pure black;
                                                         * the touch deck (input_touch.c) uses the same tone. */
    ESP_LOGI(TAG, "oneTimeSetup done (strip=%p, %dx%d game @ %d,%d, %dx scale on %dx%d panel)",
             s_strip, s_dst_w, s_dst_h, s_ox, s_oy, s_scale, s_panel_w, s_panel_h);

    /* Audio: bring up the board's codec (if any). The Host owns a small stereo buffer that fake-08 fills
     * each loop (poll path); playFilledAudioBuffer blocks in the I2S write, self-pacing the loop. A board
     * with no audio (board_audio_init != 0) leaves s_audio_ok false and the Host runs silent. */
    s_audio_ok = (s_audio != nullptr && board_audio_init() == 0);
    if (s_audio_ok) {
        s_audio_buf = (int16_t *)heap_caps_malloc(AUDIO_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_DEFAULT);
        if (!s_audio_buf) s_audio_ok = false;
    }
    if (s_audio_ok) {
        /* Pin to core 1 so the ~10 ms/buffer synth never steals from the 60 Hz game loop on core 0. Prio
         * above idle; it yields every buffer in the blocking I2S write, which also feeds the task watchdog. */
        xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 6, &s_audio_task, 1);
    }
    ESP_LOGI(TAG, "audio %s", s_audio_ok ? "enabled (ES8311, core-1 task, 22050 Hz S16 stereo)"
                                         : "disabled (no board audio)");
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
extern "C" volatile int g_hud_owned_by_app;   /* set by an app loop that draws a truer game-frame fps itself */
#endif

void Host::waitForTargetFps() {
    int64_t now = esp_timer_get_time();
#ifdef SHOW_FPS
    /* On-screen FPS HUD: measure the actual loop (render) rate over ~30 ticks and repaint only when the
     * integer value changes (the HUD lives outside the game blit region, so it persists). NOTE: this counts
     * coroutine resumes, which for a 30 fps cart is ~2x the drawn-frame rate — so if an app loop owns the HUD
     * (g_hud_owned_by_app) and draws the true game-frame fps, this generic meter stands down. */
    if (!g_hud_owned_by_app) {
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
    /* Frame pacing (video). Audio now lives on the core-1 task, so this is the sole pacer again — it holds
     * the game loop at 60 Hz on core 0 (the cart self-divides to its logical 30/60 fps; audio is independent). */
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

/* The actual upscale + panel blit, reading a fixed fb (4bpp, 2 px/byte) + palette. Straight mapping pico
 * (x,y) -> panel (sc*x, sc*y); built + blitted in strips so the working buffer stays in fast internal SRAM.
 * The nibble-unpack is inlined (one byte -> two pixels) to avoid a per-pixel call. */
static void host_blit(const uint8_t *fb, const uint8_t *pal) {
    const int sc = s_scale;
    for (int sy = 0; sy < PICO_H; sy += STRIP_PR) {
        for (int ry = 0; ry < STRIP_PR; ry++) {
            const uint8_t *src = fb + ((sy + ry) << 6);
            uint16_t *row0 = s_strip + (ry * sc) * s_dst_w;
            for (int x = 0; x < PICO_W; x += 2) {
                uint8_t b = src[x >> 1];
                uint16_t c0 = s_lut[pal[b & 0x0f] & 0x8f];
                uint16_t c1 = s_lut[pal[b >> 4]   & 0x8f];
                int dx = x * sc;
                for (int k = 0; k < sc; k++) row0[dx + k] = c0;
                for (int k = 0; k < sc; k++) row0[dx + sc + k] = c1;
            }
            for (int k = 1; k < sc; k++)
                memcpy(row0 + (size_t)k * s_dst_w, row0, (size_t)s_dst_w * sizeof(uint16_t));
        }
        board_lcd_blit(s_ox, s_oy + (sy * sc), s_dst_w, s_strip_rows, s_strip);
    }
}

void Host::drawFrame(uint8_t *picoFb, uint8_t *screenPaletteMap, uint8_t drawMode) {
    (void)drawMode; /* draw-only milestone: only the default draw mode */
    if (!s_strip) return;
    /* Skip re-blitting an unchanged framebuffer: a 30 fps cart presents the SAME fb on its bare-yield Step, so
     * ~half the host's per-Step blits are redundant. memcmp of the 8 KB fb is ~us vs the ~6 ms blit it saves.
     * (Palette-only changes with no pixel change wait for the next real draw — PICO-8 carts pal() inside _draw.) */
    if (s_fb_shadow) {
        const size_t fbsz = (size_t)PICO_W * PICO_H / 2;
        if (memcmp(s_fb_shadow, picoFb, fbsz) == 0) return;   /* nothing changed -> keep the panel as-is */
        memcpy(s_fb_shadow, picoFb, fbsz);
    }
    host_blit(picoFb, screenPaletteMap);
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

/* The game loop must NOT fill audio — the core-1 audio_task pulls from the synth engine directly and does
 * the blocking I2S write, so audio pacing is off the game loop (which stays at 60 Hz via the video timer).
 * Returning false here makes fake-08's GameLoop skip its inline fill/play entirely. */
bool   Host::shouldFillAudioBuff()   { return false; }
void  *Host::getAudioBufferPointer() { return s_audio_buf; }
size_t Host::getAudioBufferSize()    { return AUDIO_FRAMES; }   /* stereo frames (uint32 units), NOT bytes */
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
