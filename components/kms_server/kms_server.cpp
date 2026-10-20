#include "kms_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include <Arduino.h>
#include <strings.h>

#include "esphome/core/log.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

// vlmcsd is plain C — keep C linkage.
extern "C" {
#include "types.h"
#include "kms.h"
#include "kmsdata.h"
#include "rpc.h"
#include "shared_globals.h"
#include "helpers.h"
#include "output.h"
#include "endian.h"
}

namespace esphome {
namespace kms_server {

static const char *const TAG = "kms_server";

// Instance for the C hook (single KMS server per device).
static KmsServer *s_instance = nullptr;
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;

// Upstream response callback we wrap to observe successful activations.
// v4/v5/v6 all funnel through CreateResponseBase — no vendored files touched.
static RequestCallback_t s_real_create_response = nullptr;

extern "C" HRESULT __stdcall kms_hook_create_response(REQUEST *base_request, RESPONSE *const base_response,
                                                      BYTE *const hw_id, const char *const ipstr) {
  HRESULT r = s_real_create_response(base_request, base_response, hw_id, ipstr);
  if (!FAILED(r) && base_request != nullptr && base_response != nullptr && KmsData != nullptr &&
      s_instance != nullptr) {
    char *product_name = nullptr;
    const int32_t total = KmsData->AppItemCount + KmsData->KmsItemCount + KmsData->SkuItemCount;
    int32_t index =
        getProductIndex(&base_request->ActID, KmsData->AppItemList, total, &product_name, nullptr);
    if (index < 0)
      index =
          getProductIndex(&base_request->KMSID, KmsData->AppItemList, total, &product_name, nullptr);
    if (index < 0)
      index =
          getProductIndex(&base_request->AppID, KmsData->AppItemList, total, &product_name, nullptr);
    char guidbuf[64];
    if (index < 0 || product_name == nullptr || strcasecmp(product_name, "Unknown") == 0) {
      uuid2StringLE(&base_request->ActID, guidbuf);
      product_name = guidbuf;
    }
    s_instance->record_activation(product_name, ipstr != nullptr ? ipstr : "?",
                                   LE16(base_request->MajorVer), LE16(base_request->MinorVer));
  }
  return r;
}

/* Required by vlmcsd/rpc.c (network.h):
 * send/recv exactly `len` bytes, return 1 on success, 0 on failure.
 * Maps directly onto lwIP BSD sockets on ESP32. */
extern "C" int_fast8_t sendrecv(SOCKET sock, BYTE *data, int len, int_fast8_t do_send) {
  int total = 0;
  while (total < len) {
    int n;
    if (do_send) {
      n = send(sock, data + total, len - total, 0);
    } else {
      n = recv(sock, data + total, len - total, 0);
    }
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }
    if (n == 0) {
      // Peer closed.
      return 0;
    }
    total += n;
  }
  return 1;
}

void KmsServer::record_activation(const char *product, const char *client, uint16_t vmaj,
                                  uint16_t vmin) {
  portENTER_CRITICAL(&s_pending_mux);
  strncpy(pending_.product, product != nullptr ? product : "?", sizeof(pending_.product) - 1);
  pending_.product[sizeof(pending_.product) - 1] = '\0';
  strncpy(pending_.client, client != nullptr ? client : "?", sizeof(pending_.client) - 1);
  pending_.client[sizeof(pending_.client) - 1] = '\0';
  snprintf(pending_.version, sizeof(pending_.version), "%u.%u", vmaj, vmin);
  time_t now = time(nullptr);
  if (now > 946684800L) {
    struct tm tmv {};
    gmtime_r(&now, &tmv);
    snprintf(pending_.time, sizeof(pending_.time), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  } else {
    pending_.time[0] = '\0';  // no SNTP yet — loop() falls back to uptime
  }
  pending_.dirty = true;
  portEXIT_CRITICAL(&s_pending_mux);
}

void KmsServer::install_hook() {
  if (s_real_create_response == nullptr) {
    s_real_create_response = CreateResponseBase;
    CreateResponseBase = kms_hook_create_response;
  }
}

void KmsServer::setup() {
  ESP_LOGCONFIG(TAG, "Initializing KMS server (vlmcsd port)...");
  s_instance = this;

  // Mirror vlmcsd -A / -R options.
  VLActivationInterval = activation_interval_;
  VLRenewalInterval = renewal_interval_;

  loadKmsData();
  if (KmsData == nullptr) {
    ESP_LOGE(TAG, "Failed to load internal KMS data (kmsdata.c). Check build.");
    mark_failed();
    return;
  }

  randomNumberInit();
  srand((unsigned) esp_random());
  install_hook();

  ESP_LOGI(TAG, "KMS data loaded, starting TCP :%u (v4/v5/v6, always-activated)", port_);

  BaseType_t ok = xTaskCreatePinnedToCore(
      &KmsServer::task_trampoline, "kms_server", 12288, this, 5, nullptr, 0);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create KMS server task");
    mark_failed();
    return;
  }
  running_ = true;
}

void KmsServer::loop() { publish_pending(); }

void KmsServer::publish_pending() {
  char product[48]{};
  char client[40]{};
  char version[16]{};
  char timestr[24]{};
  bool has = false;

  portENTER_CRITICAL(&s_pending_mux);
  if (pending_.dirty) {
    memcpy(product, pending_.product, sizeof(product));
    memcpy(client, pending_.client, sizeof(client));
    memcpy(version, pending_.version, sizeof(version));
    memcpy(timestr, pending_.time, sizeof(timestr));
    pending_.dirty = false;
    has = true;
  }
  portEXIT_CRITICAL(&s_pending_mux);

  if (!has)
    return;

  if (timestr[0] == '\0') {
    // No wall clock (time: component / SNTP not synced yet) — show uptime instead.
    uint32_t s = millis() / 1000;
    snprintf(timestr, sizeof(timestr), "up+%ud%02uh", (unsigned) (s / 86400),
             (unsigned) ((s % 86400) / 3600));
  }

  if (activation_count_sensor_ != nullptr)
    activation_count_sensor_->publish_state(activation_count_);
  if (last_product_sensor_ != nullptr)
    last_product_sensor_->publish_state(product);
  if (last_client_sensor_ != nullptr)
    last_client_sensor_->publish_state(client);
  if (last_version_sensor_ != nullptr)
    last_version_sensor_->publish_state(version);
  if (last_time_sensor_ != nullptr)
    last_time_sensor_->publish_state(timestr);
}

void KmsServer::dump_config() {
  ESP_LOGCONFIG(TAG, "KMS server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Activation interval: %u min", activation_interval_);
  ESP_LOGCONFIG(TAG, "  Renewal interval: %u min", renewal_interval_);
  ESP_LOGCONFIG(TAG, "  Activations served since boot: %u", activation_count_);
  ESP_LOGCONFIG(TAG, "  Protocol: KMS v4/v5/v6 (RPC over TCP)");
}

void KmsServer::task_trampoline(void *param) {
  static_cast<KmsServer *>(param)->server_task();
  vTaskDelete(nullptr);
}

void KmsServer::server_task() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed: %s", strerror(errno));
    return;
  }

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct timeval tv;
  tv.tv_sec = 30;
  tv.tv_usec = 0;
  setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(listen_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);

  if (bind(listen_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind(:%u) failed: %s", port_, strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  if (listen(listen_fd_, 4) < 0) {
    ESP_LOGE(TAG, "listen() failed: %s", strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  ESP_LOGI(TAG, "Listening on TCP :%u — point clients at <esp-ip>:%u", port_, port_);

  DWORD assoc_group = 0;
  while (true) {
    struct sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    int client = accept(listen_fd_, (struct sockaddr *) &peer, &peer_len);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      ESP_LOGW(TAG, "accept() failed: %s", strerror(errno));
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    char ipstr[48];
    snprintf(ipstr, sizeof(ipstr), "%s", inet_ntoa(peer.sin_addr));
    ESP_LOGD(TAG, "KMS client connected: %s", ipstr);

    // Blocking RPC dialog (bind + request/response). Returns on disconnect.
    rpcServer(client, assoc_group++, ipstr);
    close(client);

    activation_count_++;
    ESP_LOGI(TAG, "KMS activation served #%lu to %s", (unsigned long) activation_count_, ipstr);

    // Wake loop() promptly so HA sensors update even with slow polling.
    // The pending slot already holds product/client/version/time from the hook.
  }
}

}  // namespace kms_server
}  // namespace esphome
