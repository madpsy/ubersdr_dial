#include "dial_link.h"

const char *const kV2Modes[] = {"lsb", "usb", "am", "sam", "nfm", "fm", "cwl", "cwu", "iq"};
const int kV2NumModes = sizeof(kV2Modes) / sizeof(kV2Modes[0]);

// static/v2/src/radio/constants.js TUNING_STEPS
const uint32_t kDialSteps[] = {1, 10, 100, 500, 1000, 5000, 9000, 10000, 100000};
const int kDialNumSteps = sizeof(kDialSteps) / sizeof(kDialSteps[0]);

// controls/dial.js DIAL_BANDS, with a starting frequency for receiver mode's
// first visit (afterwards each band remembers where you were).
const DialBand kDialBands[] = {
    {"160m", 1810000, 2000000, 1840000},   {"80m", 3500000, 4000000, 3700000},
    {"60m", 5250000, 5450000, 5357000},    {"40m", 7000000, 7300000, 7150000},
    {"30m", 10100000, 10150000, 10136000}, {"20m", 14000000, 14350000, 14200000},
    {"17m", 18068000, 18168000, 18130000}, {"15m", 21000000, 21450000, 21250000},
    {"12m", 24890000, 24990000, 24950000}, {"10m", 28000000, 29700000, 28500000},
    {"6m", 50000000, 52000000, 50150000},
};
const int kDialNumBands = sizeof(kDialBands) / sizeof(kDialBands[0]);
