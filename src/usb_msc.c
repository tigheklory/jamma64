#include "hardware/watchdog.h"
#include "usb_msc.h"
#include "wifi_config.h"

#include "pico/stdlib.h"
#include "tusb.h"

#include <string.h>
#include <stdio.h>

// ---- A tiny RAM disk ----
#define SECTOR_SIZE 512
#define SECTOR_COUNT 128  // 64KB disk
static uint8_t disk[SECTOR_SIZE * SECTOR_COUNT];

// Super-minimal FAT12-ish is annoying to handcraft.
// Instead: we present a "fake" disk and only care about one file write by name.
// Many OSes will still write directory entries; implementing full FAT is more work.
//
// Practical approach: Use TinyUSB's MSC "read/write blocks" and just accept raw writes,
// then search the disk image for "SSID=" and "PASSWORD=" patterns.
// This is crude but works for a single config file flow.

static bool did_save = false;

void usb_msc_init(void) {
  // Initialize to zeros; OS will format if needed. We just need a writable medium.
  memset(disk, 0, sizeof(disk));
  tusb_init();
}

bool usb_msc_handle_wifi_txt(const char *data, unsigned len) {
  wifi_creds_t c;
  if (!wifi_parse_txt(data, len, &c)) return false;

  if (!c.valid) {
    // FORGET path
    wifi_config_erase();
    printf("Wi-Fi creds erased from flash.\n");
    return true;
  }

  if (wifi_config_save(&c)) {
    printf("Wi-Fi creds saved. Rebooting...\n");
    return true;
  }
  return false;
}

// ---- TinyUSB callbacks ----

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  (void)lun;
  uint32_t addr = lba * SECTOR_SIZE + offset;
  if (addr + bufsize > sizeof(disk)) return -1;
  memcpy(buffer, &disk[addr], bufsize);
  return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  (void)lun;
  uint32_t addr = lba * SECTOR_SIZE + offset;
  if (addr + bufsize > sizeof(disk)) return -1;
  memcpy(&disk[addr], buffer, bufsize);

  // After any write, scan for a wifi.txt marker once (cheap heuristic).
  // Look for "SSID=" in the disk image and parse a nearby chunk.
  if (!did_save) {
    const char *p = (const char*)disk;
    const char *hit = NULL;
    for (size_t i = 0; i + 5 < sizeof(disk); i++) {
      if (!memcmp(p + i, "SSID=", 5)) { hit = p + i; break; }
    }
    if (hit) {
      // Grab a small window around it and parse
      size_t start = (size_t)(hit - p);
      size_t window = 256;
      if (start + window > sizeof(disk)) window = sizeof(disk) - start;
      if (usb_msc_handle_wifi_txt(hit, (unsigned)window)) {
        did_save = true;
      }
    }
  }

  return (int32_t)bufsize;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
  (void)lun;
  // If we saved creds, reboot shortly after write completes
  if (did_save) {
    sleep_ms(300);
    watchdog_reboot(0, 0, 0);
  }
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  const char vid[] = "JAMMA64 ";
  const char pid[] = "CONFIG DRIVE     ";
  const char rev[] = "1.0 ";
  memcpy(vendor_id, vid, 8);
  memcpy(product_id, pid, 16);
  memcpy(product_rev, rev, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  (void)lun;
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
  (void)lun;
  *block_count = SECTOR_COUNT;
  *block_size  = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun; (void)power_condition; (void)start; (void)load_eject;
  return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
  (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
  // Use default handling
  return 0;
}
