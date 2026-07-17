# fake-08 as an ESP-IDF component — the pico-e32 (ESP32-S3) platform port.
#
# source/ is byte-identical to upstream fake-08 (the 1-to-1 rule); this file and platform/esp32/ are the
# additive port. Built as part of the firmware/pico-e32-fake08 app. The Lua VM is the repo's shared
# z8lua component (NOT fake-08's nested libs/z8lua, which is left un-inited) — one VM, one place.
# See docs/runtime/pico-e32-fake08-port.md (in the pico-e32 repo).

# fake-08's PICO-8 runtime (C++), minus the desktop entry point — the app provides app_main.
file(GLOB FAKE08_SRCS "${CMAKE_CURRENT_LIST_DIR}/source/*.cpp")
list(FILTER FAKE08_SRCS EXCLUDE REGEX "/main\\.cpp$")

idf_component_register(
    SRCS ${FAKE08_SRCS}
         "${CMAKE_CURRENT_LIST_DIR}/platform/esp32/ESP32Host.cpp"
         "${CMAKE_CURRENT_LIST_DIR}/libs/lodepng/lodepng.cpp"
         "${CMAKE_CURRENT_LIST_DIR}/libs/miniz/miniz.c"
         "${CMAKE_CURRENT_LIST_DIR}/libs/simpleini/ConvertUTF.c"
    INCLUDE_DIRS "source" "platform/esp32" "libs/lodepng" "libs/miniz" "libs/simpleini"
    REQUIRES z8lua esp_timer
)

# Upstream third-party code: silence its warnings so a -Werror app build still passes.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# fake-08 is a C++17 codebase (its math helpers declare a global `lerp`, which collides with C++20's
# std::lerp under IDF's default newer standard; `using namespace std` in graphics.cpp surfaces the clash).
# Compile it at gnu++17 — its intended standard — so source/ stays byte-identical. (Last -std wins.)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# lodepng disk I/O stays ENABLED: fake-08's cart.cpp:310 (loadCartFromPng(filename)) references lodepng's
# file overload unconditionally, so LODEPNG_NO_COMPILE_DISK would break the byte-identical source. fopen/
# fread come from newlib; the file path is simply never taken here (we load carts from memory via LoadCart).

# The board's board_lcd_* symbols (board_lcd_blit/fill/rgb565) are defined in the app's board.cpp and
# resolved at the final app link — the ESP32 Host declares them via platform/esp32/fake08_board.h.
