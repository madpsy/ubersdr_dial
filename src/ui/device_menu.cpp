#include "device_menu.h"

#include "../app/saver.h"
#include "../app/settings.h"
#include "../hw/display.h"
#include "../hw/haptic.h"

static const MenuChoice kBrightChoices[] = {{"20%", 20}, {"40%", 40}, {"60%", 60}, {"80%", 80}, {"100%", 100}};
static const MenuChoice kOnOff[] = {{"On", 1}, {"Off", 0}};
static const MenuChoice kSaverChoices[] = {
    {"Dim", SAVER_DIM},         {"Screen off", SAVER_OFF}, {"Dim, then\noff", SAVER_DIM_OFF},
    {"Spectrum off", SAVER_SPECTRUM}, {"Never", SAVER_NEVER}};
static const MenuChoice kSaverTimes[] = {{"30 s", 30}, {"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600}};
static const MenuChoice kSaverOffTimes[] = {{"Auto\n3x dim", 0}, {"5 min", 300},   {"10 min", 600},
                                            {"20 min", 1200},     {"30 min", 1800}, {"1 hour", 3600}};

static const MenuChoice kSleepTimes[] = {{"15 min", 15}, {"30 min", 30}, {"1 hour", 60}, {"2 hours", 120}, {"Never", 0}};

const MenuItem kDeviceMenu[] = {
    {LV_SYMBOL_EYE_OPEN, "Brightness", [] { return (int)g_settings.brightness; },
     [](int v) {
       g_settings.brightness = v;
       display_set_brightness(v);
       settings_save();
     },
     kBrightChoices, 5},
    {LV_SYMBOL_BELL, "Haptics", [] { return (int)g_settings.haptics; },
     [](int v) {
       g_settings.haptics = v;
       haptic_enable(v);
       settings_save();
     },
     kOnOff, 2, [] { return haptic_present(); }},
    {LV_SYMBOL_EYE_CLOSE, "Screen saver", [] { return (int)g_settings.saver_mode; },
     [](int v) {
       g_settings.saver_mode = v;
       settings_save();
     },
     kSaverChoices, 5},
    {LV_SYMBOL_LOOP, "Saver after", [] { return (int)g_settings.saver_secs; },
     [](int v) {
       g_settings.saver_secs = v;
       settings_save();
     },
     kSaverTimes, 5, [] { return g_settings.saver_mode != SAVER_NEVER; }},
    {LV_SYMBOL_POWER, "Screen off after", [] { return (int)g_settings.saver_off_secs; },
     [](int v) {
       g_settings.saver_off_secs = v;
       settings_save();
     },
     kSaverOffTimes, 6, [] { return g_settings.saver_mode == SAVER_DIM_OFF; }},
    // Deep sleep; only when nothing's using the dial (no page connected,
    // receiver muted). A knob turn wakes it, back into the same mode.
    {LV_SYMBOL_DOWNLOAD, "Sleep after", [] { return (int)g_settings.sleep_mins; },
     [](int v) {
       g_settings.sleep_mins = v;
       settings_save();
     },
     kSleepTimes, 5},
};
const int kNumDeviceMenu = sizeof(kDeviceMenu) / sizeof(kDeviceMenu[0]);
