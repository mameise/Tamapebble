#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define FPS 20
#define FPS_DELAY 1000/FPS //ms
#define STEP_DELAY 1 //ms
// Moderate reduction from Stefan's 600. CPU load itself isn't the main
// crash cause (crashes happen even with very low load) — the issue is
// CPU activity colliding with the backlight ramp-up on Pebble Time 2.
// That's handled separately via the tap-aware throttling in milli_tick.
#define STEPS_PER_DELAY 400

#define VRAM_SIZE (64 + 13)
#define BYTES_PER_LINE 32
#define BITS_PER_BYTE 8

//#undef PBL_COLOR // only used for testing B&W

#include <pebble.h>
#include "tamalib/tamalib.h"
#include "tama_rtc_sync.h"
//#include "rom.h" 

static void initTamalib(void); 
static void saveCurrentStateAndQuit();
static void autosave_timer_callback(void *data);
static void rtc_sync_timer_callback(void *data);
static bool persistSaveState(void);
static bool persistLoadState(void);
static bool loadRomFromResource(void);
static void loadSettingsFromPersist(void);
static void autosave_timer_callback(void *data);
static void update_clock_text(void);
static void battery_handler(BatteryChargeState state);

static Window *s_main_window;
static BitmapLayer *s_background_layer;
static Layer *s_tama_bg_layer = NULL;  // white background behind Tama + icons (Emery)
static Layer *s_bg_custom_layer = NULL;  // custom drawn background (Emery only)
static Layer *s_screen_layer;
static Layer *s_icons_layer;
static Layer *s_hands_layer = NULL;  // analog clock hands (Emery)
static TextLayer *s_text_layer;
static TextLayer *s_battery_layer = NULL;
static char s_battery_text[8] = "";
static TextLayer *s_time_layer = NULL;
static char s_time_text[8] = "";
static TextLayer *s_date_layer = NULL;
static char s_date_text[16] = "";

// Shadow text layers used to fake an outline effect (4 offsets per text).
// Only created on Emery — null on other platforms.
static TextLayer *s_time_shadow[4]    = {NULL, NULL, NULL, NULL};
static TextLayer *s_battery_shadow[4] = {NULL, NULL, NULL, NULL};
static TextLayer *s_date_shadow[4]    = {NULL, NULL, NULL, NULL};
//static GFont s_lcd_font;

// Bitmaps
static GBitmap *s_bitmap_bg;
static GBitmap *s_bitmap_icon1;
static GBitmap *s_bitmap_icon2;
static GBitmap *s_bitmap_icon3;
static GBitmap *s_bitmap_icon4;
static GBitmap *s_bitmap_icon5;
static GBitmap *s_bitmap_icon6;
static GBitmap *s_bitmap_icon7;
static GBitmap *s_bitmap_icon8;

//static const uint8_t LCD_WIDTH  = 32;
//static const uint8_t LCD_HEIGHT = 16;

static int8_t s_selectedIcon = -1; // -1 is none, 0-6 says what icon
static bool s_showingAttentionIcon = false;
static bool s_js_ready;
static bool s_pixelsChanged = false;

static bool_t s_screen_buffer[LCD_HEIGHT][LCD_WIDTH] = {{0}};
// 8192 instead of 6144 — the Tama CPU's pc register is 13 bits (range 0..8191),
// and tamalib's cpu_step does `op = g_program[pc]` without bounds checking.
// If pc ever lands in the unused range (6144..8191), we'd be reading
// out-of-bounds memory and triggering kernel-level crashes. Pad the buffer
// to cover the full pc range; the extra entries are filled with zero, which
// the CPU interprets as a no-op-ish instruction. Costs 4KB extra static RAM.
static u12_t g_program[8192] = {0};
static bool s_hasReceivedRom = false;
static bool s_hasReceivedSaveFile = false;
static bool s_loadedFromPersist = false;  // true if we already loaded state from local watch storage
static bool s_clearTextLayerOnScreenRefresh = false;
static flat_state_t stateToLoad = {0};

//ticks
static AppTimer *milli_tick_handler;
static AppTimer *screen_tick_handler;

// Local boot from app resource + persist storage.
// This default can be overridden at runtime via settings.
#define RESOURCE_BOOT_ENABLED 1

// Persistent settings keys (separate from save-state keys)
#define PERSIST_KEY_USE_EMBEDDED_ROM  200
#define PERSIST_KEY_VIBRATION_ENABLED 201
#define PERSIST_KEY_TEXT_COLOR        204
#define PERSIST_KEY_TEXT_OUTLINE      205
#define PERSIST_KEY_TEXT_OUTLINE_COLOR 206
#define PERSIST_KEY_HANDS_COLOR       207
#define PERSIST_KEY_HANDS_OUTLINE_COLOR 208
#define PERSIST_KEY_HANDS_THICKNESS   209
#define PERSIST_KEY_BG_USE_CUSTOM     213
#define PERSIST_KEY_BG_FILL_COLOR     210
#define PERSIST_KEY_BG_MARKERS_COLOR  211
#define PERSIST_KEY_BG_MARKERS_STYLE  212
#define PERSIST_KEY_ICONS_SMALL       214
#define PERSIST_KEY_TAMA_INVERT       215
#define PERSIST_KEY_TAMA_BG_COLOR     216
#define PERSIST_KEY_TAMA_BG_ENABLED   217

// Crash / lifecycle diagnostics keys
#define PERSIST_KEY_LAST_LAUNCH_TS    300  // time_t of last init()
#define PERSIST_KEY_LAST_HEARTBEAT_TS 301  // time_t updated every minute while running
#define PERSIST_KEY_LAST_LAUNCH_REASON 302 // AppLaunchReason of last init
#define PERSIST_KEY_CRASH_COUNT       303  // counter of detected crashes
#define PERSIST_KEY_WAKEUP_ID         304  // WakeupId of next-scheduled self-relaunch
#define PERSIST_KEY_LAST_ACTIVITY     305  // most recent "what was the app doing" marker
#define PERSIST_KEY_PREV_ACTIVITY     306  // the marker BEFORE the current one

// Activity markers — small ints indicating what the app was last doing.
// Persisted regularly; on next start we can read this to see what was
// running when the crash happened.
typedef enum {
  ACT_NONE             = 0,
  ACT_TAMALIB_STEP     = 1,
  ACT_SCREEN_UPDATE    = 2,
  ACT_AUTOSAVE         = 3,
  ACT_RTC_SYNC         = 4,
  ACT_INBOX_ROM        = 5,
  ACT_INBOX_STATE      = 6,
  ACT_INBOX_SETTINGS   = 7,
  ACT_CLOCK_UPDATE     = 8,
  ACT_HANDS_REDRAW     = 9,
  // Finer-grained markers for individual render paths so we can pinpoint
  // exactly which layer's update_proc was running when a crash happens.
  ACT_DRAW_SCREEN_PROC = 10,
  ACT_DRAW_HANDS_PROC  = 11,
  ACT_DRAW_TAMA_BG     = 12,
  ACT_DRAW_ICONS       = 13,
} ActivityMarker;

static const char* activity_name(int a) {
  switch (a) {
    case ACT_TAMALIB_STEP:     return "TAMALIB_STEP";
    case ACT_SCREEN_UPDATE:    return "SCREEN_UPDATE";
    case ACT_AUTOSAVE:         return "AUTOSAVE";
    case ACT_RTC_SYNC:         return "RTC_SYNC";
    case ACT_INBOX_ROM:        return "INBOX_ROM";
    case ACT_INBOX_STATE:      return "INBOX_STATE";
    case ACT_INBOX_SETTINGS:   return "INBOX_SETTINGS";
    case ACT_CLOCK_UPDATE:     return "CLOCK_UPDATE";
    case ACT_HANDS_REDRAW:     return "HANDS_REDRAW";
    case ACT_DRAW_SCREEN_PROC: return "DRAW_SCREEN_PROC";
    case ACT_DRAW_HANDS_PROC:  return "DRAW_HANDS_PROC";
    case ACT_DRAW_TAMA_BG:     return "DRAW_TAMA_BG";
    case ACT_DRAW_ICONS:       return "DRAW_ICONS";
    default:                   return "NONE";
  }
}

// Set activity marker. To balance flash wear vs diagnostic precision:
//   - "hot loop" activities (TAMALIB_STEP, SCREEN_UPDATE) cycle very fast
//     (multiple times per second). We track them in-memory and only flush
//     once per ~10s.
//   - "rare" activities (AUTOSAVE, RTC_SYNC, INBOX_*) are persisted
//     immediately so we capture them precisely if a crash happens during one.
// Set activity marker. We track the *most recent* activity in memory
// (no flash write each frame), and periodically flush it to persist storage
// every 10 seconds. On crash, the most recent flushed value tells us roughly
// what the app was doing — accurate to within ~10s, which is enough to
// pinpoint the problematic code path.
static void set_activity(ActivityMarker act) {
  static int s_current_activity = -1;
  static int s_last_persisted_activity = -1;
  static time_t s_last_persist_write = 0;

  if (act == s_current_activity) return;
  s_current_activity = act;

  time_t now = time(NULL);
  // Persist on every change, but throttle to once per 10s for flash wear
  if (now - s_last_persist_write >= 10 && act != s_last_persisted_activity) {
    // Track previous for diagnostics
    if (s_last_persisted_activity >= 0) {
      persist_write_int(PERSIST_KEY_PREV_ACTIVITY, s_last_persisted_activity);
    }
    persist_write_int(PERSIST_KEY_LAST_ACTIVITY, (int)act);
    s_last_persisted_activity = act;
    s_last_persist_write = now;
  }
}
#define PERSIST_KEY_SOUND_ENABLED     202
#define PERSIST_KEY_SOUND_VOLUME      203

// Runtime settings — loaded from persist on startup, updated from Clay config.
static bool s_use_embedded_rom  = true;
static bool s_vibration_enabled = true;

// Customization settings. Colors stored as Pebble's argb8 packed format
// (1 byte). Defaults initialized to white text / black outline so even if
// loadSettingsFromPersist hasn't run yet, the first render is sensible.
static uint8_t s_text_color_argb         = 0xFF;  // GColorWhiteARGB8
static uint8_t s_text_outline_color_argb = 0xC0;  // GColorBlackARGB8
static bool    s_text_outline_enabled    = true;
static uint8_t s_hands_color_argb        = 0xFF;
static uint8_t s_hands_outline_color_argb = 0xC0;
static uint8_t s_hands_thickness         = 1;  // 0=thin, 1=normal, 2=thick

// Background customization (Emery only). When s_bg_use_custom is true,
// we draw the watchface background and hour markers in code instead of
// using the bgEmery.png resource — this lets the user pick the
// background fill color, the marker color, and the marker style.
static bool    s_bg_use_custom           = false;  // false = use PNG, true = code-drawn
static uint8_t s_bg_fill_color_argb      = 0xC0;  // black background
static uint8_t s_bg_markers_color_argb   = 0xFF;  // white markers
static uint8_t s_bg_markers_style        = 0;     // 0=Arabic, 1=Roman, 2=Ticks

// Menu icon size (Emery only). false = default 27x22, true = compact 18x15.
// Picking compact frees up vertical space around the Tama LCD, useful if
// the user wants a tighter look.
static bool    s_icons_small             = false;

// Tama-display customization. Three toggles/colors that affect how the
// Tama LCD area is drawn:
//   s_tama_invert     — when true, draw Tama pixels in the BG color and the
//                       background in pixel color (inverted look). Also
//                       inverts menu icons via GCompOpAssignInverted.
//   s_tama_bg_color   — the color of the rounded background behind the
//                       Tama LCD + icons. Default white.
//   s_tama_bg_enabled — when false, don't draw the Tama background layer
//                       at all (so the watchface background shows through).
static bool    s_tama_invert             = false;
static uint8_t s_tama_bg_color_argb      = 0xFF;  // default white
static bool    s_tama_bg_enabled         = true;
static bool s_sound_enabled     = false;  // OFF by default — opt-in feature
static uint8_t s_sound_volume   = 60;

// Tama buzzer state — Stefan Bauwens' approach from the dev branch.
// Just store the frequency on set_frequency, and call speaker_play_tone()
// directly from play_frequency. Stefan confirmed this is what he ships.
static uint16_t s_speakerFreq = 0;

// Auto-save: every N minutes, persist state to local watch storage.
// Uses persist_write_data() — no AppMessage, no phone roundtrip, no xhr.
// Much more robust than the AppMessage-based save+quit flow.
#define AUTOSAVE_ENABLED 1
#define AUTOSAVE_INTERVAL_MS (5 * 60 * 1000)
static AppTimer *s_autosave_timer = NULL;

// Persist storage layout — we split flat_state_t across 3 keys because
// each persist key is limited to 256 bytes. We also write a magic number
// + version so we can detect corrupt/old data.
#define PERSIST_KEY_MAGIC         100
#define PERSIST_KEY_STATE_HEADER  101  // registers, timers, interrupts
#define PERSIST_KEY_STATE_MEM1    102  // first half of memory[]
#define PERSIST_KEY_STATE_MEM2    103  // second half of memory[]
#define PERSIST_KEY_ICONS         104  // selected_icon + showing_attention_icon

// Bumped from 0x54414D41 ('TAMA') when we upgraded to the newer upstream
// tamalib (more fields in flat_state_t — old saves are not migrate-able).
// Any old persisted state will be ignored on first start with this build.
#define PERSIST_MAGIC_VALUE       0x54414D42  // 'TAMB' = 'TAMA' v2
#define PERSIST_VERSION           1

static void Quit()
{
  window_stack_pop_all(false);
}

static void Message(const char * text) // Write message to screen
{
    layer_set_hidden((Layer *)s_screen_layer, true); // hide screen layer so we can read text
    text_layer_set_text(s_text_layer, text);
}

/*****************************/
/*   START HAL T FUNCTIONS   */
/*****************************/
static void * hal_malloc(u32_t size) { return 0; } // unused
static void hal_free(void *ptr) { } // unused

static void hal_halt(void)
{
	//Not yet implemented
}

static bool_t hal_is_log_enabled(log_level_t level) { return false; } // unused
static void hal_log(log_level_t level, char *buff, ...) { } // unused

static timestamp_t hal_get_timestamp(void)
{
  time_t seconds;
  uint16_t milliseconds;
  time_ms(&seconds, &milliseconds);

  //return microseconds
  return (int)seconds * 1000000 + (int)milliseconds * 1000; 
}

static void hal_sleep_until(timestamp_t ts) //this makes the time be accurate
{
  while((int) (ts - hal_get_timestamp()) > 0);
}

static void hal_update_screen(void) //since we're not using tamalib_mainloop we must call this ourselves
{
  layer_mark_dirty(s_screen_layer); //Tell the system to redraw screen
}

static void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val)
{
  // Defensive bounds check — a stray value from tamalib could otherwise
  // corrupt memory and trigger a kernel-level crash (full watch reboot).
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
  s_screen_buffer[y][x] = val;
  s_pixelsChanged = true;
}

static void hal_set_lcd_icon(u8_t icon, bool_t val)
{
  if (icon == 7)
  {
    s_showingAttentionIcon = val;
  }
  else
  {
    if (!val && s_selectedIcon == icon)
    {
      s_selectedIcon = -1;
    }
    else if (val)
    {
      s_selectedIcon = icon;
    }
  }
  layer_mark_dirty(s_icons_layer);
  if (s_tama_bg_layer) layer_mark_dirty(s_tama_bg_layer);
}

// Vibration on Tama buzzer activity. Goal: exactly one vibration when
// the tama starts asking for attention. The cooldown is anchored to the
// rising edge of the attention icon — not to the buzzer or last vibration.
// So while the user is dismissing the request through menus (icon still on,
// buzzer toggling), we don't keep vibrating.
//
// Lifecycle:
//   attention icon off->on: arm vibration, ready to fire on next buzzer-on
//   buzzer off->on while armed: vibrate, disarm
//   attention icon on->off: reset (next icon-on will arm again)
static bool s_buzzer_on = false;          // current buzzer state
static bool s_prev_attention = false;     // previous attention-icon state
static bool s_vibe_armed = false;         // ready to vibrate on next buzzer

static void hal_set_frequency(u32_t freq)
{
  s_speakerFreq = freq;
}

static void hal_play_frequency(bool_t en)
{
  // --- Sound: exact same approach as Stefan Bauwens' dev branch
  if (en)
  {
#if defined(PBL_SPEAKER)
    if (s_sound_enabled) {
      speaker_stop();
      speaker_play_tone(s_speakerFreq / 10, 5000, 100, SpeakerWaveformSquare);
    }
#endif
  }
  else
  {
#if defined(PBL_SPEAKER)
    if (s_sound_enabled) {
      speaker_stop();
    }
#endif
  }

  // --- Vibration: keep our attention-icon-anchored logic
  if (s_showingAttentionIcon && !s_prev_attention) {
    s_vibe_armed = true;
  }
  if (!s_showingAttentionIcon && s_prev_attention) {
    s_vibe_armed = false;
  }
  s_prev_attention = s_showingAttentionIcon;

  if (en && !s_buzzer_on && s_vibe_armed && s_vibration_enabled) {
    vibes_long_pulse();
    s_vibe_armed = false;
  }

  s_buzzer_on = en;
}

static int hal_handler(void)
{
  return 0;
  //Not implemented as we're not using the tamalib_mainloop()
}

static hal_t hal = {
	.malloc = &hal_malloc,
	.free = &hal_free,
	.halt = &hal_halt,
	.is_log_enabled = &hal_is_log_enabled,
	.log = &hal_log,
	.sleep_until = &hal_sleep_until,
	.get_timestamp = &hal_get_timestamp,
	.update_screen = &hal_update_screen,
	.set_lcd_matrix = &hal_set_lcd_matrix,
	.set_lcd_icon = &hal_set_lcd_icon,
	.set_frequency = &hal_set_frequency,
	.play_frequency = &hal_play_frequency,
	.handler = &hal_handler,
};

/***************************/
/*   END HAL T FUNCTIONS   */
/***************************/

void set_screen_to_last_state(uint8_t *fullRam) { // gets screen data from memory and sets it to the screen
    uint8_t vram[VRAM_SIZE];

    memcpy(vram, fullRam + 320, VRAM_SIZE);

    uint8_t adjustedVram[VRAM_SIZE];
    int idx = 0;

    // Helper macro to copy forward
    #define COPY_RANGE(start, end) \
        for (int i = (start); i < (end); i++) { \
            adjustedVram[idx++] = vram[i]; \
        }

    // Helper macro to copy reversed
    #define COPY_RANGE_REVERSE(start, end) \
        for (int i = (end) - 1; i >= (start); i--) { \
            adjustedVram[idx++] = vram[i]; \
        }

    // Replicating your JS slices
    COPY_RANGE(0, 8);
    COPY_RANGE(9, 17);
    COPY_RANGE_REVERSE(29, 37);
    COPY_RANGE_REVERSE(20, 28);
    COPY_RANGE(40, 48);
    COPY_RANGE(49, 57);
    COPY_RANGE_REVERSE(69, 77);
    COPY_RANGE_REVERSE(60, 68);

    int totalBytes = idx;

    for (int i = 0; i < totalBytes; i++) {
        uint8_t byte = adjustedVram[i];

        int x = (i % BYTES_PER_LINE);
        int baseY = (i / BYTES_PER_LINE) * BITS_PER_BYTE;

        for (int bitIndex = 0; bitIndex < BITS_PER_BYTE; bitIndex++) {
          int bit = (byte >> bitIndex) & 1;  
          //int bit = (byte >> (7 - bitIndex)) & 1;

            int y = baseY + bitIndex;

            hal.set_lcd_matrix(x, y, bit);
        }
    }

    #undef COPY_RANGE
    #undef COPY_RANGE_REVERSE
}

static void milli_tick() //runs once every ms.
{
  if (s_hasReceivedRom && s_hasReceivedSaveFile)
  {
    set_activity(ACT_TAMALIB_STEP);
    for (size_t i = 0; i < STEPS_PER_DELAY; i++)
    {
        tamalib_step();
    } 
  } 
  milli_tick_handler = app_timer_register(STEP_DELAY, milli_tick, NULL); // calls itself in 1ms
}

static void screen_tick() // runs every 33 ms for about 30fps
{
  if (s_hasReceivedRom && s_hasReceivedSaveFile)
  {
    set_activity(ACT_SCREEN_UPDATE);
    hal_update_screen();
  }

  if (s_clearTextLayerOnScreenRefresh)
  {
    s_clearTextLayerOnScreenRefresh = false;
    Message(" ");
    layer_set_hidden((Layer *)s_screen_layer, false); // unhide screen layer
  }

  screen_tick_handler = app_timer_register(FPS_DELAY, screen_tick, NULL);
}

// Button presses
static void on_button_up_press(ClickRecognizerRef recognizer, void *context) //1
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_LEFT, BTN_STATE_PRESSED);
}
static void on_button_select_press(ClickRecognizerRef recognizer, void *context) //2
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
}
static void on_button_down_press(ClickRecognizerRef recognizer, void *context) //3
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
}


// Button releases
static void on_button_up_release(ClickRecognizerRef recognizer, void *context) //4
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_LEFT, BTN_STATE_RELEASED);
}
static void on_button_select_release(ClickRecognizerRef recognizer, void *context) //5
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
}
static void on_button_down_release(ClickRecognizerRef recognizer, void *context) //6
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
}

static void on_button_back(ClickRecognizerRef recognizer, void *context) //back
{
  // saveCurrentStateAndQuit() now persists locally and quits — no phone
  // roundtrip needed, works even with Bluetooth off.
  saveCurrentStateAndQuit();
}

static void click_config_provider(void *context) {
  // subscribe to button presses here
  window_raw_click_subscribe(BUTTON_ID_UP, on_button_up_press, on_button_up_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, on_button_select_press, on_button_select_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, on_button_down_press, on_button_down_release, NULL);
  
  window_single_click_subscribe(BUTTON_ID_BACK, on_button_back);
}

// Handles drawing icons layers
static void icons_update_proc(Layer *layer, GContext *ctx) {
  set_activity(ACT_DRAW_ICONS);
  // Set the draw color
  graphics_context_set_fill_color(ctx, GColorBlack);

  // GCompOpSet preserves icon transparency. Inversion (if enabled) is
  // handled at bitmap-load time via invert_bitmap_in_place(), not here —
  // GCompOpAssignInverted doesn't behave correctly on color Pebbles
  // because it also inverts the alpha channel.
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  if(s_selectedIcon >= 0)
  {
    #if defined(PBL_PLATFORM_EMERY)
    // Emery layout: 2 rows of 4 icons, centered on screen.
    // Default (27x22): start x=41, step 30 (=27+3 gap), rows at y=118 and y=176.
    // Compact (18x15): start x=58, step 22 (=18+4 gap), rows at y=121 and y=185
    // — slightly tighter, with rows pulled a bit closer to the Tama LCD.
    uint8_t icon_w   = s_icons_small ? 18 : 27;
    uint8_t icon_h   = s_icons_small ? 15 : 22;
    uint8_t step     = s_icons_small ? 22 : 30;
    uint8_t start_x  = s_icons_small ? 58 : 41;
    uint8_t y_top    = s_icons_small ? 125 : 118;
    uint8_t y_bottom = s_icons_small ? 176 : 176;
    uint8_t xPos = start_x + ((s_selectedIcon % 4) * step);
    uint8_t yPos = (s_selectedIcon > 3 ? y_bottom : y_top);
    #elif defined(PBL_PLATFORM_GABBRO)
    uint8_t xPos = 12 + ((s_selectedIcon%4) * 40); 
    uint8_t yPos = (s_selectedIcon > 3 ? 120 : 0);
    uint8_t icon_w = 27, icon_h = 22;
    #else
    uint8_t xPos = 12 + ((s_selectedIcon%4) * 32);
    uint8_t yPos = (s_selectedIcon > 3 ? 100 : 0);
    uint8_t icon_w = 22, icon_h = 18;
    #endif

    GBitmap* selected_icon = s_bitmap_icon7;
    switch(s_selectedIcon)
    {
      case 0:
        selected_icon = s_bitmap_icon1;
        break;
      case 1:
        selected_icon = s_bitmap_icon2;
        break;
      case 2:
        selected_icon = s_bitmap_icon3;
        break;
      case 3:
        selected_icon = s_bitmap_icon4;
        break;
      case 4:
        selected_icon = s_bitmap_icon5;
        break;
      case 5:
        selected_icon = s_bitmap_icon6;
        break;
      case 6:
        selected_icon = s_bitmap_icon7;
        break;
      default:
        selected_icon = s_bitmap_icon7;
        break;
    }

    // Draw selected icon if selected
    graphics_draw_bitmap_in_rect(ctx, selected_icon, GRect(xPos, yPos, icon_w, icon_h));
  }

  // Handle attention icon
  if(s_showingAttentionIcon)
  {
    #if defined(PBL_PLATFORM_EMERY)
    // Attention icon at rightmost position in the bottom row
    uint8_t icon_w_a   = s_icons_small ? 18 : 27;
    uint8_t icon_h_a   = s_icons_small ? 15 : 22;
    uint8_t step_a     = s_icons_small ? 22 : 30;
    uint8_t start_x_a  = s_icons_small ? 58 : 41;
    uint8_t y_bottom_a = s_icons_small ? 176 : 176;
    graphics_draw_bitmap_in_rect(ctx, s_bitmap_icon8,
      GRect(start_x_a + 3*step_a, y_bottom_a, icon_w_a, icon_h_a));
    #elif defined(PBL_PLATFORM_GABBRO)
    graphics_draw_bitmap_in_rect(ctx, s_bitmap_icon8, GRect(12+(40*3), 120, 27, 22)); 
    #else
    graphics_draw_bitmap_in_rect(ctx, s_bitmap_icon8, GRect(108, 100, 22, 18));
    #endif
  }
}

// White background behind Tama LCD + menu icons (Emery only).
// Dynamic: small frame around just the Tama when no icons are shown,
// extends in height AND width to cover menu icon rows when icons are active.
static void tama_bg_update_proc(Layer *layer, GContext *ctx)
{
#if defined(PBL_PLATFORM_EMERY)
  set_activity(ACT_DRAW_TAMA_BG);
  // The layer covers a 130x96 region at absolute (35, 110). We dynamically
  // size the white background rectangle inside it depending on which icon
  // rows are active and on the user's icon-size setting.
  //
  // Default icons (27x22):
  //   Top icons at absolute y=118..140  -> local y=8..30
  //   Tama LCD at absolute y=142..174   -> local y=32..64
  //   Bot icons at absolute y=176..198  -> local y=66..88
  //   Icons span absolute x=41..158     -> local x=6..123
  //
  // Compact icons (18x15):
  //   Top icons at absolute y=125..140  -> local y=15..30
  //   Tama LCD at absolute y=142..174   -> local y=32..64
  //   Bot icons at absolute y=176..191  -> local y=66..81
  //   Icons span absolute x=58..142     -> local x=23..107
  //
  // Top and bottom gaps to the Tama LCD are 2px in both icon-size modes,
  // matching the original (default) layout — keeps everything tight.

  bool icon_top_active    = (s_selectedIcon >= 0 && s_selectedIcon <= 3);
  bool icon_bottom_active = (s_selectedIcon >= 4 && s_selectedIcon <= 7);
  bool attention_active   = s_showingAttentionIcon;
  bool need_top    = icon_top_active;
  bool need_bottom = icon_bottom_active || attention_active;
  bool need_wide   = need_top || need_bottom;

  int top, bottom, left, right;

  if (s_icons_small) {
    top    = need_top    ? 11 : 28;
    bottom = need_bottom ? 85 : 68;
    if (need_wide) {
      // Cover compact icon row (local x=23..107) with a small margin
      left  = 20;
      right = 111;
    } else {
      // Just around the Tama (local x=33..97), with a 4px margin
      left  = 29;
      right = 101;
    }
  } else {
    top    = need_top    ? 4  : 28;
    bottom = need_bottom ? 92 : 68;
    if (need_wide) {
      // Cover default icon row (local x=6..123) with a small margin
      left  = 3;
      right = 127;
    } else {
      left  = 29;
      right = 101;
    }
  }

  GRect rect = GRect(left, top, right - left, bottom - top);

  // Respect the user setting: skip drawing entirely when disabled.
  // The user's chosen BG color is always used here, independently of the
  // invert setting — invert only flips the Tama pixels + icons, not the
  // background.
  if (!s_tama_bg_enabled) return;

  GColor fill = (GColor){.argb = s_tama_bg_color_argb};
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, rect, 6, GCornersAll);
#endif
}

#if defined(PBL_PLATFORM_EMERY)
// Roman numeral strings for the 12 hour markers.
static const char* const ROMAN_NUMERALS[12] = {
  "XII", "I", "II", "III", "IV", "V",
  "VI", "VII", "VIII", "IX", "X", "XI",
};
static const char* const ARABIC_NUMERALS[12] = {
  "12", "1", "2", "3", "4", "5",
  "6", "7", "8", "9", "10", "11",
};

// Hour-marker positions on the rectangular Emery face (200x228).
// We don't put them on a circle — the central area is needed for the
// digital time, battery, date, Tama LCD and analog hands. Instead, markers
// sit along the four edges (top, right, bottom, left), mirroring the
// original bgEmery.png layout. Each entry is the CENTER point of the
// marker; the renderer draws a 36x22 text box centered on it.
typedef struct {
  int16_t x;
  int16_t y;
} marker_pos_t;
static const marker_pos_t MARKER_POSITIONS[12] = {
  /*  0 (12) */ { 100,  10 },
  /*  1 ( 1) */ { 158,  10 },
  /*  2 ( 2) */ { 184,  62 },
  /*  3 ( 3) */ { 184, 114 },
  /*  4 ( 4) */ { 184, 166 },
  /*  5 ( 5) */ { 158, 215 },
  /*  6 ( 6) */ { 100, 215 },
  /*  7 ( 7) */ {  42, 215 },
  /*  8 ( 8) */ {  16, 166 },
  /*  9 ( 9) */ {  16, 114 },
  /* 10 (10) */ {  16,  62 },
  /* 11 (11) */ {  42,  10 },
};

#define BG_TICK_LENGTH      10  // length of a tick mark (px)
#endif

// Custom watchface background (Emery only). Fills the whole layer with
// the user-chosen background color, then draws 12 hour markers along the
// edges. Markers can be Arabic, Roman, or tick-marks depending on the
// user setting.
static void bg_custom_update_proc(Layer *layer, GContext *ctx)
{
#if defined(PBL_PLATFORM_EMERY)
  GRect bounds = layer_get_bounds(layer);

  GColor fill    = (GColor){.argb = s_bg_fill_color_argb};
  GColor markers = (GColor){.argb = s_bg_markers_color_argb};

  // Fill background
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GFont marker_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  for (int i = 0; i < 12; i++) {
    int16_t mx = MARKER_POSITIONS[i].x;
    int16_t my = MARKER_POSITIONS[i].y;

    if (s_bg_markers_style == 2) {
      // Tick marks: draw a short line at the marker position, oriented
      // toward the nearest edge. Thicker for 12/3/6/9, thinner otherwise.
      int width = (i % 3 == 0) ? 4 : 2;
      graphics_context_set_stroke_color(ctx, markers);
      graphics_context_set_stroke_width(ctx, width);

      GPoint p1, p2;
      // Determine orientation by which edge this marker sits on
      if (i == 0 || i == 6) {
        // Top/bottom center: vertical tick
        p1 = GPoint(mx, my - BG_TICK_LENGTH / 2);
        p2 = GPoint(mx, my + BG_TICK_LENGTH / 2);
      } else if (i == 3 || i == 9) {
        // Left/right center: horizontal tick
        p1 = GPoint(mx - BG_TICK_LENGTH / 2, my);
        p2 = GPoint(mx + BG_TICK_LENGTH / 2, my);
      } else if (i == 1 || i == 11 || i == 5 || i == 7) {
        // Corner positions on top/bottom rows: vertical tick
        p1 = GPoint(mx, my - BG_TICK_LENGTH / 2);
        p2 = GPoint(mx, my + BG_TICK_LENGTH / 2);
      } else {
        // Side positions (2, 4, 8, 10): horizontal tick
        p1 = GPoint(mx - BG_TICK_LENGTH / 2, my);
        p2 = GPoint(mx + BG_TICK_LENGTH / 2, my);
      }
      graphics_draw_line(ctx, p1, p2);
    } else {
      // Text label (Arabic or Roman). Center the text box on the marker
      // point. Text frame is 32x22 (wide enough for "VIII").
      const char* label = (s_bg_markers_style == 1)
        ? ROMAN_NUMERALS[i] : ARABIC_NUMERALS[i];
      GRect text_rect = GRect(mx - 16, my - 11, 32, 22);
      graphics_context_set_text_color(ctx, markers);
      graphics_draw_text(ctx, label, marker_font, text_rect,
                         GTextOverflowModeWordWrap,
                         GTextAlignmentCenter, NULL);
    }
  }
#endif
}

// Analog clock hands (Emery only). Draws hour + minute hand rotating around
// the screen center, with a black outline so they're visible over both the
// black screen background and the white Tama-area background.
static void hands_update_proc(Layer *layer, GContext *ctx)
{
#if defined(PBL_PLATFORM_EMERY)
  set_activity(ACT_DRAW_HANDS_PROC);
  GRect bounds = layer_get_bounds(layer);
  GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;

  int hour = t->tm_hour % 12;
  int minute = t->tm_min;

  int32_t hour_angle = TRIG_MAX_ANGLE * (hour * 60 + minute) / (12 * 60);
  int32_t min_angle  = TRIG_MAX_ANGLE * minute / 60;

  int hh_len = 38;
  int mh_len = 60;

  GPoint hour_end = {
    .x = (int16_t)(sin_lookup(hour_angle) * hh_len / TRIG_MAX_RATIO) + center.x,
    .y = (int16_t)(-cos_lookup(hour_angle) * hh_len / TRIG_MAX_RATIO) + center.y,
  };

  GPoint min_end = {
    .x = (int16_t)(sin_lookup(min_angle) * mh_len / TRIG_MAX_RATIO) + center.x,
    .y = (int16_t)(-cos_lookup(min_angle) * mh_len / TRIG_MAX_RATIO) + center.y,
  };

  // Thickness presets — outline is always thickness+2 to give a visible border
  int inner_thick_h, inner_thick_m;
  switch (s_hands_thickness) {
    case 0: inner_thick_h = 3; inner_thick_m = 1; break;
    case 2: inner_thick_h = 6; inner_thick_m = 3; break;
    default: inner_thick_h = 4; inner_thick_m = 2; break;
  }
  int outline_thick_h = inner_thick_h + 2;
  int outline_thick_m = inner_thick_m + 2;

  GColor hands_color    = (GColor){.argb = s_hands_color_argb};
  GColor outline_color  = (GColor){.argb = s_hands_outline_color_argb};

  // Hour hand: outline first, then inner color
  graphics_context_set_stroke_color(ctx, outline_color);
  graphics_context_set_stroke_width(ctx, outline_thick_h);
  graphics_draw_line(ctx, center, hour_end);
  graphics_context_set_stroke_color(ctx, hands_color);
  graphics_context_set_stroke_width(ctx, inner_thick_h);
  graphics_draw_line(ctx, center, hour_end);

  // Minute hand
  graphics_context_set_stroke_color(ctx, outline_color);
  graphics_context_set_stroke_width(ctx, outline_thick_m);
  graphics_draw_line(ctx, center, min_end);
  graphics_context_set_stroke_color(ctx, hands_color);
  graphics_context_set_stroke_width(ctx, inner_thick_m);
  graphics_draw_line(ctx, center, min_end);

  // Center dot
  graphics_context_set_fill_color(ctx, outline_color);
  graphics_fill_circle(ctx, center, 5);
  graphics_context_set_fill_color(ctx, hands_color);
  graphics_fill_circle(ctx, center, 3);
#endif
}

// Handles drawing screen layer
static void screen_update_proc(Layer *layer, GContext *ctx) { 
  set_activity(ACT_DRAW_SCREEN_PROC);

  // Pixel color: black by default, white when inverted. The background
  // behind the Tama (drawn by tama_bg_update_proc) is independent — the
  // user can pick any color for it, regardless of the invert setting.
  GColor pixel_color = s_tama_invert ? GColorWhite : GColorBlack;
  graphics_context_set_fill_color(ctx, pixel_color);

  //draw pixels
  for (size_t h = 0; h < LCD_HEIGHT; h++)
  {
    for (size_t w = 0; w < LCD_WIDTH; w++)
    {
      if (s_screen_buffer[h][w])
      {
        #if defined(PBL_PLATFORM_EMERY)
        // 2x scale for Emery: 32x16 tama pixels -> 64x32 pebble pixels
        graphics_fill_rect(ctx, GRect(w * 2, h * 2, 2, 2), 0, GCornerNone);
        #elif defined(PBL_PLATFORM_GABBRO)
        graphics_fill_rect(ctx, GRect(w * 5, h * 5, 4, 4), 0, GCornerNone);
        #else
        graphics_fill_rect(ctx, GRect(w * 4, h * 4, 3, 3), 0, GCornerNone);
        #endif
      }
    }
  }
}

static void prv_inbox_received_handler(DictionaryIterator *iter, void *context) {  
  APP_LOG(APP_LOG_LEVEL_DEBUG, "inbox received");
  set_activity(ACT_INBOX_SETTINGS);

  Tuple *ready_tuple_t = dict_find(iter, MESSAGE_KEY_JSReady);

  if(ready_tuple_t && !s_js_ready) {
    // PebbleKit JS is ready! Safe to send messages
    s_js_ready = true;
    // Only show the "loading from phone" message if we don't have a ROM yet.
    // If we already booted from local resource, this would just overlay text
    // on top of a happily-running emulator.
    if (!s_hasReceivedRom) {
      Message("Loading ROM 0%");
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "JS ready but we already booted locally");
    }
  }

  Tuple *reset_tamagotchi_t = dict_find(iter, MESSAGE_KEY_reset_tamagotchi);
  if (reset_tamagotchi_t)
  {
    if (s_hasReceivedRom)
    {
      // Clear local persist storage too so we don't restore old state on next start
      persist_delete(PERSIST_KEY_MAGIC);
      persist_delete(PERSIST_KEY_STATE_HEADER);
      persist_delete(PERSIST_KEY_STATE_MEM1);
      persist_delete(PERSIST_KEY_STATE_MEM2);
      persist_delete(PERSIST_KEY_ICONS);
      s_loadedFromPersist = false;

      s_clearTextLayerOnScreenRefresh = true;
      s_hasReceivedSaveFile = false;
      initTamalib();
    }
  }

  // Settings updates from Clay
  Tuple *use_embedded_rom_t = dict_find(iter, MESSAGE_KEY_UseEmbeddedRom);
  if (use_embedded_rom_t) {
    bool v = (use_embedded_rom_t->value->int32 != 0);
    s_use_embedded_rom = v;
    persist_write_bool(PERSIST_KEY_USE_EMBEDDED_ROM, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: UseEmbeddedRom = %d (takes effect next start)", (int)v);
  }
  Tuple *vibration_enabled_t = dict_find(iter, MESSAGE_KEY_VibrationEnabled);
  if (vibration_enabled_t) {
    bool v = (vibration_enabled_t->value->int32 != 0);
    s_vibration_enabled = v;
    persist_write_bool(PERSIST_KEY_VIBRATION_ENABLED, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: VibrationEnabled = %d", (int)v);
  }
  Tuple *sound_enabled_t = dict_find(iter, MESSAGE_KEY_SoundEnabled);
  if (sound_enabled_t) {
    bool v = (sound_enabled_t->value->int32 != 0);
    s_sound_enabled = v;
    persist_write_bool(PERSIST_KEY_SOUND_ENABLED, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: SoundEnabled = %d", (int)v);
  }
  Tuple *sound_volume_t = dict_find(iter, MESSAGE_KEY_SoundVolume);
  if (sound_volume_t) {
    int v = sound_volume_t->value->int32;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    s_sound_volume = (uint8_t)v;
    persist_write_int(PERSIST_KEY_SOUND_VOLUME, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: SoundVolume = %d", v);
  }

  // ---- Customization settings ----
  bool customization_changed = false;

  Tuple *text_color_t = dict_find(iter, MESSAGE_KEY_TextColor);
  if (text_color_t) {
    s_text_color_argb = (uint8_t)text_color_t->value->int32;
    persist_write_int(PERSIST_KEY_TEXT_COLOR, s_text_color_argb);
    customization_changed = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: TextColor = 0x%02x", (int)s_text_color_argb);
  }
  Tuple *text_outline_t = dict_find(iter, MESSAGE_KEY_TextOutline);
  if (text_outline_t) {
    s_text_outline_enabled = (text_outline_t->value->int32 != 0);
    persist_write_bool(PERSIST_KEY_TEXT_OUTLINE, s_text_outline_enabled);
    customization_changed = true;
  }
  Tuple *text_outline_color_t = dict_find(iter, MESSAGE_KEY_TextOutlineColor);
  if (text_outline_color_t) {
    s_text_outline_color_argb = (uint8_t)text_outline_color_t->value->int32;
    persist_write_int(PERSIST_KEY_TEXT_OUTLINE_COLOR, s_text_outline_color_argb);
    customization_changed = true;
  }
  Tuple *hands_color_t = dict_find(iter, MESSAGE_KEY_HandsColor);
  if (hands_color_t) {
    s_hands_color_argb = (uint8_t)hands_color_t->value->int32;
    persist_write_int(PERSIST_KEY_HANDS_COLOR, s_hands_color_argb);
    customization_changed = true;
  }
  Tuple *hands_outline_color_t = dict_find(iter, MESSAGE_KEY_HandsOutlineColor);
  if (hands_outline_color_t) {
    s_hands_outline_color_argb = (uint8_t)hands_outline_color_t->value->int32;
    persist_write_int(PERSIST_KEY_HANDS_OUTLINE_COLOR, s_hands_outline_color_argb);
    customization_changed = true;
  }
  Tuple *hands_thickness_t = dict_find(iter, MESSAGE_KEY_HandsThickness);
  if (hands_thickness_t) {
    int v = hands_thickness_t->value->int32;
    if (v < 0) v = 0; else if (v > 2) v = 2;
    s_hands_thickness = (uint8_t)v;
    persist_write_int(PERSIST_KEY_HANDS_THICKNESS, v);
    customization_changed = true;
  }

  Tuple *bg_use_custom_t = dict_find(iter, MESSAGE_KEY_BgUseCustom);
  if (bg_use_custom_t) {
    bool new_val = (bg_use_custom_t->value->int32 != 0);
    if (new_val != s_bg_use_custom) {
      // Toggling between PNG and custom requires recreating layers —
      // simplest is to ask the user to relaunch the app. Persist the new
      // value so the change takes effect on next start.
      s_bg_use_custom = new_val;
      persist_write_bool(PERSIST_KEY_BG_USE_CUSTOM, s_bg_use_custom);
      APP_LOG(APP_LOG_LEVEL_INFO,
              "BgUseCustom changed to %d -- restart app to apply",
              (int)s_bg_use_custom);
    }
  }
  Tuple *bg_fill_t = dict_find(iter, MESSAGE_KEY_BgFillColor);
  if (bg_fill_t) {
    s_bg_fill_color_argb = (uint8_t)bg_fill_t->value->int32;
    persist_write_int(PERSIST_KEY_BG_FILL_COLOR, s_bg_fill_color_argb);
    customization_changed = true;
  }
  Tuple *bg_markers_t = dict_find(iter, MESSAGE_KEY_BgMarkersColor);
  if (bg_markers_t) {
    s_bg_markers_color_argb = (uint8_t)bg_markers_t->value->int32;
    persist_write_int(PERSIST_KEY_BG_MARKERS_COLOR, s_bg_markers_color_argb);
    customization_changed = true;
  }
  Tuple *bg_style_t = dict_find(iter, MESSAGE_KEY_BgMarkersStyle);
  if (bg_style_t) {
    int v = bg_style_t->value->int32;
    if (v < 0) v = 0; else if (v > 2) v = 2;
    s_bg_markers_style = (uint8_t)v;
    persist_write_int(PERSIST_KEY_BG_MARKERS_STYLE, v);
    customization_changed = true;
  }
  Tuple *icons_small_t = dict_find(iter, MESSAGE_KEY_IconsSmall);
  if (icons_small_t) {
    bool new_val = (icons_small_t->value->int32 != 0);
    if (new_val != s_icons_small) {
      // Like BgUseCustom, switching icon size requires reloading bitmap
      // resources. Persist and ask user to relaunch the app.
      s_icons_small = new_val;
      persist_write_bool(PERSIST_KEY_ICONS_SMALL, s_icons_small);
      APP_LOG(APP_LOG_LEVEL_INFO,
              "IconsSmall changed to %d -- restart app to apply",
              (int)s_icons_small);
    }
  }
  Tuple *tama_invert_t = dict_find(iter, MESSAGE_KEY_TamaInvert);
  if (tama_invert_t) {
    bool new_val = (tama_invert_t->value->int32 != 0);
    if (new_val != s_tama_invert) {
      // Inversion is baked into the icon bitmaps at load time (because
      // GCompOpAssignInverted is unreliable on color Pebbles), so changing
      // this setting needs an app restart.
      s_tama_invert = new_val;
      persist_write_bool(PERSIST_KEY_TAMA_INVERT, s_tama_invert);
      APP_LOG(APP_LOG_LEVEL_INFO,
              "TamaInvert changed to %d -- restart app to apply",
              (int)s_tama_invert);
    }
  }
  Tuple *tama_bg_color_t = dict_find(iter, MESSAGE_KEY_TamaBgColor);
  if (tama_bg_color_t) {
    s_tama_bg_color_argb = (uint8_t)tama_bg_color_t->value->int32;
    persist_write_int(PERSIST_KEY_TAMA_BG_COLOR, s_tama_bg_color_argb);
    customization_changed = true;
  }
  Tuple *tama_bg_enabled_t = dict_find(iter, MESSAGE_KEY_TamaBgEnabled);
  if (tama_bg_enabled_t) {
    s_tama_bg_enabled = (tama_bg_enabled_t->value->int32 != 0);
    persist_write_bool(PERSIST_KEY_TAMA_BG_ENABLED, s_tama_bg_enabled);
    customization_changed = true;
  }

  if (customization_changed) {
    // Re-render everything that uses these colors
    if (s_hands_layer) layer_mark_dirty(s_hands_layer);
    if (s_bg_custom_layer) layer_mark_dirty(s_bg_custom_layer);
    if (s_tama_bg_layer) layer_mark_dirty(s_tama_bg_layer);
    if (s_icons_layer) layer_mark_dirty(s_icons_layer);
    if (s_screen_layer) layer_mark_dirty(s_screen_layer);
    update_clock_text();  // re-renders time + date text layers
    battery_handler(battery_state_service_peek());
    APP_LOG(APP_LOG_LEVEL_INFO,
            "Customization updated: text=0x%02x outline=%d/0x%02x hands=0x%02x/0x%02x thick=%d bg=0x%02x markers=0x%02x style=%d tama_inv=%d tama_bg_col=0x%02x tama_bg_en=%d",
            (int)s_text_color_argb, (int)s_text_outline_enabled,
            (int)s_text_outline_color_argb,
            (int)s_hands_color_argb, (int)s_hands_outline_color_argb,
            (int)s_hands_thickness,
            (int)s_bg_fill_color_argb, (int)s_bg_markers_color_argb,
            (int)s_bg_markers_style,
            (int)s_tama_invert, (int)s_tama_bg_color_argb,
            (int)s_tama_bg_enabled);
  }

  // handle incoming rom
  Tuple *offset_t = dict_find(iter, MESSAGE_KEY_ROMOffset);
  Tuple *chunk_t = dict_find(iter, MESSAGE_KEY_ROMChunk);

  if(offset_t && chunk_t) {
    // If we already loaded the ROM locally from resource, ignore phone-sent chunks.
    // Otherwise tamalib is already running on g_program[] and we'd corrupt it.
    if (s_hasReceivedRom) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Ignoring ROM chunk (already have local ROM)");
      return;
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk received");
    int offset = offset_t->value->int16;
    uint8_t *chunk = chunk_t->value->data;
    int index = 0;

    // Convert bytes → u12_t values
    for (int i = 0; i < chunk_t->length; i += 2) {
        index = (offset + i) / 2;

        if (index >= 6144) break; // safety

        u12_t value = chunk[i] | (chunk[i + 1] << 8);

        g_program[index] = value & 0x0FFF; // ensure 12-bit

        static char progress_text[25];
        int percentage = (offset * 100)/12288;
        snprintf(progress_text, sizeof(progress_text), "Loading ROM %d%%", percentage);
        Message(progress_text);
    }
    if (index == 6143)
    {
      // we reached the end and can safely start now
      Message("Loading ROM 100%");
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Reached end of ROM!");
      s_hasReceivedRom = true;

      // Check local watch storage first — if we have a recent persist-state,
      // use it and skip waiting for JS to send one. JS will still send its
      // copy but we'll just ignore it (saveStateDict won't replace what
      // we already loaded; cpu_init_from_state was already called).
      if (persistLoadState())
      {
        APP_LOG(APP_LOG_LEVEL_INFO, "Using local persist state, skipping JS save");
        s_hasReceivedSaveFile = true;
        s_loadedFromPersist = true;
        s_clearTextLayerOnScreenRefresh = true;  // clear "Loading ROM 100%"
        initTamalib();
      }
      // else: wait for save file from js (original behavior)
    }
  }

  // Handle (error) messages
  Tuple *JSMessage_t = dict_find(iter, MESSAGE_KEY_JSMessage);
  if (JSMessage_t)
  {
    char *jsMessage = JSMessage_t->value->cstring;
    Message(jsMessage);
  }

  Tuple *JSFinishedSaving_t = dict_find(iter, MESSAGE_KEY_JSFinishedSaving);
  if (JSFinishedSaving_t)
  {
    Quit(); // we quit gracefully
  }

  // Handle incoming save state
  Tuple *STATEnone_t = dict_find(iter, MESSAGE_KEY_STATEnone);

  Tuple *STATEpc_t = dict_find(iter, MESSAGE_KEY_STATEpc);
  Tuple *STATEx_t = dict_find(iter, MESSAGE_KEY_STATEx);
  Tuple *STATEy_t = dict_find(iter, MESSAGE_KEY_STATEy);
  Tuple *STATEa_t = dict_find(iter, MESSAGE_KEY_STATEa);
  Tuple *STATEb_t = dict_find(iter, MESSAGE_KEY_STATEb);
  Tuple *STATEnp_t = dict_find(iter, MESSAGE_KEY_STATEnp);
  Tuple *STATEsp_t = dict_find(iter, MESSAGE_KEY_STATEsp);
  Tuple *STATEflags_t = dict_find(iter, MESSAGE_KEY_STATEflags);

  Tuple *STATEtick_counter_t = dict_find(iter, MESSAGE_KEY_STATEtick_counter);
  Tuple *STATEclk_timer_timestamp_t = dict_find(iter, MESSAGE_KEY_STATEclk_timer_timestamp);
  Tuple *STATEprog_timer_timestamp_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_timestamp);
  Tuple *STATEprog_timer_enabled_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_enabled);
  Tuple *STATEprog_timer_data_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_data);
  Tuple *STATEprog_timer_rld_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_rld);
  Tuple *STATEcall_depth_t = dict_find(iter, MESSAGE_KEY_STATEcall_depth);

  Tuple *STATEinterrupts_t = dict_find(iter, MESSAGE_KEY_STATEinterrupts);
  Tuple *STATEmemory_t = dict_find(iter, MESSAGE_KEY_STATEmemory);

  Tuple *STATEselected_icon_t = dict_find(iter, MESSAGE_KEY_STATEselected_icon);
  Tuple *STATEshowing_attention_icon_t = dict_find(iter, MESSAGE_KEY_STATEshowing_attention_icon);

  if(STATEnone_t)
  {
    // If we already booted from local resources, ignore the "no save" signal
    // from phone — we already started up just fine.
    if (s_hasReceivedSaveFile) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Ignoring STATEnone (already running)");
    } else {
      s_clearTextLayerOnScreenRefresh = true;
      initTamalib();
    }
  }

  if (STATEpc_t && STATEmemory_t) // assume whole block
  {
    if (s_loadedFromPersist || s_hasReceivedSaveFile) {
      // We already booted (from persist or just running fresh from resource).
      // Ignore the JS-side state to avoid overwriting good data.
      APP_LOG(APP_LOG_LEVEL_INFO, "Ignoring JS state (already running)");
      return;
    }

    Message("Loading save state...");

    uint16_t state_pc = STATEpc_t->value->uint16;
    uint16_t state_x = STATEx_t->value->uint16;
    uint16_t state_y = STATEy_t->value->uint16;
    uint8_t state_a = STATEa_t->value->uint8;
    uint8_t state_b = STATEb_t->value->uint8;
    uint8_t state_np = STATEnp_t->value->uint8;
    uint8_t state_sp = STATEsp_t->value->uint8;
    uint8_t state_flags = STATEflags_t->value->uint8;

    uint32_t state_tick_counter = STATEtick_counter_t->value->uint32;
    uint32_t state_clk_timer_timestamp = STATEclk_timer_timestamp_t->value->uint32;
    uint32_t state_prog_timer_timestamp = STATEprog_timer_timestamp_t->value->uint32;
    uint8_t state_prog_timer_enabled = STATEprog_timer_enabled_t->value->uint8;
    uint8_t state_prog_timer_data = STATEprog_timer_data_t->value->uint8;
    uint8_t state_prog_timer_rld = STATEprog_timer_rld_t->value->uint8;
    uint32_t state_call_depth = STATEcall_depth_t->value->uint32;

    uint8_t *state_interrupts = STATEinterrupts_t->value->data;
    uint8_t *state_memory = STATEmemory_t->value->data;

    stateToLoad.pc = state_pc;
    stateToLoad.x = state_x;
    stateToLoad.y = state_y;
    stateToLoad.a = state_a;
    stateToLoad.b = state_b;
    stateToLoad.np = state_np;
    stateToLoad.sp = state_sp;
    stateToLoad.flags = state_flags;

    stateToLoad.tick_counter = state_tick_counter;
    // Phone state-loading: phone only sends one clk_timer_timestamp (legacy
    // format from before the tamalib upgrade). Map it to the 2hz timer slot
    // and zero the rest — the emulator will quickly re-derive correct
    // timer phases once it starts running.
    stateToLoad.clk_timer_2hz_timestamp = state_clk_timer_timestamp;
    stateToLoad.clk_timer_4hz_timestamp = 0;
    stateToLoad.clk_timer_8hz_timestamp = 0;
    stateToLoad.clk_timer_16hz_timestamp = 0;
    stateToLoad.clk_timer_32hz_timestamp = 0;
    stateToLoad.clk_timer_64hz_timestamp = 0;
    stateToLoad.clk_timer_128hz_timestamp = 0;
    stateToLoad.clk_timer_256hz_timestamp = 0;
    stateToLoad.prog_timer_timestamp = state_prog_timer_timestamp;
    stateToLoad.prog_timer_enabled = state_prog_timer_enabled;
    stateToLoad.prog_timer_data = state_prog_timer_data;
    stateToLoad.prog_timer_rld = state_prog_timer_rld;
    stateToLoad.call_depth = state_call_depth;

    stateToLoad.interrupts[0].factor_flag_reg = state_interrupts[0];
    stateToLoad.interrupts[0].mask_reg        = state_interrupts[1];  
    stateToLoad.interrupts[0].triggered       = state_interrupts[2];  
    stateToLoad.interrupts[0].vector          = state_interrupts[3];  
    stateToLoad.interrupts[1].factor_flag_reg = state_interrupts[4];
    stateToLoad.interrupts[1].mask_reg        = state_interrupts[5];  
    stateToLoad.interrupts[1].triggered       = state_interrupts[6];  
    stateToLoad.interrupts[1].vector          = state_interrupts[7];  
    stateToLoad.interrupts[2].factor_flag_reg = state_interrupts[8];
    stateToLoad.interrupts[2].mask_reg        = state_interrupts[9];  
    stateToLoad.interrupts[2].triggered       = state_interrupts[10];  
    stateToLoad.interrupts[2].vector          = state_interrupts[11]; 
    stateToLoad.interrupts[3].factor_flag_reg = state_interrupts[12];
    stateToLoad.interrupts[3].mask_reg        = state_interrupts[13];  
    stateToLoad.interrupts[3].triggered       = state_interrupts[14];  
    stateToLoad.interrupts[3].vector          = state_interrupts[15]; 
    stateToLoad.interrupts[4].factor_flag_reg = state_interrupts[16];
    stateToLoad.interrupts[4].mask_reg        = state_interrupts[17];  
    stateToLoad.interrupts[4].triggered       = state_interrupts[18];  
    stateToLoad.interrupts[4].vector          = state_interrupts[19];         
    stateToLoad.interrupts[5].factor_flag_reg = state_interrupts[20];
    stateToLoad.interrupts[5].mask_reg        = state_interrupts[21];  
    stateToLoad.interrupts[5].triggered       = state_interrupts[22];  
    stateToLoad.interrupts[5].vector          = state_interrupts[23]; 

    memcpy(stateToLoad.memory, state_memory, sizeof(stateToLoad.memory));

    s_hasReceivedSaveFile = true;
    s_clearTextLayerOnScreenRefresh = true;

    if (STATEselected_icon_t && STATEshowing_attention_icon_t)
    {
      s_selectedIcon = STATEselected_icon_t->value->int8;
      s_showingAttentionIcon = STATEshowing_attention_icon_t->value->int8;

      layer_mark_dirty(s_icons_layer);
      if (s_tama_bg_layer) layer_mark_dirty(s_tama_bg_layer);
    }

    initTamalib();
  }
}

// Time + date indicators in the watch frame area (above and below the tama LCD).
// Updated via AppTimer every 30s — safe and doesn't reintroduce the
// tick_timer_service issue we had earlier.
static AppTimer *s_clock_timer = NULL;

// Apply current customization (text color + outline) to a primary text
// layer and its 4 shadow layers. Shadows are kept hidden when outline is
// disabled.
static void apply_text_layer_style(TextLayer *primary, TextLayer **shadows)
{
  if (!primary) return;
  GColor fill = (GColor){.argb = s_text_color_argb};
  GColor outline = (GColor){.argb = s_text_outline_color_argb};
  text_layer_set_text_color(primary, fill);
  if (shadows) {
    for (int i = 0; i < 4; i++) {
      if (!shadows[i]) continue;
      text_layer_set_text_color(shadows[i], outline);
      layer_set_hidden(text_layer_get_layer(shadows[i]), !s_text_outline_enabled);
    }
  }
}

// Mirror the text content from primary into all 4 shadow layers.
static void sync_shadow_text(TextLayer **shadows, const char *text)
{
  if (!shadows) return;
  for (int i = 0; i < 4; i++) {
    if (shadows[i]) text_layer_set_text(shadows[i], text);
  }
}

static void update_clock_text(void)
{
  if (!s_time_layer || !s_date_layer) return;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;

  // Time: HH:MM (24h)
  strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  text_layer_set_text(s_time_layer, s_time_text);
  sync_shadow_text(s_time_shadow, s_time_text);

  // Date: e.g. "Mo 21.05"
  strftime(s_date_text, sizeof(s_date_text), "%a %d.%m", t);
  text_layer_set_text(s_date_layer, s_date_text);
  sync_shadow_text(s_date_shadow, s_date_text);

  // Re-apply styling (in case colors changed via settings update)
  apply_text_layer_style(s_time_layer,    s_time_shadow);
  apply_text_layer_style(s_date_layer,    s_date_shadow);
  apply_text_layer_style(s_battery_layer, s_battery_shadow);

  // Trigger redraw of analog hands (Emery only)
  if (s_hands_layer) {
    layer_mark_dirty(s_hands_layer);
  }

  // Heartbeat: record we're still alive. Throttled to once every 5 minutes
  // to avoid flash wear from persist writes — we only need this for crash
  // diagnostics, which is OK with 5-min granularity.
  static time_t s_last_heartbeat_write = 0;
  if (now - s_last_heartbeat_write >= 5 * 60) {
    persist_write_int(PERSIST_KEY_LAST_HEARTBEAT_TS, now);
    s_last_heartbeat_write = now;
  }
}

// Schedule the next clock update at the next minute boundary, so that the
// digital time stays in sync with the analog hands (both reflect the actual
// minute, not a 30s-offset version).
static void schedule_next_clock_update(void);

static void clock_timer_callback(void *data)
{
  s_clock_timer = NULL;
  set_activity(ACT_CLOCK_UPDATE);
  update_clock_text();
  schedule_next_clock_update();
}

static void schedule_next_clock_update(void)
{
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) {
    // Fallback if localtime fails — try again in 30s
    s_clock_timer = app_timer_register(30 * 1000, clock_timer_callback, NULL);
    return;
  }
  // Milliseconds remaining until the next full minute.
  // localtime gives us seconds 0..59 of the current minute.
  int seconds_into_minute = t->tm_sec;
  uint32_t ms_until_next_minute = (60 - seconds_into_minute) * 1000;
  // Add a tiny 100ms safety buffer so we fire just AFTER the minute change,
  // not right on it (avoids edge cases where seconds == 0 means we just
  // missed the tick).
  if (ms_until_next_minute < 100) ms_until_next_minute = 100;
  s_clock_timer = app_timer_register(ms_until_next_minute, clock_timer_callback, NULL);
}

// Battery indicator: small text at bottom of screen showing percent + charging.
static void battery_handler(BatteryChargeState state)
{
  if (!s_battery_layer) return;
  snprintf(s_battery_text, sizeof(s_battery_text), "%s%d%%",
           state.is_charging ? "+" : "",
           (int)state.charge_percent);
  text_layer_set_text(s_battery_layer, s_battery_text);
  sync_shadow_text(s_battery_shadow, s_battery_text);
}

// Pixel-by-pixel invert of a GBitmap, for icons in inverted-Tama mode.
// Pebble's GCompOpAssignInverted doesn't work right for palettized
// bitmaps with alpha — it inverts the alpha channel too, leaving the
// icon as a solid block instead of just flipping black<->white. We
// handle this manually: walk every pixel, and if it's opaque, swap
// black<->white while keeping the alpha. Transparent pixels are left
// alone. Works for both 1-bit and 8-bit Pebble bitmap formats.
static void invert_bitmap_in_place(GBitmap *bmp)
{
#if defined(PBL_COLOR)
  if (!bmp) return;
  GRect bounds = gbitmap_get_bounds(bmp);
  GBitmapFormat fmt = gbitmap_get_format(bmp);
  if (fmt != GBitmapFormat8Bit && fmt != GBitmapFormat8BitCircular) {
    // Other formats (1-bit etc.) handled differently — for our PNG-derived
    // icons on Emery this branch shouldn't run. Skip safely.
    return;
  }
  uint8_t *data = gbitmap_get_data(bmp);
  uint16_t stride = gbitmap_get_bytes_per_row(bmp);
  for (int16_t y = 0; y < bounds.size.h; y++) {
    uint8_t *row = data + (y * stride);
    for (int16_t x = 0; x < bounds.size.w; x++) {
      uint8_t argb = row[x];
      uint8_t a = (argb >> 6) & 0x3;
      // Only invert visible (opaque or partially opaque) pixels.
      // Pixel format: AARRGGBB (2 bits each).
      if (a > 0) {
        uint8_t r = (argb >> 4) & 0x3;
        uint8_t g = (argb >> 2) & 0x3;
        uint8_t b = argb & 0x3;
        r = 3 - r;
        g = 3 - g;
        b = 3 - b;
        row[x] = (a << 6) | (r << 4) | (g << 2) | b;
      }
    }
  }
#endif
}

static void main_window_load(Window *window) {
  // Get information about the Window
  Layer *window_layer = window_get_root_layer(window);

#if defined(PBL_PLATFORM_EMERY)
  if (s_bg_use_custom) {
    // Custom code-drawn background — fully configurable colors/markers.
    GRect bg_bounds = GRect(0, 0, 200, 228);
    s_bg_custom_layer = layer_create(bg_bounds);
    layer_set_update_proc(s_bg_custom_layer, bg_custom_update_proc);
    layer_add_child(window_layer, s_bg_custom_layer);
    s_bitmap_bg = NULL;
    s_background_layer = NULL;
  } else {
    // Classic PNG background (bgEmery.png).
    s_bitmap_bg = gbitmap_create_with_resource(RESOURCE_ID_BG_IMAGE);
    s_background_layer = bitmap_layer_create(GRect(0, 0, 200, 228));
    bitmap_layer_set_compositing_mode(s_background_layer, GCompOpSet);
    bitmap_layer_set_bitmap(s_background_layer, s_bitmap_bg);
    layer_add_child(window_layer, bitmap_layer_get_layer(s_background_layer));
    s_bg_custom_layer = NULL;
  }
#else
  // Other platforms always use their PNG background.
  #if defined(PBL_COLOR)
  s_bitmap_bg = gbitmap_create_with_resource(RESOURCE_ID_BG_IMAGE);
  #else
  s_bitmap_bg = gbitmap_create_with_resource(RESOURCE_ID_BG_IMAGE_BW);
  #endif

  #if defined(PBL_PLATFORM_CHALK)
  s_background_layer = bitmap_layer_create(GRect(0, 0, 180, 180));
  #elif defined(PBL_PLATFORM_GABBRO)
  s_background_layer = bitmap_layer_create(GRect(0, 0, 260, 260));
  #else
  s_background_layer = bitmap_layer_create(GRect(0, 0, 144, 168));
  #endif
  bitmap_layer_set_compositing_mode(s_background_layer, GCompOpSet);
  bitmap_layer_set_bitmap(s_background_layer, s_bitmap_bg);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_background_layer));
#endif

#if defined(PBL_PLATFORM_EMERY)
  // White background rectangle behind the Tama LCD + menu icons area.
  // Covers from the top menu icons row down to bottom menu icons row.
  // Icons: y=118..140 (above), Tama: y=142..174, icons: y=176..198
  // So total area: y=114..202, with margin: y=110..206
  // Horizontal: icons stretch from x=41 to x=158 (4 icons of 27px + 3 gaps of 3px)
  // Add margin: x=35..165
  s_tama_bg_layer = layer_create(GRect(35, 110, 130, 96));
  layer_set_update_proc(s_tama_bg_layer, tama_bg_update_proc);
  layer_add_child(window_layer, s_tama_bg_layer);
#endif

  // Create bitmaps for icons. On Emery, we offer two sizes — default
  // (27x22) or compact (18x15) — selectable via the IconsSmall setting.
  // Other platforms always use the default size.
#if defined(PBL_PLATFORM_EMERY)
  if (s_icons_small) {
    s_bitmap_icon1 = gbitmap_create_with_resource(RESOURCE_ID_ICON1_SM);
    s_bitmap_icon2 = gbitmap_create_with_resource(RESOURCE_ID_ICON2_SM);
    s_bitmap_icon3 = gbitmap_create_with_resource(RESOURCE_ID_ICON3_SM);
    s_bitmap_icon4 = gbitmap_create_with_resource(RESOURCE_ID_ICON4_SM);
    s_bitmap_icon5 = gbitmap_create_with_resource(RESOURCE_ID_ICON5_SM);
    s_bitmap_icon6 = gbitmap_create_with_resource(RESOURCE_ID_ICON6_SM);
    s_bitmap_icon7 = gbitmap_create_with_resource(RESOURCE_ID_ICON7_SM);
    s_bitmap_icon8 = gbitmap_create_with_resource(RESOURCE_ID_ICON8_SM);
  } else {
    s_bitmap_icon1 = gbitmap_create_with_resource(RESOURCE_ID_ICON1);
    s_bitmap_icon2 = gbitmap_create_with_resource(RESOURCE_ID_ICON2);
    s_bitmap_icon3 = gbitmap_create_with_resource(RESOURCE_ID_ICON3);
    s_bitmap_icon4 = gbitmap_create_with_resource(RESOURCE_ID_ICON4);
    s_bitmap_icon5 = gbitmap_create_with_resource(RESOURCE_ID_ICON5);
    s_bitmap_icon6 = gbitmap_create_with_resource(RESOURCE_ID_ICON6);
    s_bitmap_icon7 = gbitmap_create_with_resource(RESOURCE_ID_ICON7);
    s_bitmap_icon8 = gbitmap_create_with_resource(RESOURCE_ID_ICON8);
  }
#else
  s_bitmap_icon1 = gbitmap_create_with_resource(RESOURCE_ID_ICON1);
  s_bitmap_icon2 = gbitmap_create_with_resource(RESOURCE_ID_ICON2);
  s_bitmap_icon3 = gbitmap_create_with_resource(RESOURCE_ID_ICON3);
  s_bitmap_icon4 = gbitmap_create_with_resource(RESOURCE_ID_ICON4);
  s_bitmap_icon5 = gbitmap_create_with_resource(RESOURCE_ID_ICON5);
  s_bitmap_icon6 = gbitmap_create_with_resource(RESOURCE_ID_ICON6);
  s_bitmap_icon7 = gbitmap_create_with_resource(RESOURCE_ID_ICON7);
  s_bitmap_icon8 = gbitmap_create_with_resource(RESOURCE_ID_ICON8);
#endif

  // If invert mode is enabled, flip the pixel colors of every icon bitmap.
  // This is more reliable than using GCompOpAssignInverted at draw time,
  // which on color Pebbles incorrectly inverts the alpha channel and makes
  // the icons unreadable.
  if (s_tama_invert) {
    invert_bitmap_in_place(s_bitmap_icon1);
    invert_bitmap_in_place(s_bitmap_icon2);
    invert_bitmap_in_place(s_bitmap_icon3);
    invert_bitmap_in_place(s_bitmap_icon4);
    invert_bitmap_in_place(s_bitmap_icon5);
    invert_bitmap_in_place(s_bitmap_icon6);
    invert_bitmap_in_place(s_bitmap_icon7);
    invert_bitmap_in_place(s_bitmap_icon8);
  }

  // Create icons layer
#if defined(PBL_PLATFORM_CHALK)
  s_icons_layer = layer_create(GRect(0+18, 24+6, 144, 146));
#elif defined(PBL_PLATFORM_GABBRO) 
  s_icons_layer = layer_create(GRect(0+45, 60, 180, 183)); 
#elif defined(PBL_PLATFORM_EMERY)
  // Icons layer covers most of the screen so we can use absolute-ish coords inside
  s_icons_layer = layer_create(GRect(0, 0, 200, 228)); 
#else
  s_icons_layer = layer_create(GRect(0, 24, 144, 146));
#endif
  layer_set_update_proc(s_icons_layer, icons_update_proc);

  // Add to window
  layer_add_child(window_layer, s_icons_layer);

  // Create screen Layer
#if defined(PBL_PLATFORM_CHALK)
  s_screen_layer = layer_create(GRect(8+18, 51+6, 128, 64));
#elif defined(PBL_PLATFORM_GABBRO)
  s_screen_layer = layer_create(GRect(50, 92, 160, 80));
#elif defined(PBL_PLATFORM_EMERY)
  // New Emery layout: tama LCD 2x scaled (64x32), centered horizontally,
  // positioned in the bottom half. Surrounded by hour markers, menu icons,
  // and analog clock hands.
  s_screen_layer = layer_create(GRect(68, 142, 64, 32));
#else
  s_screen_layer = layer_create(GRect(8, 51, 128, 64));
#endif
  layer_set_update_proc(s_screen_layer, screen_update_proc);

  // Add to window  
  layer_add_child(window_layer, s_screen_layer);

  // Font
  //s_lcd_font    = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SMALL_LCD_9));

  // Create text layer
  #if defined(PBL_PLATFORM_CHALK)
  s_text_layer = text_layer_create(GRect(6+18, 60+6, 128, 50)); 
  #elif defined(PBL_PLATFORM_GABBRO)
  s_text_layer = text_layer_create(GRect(50, 60+46, 158, 50));
  #elif defined(PBL_PLATFORM_EMERY)
  s_text_layer = text_layer_create(GRect(10, 100, 180, 30));
  #else   
  s_text_layer = text_layer_create(GRect(6, 60, 128, 50)); 
  #endif
  text_layer_set_background_color(s_text_layer, GColorClear);
  //text_layer_set_font(s_text_layer, s_lcd_font);
  Message("Waiting for phone...");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_text_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));

  // Battery (right) + Time (left) at top, Date at bottom — all on the watch frame.
  // Sizes & positions tuned per platform; weiße fette Schrift gegen dunklen Rahmen.

#if defined(PBL_PLATFORM_GABBRO)
  // Pebble Time 2: 260x260, tama LCD at y=92-172
  s_time_layer    = text_layer_create(GRect(10,   2, 130, 50));
  s_battery_layer = text_layer_create(GRect(140, 10, 115, 36));
  s_date_layer    = text_layer_create(GRect(10, 215, 240, 32));
  GFont time_font    = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
#elif defined(PBL_PLATFORM_EMERY)
  // Pebble Time 2: 200x228 with clock face layout.
  // Digital time: centered, between top hour markers (12 row) and middle.
  // Battery + Date: in the band BELOW that, ABOVE the analog hand area.
  // The y-offset (70 px) leaves ~8 px gap below the "10"/"2" markers
  // which sit at y=62.
  s_time_layer    = text_layer_create(GRect(0,   26, 200, 30));
  s_battery_layer = text_layer_create(GRect(35,  70,  70, 20));
  s_date_layer    = text_layer_create(GRect(95,  70,  85, 20));
  GFont time_font    = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#elif defined(PBL_PLATFORM_CHALK)
  // Round 180x180 — corners less useful, use top-center / bottom-center
  s_time_layer    = text_layer_create(GRect(10,   4, 80, 30));
  s_battery_layer = text_layer_create(GRect(90,   8, 80, 22));
  s_date_layer    = text_layer_create(GRect(0,  148, 180, 22));
  GFont time_font    = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#else
  // Diorite/Basalt 144x168, tama LCD at y=51-115
  s_time_layer    = text_layer_create(GRect(4,   0, 80, 28));
  s_battery_layer = text_layer_create(GRect(84,  6, 60, 22));
  s_date_layer    = text_layer_create(GRect(0, 135, 144, 22));
  GFont time_font    = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#endif

  // Common style for all three. Time gets its own (bigger) font.
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
#if defined(PBL_PLATFORM_EMERY)
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
#else
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentLeft);
#endif
  text_layer_set_font(s_time_layer, time_font);

  TextLayer *small_infos[2] = { s_battery_layer, s_date_layer };
#if defined(PBL_PLATFORM_EMERY)
  // Emery: battery LEFT, date RIGHT
  GTextAlignment small_aligns[2] = { GTextAlignmentLeft, GTextAlignmentRight };
#else
  GTextAlignment small_aligns[2] = { GTextAlignmentRight, GTextAlignmentCenter };
#endif
  for (int i = 0; i < 2; i++) {
    text_layer_set_background_color(small_infos[i], GColorClear);
    text_layer_set_text_color(small_infos[i], GColorWhite);
    text_layer_set_text_alignment(small_infos[i], small_aligns[i]);
    text_layer_set_font(small_infos[i], small_font);
  }

#if defined(PBL_PLATFORM_EMERY)
  // Outline shadow layers for Emery: 4 offsets per text (NW, NE, SW, SE).
  // We attach them FIRST so the primary text draws on top.
  static const int OFF[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}};

  TextLayer *primaries[3]      = { s_time_layer, s_battery_layer, s_date_layer };
  TextLayer **shadow_arrays[3] = { s_time_shadow, s_battery_shadow, s_date_shadow };
  GFont        fonts[3]        = { time_font, small_font, small_font };
  GRect frames[3] = {
    GRect(0,   26, 200, 30),
    GRect(35,  70,  70, 20),
    GRect(95,  70,  85, 20),
  };
  GTextAlignment aligns_em[3] = {
    GTextAlignmentCenter, GTextAlignmentLeft, GTextAlignmentRight
  };

  for (int t = 0; t < 3; t++) {
    for (int i = 0; i < 4; i++) {
      GRect r = frames[t];
      r.origin.x += OFF[i][0];
      r.origin.y += OFF[i][1];
      shadow_arrays[t][i] = text_layer_create(r);
      text_layer_set_background_color(shadow_arrays[t][i], GColorClear);
      text_layer_set_text_color(shadow_arrays[t][i], GColorBlack);
      text_layer_set_text_alignment(shadow_arrays[t][i], aligns_em[t]);
      text_layer_set_font(shadow_arrays[t][i], fonts[t]);
      layer_add_child(window_layer, text_layer_get_layer(shadow_arrays[t][i]));
    }
  }
  // primaries attached AFTER shadows so they draw on top
  for (int t = 0; t < 3; t++) {
    layer_add_child(window_layer, text_layer_get_layer(primaries[t]));
  }
#else
  // Non-Emery platforms: simple attach in original order
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));
  for (int i = 0; i < 2; i++) {
    layer_add_child(window_layer, text_layer_get_layer(small_infos[i]));
  }
#endif

  // Initial values + subscriptions
  battery_state_service_subscribe(battery_handler);
  battery_handler(battery_state_service_peek());

  update_clock_text();
  schedule_next_clock_update();

#if defined(PBL_PLATFORM_EMERY)
  // Analog clock hands — added LAST so they draw on top of everything
  // (tama LCD, icons, text). This way they're visible across the whole face.
  s_hands_layer = layer_create(GRect(0, 0, 200, 228));
  layer_set_update_proc(s_hands_layer, hands_update_proc);
  layer_add_child(window_layer, s_hands_layer);
#endif

  // Sub to ticks
  milli_tick_handler = app_timer_register(STEP_DELAY, milli_tick, NULL);
  screen_tick_handler = app_timer_register(FPS_DELAY, screen_tick, NULL);
}

static void main_window_unload(Window *window) {
  // Cancel clock update timer
  if (s_clock_timer) {
    app_timer_cancel(s_clock_timer);
    s_clock_timer = NULL;
  }

  // Unsubscribe battery service
  battery_state_service_unsubscribe();

  // Destroy shadow layers first (only created on Emery, NULL otherwise)
  for (int i = 0; i < 4; i++) {
    if (s_time_shadow[i])    { text_layer_destroy(s_time_shadow[i]);    s_time_shadow[i]    = NULL; }
    if (s_battery_shadow[i]) { text_layer_destroy(s_battery_shadow[i]); s_battery_shadow[i] = NULL; }
    if (s_date_shadow[i])    { text_layer_destroy(s_date_shadow[i]);    s_date_shadow[i]    = NULL; }
  }

  if (s_battery_layer) {
    text_layer_destroy(s_battery_layer);
    s_battery_layer = NULL;
  }
  if (s_time_layer) {
    text_layer_destroy(s_time_layer);
    s_time_layer = NULL;
  }
  if (s_date_layer) {
    text_layer_destroy(s_date_layer);
    s_date_layer = NULL;
  }
  if (s_hands_layer) {
    layer_destroy(s_hands_layer);
    s_hands_layer = NULL;
  }
  if (s_tama_bg_layer) {
    layer_destroy(s_tama_bg_layer);
    s_tama_bg_layer = NULL;
  }

  // Destroy background bitmap and its layer (only allocated on non-Emery
  // platforms — they're NULL otherwise).
  if (s_bitmap_bg) {
    gbitmap_destroy(s_bitmap_bg);
    s_bitmap_bg = NULL;
  }
  if (s_background_layer) {
    bitmap_layer_destroy(s_background_layer);
    s_background_layer = NULL;
  }
  // Destroy custom background layer (Emery only)
  if (s_bg_custom_layer) {
    layer_destroy(s_bg_custom_layer);
    s_bg_custom_layer = NULL;
  }

  // Destroy text layer
  text_layer_destroy(s_text_layer);

  // Unload font
  //fonts_unload_custom_font(s_lcd_font);

  // Destroy icon bitmaps
  gbitmap_destroy(s_bitmap_icon1);
  gbitmap_destroy(s_bitmap_icon2);
  gbitmap_destroy(s_bitmap_icon3);
  gbitmap_destroy(s_bitmap_icon4);
  gbitmap_destroy(s_bitmap_icon5);
  gbitmap_destroy(s_bitmap_icon6);
  gbitmap_destroy(s_bitmap_icon7);
  gbitmap_destroy(s_bitmap_icon8);

  // Destory icons layer
  layer_destroy(s_icons_layer);

  // Destroy screen layer
  layer_destroy(s_screen_layer);

  // Unsubscribe to ticks
  app_timer_cancel(milli_tick_handler);
  app_timer_cancel(screen_tick_handler);
}

static void initTamalib() {
  // Register HAL
  tamalib_register_hal(&hal);

  // Check for save file
  if (s_hasReceivedSaveFile) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Save state found. Loading...");

    cpu_init_from_state(g_program, &stateToLoad, NULL, 1000000);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "cpu_init_from_state done");
    set_screen_to_last_state(stateToLoad.memory); 
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Save state loaded!");
  }
  else
  {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "No save file");
    s_hasReceivedSaveFile = true;
    tamalib_init(g_program, NULL, 1000000);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "tamalib_init done");
  }

  // Initialer RTC -> Tama Sync (in beiden Pfaden, da cpu_state nun bereit ist)
  tama_rtc_initial_sync();
}

// RTC sync via AppTimer (statt tick_timer_service). Robusterer Ansatz
// weil tick_timer_service primär für Watchfaces ist und in Watchapps
// manchmal unerwartete Side-Effects hat.
#define RTC_SYNC_CHECK_INTERVAL_MS (2 * 60 * 60 * 1000)  // alle 2 Stunden
static AppTimer *s_rtc_sync_timer = NULL;

static void rtc_sync_timer_callback(void *data)
{
  s_rtc_sync_timer = NULL;
  set_activity(ACT_RTC_SYNC);

  if (s_hasReceivedRom && s_hasReceivedSaveFile) {
    tama_rtc_periodic_check();
  }

  // Reschedule
  s_rtc_sync_timer = app_timer_register(RTC_SYNC_CHECK_INTERVAL_MS, rtc_sync_timer_callback, NULL);
}

// One-shot timer callback: try local boot after init() returned and the
// Pebble event loop is running. This avoids any lifecycle weirdness from
// initializing tamalib/timers while still inside init().
static const char* launch_reason_name(AppLaunchReason r) {
  switch (r) {
    case APP_LAUNCH_SYSTEM:           return "SYSTEM";
    case APP_LAUNCH_USER:             return "USER";
    case APP_LAUNCH_PHONE:            return "PHONE";
    case APP_LAUNCH_WAKEUP:           return "WAKEUP";
    case APP_LAUNCH_WORKER:           return "WORKER";
    case APP_LAUNCH_QUICK_LAUNCH:     return "QUICK_LAUNCH";
    case APP_LAUNCH_TIMELINE_ACTION:  return "TIMELINE_ACTION";
    case APP_LAUNCH_SMARTSTRAP:       return "SMARTSTRAP";
    default:                          return "UNKNOWN";
  }
}

static void init() {
  AppLaunchReason reason = launch_reason();
  APP_LOG(APP_LOG_LEVEL_INFO, "App launched. reason=%d (%s)",
          (int)reason, launch_reason_name(reason));

  // --- Crash / lifecycle diagnostics ----------------------------------
  // Check if the previous run died unexpectedly: if last heartbeat is far in
  // the past but the previous launch wasn't a clean exit, we likely crashed.
  time_t now = time(NULL);
  if (persist_exists(PERSIST_KEY_LAST_HEARTBEAT_TS)) {
    time_t last_hb = persist_read_int(PERSIST_KEY_LAST_HEARTBEAT_TS);
    int gap_sec = (int)(now - last_hb);
    AppLaunchReason last_reason = persist_exists(PERSIST_KEY_LAST_LAUNCH_REASON)
      ? persist_read_int(PERSIST_KEY_LAST_LAUNCH_REASON)
      : APP_LAUNCH_SYSTEM;
    int last_activity = persist_exists(PERSIST_KEY_LAST_ACTIVITY)
      ? persist_read_int(PERSIST_KEY_LAST_ACTIVITY) : 0;
    int prev_activity = persist_exists(PERSIST_KEY_PREV_ACTIVITY)
      ? persist_read_int(PERSIST_KEY_PREV_ACTIVITY) : 0;
    APP_LOG(APP_LOG_LEVEL_INFO,
            "Diagnostics: heartbeat_age=%ds last_reason=%d (%s)",
            gap_sec, (int)last_reason, launch_reason_name(last_reason));
    APP_LOG(APP_LOG_LEVEL_INFO,
            "Diagnostics: activity %d (%s) <- %d (%s)",
            last_activity, activity_name(last_activity),
            prev_activity, activity_name(prev_activity));
    // If the gap is more than ~7 minutes (we heartbeat every 5min),
    // count this as a crash.
    if (gap_sec > 7 * 60) {
      int crash_count = persist_exists(PERSIST_KEY_CRASH_COUNT)
        ? persist_read_int(PERSIST_KEY_CRASH_COUNT) : 0;
      crash_count++;
      persist_write_int(PERSIST_KEY_CRASH_COUNT, crash_count);
      APP_LOG(APP_LOG_LEVEL_WARNING,
              "Detected likely crash (gap=%ds). Total crash count: %d",
              gap_sec, crash_count);
    }
  }
  persist_write_int(PERSIST_KEY_LAST_LAUNCH_TS, now);
  persist_write_int(PERSIST_KEY_LAST_LAUNCH_REASON, reason);
  persist_write_int(PERSIST_KEY_LAST_HEARTBEAT_TS, now);

  // Log heap usage at startup so we can spot memory issues over time.
  APP_LOG(APP_LOG_LEVEL_INFO, "Heap at init: free=%u bytes used=%u bytes",
          (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());

  // --- Self-relaunch via Wakeup API -----------------------------------
  // Schedule a wakeup 5 minutes from now. If the app is still running
  // when the wakeup fires, it's effectively a no-op (we don't subscribe
  // to the wakeup_handler, so there's no in-app event). If the app has
  // crashed in the meantime, the OS will re-launch us. This gives us
  // automatic recovery within ~5 minutes of any crash.
  //
  // Each init() re-schedules the next wakeup, so as long as the app is
  // alive, it keeps pushing recovery checks ~5 min into the future.
  // We cancel the previously-scheduled one first to avoid hitting the
  // 8-simultaneous-wakeups limit.
  if (persist_exists(PERSIST_KEY_WAKEUP_ID)) {
    WakeupId old = persist_read_int(PERSIST_KEY_WAKEUP_ID);
    wakeup_cancel(old);
  }
  time_t wakeup_time = now + 5 * 60;  // 5 minutes from now
  // notify_if_missed=true: if the watch was off (powered down, crashed,
  // etc.) when the wakeup should have fired, fire it as soon as the watch
  // is back online. Without this, missed wakeups are silently dropped.
  WakeupId wid = wakeup_schedule(wakeup_time, 0, true);
  if (wid >= 0) {
    persist_write_int(PERSIST_KEY_WAKEUP_ID, wid);
    APP_LOG(APP_LOG_LEVEL_INFO, "Self-relaunch wakeup scheduled in 5min (id=%d)",
            (int)wid);
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed: %d", (int)wid);
  }

  // Create main Window element and assign to pointer
  s_main_window = window_create();

  // Set handlers to manage the elements inside the Window
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  // Auto RTC -> Tama sync via AppTimer (alle 2h, mit Drift-Toleranz intern)
  s_rtc_sync_timer = app_timer_register(RTC_SYNC_CHECK_INTERVAL_MS, rtc_sync_timer_callback, NULL);

  // Start auto-save timer (saves every AUTOSAVE_INTERVAL_MS without quitting).
  // Disabled by default — flip AUTOSAVE_ENABLED above to 1 to enable.
#if AUTOSAVE_ENABLED
  s_autosave_timer = app_timer_register(AUTOSAVE_INTERVAL_MS, autosave_timer_callback, NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "Auto-save enabled, interval=%d ms", AUTOSAVE_INTERVAL_MS);
#else
  APP_LOG(APP_LOG_LEVEL_INFO, "Auto-save disabled");
#endif

  // Load user settings from persist BEFORE the window's main_window_load
  // runs (it uses these settings when applying initial text/hands colors).
  loadSettingsFromPersist();

  // Show the Window on the watch, with animated=true
  window_stack_push(s_main_window, true);

  // Try local boot if enabled by user setting (default: on)
  if (s_use_embedded_rom) {
    if (loadRomFromResource()) {
      s_hasReceivedRom = true;
      s_clearTextLayerOnScreenRefresh = true;

      if (persistLoadState()) {
        APP_LOG(APP_LOG_LEVEL_INFO, "Local boot: ROM + persist state loaded");
        s_hasReceivedSaveFile = true;
        s_loadedFromPersist = true;
        initTamalib();
      } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "Local boot: ROM loaded, no persist state -> fresh start");
        initTamalib();
      }
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "No local ROM resource — falling back to phone");
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "Embedded ROM disabled by setting — using phone-driven boot");
  }

  // Open AppMessage connection
  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_open(2048, 2048);

  // Listen for button events
  window_set_click_config_provider(s_main_window, click_config_provider);
}

// NOTE: the old `saveCurrentState(bool isAutoSave)` function that sent
// state via AppMessage to the phone has been removed. We now save locally
// to persist storage instead (see persistSaveState/saveCurrentStateAndQuit),
// which is faster, more reliable, and works without Bluetooth.

static void saveCurrentStateAndQuit()
{
  // Manual exit: save locally to watch persist storage (fast, reliable,
  // works without Bluetooth) and quit. We don't bother trying to send
  // state to the phone here — the local save is sufficient since we
  // boot from local storage on next startup. The auto-save timer also
  // keeps the local state fresh while the app is running.
  if (s_hasReceivedRom && s_hasReceivedSaveFile) {
    if (persistSaveState()) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Manual exit: state saved to watch storage");
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Manual exit: persistSaveState failed");
    }
  }
  Quit();
}

// Load ROM from app resource (built-in, no phone needed).
// Returns true on success.
static bool loadRomFromResource(void)
{
  ResHandle handle = resource_get_handle(RESOURCE_ID_TAMA_ROM);
  if (!handle) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "loadRomFromResource: no resource handle");
    return false;
  }

  size_t res_size = resource_size(handle);
  const size_t expected = 6144 * 2;  // 6144 u12 values, packed as 2 bytes each
  if (res_size != expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: size %d != expected %d",
            (int)res_size, (int)expected);
    return false;
  }

  // Read into a temporary byte buffer on the heap (12 KB stays off the stack)
  uint8_t *buf = malloc(res_size);
  if (!buf) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: malloc failed");
    return false;
  }

  size_t read = resource_load(handle, buf, res_size);
  if (read != res_size) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: read %d != expected %d",
            (int)read, (int)res_size);
    free(buf);
    return false;
  }

  // Unpack: 2 bytes per 12-bit value, low byte first.
  // Same format as the JS-chunked-ROM-receive path.
  for (int i = 0; i < 6144; i++) {
    u12_t value = buf[i * 2] | (buf[i * 2 + 1] << 8);
    g_program[i] = value & 0x0FFF;  // ensure 12-bit
  }

  free(buf);
  APP_LOG(APP_LOG_LEVEL_INFO, "loadRomFromResource: ROM loaded (%d bytes)", (int)res_size);
  return true;
}

// Load settings from persist (with safe defaults if no values stored)
static void loadSettingsFromPersist(void)
{
  if (persist_exists(PERSIST_KEY_USE_EMBEDDED_ROM)) {
    s_use_embedded_rom = persist_read_bool(PERSIST_KEY_USE_EMBEDDED_ROM);
  }
  if (persist_exists(PERSIST_KEY_VIBRATION_ENABLED)) {
    s_vibration_enabled = persist_read_bool(PERSIST_KEY_VIBRATION_ENABLED);
  }
  if (persist_exists(PERSIST_KEY_SOUND_ENABLED)) {
    s_sound_enabled = persist_read_bool(PERSIST_KEY_SOUND_ENABLED);
  }
  if (persist_exists(PERSIST_KEY_SOUND_VOLUME)) {
    int v = persist_read_int(PERSIST_KEY_SOUND_VOLUME);
    if (v >= 0 && v <= 100) s_sound_volume = (uint8_t)v;
  }

  // Customization: defaults are white text/hands with black outline
  s_text_color_argb         = GColorWhiteARGB8;
  s_text_outline_color_argb = GColorBlackARGB8;
  s_hands_color_argb        = GColorWhiteARGB8;
  s_hands_outline_color_argb = GColorBlackARGB8;
  s_text_outline_enabled    = true;
  s_hands_thickness         = 1;

  if (persist_exists(PERSIST_KEY_TEXT_COLOR)) {
    s_text_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_TEXT_COLOR);
  }
  if (persist_exists(PERSIST_KEY_TEXT_OUTLINE)) {
    s_text_outline_enabled = persist_read_bool(PERSIST_KEY_TEXT_OUTLINE);
  }
  if (persist_exists(PERSIST_KEY_TEXT_OUTLINE_COLOR)) {
    s_text_outline_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_TEXT_OUTLINE_COLOR);
  }
  if (persist_exists(PERSIST_KEY_HANDS_COLOR)) {
    s_hands_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_HANDS_COLOR);
  }
  if (persist_exists(PERSIST_KEY_HANDS_OUTLINE_COLOR)) {
    s_hands_outline_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_HANDS_OUTLINE_COLOR);
  }
  if (persist_exists(PERSIST_KEY_HANDS_THICKNESS)) {
    int v = persist_read_int(PERSIST_KEY_HANDS_THICKNESS);
    if (v >= 0 && v <= 2) s_hands_thickness = (uint8_t)v;
  }
  if (persist_exists(PERSIST_KEY_BG_USE_CUSTOM)) {
    s_bg_use_custom = persist_read_bool(PERSIST_KEY_BG_USE_CUSTOM);
  }
  if (persist_exists(PERSIST_KEY_BG_FILL_COLOR)) {
    s_bg_fill_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_BG_FILL_COLOR);
  }
  if (persist_exists(PERSIST_KEY_BG_MARKERS_COLOR)) {
    s_bg_markers_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_BG_MARKERS_COLOR);
  }
  if (persist_exists(PERSIST_KEY_BG_MARKERS_STYLE)) {
    int v = persist_read_int(PERSIST_KEY_BG_MARKERS_STYLE);
    if (v >= 0 && v <= 2) s_bg_markers_style = (uint8_t)v;
  }
  if (persist_exists(PERSIST_KEY_ICONS_SMALL)) {
    s_icons_small = persist_read_bool(PERSIST_KEY_ICONS_SMALL);
  }
  if (persist_exists(PERSIST_KEY_TAMA_INVERT)) {
    s_tama_invert = persist_read_bool(PERSIST_KEY_TAMA_INVERT);
  }
  if (persist_exists(PERSIST_KEY_TAMA_BG_COLOR)) {
    s_tama_bg_color_argb = (uint8_t)persist_read_int(PERSIST_KEY_TAMA_BG_COLOR);
  }
  if (persist_exists(PERSIST_KEY_TAMA_BG_ENABLED)) {
    s_tama_bg_enabled = persist_read_bool(PERSIST_KEY_TAMA_BG_ENABLED);
  }

  APP_LOG(APP_LOG_LEVEL_INFO,
          "Settings: embedded_rom=%d vibration=%d sound=%d vol=%d",
          (int)s_use_embedded_rom, (int)s_vibration_enabled,
          (int)s_sound_enabled, (int)s_sound_volume);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "Customization: text=0x%02x outline=%d/0x%02x",
          (int)s_text_color_argb, (int)s_text_outline_enabled,
          (int)s_text_outline_color_argb);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "Customization: hands=0x%02x/0x%02x thickness=%d",
          (int)s_hands_color_argb, (int)s_hands_outline_color_argb,
          (int)s_hands_thickness);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "Customization: bg_use_custom=%d fill=0x%02x markers=0x%02x/style=%d",
          (int)s_bg_use_custom, (int)s_bg_fill_color_argb,
          (int)s_bg_markers_color_argb, (int)s_bg_markers_style);
}

// Save current state to local watch storage (persist API). Synchronous,
// no AppMessage involved. Returns true on success.
static bool persistSaveState(void)
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return false;

  // Mark activity so we know if a crash happened during the save.
  set_activity(ACT_AUTOSAVE);

  // flat_state_t is ~520 bytes (incl. memory[]). Putting it on the stack
  // during the save would put us close to Pebble's 8KB app-stack limit,
  // especially since we're called from a timer callback inside the event
  // loop which already has its own stack frames. Static storage avoids
  // any chance of stack overflow.
  static flat_state_t st;
  st = cpu_get_flat_state();

  // Sanity check: refuse to save obviously-bad state
  if (st.pc == 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistSaveState: refusing to save state with pc=0");
    return false;
  }

  // Header: everything except the memory[] array.
  // Order/packing: explicit struct so layout is stable across builds.
  // NOTE: layout must match the new upstream tamalib (multiple clock-timer
  //       timestamps, cpu_halted flag). Old saves (header format from the
  //       2021 tamalib) are not compatible — magic value bump handles that.
  typedef struct __attribute__((packed)) {
    uint16_t pc;
    uint16_t x;
    uint16_t y;
    uint8_t  a;
    uint8_t  b;
    uint8_t  np;
    uint8_t  sp;
    uint8_t  flags;
    uint32_t tick_counter;
    uint32_t clk_timer_2hz_timestamp;
    uint32_t clk_timer_4hz_timestamp;
    uint32_t clk_timer_8hz_timestamp;
    uint32_t clk_timer_16hz_timestamp;
    uint32_t clk_timer_32hz_timestamp;
    uint32_t clk_timer_64hz_timestamp;
    uint32_t clk_timer_128hz_timestamp;
    uint32_t clk_timer_256hz_timestamp;
    uint32_t prog_timer_timestamp;
    uint8_t  prog_timer_enabled;
    uint8_t  prog_timer_data;
    uint8_t  prog_timer_rld;
    uint32_t call_depth;
    uint8_t  cpu_halted;
    // 6 interrupts × 4 bytes each = 24 bytes
    uint8_t  interrupts[24];
  } persist_header_t;

  persist_header_t hdr;
  hdr.pc = st.pc;
  hdr.x = st.x;
  hdr.y = st.y;
  hdr.a = st.a;
  hdr.b = st.b;
  hdr.np = st.np;
  hdr.sp = st.sp;
  hdr.flags = st.flags;
  hdr.tick_counter = st.tick_counter;
  hdr.clk_timer_2hz_timestamp = st.clk_timer_2hz_timestamp;
  hdr.clk_timer_4hz_timestamp = st.clk_timer_4hz_timestamp;
  hdr.clk_timer_8hz_timestamp = st.clk_timer_8hz_timestamp;
  hdr.clk_timer_16hz_timestamp = st.clk_timer_16hz_timestamp;
  hdr.clk_timer_32hz_timestamp = st.clk_timer_32hz_timestamp;
  hdr.clk_timer_64hz_timestamp = st.clk_timer_64hz_timestamp;
  hdr.clk_timer_128hz_timestamp = st.clk_timer_128hz_timestamp;
  hdr.clk_timer_256hz_timestamp = st.clk_timer_256hz_timestamp;
  hdr.prog_timer_timestamp = st.prog_timer_timestamp;
  hdr.prog_timer_enabled = st.prog_timer_enabled;
  hdr.prog_timer_data = st.prog_timer_data;
  hdr.prog_timer_rld = st.prog_timer_rld;
  hdr.call_depth = st.call_depth;
  hdr.cpu_halted = st.cpu_halted;
  for (int i = 0; i < 6; i++) {
    hdr.interrupts[i*4+0] = st.interrupts[i].factor_flag_reg;
    hdr.interrupts[i*4+1] = st.interrupts[i].mask_reg;
    hdr.interrupts[i*4+2] = st.interrupts[i].triggered;
    hdr.interrupts[i*4+3] = st.interrupts[i].vector;
  }

  // Split memory[] into two halves to stay under 256-byte persist limit.
  // MEM_BUFFER_SIZE × sizeof(u4_t) bytes total — typically ~464 bytes.
  const size_t mem_total = sizeof(st.memory);
  const size_t mem_half  = mem_total / 2;
  const size_t mem_rest  = mem_total - mem_half;

  status_t s;
  s = persist_write_data(PERSIST_KEY_STATE_HEADER, &hdr, sizeof(hdr));
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist header write failed: %d", (int)s); return false; }

  s = persist_write_data(PERSIST_KEY_STATE_MEM1, &st.memory[0], mem_half);
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist mem1 write failed: %d", (int)s); return false; }

  s = persist_write_data(PERSIST_KEY_STATE_MEM2, ((uint8_t*)st.memory) + mem_half, mem_rest);
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist mem2 write failed: %d", (int)s); return false; }

  // Icons (small, one key)
  uint8_t icons[2] = { (uint8_t)s_selectedIcon, (uint8_t)s_showingAttentionIcon };
  persist_write_data(PERSIST_KEY_ICONS, icons, sizeof(icons));

  // Magic LAST — only set after all other writes succeeded.
  // Reader checks this first; if missing/wrong, the rest is ignored.
  uint32_t magic = PERSIST_MAGIC_VALUE;
  persist_write_data(PERSIST_KEY_MAGIC, &magic, sizeof(magic));

  return true;
}

// Read persisted state into stateToLoad. Returns true if a valid state
// was found and loaded. Called during init() before tamalib_init().
static bool persistLoadState(void)
{
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: starting");

  if (!persist_exists(PERSIST_KEY_MAGIC)) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: no magic key -> first time or wiped");
    return false;
  }

  uint32_t magic = 0;
  persist_read_data(PERSIST_KEY_MAGIC, &magic, sizeof(magic));
  if (magic != PERSIST_MAGIC_VALUE) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: bad magic 0x%lx", (unsigned long)magic);
    return false;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: magic ok");

  if (!persist_exists(PERSIST_KEY_STATE_HEADER) ||
      !persist_exists(PERSIST_KEY_STATE_MEM1) ||
      !persist_exists(PERSIST_KEY_STATE_MEM2)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: missing keys");
    return false;
  }

  typedef struct __attribute__((packed)) {
    uint16_t pc; uint16_t x; uint16_t y;
    uint8_t  a; uint8_t b; uint8_t np; uint8_t sp; uint8_t flags;
    uint32_t tick_counter;
    uint32_t clk_timer_2hz_timestamp;
    uint32_t clk_timer_4hz_timestamp;
    uint32_t clk_timer_8hz_timestamp;
    uint32_t clk_timer_16hz_timestamp;
    uint32_t clk_timer_32hz_timestamp;
    uint32_t clk_timer_64hz_timestamp;
    uint32_t clk_timer_128hz_timestamp;
    uint32_t clk_timer_256hz_timestamp;
    uint32_t prog_timer_timestamp;
    uint8_t  prog_timer_enabled;
    uint8_t  prog_timer_data;
    uint8_t  prog_timer_rld;
    uint32_t call_depth;
    uint8_t  cpu_halted;
    uint8_t  interrupts[24];
  } persist_header_t;

  persist_header_t hdr;
  int read = persist_read_data(PERSIST_KEY_STATE_HEADER, &hdr, sizeof(hdr));
  if (read != (int)sizeof(hdr)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: header size mismatch %d", read);
    return false;
  }

  stateToLoad.pc = hdr.pc;
  stateToLoad.x = hdr.x;
  stateToLoad.y = hdr.y;
  stateToLoad.a = hdr.a;
  stateToLoad.b = hdr.b;
  stateToLoad.np = hdr.np;
  stateToLoad.sp = hdr.sp;
  stateToLoad.flags = hdr.flags;
  stateToLoad.tick_counter = hdr.tick_counter;
  stateToLoad.clk_timer_2hz_timestamp   = hdr.clk_timer_2hz_timestamp;
  stateToLoad.clk_timer_4hz_timestamp   = hdr.clk_timer_4hz_timestamp;
  stateToLoad.clk_timer_8hz_timestamp   = hdr.clk_timer_8hz_timestamp;
  stateToLoad.clk_timer_16hz_timestamp  = hdr.clk_timer_16hz_timestamp;
  stateToLoad.clk_timer_32hz_timestamp  = hdr.clk_timer_32hz_timestamp;
  stateToLoad.clk_timer_64hz_timestamp  = hdr.clk_timer_64hz_timestamp;
  stateToLoad.clk_timer_128hz_timestamp = hdr.clk_timer_128hz_timestamp;
  stateToLoad.clk_timer_256hz_timestamp = hdr.clk_timer_256hz_timestamp;
  stateToLoad.prog_timer_timestamp = hdr.prog_timer_timestamp;
  stateToLoad.prog_timer_enabled = hdr.prog_timer_enabled;
  stateToLoad.prog_timer_data = hdr.prog_timer_data;
  stateToLoad.prog_timer_rld = hdr.prog_timer_rld;
  stateToLoad.call_depth = hdr.call_depth;
  stateToLoad.cpu_halted = hdr.cpu_halted;
  for (int i = 0; i < 6; i++) {
    stateToLoad.interrupts[i].factor_flag_reg = hdr.interrupts[i*4+0];
    stateToLoad.interrupts[i].mask_reg        = hdr.interrupts[i*4+1];
    stateToLoad.interrupts[i].triggered       = hdr.interrupts[i*4+2];
    stateToLoad.interrupts[i].vector          = hdr.interrupts[i*4+3];
  }

  const size_t mem_total = sizeof(stateToLoad.memory);
  const size_t mem_half  = mem_total / 2;
  const size_t mem_rest  = mem_total - mem_half;

  int r1 = persist_read_data(PERSIST_KEY_STATE_MEM1, &stateToLoad.memory[0], mem_half);
  int r2 = persist_read_data(PERSIST_KEY_STATE_MEM2, ((uint8_t*)stateToLoad.memory) + mem_half, mem_rest);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: mem reads r1=%d r2=%d expected=%d+%d",
          r1, r2, (int)mem_half, (int)mem_rest);
  if (r1 != (int)mem_half || r2 != (int)mem_rest) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: memory size mismatch -> aborting");
    return false;
  }

  if (persist_exists(PERSIST_KEY_ICONS)) {
    uint8_t icons[2] = {0};
    persist_read_data(PERSIST_KEY_ICONS, icons, sizeof(icons));
    s_selectedIcon = (int8_t)icons[0];
    s_showingAttentionIcon = (bool)icons[1];
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "persistLoadState: loaded state from watch storage (pc=%d)", (int)hdr.pc);
  return true;
}

// AppTimer callback: triggers persist-based auto-save and reschedules.
static void autosave_timer_callback(void *data)
{
  s_autosave_timer = NULL;
  set_activity(ACT_AUTOSAVE);

  if (s_hasReceivedRom && s_hasReceivedSaveFile) {
    if (persistSaveState()) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Auto-save: persisted to watch storage");
    }
  }

  // Reschedule
  s_autosave_timer = app_timer_register(AUTOSAVE_INTERVAL_MS, autosave_timer_callback, NULL);
}

static void deinit() {
  if (s_autosave_timer) {
    app_timer_cancel(s_autosave_timer);
    s_autosave_timer = NULL;
  }
  if (s_rtc_sync_timer) {
    app_timer_cancel(s_rtc_sync_timer);
    s_rtc_sync_timer = NULL;
  }

  // Clean exit — user closed the app. Cancel the pending self-relaunch
  // wakeup so we don't pop back open in ~5 minutes against the user's wish.
  if (persist_exists(PERSIST_KEY_WAKEUP_ID)) {
    WakeupId wid = persist_read_int(PERSIST_KEY_WAKEUP_ID);
    wakeup_cancel(wid);
    persist_delete(PERSIST_KEY_WAKEUP_ID);
    APP_LOG(APP_LOG_LEVEL_INFO, "Clean exit: cancelled self-relaunch wakeup");
  }

  // Update heartbeat to "now" so the next start doesn't mistake a clean
  // exit for a crash.
  persist_write_int(PERSIST_KEY_LAST_HEARTBEAT_TS, time(NULL));

  window_destroy(s_main_window);

  // Release tamalib
  tamalib_release();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}