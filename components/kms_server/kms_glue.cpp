// kms_glue.cpp - ESPHome/ESP-IDF platform glue for the vendored vlmcsd core.
//
// The upstream vlmcsd sources expect a POSIX host (helpers.c/output.c provide
// getopt/file/syslog/SHM plumbing). Like kmserver-esp32's esp_support.cpp we do
// NOT compile helpers.c/output.c; instead this file implements exactly the
// symbols the compiled core (kms.c/rpc.c/crypto/endian/kmsdata/shared_globals)
// needs: sockets, logging via ESP_LOG, hardware-RNG seeding, UCS-2 helpers
// and the compiled-in KMS data loader.
//
// All functions have C linkage so kms.c/rpc.c link cleanly.

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "esp_system.h"

#include "esphome/core/log.h"

static const char *const GLUE_TAG = "kms_server";

extern "C" {
#ifndef CONFIG
#define CONFIG "config.h"
#endif
#include CONFIG

#include "types.h"
#include "kms.h"
#include "kmsdata.h"
#include "helpers.h"
#include "output.h"
#include "network.h"
#include "shared_globals.h"
#include "endian.h"

// ---------------------------------------------------------------------------
// Logging -> ESP_LOG (logger() is compiled out upstream via NO_LOG)
// ---------------------------------------------------------------------------

int printerrorf(const char *const fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ESP_LOGE(GLUE_TAG, "%s", buf);
  return n;
}

int errorout(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ESP_LOGE(GLUE_TAG, "%s", buf);
  return n;
}

void uuid2StringLE(const GUID *const guid, char *const string) {
  // NB: guid->Data4 is only 4-byte aligned; memcpy the 8 bytes instead of a
  // direct uint64_t load (faults on strict-alignment CPUs).
  uint64_t tail;
  memcpy(&tail, guid->Data4, sizeof(tail));
  sprintf(string, "%08x-%04x-%04x-%04x-%012llx", (unsigned int) LE32(guid->Data1),
          (unsigned int) LE16(guid->Data2), (unsigned int) LE16(guid->Data3),
          (unsigned int) BE16(*(uint16_t *) guid->Data4),
          (unsigned long long) BE64(tail) & 0xffffffffffffLL);
}

// Verbose logging disabled via NO_VERBOSE_LOG; stubs keep the linker happy.
void logRequestVerbose(REQUEST *Request, const PRINTFUNC p) {
  (void) Request;
  (void) p;
}
void logResponseVerbose(const char *const ePID, const BYTE *const hwid, RESPONSE *response,
                        const PRINTFUNC p) {
  (void) ePID;
  (void) hwid;
  (void) response;
  (void) p;
}

// ---------------------------------------------------------------------------
// Network backend (lwIP BSD sockets). The KMS FreeRTOS task in
// kms_server.cpp owns the listen socket; these satisfy network.h.
// ---------------------------------------------------------------------------

int listenOnAllAddresses(void) {
  s_server = socket(AF_INET, SOCK_STREAM, 0);
  if (s_server == INVALID_SOCKET) {
    printerrorf("Fatal: socket() failed: %s\n", strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(s_server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(1688);

  if (bind(s_server, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    printerrorf("Fatal: bind(:1688) failed: %s\n", strerror(errno));
    close(s_server);
    s_server = INVALID_SOCKET;
    return -1;
  }
  if (listen(s_server, 4) < 0) {
    printerrorf("Fatal: listen() failed: %s\n", strerror(errno));
    close(s_server);
    s_server = INVALID_SOCKET;
    return -1;
  }
  return 0;
}

void closeAllListeningSockets(void) {
  if (s_server != INVALID_SOCKET) {
    close(s_server);
    s_server = INVALID_SOCKET;
  }
}

int_fast8_t isDisconnected(const SOCKET s) {
  char buf[1];
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0)
    return TRUE;
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  int n = recv(s, buf, 1, MSG_PEEK);
  fcntl(s, F_SETFL, flags);
  if (n == 0)
    return TRUE;
  return FALSE;
}

// ---------------------------------------------------------------------------
// Memory / fatal errors
// ---------------------------------------------------------------------------

__noreturn void OutOfMemory(void) {
  ESP_LOGE(GLUE_TAG, "Fatal: Out of memory");
  abort();
  while (1) {
  }
}

void *vlmcsd_malloc(size_t len) {
  void *buf = malloc(len);
  if (!buf)
    OutOfMemory();
  return buf;
}

char *vlmcsd_strdup(const char *src) {
  char *dst = strdup(src);
  if (!dst)
    OutOfMemory();
  return dst;
}

void exitOnWarningLevel(const int_fast8_t level) {
  (void) level;  // warnings never abort on ESP32
}

// ---------------------------------------------------------------------------
// RNG (seeded from hardware RNG)
// ---------------------------------------------------------------------------

void randomNumberInit(void) { srand(esp_random()); }

// ---------------------------------------------------------------------------
// UTF-8 <-> UCS-2 helpers (verbatim upstream logic - pure code)
// ---------------------------------------------------------------------------

WCHAR utf8_to_ucs2_char(const unsigned char *input, const unsigned char **end_ptr) {
  *end_ptr = input;
  if (input[0] == 0)
    return (WCHAR) ~0;
  if (input[0] < 0x80) {
    *end_ptr = input + 1;
    return LE16(input[0]);
  }
  if ((input[0] & 0xE0) == 0xE0) {
    if (input[1] == 0 || input[2] == 0)
      return (WCHAR) ~0;
    *end_ptr = input + 3;
    return LE16((input[0] & 0x0F) << 12 | (input[1] & 0x3F) << 6 | (input[2] & 0x3F));
  }
  if ((input[0] & 0xC0) == 0xC0) {
    if (input[1] == 0)
      return (WCHAR) ~0;
    *end_ptr = input + 2;
    return LE16((input[0] & 0x1F) << 6 | (input[1] & 0x3F));
  }
  return (WCHAR) ~0;
}

int ucs2_to_utf8_char(const WCHAR ucs2_le, char *utf8) {
  const WCHAR ucs2 = LE16(ucs2_le);
  if (ucs2 < 0x80) {
    utf8[0] = (char) ucs2;
    utf8[1] = '\0';
    return 1;
  }
  if (ucs2 >= 0x80 && ucs2 < 0x800) {
    utf8[0] = (char) ((ucs2 >> 6) | 0xC0);
    utf8[1] = (char) ((ucs2 & 0x3F) | 0x80);
    utf8[2] = '\0';
    return 2;
  }
  if (ucs2 >= 0x800 && ucs2 < 0xFFFF) {
    if (ucs2 >= 0xD800 && ucs2 <= 0xDFFF)
      return -1;
    utf8[0] = ((ucs2 >> 12)) | 0xE0;
    utf8[1] = ((ucs2 >> 6) & 0x3F) | 0x80;
    utf8[2] = ((ucs2) & 0x3F) | 0x80;
    utf8[3] = '\0';
    return 3;
  }
  return -1;
}

size_t utf8_to_ucs2(WCHAR *const ucs2_le, const char *const utf8, const size_t maxucs2,
                    const size_t maxutf8) {
  const unsigned char *current_utf8 = (unsigned char *) utf8;
  WCHAR *current_ucs2_le = ucs2_le;
  for (; *current_utf8; current_ucs2_le++) {
    size_t size = (char *) current_utf8 - utf8;
    if (size >= maxutf8)
      return (size_t) -1;
    if (((*current_utf8 & 0xc0) == 0xc0) && (size >= maxutf8 - 1))
      return (size_t) -1;
    if (((*current_utf8 & 0xe0) == 0xe0) && (size >= maxutf8 - 2))
      return (size_t) -1;
    if (current_ucs2_le - ucs2_le >= (intptr_t) maxucs2 - 1)
      return (size_t) -1;
    *current_ucs2_le = utf8_to_ucs2_char(current_utf8, &current_utf8);
    current_ucs2_le[1] = 0;
    if (*current_ucs2_le == (WCHAR) -1)
      return (size_t) -1;
  }
  return current_ucs2_le - ucs2_le;
}

BOOL ucs2_to_utf8(const WCHAR *const ucs2_le, char *utf8, size_t maxucs2, size_t maxutf8) {
  char utf8_char[4];
  const WCHAR *current_ucs2 = ucs2_le;
  unsigned int index_utf8 = 0;
  for (*utf8 = 0; *current_ucs2; current_ucs2++) {
    if (current_ucs2 - ucs2_le > (intptr_t) maxucs2)
      return FALSE;
    int len = ucs2_to_utf8_char(*current_ucs2, utf8_char);
    if (len < 0 || index_utf8 + (unsigned) len > maxutf8)
      return FALSE;
    strncat(utf8, utf8_char, len);
    index_utf8 += len;
  }
  return TRUE;
}

void hex2bin(BYTE *const bin, const char *hex, const size_t maxbin) {
  static const char *const hexdigits = "0123456789ABCDEF";
  const unsigned char *nextchar;
  size_t i;
  for (i = 0; (i < 16) && utf8_to_ucs2_char((const unsigned char *) hex, &nextchar) != (WCHAR) -1;
       hex = (const char *) nextchar) {
    const char *pos = strchr(hexdigits, toupper((int) *hex));
    if (!pos)
      continue;
    if (!(i & 1))
      bin[i >> 1] = 0;
    bin[i >> 1] |= (char) (pos - hexdigits);
    if (!(i & 1))
      bin[i >> 1] <<= 4;
    i++;
    if (i >> 1 > maxbin)
      break;
  }
}

BOOL stringToInt(const char *const szValue, const unsigned int min, const unsigned int max,
                 unsigned int *const value) {
  char *nextchar;
  errno = 0;
  long long result = strtoll(szValue, &nextchar, 10);
  if (errno || result < (long long) min || result > (long long) max || *nextchar)
    return FALSE;
  *value = (unsigned int) result;
  return TRUE;
}

int_fast8_t string2UuidLE(const char *const restrict input, GUID *const restrict guid) {
  int i;
  if (strlen(input) < GUID_STRING_LENGTH)
    return FALSE;
  if (input[8] != '-' || input[13] != '-' || input[18] != '-' || input[23] != '-')
    return FALSE;
  for (i = 0; i < GUID_STRING_LENGTH; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23)
      continue;
    const char c = (char) toupper((int) input[i]);
    if (c < '0' || c > 'F' || (c > '9' && c < 'A'))
      return FALSE;
  }
  char inputCopy[GUID_STRING_LENGTH + 1];
  strncpy(inputCopy, input, GUID_STRING_LENGTH + 1);
  inputCopy[8] = inputCopy[13] = inputCopy[18] = 0;
  hex2bin((BYTE *) &guid->Data1, inputCopy, 8);
  hex2bin((BYTE *) &guid->Data2, inputCopy + 9, 4);
  hex2bin((BYTE *) &guid->Data3, inputCopy + 14, 4);
  hex2bin(guid->Data4, input + 19, 16);
  guid->Data1 = BS32(guid->Data1);
  guid->Data2 = BS16(guid->Data2);
  guid->Data3 = BS16(guid->Data3);
  return TRUE;
}

// Unused on ESP32 (no CLI / DNS / ini file) - stubs for the linker.
unsigned int getOptionArgumentInt(const char o, const unsigned int min, const unsigned int max) {
  (void) o;
  (void) min;
  (void) max;
  return min;
}
void optReset(void) {}
__pure DWORD timeSpanString2Seconds(const char *const restrict argument) {
  (void) argument;
  return 120;
}
char *win_strerror(const int message) {
  (void) message;
  return (char *) "error";
}
void parseAddress(char *const addr, char **szHost, char **szPort) {
  (void) addr;
  *szHost = NULL;
  *szPort = NULL;
}
void getExeName(void) {}
__pure BOOL getArgumentBool(int_fast8_t *result, const char *const argument) {
  (void) result;
  (void) argument;
  return FALSE;
}

// ---------------------------------------------------------------------------
// KMS data (compiled-in only, NO_EXTERNAL_DATA)
// ---------------------------------------------------------------------------

void loadKmsData(void) {
  KmsData = (PVlmcsdHeader_t) DefaultKmsData;
  size_t size = getDefaultKmsDataSize();

  KmsData->MajorVer = LE16(KmsData->MajorVer);
  KmsData->MinorVer = LE16(KmsData->MinorVer);
  KmsData->AppItemCount = LE32(KmsData->AppItemCount);
  KmsData->KmsItemCount = LE32(KmsData->KmsItemCount);
  KmsData->SkuItemCount = LE32(KmsData->SkuItemCount);
  KmsData->HostBuildCount = LE32(KmsData->HostBuildCount);

  uint32_t i;
  for (i = 0; i < vlmcsd_countof(KmsData->Datapointers); i++) {
    KmsData->Datapointers[i].Pointer = (BYTE *) KmsData + LE64(KmsData->Datapointers[i].Offset);
  }
  for (i = 0; i < KmsData->CsvlkCount; i++) {
    PCsvlkData_t csvlkData = &KmsData->CsvlkData[i];
    csvlkData->EPid = (char *) KmsData + LE64(csvlkData->EPidOffset);
    csvlkData->ReleaseDate = LE64(csvlkData->ReleaseDate);
    csvlkData->GroupId = LE32(csvlkData->GroupId);
    csvlkData->MinKeyId = LE32(csvlkData->MinKeyId);
    csvlkData->MaxKeyId = LE32(csvlkData->MaxKeyId);
  }
  for (i = 0; i < (uint32_t) KmsData->HostBuildCount; i++) {
    PHostBuild_t hostBuild = &KmsData->HostBuildList[i];
    hostBuild->BuildNumber = LE32(hostBuild->BuildNumber);
    hostBuild->Flags = (HostBuildFlag) LE32(hostBuild->Flags);
    hostBuild->PlatformId = LE32(hostBuild->PlatformId);
    hostBuild->ReleaseDate = LE64(hostBuild->ReleaseDate);
    hostBuild->DisplayName = (char *) KmsData + LE64(hostBuild->DisplayNameOffset);
  }
  const uint32_t totalItemCount = KmsData->AppItemCount + KmsData->KmsItemCount + KmsData->SkuItemCount;
  for (i = 0; i < totalItemCount; i++) {
    PVlmcsdData_t item = &KmsData->AppItemList[i];
    item->Name = (char *) KmsData + LE64(item->NameOffset);
  }
  (void) size;
}

}  // extern "C"
