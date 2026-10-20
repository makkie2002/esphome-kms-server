#ifndef CONFIG_H_ESP32_ESPHOME
#define CONFIG_H_ESP32_ESPHOME

/* ESPHome KMS server (ESP32) — vlmcsd port config.
 * Based on upstream vlmcsd config.h, tuned for ESP32 + ESPHome.
 * Upstream: https://github.com/Wind4/vlmcsd (archived, Hotbird64)
 * Docker reference: https://github.com/mikolatero/docker-vlmcsd
 */

#define VERSION "esphome-kms 1.0 (vlmcsd)"

/* Fixed HWID, same default as upstream (Ratiborus VM). */
#ifndef HWID
#define HWID 0x3A, 0x1C, 0x04, 0x96, 0x00, 0xB6, 0x00, 0x76
#endif

/* ---- Minimal / embedded-friendly feature set ---- */
#define NO_TAP
#define NO_INI_FILE
#define NO_PID_FILE
#define NO_USER_SWITCH
#define NO_HELP
#define NO_CUSTOM_INTERVALS
#define NO_PRIVATE_IP_DETECT
#define NO_SIGHUP
#define NO_LIMIT
#define NO_VERBOSE_LOG
#define NO_LOG
#define NO_DNS
#define NO_EXTERNAL_DATA
#define UNSAFE_DATA_LOAD
#define NO_FREEBIND
#define NO_CL_PIDS
#define NO_VERSION_INFORMATION
#define NO_RANDOM_EPID

/* Always-activated, no client counting (saves RAM, no shm/sem needed).
 * Behaves like vlmcsd -E0 -M0 -K0 : first request activates. */
#define NO_STRICT_MODES
#define NO_CLIENT_LIST

/* Full product database so HA sensors show friendly names
 * (e.g. "Windows 11 Pro") instead of GUIDs. Same as kmserver-esp32. */
#define FULL_INTERNAL_DATA

/* Single listen socket, we manage accept() ourselves. */
#define SIMPLE_SOCKETS

/* Suppress getExeName() (needs /proc, realpath) — unused with NO_EXTERNAL_DATA. */
#define DATA_FILE "-"

/* Keep full RPC (NDR64 + BTFN) for Win10/11 + Office compat.
 * Do NOT define SIMPLE_RPC. */

#endif /* CONFIG_H_ESP32_ESPHOME */
