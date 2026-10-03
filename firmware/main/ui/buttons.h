#pragma once
// Staff buttons (TCA9555 EXIO 9/10/11 = KEY 1/2/3, active low):
//   KEY 1 short press       privacy pause on/off
//   KEY 2 short press       show status for 3 s
//   KEY 1 + KEY 3, 10 s     open the Wi-Fi setup hotspot
void buttons_start(void);
