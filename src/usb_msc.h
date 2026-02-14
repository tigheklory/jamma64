#pragma once
#include <stdbool.h>

// Call once at startup
void usb_msc_init(void);

// Called when wifi.txt is written to the drive
// Returns true if it saved creds and requested reboot
bool usb_msc_handle_wifi_txt(const char *data, unsigned len);

// True after creds have been saved/erased and a reboot is recommended.
bool usb_msc_reboot_required(void);
