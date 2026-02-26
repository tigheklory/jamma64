#include "usb_msc.h"
#include "wifi_config.h"
#include "mapping_store.h"

#include "pico/stdlib.h"
#include "tusb.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifndef JAMMA64_ENABLE_USB_DEBUG
#define JAMMA64_ENABLE_USB_DEBUG 0
#endif

#if JAMMA64_ENABLE_USB_DEBUG
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

#define SECTOR_SIZE 512u
#define SECTOR_COUNT 256u  // 128KB disk

// FAT12 layout
#define RESERVED_SECTORS 1u
#define FAT_COUNT 2u
#define SECTORS_PER_FAT 1u
#define ROOT_ENTRY_COUNT 32u
#define ROOT_DIR_SECTORS ((ROOT_ENTRY_COUNT * 32u) / SECTOR_SIZE)  // 2

#define FAT1_START_LBA RESERVED_SECTORS
#define FAT2_START_LBA (FAT1_START_LBA + SECTORS_PER_FAT)
#define ROOT_START_LBA (FAT2_START_LBA + SECTORS_PER_FAT)
#define DATA_START_LBA (ROOT_START_LBA + ROOT_DIR_SECTORS)

#define WIFI_FILE_CLUSTER 2u
#define MAPS_FILE_CLUSTER 3u

static uint8_t disk[SECTOR_SIZE * SECTOR_COUNT];

static bool wifi_scan_pending;
static bool reboot_required;
static uint8_t sense_key;
static uint8_t sense_asc;
static uint8_t sense_ascq;

static const char k_wifi_template[] =
  "# JAMMA64 Wi-Fi config\r\n"
  "# Edit values and save this file.\r\n"
  "SSID=\r\n"
  "PASSWORD=\r\n"
  "# Optional: REBOOT=1\r\n";

static uint32_t build_wifi_file_contents(char *out, uint32_t out_max) {
  if (!out || out_max == 0u) return 0u;

  wifi_creds_t creds;
  if (wifi_config_load(&creds) && creds.valid) {
    int n = snprintf(
      out,
      out_max,
      "# JAMMA64 Wi-Fi config\r\n"
      "# Edit values and save this file.\r\n"
      "SSID=%s\r\n"
      "PASSWORD=%s\r\n"
      "# Optional: REBOOT=1\r\n",
      creds.ssid,
      creds.password
    );
    if (n > 0 && (uint32_t)n < out_max) return (uint32_t)n;
  }

  size_t tlen = strlen(k_wifi_template);
  if (tlen >= out_max) tlen = out_max - 1u;
  memcpy(out, k_wifi_template, tlen);
  out[tlen] = '\0';
  return (uint32_t)tlen;
}

static inline uint8_t *lba_ptr(uint32_t lba) {
  return &disk[lba * SECTOR_SIZE];
}

static void fat12_set(uint16_t cluster, uint16_t value) {
  uint8_t *fat1 = lba_ptr(FAT1_START_LBA);
  uint8_t *fat2 = lba_ptr(FAT2_START_LBA);
  uint32_t off = (cluster * 3u) / 2u;
  value &= 0x0FFFu;

  if ((cluster & 1u) == 0u) {
    fat1[off] = (uint8_t)(value & 0xFFu);
    fat1[off + 1] = (uint8_t)((fat1[off + 1] & 0xF0u) | ((value >> 8) & 0x0Fu));
  } else {
    fat1[off] = (uint8_t)((fat1[off] & 0x0Fu) | ((value << 4) & 0xF0u));
    fat1[off + 1] = (uint8_t)((value >> 4) & 0xFFu);
  }
  fat2[off] = fat1[off];
  fat2[off + 1] = fat1[off + 1];
}

static void set_sense(uint8_t key, uint8_t asc, uint8_t ascq) {
  sense_key = key;
  sense_asc = asc;
  sense_ascq = ascq;
  tud_msc_set_sense(0, key, asc, ascq);
}

static void write_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t fat12_get(uint16_t cluster) {
  const uint8_t *fat = lba_ptr(FAT1_START_LBA);
  uint32_t off = (cluster * 3u) / 2u;
  uint16_t v;
  if ((cluster & 1u) == 0u) {
    v = (uint16_t)(fat[off] | ((fat[off + 1] & 0x0Fu) << 8));
  } else {
    v = (uint16_t)(((fat[off] >> 4) & 0x0Fu) | (fat[off + 1] << 4));
  }
  return (uint16_t)(v & 0x0FFFu);
}

static bool is_fat12_eoc(uint16_t v) {
  return v >= 0x0FF8u;
}

static uint32_t cluster_to_lba(uint16_t cluster) {
  return DATA_START_LBA + (uint32_t)(cluster - 2u);
}

static void usb_msc_build_fat_image(void) {
  memset(disk, 0, sizeof(disk));
  char wifi_file[SECTOR_SIZE];
  char maps_file[SECTOR_SIZE];
  uint32_t wifi_len = build_wifi_file_contents(wifi_file, sizeof(wifi_file));
  uint32_t maps_len = (uint32_t)mapping_store_export_json(maps_file, sizeof(maps_file));
  if (maps_len == 0u) {
    static const char k_empty_json[] = "{\"version\":1,\"active\":\"\",\"profiles\":[]}\r\n";
    maps_len = (uint32_t)strlen(k_empty_json);
    memcpy(maps_file, k_empty_json, maps_len);
  }

  // Boot sector
  uint8_t *bs = lba_ptr(0);
  bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
  memcpy(&bs[3], "MSDOS5.0", 8);
  write_le16(&bs[11], SECTOR_SIZE);
  bs[13] = 1;  // sectors/cluster
  write_le16(&bs[14], RESERVED_SECTORS);
  bs[16] = FAT_COUNT;
  write_le16(&bs[17], ROOT_ENTRY_COUNT);
  write_le16(&bs[19], SECTOR_COUNT);
  bs[21] = 0xF8;  // fixed disk
  write_le16(&bs[22], SECTORS_PER_FAT);
  write_le16(&bs[24], 32);  // sectors/track
  write_le16(&bs[26], 64);  // heads
  write_le32(&bs[28], 0);   // hidden sectors
  write_le32(&bs[32], 0);   // total sectors 32 (unused)
  bs[36] = 0x80;  // drive number
  bs[38] = 0x29;  // ext boot signature
  write_le32(&bs[39], 0x4A364D41u);  // volume serial
  memcpy(&bs[43], "JAMMA64    ", 11);   // volume label
  memcpy(&bs[54], "FAT12   ", 8);
  bs[510] = 0x55;
  bs[511] = 0xAA;

  // FAT1/FAT2: media + reserved clusters
  uint8_t *fat1 = lba_ptr(FAT1_START_LBA);
  fat1[0] = 0xF8; fat1[1] = 0xFF; fat1[2] = 0xFF;
  memcpy(lba_ptr(FAT2_START_LBA), fat1, SECTOR_SIZE);
  fat12_set(WIFI_FILE_CLUSTER, 0x0FFFu);
  fat12_set(MAPS_FILE_CLUSTER, 0x0FFFu);

  // Root directory
  uint8_t *root = lba_ptr(ROOT_START_LBA);
  // Volume label entry
  memcpy(&root[0], "JAMMA64    ", 11);
  root[11] = 0x08;

  // WIFI.TXT entry
  uint8_t *wf = &root[32];
  memcpy(&wf[0], "WIFI    TXT", 11);
  wf[11] = 0x20;  // archive
  write_le16(&wf[26], WIFI_FILE_CLUSTER);
  write_le32(&wf[28], wifi_len);

  // MAPS.JSN entry (JSON export of saved named mappings)
  uint8_t *mf = &root[64];
  memcpy(&mf[0], "MAPS    JSN", 11);
  mf[11] = 0x20;  // archive
  write_le16(&mf[26], MAPS_FILE_CLUSTER);
  write_le32(&mf[28], maps_len);

  // File data
  uint8_t *wifi_data = lba_ptr(cluster_to_lba(WIFI_FILE_CLUSTER));
  memcpy(wifi_data, wifi_file, wifi_len);
  uint8_t *maps_data = lba_ptr(cluster_to_lba(MAPS_FILE_CLUSTER));
  memcpy(maps_data, maps_file, maps_len);
}

static bool find_wifi_entry(uint16_t *cluster, uint32_t *size) {
  const uint8_t *root = lba_ptr(ROOT_START_LBA);
  for (uint32_t i = 0; i < ROOT_ENTRY_COUNT; i++) {
    const uint8_t *e = &root[i * 32u];
    if (e[0] == 0x00) break;
    if (e[0] == 0xE5) continue;
    if (e[11] == 0x0F) continue;  // LFN
    if (memcmp(&e[0], "WIFI    TXT", 11) == 0) {
      *cluster = (uint16_t)(e[26] | (e[27] << 8));
      *size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
      return true;
    }
  }
  return false;
}

static bool read_wifi_file(char *out, uint32_t out_max, uint32_t *out_len) {
  uint16_t cluster = 0;
  uint32_t size = 0;
  if (!find_wifi_entry(&cluster, &size)) return false;
  if (cluster < 2u) return false;
  if (size == 0u || size >= out_max) return false;

  uint32_t copied = 0;
  uint16_t cur = cluster;
  while (copied < size && cur >= 2u && cur < 0x0FF8u) {
    uint32_t lba = cluster_to_lba(cur);
    uint32_t chunk = size - copied;
    if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
    memcpy(out + copied, lba_ptr(lba), chunk);
    copied += chunk;
    if (copied >= size) break;
    cur = fat12_get(cur);
    if (is_fat12_eoc(cur)) break;
  }

  if (copied != size) return false;
  out[copied] = '\0';
  *out_len = copied;
  return true;
}

void usb_msc_init(void) {
  usb_msc_build_fat_image();
  wifi_scan_pending = false;
  reboot_required = false;
  set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
  tusb_init();
}

void usb_msc_refresh_files(void) {
  usb_msc_build_fat_image();
}

bool usb_msc_handle_wifi_txt(const char *data, unsigned len) {
  wifi_creds_t c;
  if (!wifi_parse_txt(data, len, &c)) return false;

  if (!c.valid) {
    wifi_config_erase();
    DBG_PRINTF("Wi-Fi creds erased. Reboot required to apply.\n");
    return true;
  }

  if (wifi_config_save(&c)) {
    DBG_PRINTF("Wi-Fi creds saved. Reboot required to apply.\n");
    return true;
  }
  return false;
}

static bool wifi_txt_requests_reboot(const char *buf, uint32_t len) {
  char tmp[512];
  if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
  memcpy(tmp, buf, len);
  tmp[len] = '\0';

  char *saveptr = NULL;
  for (char *line = strtok_r(tmp, "\r\n", &saveptr); line; line = strtok_r(NULL, "\r\n", &saveptr)) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') continue;
    if (!strncasecmp(line, "REBOOT=", 7)) {
      const char *v = line + 7;
      while (*v == ' ' || *v == '\t') v++;
      if (!strcasecmp(v, "1") || !strcasecmp(v, "TRUE") || !strcasecmp(v, "YES")) return true;
    }
  }
  return false;
}

bool usb_msc_reboot_required(void) {
  return reboot_required;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  (void)lun;
  if (lba >= SECTOR_COUNT) return -1;
  if (offset > SECTOR_SIZE) return -1;
  if (bufsize > SECTOR_SIZE) return -1;
  if (offset + bufsize > SECTOR_SIZE) return -1;
  uint32_t addr = lba * SECTOR_SIZE + offset;
  memcpy(buffer, &disk[addr], bufsize);
  return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  if (lba >= SECTOR_COUNT) return -1;
  if (offset > SECTOR_SIZE) return -1;
  if (bufsize > SECTOR_SIZE) return -1;
  if (offset + bufsize > SECTOR_SIZE) return -1;
  uint32_t addr = lba * SECTOR_SIZE + offset;
  memcpy(&disk[addr], buffer, bufsize);
  wifi_scan_pending = true;
  return (int32_t)bufsize;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
  (void)lun;

  if (wifi_scan_pending) {
    char wifi_txt[512];
    uint32_t len = 0;
    if (read_wifi_file(wifi_txt, sizeof(wifi_txt), &len)) {
      if (usb_msc_handle_wifi_txt(wifi_txt, (unsigned)len)) {
        reboot_required = true;
        if (wifi_txt_requests_reboot(wifi_txt, len)) {
          DBG_PRINTF("REBOOT=1 requested in WIFI.TXT. Please reset device manually.\n");
        }
      }
    }
    wifi_scan_pending = false;
  }
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  const char vid[] = "JAMMA64 ";
  const char pid[] = "CONFIG DRIVE    ";
  const char rev[] = "1.0 ";
  memcpy(vendor_id, vid, 8);
  memcpy(product_id, pid, 16);
  memcpy(product_rev, rev, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  (void)lun;
  set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
  (void)lun;
  *block_count = SECTOR_COUNT;
  *block_size = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun;
  (void)power_condition;
  (void)start;
  (void)load_eject;
  return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
  void const *response = NULL;
  int32_t resplen = 0;
  (void)lun;

  static uint8_t inquiry_resp[36] = {
    0x00, 0x80, 0x05, 0x02, 36 - 4, 0, 0, 0,
    'J','A','M','M','A','6','4',' ',
    'C','O','N','F','I','G',' ','D','R','I','V','E',' ',' ',' ',
    '1','.','0',' '
  };
  uint8_t *resp = (uint8_t *)buffer;

  switch (scsi_cmd[0]) {
    case SCSI_CMD_INQUIRY:
      response = inquiry_resp;
      resplen = (int32_t)sizeof(inquiry_resp);
      set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
      break;

    case SCSI_CMD_REQUEST_SENSE: {
      if (bufsize < 18) return -1;
      memset(buffer, 0, 18);
      uint8_t *b = (uint8_t *)buffer;
      b[0] = 0x70;
      b[2] = sense_key;
      b[7] = 10;
      b[12] = sense_asc;
      b[13] = sense_ascq;
      response = buffer;
      resplen = 18;
      break;
    }

    case SCSI_CMD_TEST_UNIT_READY:
      set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
      resplen = 0;
      break;

    case SCSI_CMD_READ_CAPACITY_10:
      if (bufsize < 8) return -1;
      resp[0] = (uint8_t)(((SECTOR_COUNT - 1u) >> 24) & 0xFFu);
      resp[1] = (uint8_t)(((SECTOR_COUNT - 1u) >> 16) & 0xFFu);
      resp[2] = (uint8_t)(((SECTOR_COUNT - 1u) >> 8) & 0xFFu);
      resp[3] = (uint8_t)((SECTOR_COUNT - 1u) & 0xFFu);
      resp[4] = (uint8_t)((SECTOR_SIZE >> 24) & 0xFFu);
      resp[5] = (uint8_t)((SECTOR_SIZE >> 16) & 0xFFu);
      resp[6] = (uint8_t)((SECTOR_SIZE >> 8) & 0xFFu);
      resp[7] = (uint8_t)(SECTOR_SIZE & 0xFFu);
      response = buffer;
      resplen = 8;
      set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
      break;

    case SCSI_CMD_MODE_SENSE_6: {
      if (bufsize < 4) return -1;
      uint8_t *b = (uint8_t *)buffer;
      b[0] = 0x03;
      b[1] = 0x00;
      b[2] = 0x00;
      b[3] = 0x00;
      response = buffer;
      resplen = 4;
      set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
      break;
    }

    case SCSI_CMD_START_STOP_UNIT:
      set_sense(SCSI_SENSE_NONE, 0x00, 0x00);
      resplen = 0;
      break;

    default:
      set_sense(SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      resplen = -1;
      break;
  }

  if (resplen > (int32_t)bufsize) resplen = (int32_t)bufsize;
  if (response && resplen > 0 && response != buffer) memcpy(buffer, response, (size_t)resplen);
  return resplen;
}
