#include "web.h"
#include "profile.h"
#include "mapping_store.h"
#include "usb_msc.h"
#include "n64_virtual.h"
#include "pico/stdlib.h"

#include "lwip/apps/httpd.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifndef JAMMA64_FW_VERSION
#define JAMMA64_FW_VERSION "dev"
#endif

#ifndef JAMMA64_BUILD_DATE
#define JAMMA64_BUILD_DATE "unknown"
#endif

static size_t build_profiles_json(char *out, size_t out_max) {
  if (!out || out_max == 0u) return 0u;
  char names[MAP_STORE_MAX_PROFILES][MAP_STORE_NAME_MAX + 1];
  char active[MAP_STORE_NAME_MAX + 1];
  size_t count = mapping_store_list_names(
    names, MAP_STORE_MAX_PROFILES, active, sizeof(active));

  int n = snprintf(out, out_max, "{\"active\":\"%s\",\"names\":[", active);
  if (n < 0 || (size_t)n >= out_max) {
    out[0] = '\0';
    return 0u;
  }
  size_t used = (size_t)n;
  for (size_t i = 0; i < count; i++) {
    n = snprintf(out + used, out_max - used, "%s\"%s\"", (i ? "," : ""), names[i]);
    if (n < 0 || (size_t)n >= (out_max - used)) {
      out[0] = '\0';
      return 0u;
    }
    used += (size_t)n;
  }
  n = snprintf(out + used, out_max - used, "]}");
  if (n < 0 || (size_t)n >= (out_max - used)) {
    out[0] = '\0';
    return 0u;
  }
  used += (size_t)n;
  return used;
}

typedef struct {
  const char *arcade_key;
  phys_in_t phys;
} arcade_phys_map_t;

static const arcade_phys_map_t k_arcade_phys[] = {
  { "P1_LP", IN_P1_B1 }, { "P1_MP", IN_P1_B2 }, { "P1_HP", IN_P1_B3 },
  { "P1_LK", IN_P1_B4 }, { "P1_MK", IN_P1_B5 }, { "P1_HK", IN_P1_B6 },
  { "P1_START", IN_P1_START },
};

static const char *btn_code_for_output(n64_out_t out) {
  switch (out) {
    case N64_A: return "a";
    case N64_B: return "b";
    case N64_Z: return "z";
    case N64_START: return "start";
    case N64_L: return "l";
    case N64_R: return "r";
    case N64_CU: return "cu";
    case N64_CD: return "cd";
    case N64_CL: return "cl";
    case N64_CR: return "cr";
    case N64_DU: return "du";
    case N64_DD: return "dd";
    case N64_DL: return "dl";
    case N64_DR: return "dr";
    case N64_AU: return "au";
    case N64_AD: return "ad";
    case N64_AL: return "al";
    case N64_AR: return "ar";
    default: return "";
  }
}

static const char *find_btn_for_phys(const volatile profile_t *p, phys_in_t phys) {
  if (!p) return "";
  for (int out = 0; out < N64_OUTPUT_COUNT; out++) {
    if (p->map[out] == (uint8_t)phys) return btn_code_for_output((n64_out_t)out);
  }
  return "";
}

static size_t build_status_json(char *out, size_t out_max) {
  if (!out || out_max == 0u) return 0u;
  int n = snprintf(
    out, out_max,
    "{\"p1\":\"%s\",\"throw\":%u,\"diagpct\":%u,\"map\":{",
    g_profile.p1_stick_mode == STICK_MODE_ANALOG ? "analog" : "dpad",
    (unsigned)g_profile.analog_throw,
    (unsigned)g_profile.diagonal_scale_pct);
  if (n < 0 || (size_t)n >= out_max) return 0u;
  size_t used = (size_t)n;
  for (size_t i = 0; i < sizeof(k_arcade_phys) / sizeof(k_arcade_phys[0]); i++) {
    const char *btn = find_btn_for_phys(&g_profile, k_arcade_phys[i].phys);
    n = snprintf(out + used, out_max - used, "%s\"%s\":\"%s\"",
                 i ? "," : "", k_arcade_phys[i].arcade_key, btn);
    if (n < 0 || (size_t)n >= (out_max - used)) return 0u;
    used += (size_t)n;
  }
  n = snprintf(out + used, out_max - used, "}}");
  if (n < 0 || (size_t)n >= (out_max - used)) return 0u;
  used += (size_t)n;
  return used;
}

static bool parse_arcade_key(const char *key, phys_in_t *out) {
  if (!key || !out) return false;
  for (size_t i = 0; i < sizeof(k_arcade_phys) / sizeof(k_arcade_phys[0]); i++) {
    if (!strcasecmp(key, k_arcade_phys[i].arcade_key)) {
      *out = k_arcade_phys[i].phys;
      return true;
    }
  }
  return false;
}

static bool parse_btn_to_output(const char *btn, n64_out_t *out) {
  if (!btn || !out) return false;
  if (!strcasecmp(btn, "a")) { *out = N64_A; return true; }
  if (!strcasecmp(btn, "b")) { *out = N64_B; return true; }
  if (!strcasecmp(btn, "z")) { *out = N64_Z; return true; }
  if (!strcasecmp(btn, "start")) { *out = N64_START; return true; }
  if (!strcasecmp(btn, "l")) { *out = N64_L; return true; }
  if (!strcasecmp(btn, "r")) { *out = N64_R; return true; }
  if (!strcasecmp(btn, "cu")) { *out = N64_CU; return true; }
  if (!strcasecmp(btn, "cd")) { *out = N64_CD; return true; }
  if (!strcasecmp(btn, "cl")) { *out = N64_CL; return true; }
  if (!strcasecmp(btn, "cr")) { *out = N64_CR; return true; }
  if (!strcasecmp(btn, "du")) { *out = N64_DU; return true; }
  if (!strcasecmp(btn, "dd")) { *out = N64_DD; return true; }
  if (!strcasecmp(btn, "dl")) { *out = N64_DL; return true; }
  if (!strcasecmp(btn, "dr")) { *out = N64_DR; return true; }
  if (!strcasecmp(btn, "au")) { *out = N64_AU; return true; }
  if (!strcasecmp(btn, "ad")) { *out = N64_AD; return true; }
  if (!strcasecmp(btn, "al")) { *out = N64_AL; return true; }
  if (!strcasecmp(btn, "ar")) { *out = N64_AR; return true; }
  return false;
}

static void clear_assignments_for_phys(profile_t *p, phys_in_t phys) {
  for (int i = 0; i < N64_OUTPUT_COUNT; i++) {
    if (p->map[i] == (uint8_t)phys) p->map[i] = 0xFFu;
  }
}

static void apply_arcade_assignment(profile_t *p, phys_in_t phys, n64_out_t out) {
  if (!p) return;
  clear_assignments_for_phys(p, phys);
  p->map[out] = (uint8_t)phys;
}

static const char *cgi_map_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
  (void)iIndex;
  const char *action = "apply";
  const char *name = NULL;

  profile_t next = *(const profile_t *)&g_profile;
  bool changed = false;

  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "action")) action = pcValue[i];
    else if (!strcmp(pcParam[i], "name")) name = pcValue[i];
  }

  if (!strcasecmp(action, "status")) {
    return "/index.shtml";
  }

  if (!strcasecmp(action, "list")) {
    return "/index.shtml";
  }

  if (!strcasecmp(action, "load")) {
    if (!name || !mapping_store_load_named(name, &g_profile)) return "/index.shtml";
    usb_msc_refresh_files();
    return "/index.shtml";
  }

  if (!strcasecmp(action, "wipe")) {
    mapping_store_reset_defaults(&g_profile);
    usb_msc_refresh_files();
    return "/index.shtml";
  }

  for (int i = 0; i < iNumParams; i++) {
    phys_in_t phys;
    n64_out_t out;
    if (!strcmp(pcParam[i], "p1")) {
      if (!strcmp(pcValue[i], "dpad"))  { next.p1_stick_mode = STICK_MODE_DPAD; changed = true; }
      if (!strcmp(pcValue[i], "analog")) { next.p1_stick_mode = STICK_MODE_ANALOG; changed = true; }
      continue;
    }
    if (!strcmp(pcParam[i], "p2")) {
      if (!strcmp(pcValue[i], "dpad"))  { next.p2_stick_mode = STICK_MODE_DPAD; changed = true; }
      if (!strcmp(pcValue[i], "analog")) { next.p2_stick_mode = STICK_MODE_ANALOG; changed = true; }
      continue;
    }
    if (!strcmp(pcParam[i], "throw")) {
      int v = atoi(pcValue[i]);
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      next.analog_throw = (uint8_t)v;
      changed = true;
      continue;
    }
    if (!strcmp(pcParam[i], "diagpct")) {
      int v = atoi(pcValue[i]);
      if (v < 70) v = 70;
      if (v > 100) v = 100;
      next.diagonal_scale_pct = (uint8_t)v;
      changed = true;
      continue;
    }
    if (!parse_arcade_key(pcParam[i], &phys)) continue;
    if (!parse_btn_to_output(pcValue[i], &out)) continue;
    apply_arcade_assignment(&next, phys, out);
    changed = true;
  }

  if (changed) g_profile = next;
  if (!strcasecmp(action, "save") && name && name[0]) {
    if (mapping_store_save_named(name, &g_profile)) {
      usb_msc_refresh_files();
    }
  }
  return "/index.shtml";
}

static const char* cgi_mode_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
  (void)iIndex;
  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "p1")) {
      if (!strcmp(pcValue[i], "dpad"))  g_profile.p1_stick_mode = STICK_MODE_DPAD;
      if (!strcmp(pcValue[i], "analog")) g_profile.p1_stick_mode = STICK_MODE_ANALOG;
    }
    if (!strcmp(pcParam[i], "p2")) {
      if (!strcmp(pcValue[i], "dpad"))  g_profile.p2_stick_mode = STICK_MODE_DPAD;
      if (!strcmp(pcValue[i], "analog")) g_profile.p2_stick_mode = STICK_MODE_ANALOG;
    }
    if (!strcmp(pcParam[i], "throw")) {
      int v = atoi(pcValue[i]);
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      g_profile.analog_throw = (uint8_t)v;
    }
    if (!strcmp(pcParam[i], "diagpct")) {
      int v = atoi(pcValue[i]);
      if (v < 70) v = 70;
      if (v > 100) v = 100;
      g_profile.diagonal_scale_pct = (uint8_t)v;
    }
  }
  return "/index.shtml";
}

static bool parse_n64_button_name(const char *name, n64_out_t *out) {
  if (!name || !out) return false;
  if (!strcasecmp(name, "a")) { *out = N64_A; return true; }
  if (!strcasecmp(name, "b")) { *out = N64_B; return true; }
  if (!strcasecmp(name, "z")) { *out = N64_Z; return true; }
  if (!strcasecmp(name, "start")) { *out = N64_START; return true; }
  if (!strcasecmp(name, "l")) { *out = N64_L; return true; }
  if (!strcasecmp(name, "r")) { *out = N64_R; return true; }
  if (!strcasecmp(name, "cu") || !strcasecmp(name, "c_up")) { *out = N64_CU; return true; }
  if (!strcasecmp(name, "cd") || !strcasecmp(name, "c_down")) { *out = N64_CD; return true; }
  if (!strcasecmp(name, "cl") || !strcasecmp(name, "c_left")) { *out = N64_CL; return true; }
  if (!strcasecmp(name, "cr") || !strcasecmp(name, "c_right")) { *out = N64_CR; return true; }
  return false;
}

static bool parse_n64_dpad_name(const char *name, n64_virtual_dpad_dir_t *dir) {
  if (!name || !dir) return false;
  if (!strcasecmp(name, "du") || !strcasecmp(name, "d_up")) { *dir = N64_VDPAD_UP; return true; }
  if (!strcasecmp(name, "dd") || !strcasecmp(name, "d_down")) { *dir = N64_VDPAD_DOWN; return true; }
  if (!strcasecmp(name, "dl") || !strcasecmp(name, "d_left")) { *dir = N64_VDPAD_LEFT; return true; }
  if (!strcasecmp(name, "dr") || !strcasecmp(name, "d_right")) { *dir = N64_VDPAD_RIGHT; return true; }
  return false;
}

static bool parse_n64_analog_name(const char *name, n64_virtual_analog_dir_t *dir) {
  if (!name || !dir) return false;
  if (!strcasecmp(name, "au") || !strcasecmp(name, "analog_up")) { *dir = N64_VANALOG_UP; return true; }
  if (!strcasecmp(name, "ad") || !strcasecmp(name, "analog_down")) { *dir = N64_VANALOG_DOWN; return true; }
  if (!strcasecmp(name, "al") || !strcasecmp(name, "analog_left")) { *dir = N64_VANALOG_LEFT; return true; }
  if (!strcasecmp(name, "ar") || !strcasecmp(name, "analog_right")) { *dir = N64_VANALOG_RIGHT; return true; }
  return false;
}

static const char* cgi_press_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
  (void)iIndex;
  const char *btn_name = NULL;
  uint32_t duration_ms = 80;

  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "btn")) {
      btn_name = pcValue[i];
    } else if (!strcmp(pcParam[i], "ms")) {
      int v = atoi(pcValue[i]);
      if (v > 0) duration_ms = (uint32_t)v;
    }
  }

  n64_out_t out = N64_A;
  if (parse_n64_button_name(btn_name, &out)) {
    n64_virtual_press(out, duration_ms);
  } else {
    n64_virtual_dpad_dir_t dpad = N64_VDPAD_UP;
    if (parse_n64_dpad_name(btn_name, &dpad)) {
      n64_virtual_dpad_press(dpad, duration_ms);
    } else {
      n64_virtual_analog_dir_t dir = N64_VANALOG_UP;
      if (parse_n64_analog_name(btn_name, &dir)) {
        n64_virtual_analog_press(dir, duration_ms);
      }
    }
  }

  return "/index.shtml";
}

static const tCGI cgis[] = {
  { "/mode.cgi", cgi_mode_handler },
  { "/press.cgi", cgi_press_handler },
  { "/map.cgi", cgi_map_handler }
};

static u16_t ssi_handler(
    int iIndex,
    char *pcInsert,
    int iInsertLen,
    u16_t current_tag_part,
    u16_t *next_tag_part) {
  (void)current_tag_part;
  (void)next_tag_part;
  if (!pcInsert || iInsertLen <= 0) return 0;

  switch (iIndex) {
    case 0: { // fw
      int n = snprintf(pcInsert, (size_t)iInsertLen, "%s", JAMMA64_FW_VERSION);
      return (u16_t)((n > 0) ? n : 0);
    }
    case 1: { // build
      int n = snprintf(pcInsert, (size_t)iInsertLen, "%s", JAMMA64_BUILD_DATE);
      return (u16_t)((n > 0) ? n : 0);
    }
    case 2: { // profiles json
      char tmp[192];
      size_t used = build_profiles_json(tmp, sizeof(tmp));
      if (used == 0u) {
        int n = snprintf(pcInsert, (size_t)iInsertLen, "{\"active\":\"\",\"names\":[]}");
        return (u16_t)((n > 0) ? n : 0);
      }
      int n = snprintf(pcInsert, (size_t)iInsertLen, "%s", tmp);
      return (u16_t)((n > 0) ? n : 0);
    }
    case 3: { // status json
      char tmp[320];
      size_t used = build_status_json(tmp, sizeof(tmp));
      if (used == 0u) {
        int n = snprintf(pcInsert, (size_t)iInsertLen, "{\"p1\":\"dpad\",\"throw\":80,\"diagpct\":100,\"map\":{}}");
        return (u16_t)((n > 0) ? n : 0);
      }
      int n = snprintf(pcInsert, (size_t)iInsertLen, "%s", tmp);
      return (u16_t)((n > 0) ? n : 0);
    }
    default:
      return 0;
  }
}

static const char *ssi_tags[] = {
  "fw",
  "build",
  "profiles",
  "status"
};

void web_init(void) {
  httpd_init();
  http_set_cgi_handlers(cgis, 3);
  http_set_ssi_handler(ssi_handler, ssi_tags, 4);
}
