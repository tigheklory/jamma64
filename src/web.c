#include "web.h"
#include "profile.h"
#include "pico/stdlib.h"

#include "lwip/apps/httpd.h"
#include <string.h>
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

static const tCGI cgis[] = {
  { "/mode.cgi", cgi_mode_handler }
};

void web_init(void) {
  httpd_init();
  http_set_cgi_handlers(cgis, 1);
}
