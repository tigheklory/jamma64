#pragma once

// Standalone lwIP options for JAMMA64 (no pico-examples dependency)

// Use the no-OS lwIP mode that Pico W examples use.
#define NO_SYS                     1

// ---- Memory / pbuf pool ----
#define MEM_LIBC_MALLOC            0
#define MEM_ALIGNMENT              4
#define MEM_SIZE                   (16 * 1024)

#define MEMP_NUM_PBUF              16
#define MEMP_NUM_TCP_PCB           12
#define MEMP_NUM_TCP_PCB_LISTEN    4
#define MEMP_NUM_TCP_SEG           16

#define PBUF_POOL_SIZE             24
#define PBUF_POOL_BUFSIZE          1520

// ---- TCP/IP basics ----
#define LWIP_ARP                   1
#define LWIP_ETHERNET              1
#define LWIP_RAW                   1
#define LWIP_TCP                   1
#define LWIP_UDP                   1
#define LWIP_ICMP                  1

#define LWIP_DHCP                  1
#define LWIP_DNS                   1

// Threadsafe background arch is doing the OS glue; keep sockets off unless you need them
#define LWIP_SOCKET                0
#define LWIP_NETCONN               0

// ---- mDNS (optional; you can remove if you don't need it yet) ----
#define LWIP_MDNS_RESPONDER        1
#define LWIP_IGMP                  1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_RESP_USENETIF_EXTCALLBACK 1
#define MEMP_NUM_SYS_TIMEOUT       (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3)

// ---- HTTPD ----
#define LWIP_HTTPD                 1
#define LWIP_HTTPD_CGI             1
#define LWIP_HTTPD_SSI             1
#define LWIP_HTTPD_SSI_MULTIPART   1
#define LWIP_HTTPD_SUPPORT_POST    0
#define LWIP_HTTPD_SSI_INCLUDE_TAG 0
// map.cgi sends many query params (mode/throw/diag + P1/P2 mapping keys).
#define LWIP_HTTPD_MAX_CGI_PARAMETERS 32
// /api.shtml injects JSON via SSI; keep this large enough to avoid truncation.
#define LWIP_HTTPD_MAX_TAG_INSERT_LEN 1024

// Generated file containing html data
#define HTTPD_FSDATA_FILE          "pico_fsdata.inc"

// (Optional) Debug off
#define LWIP_STATS                 0
#define LWIP_DEBUG                 0
