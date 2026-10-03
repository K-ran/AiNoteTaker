#pragma once

// Hold KEY 9/10/11 to record from the mic; on release the clip is saved to
// /sdcard/REC#####.WAV (16 kHz mono) and played back on the speaker.
// Needs esp_board_init() and tca9555_driver_init() first.
void voice_rec_start(void);
