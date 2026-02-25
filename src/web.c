#include "web.h"
#include "profile.h"
#include "n64_virtual.h"
#include "pico/stdlib.h"

#include "lwip/apps/httpd.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

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
  if (!strcasecmp(name, "du") || !strcasecmp(name, "d_up")) { *out = N64_DU; return true; }
  if (!strcasecmp(name, "dd") || !strcasecmp(name, "d_down")) { *out = N64_DD; return true; }
  if (!strcasecmp(name, "dl") || !strcasecmp(name, "d_left")) { *out = N64_DL; return true; }
  if (!strcasecmp(name, "dr") || !strcasecmp(name, "d_right")) { *out = N64_DR; return true; }
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
    n64_virtual_analog_dir_t dir = N64_VANALOG_UP;
    if (parse_n64_analog_name(btn_name, &dir)) {
      n64_virtual_analog_press(dir, duration_ms);
    }
  }

  return "/index.shtml";
}

static const tCGI cgis[] = {
  { "/mode.cgi", cgi_mode_handler },
  { "/press.cgi", cgi_press_handler }
};

void web_init(void) {
  httpd_init();
  http_set_cgi_handlers(cgis, 2);
}
