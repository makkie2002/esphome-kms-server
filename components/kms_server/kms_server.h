#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <cstdint>

namespace esphome {
namespace sensor {
class Sensor;
}  // namespace sensor
namespace text_sensor {
class TextSensor;
}  // namespace text_sensor

namespace kms_server {

class KmsServer : public Component {
 public:
  void set_port(uint16_t port) { port_ = port; }
  void set_activation_interval(uint32_t minutes) { activation_interval_ = minutes; }
  void set_renewal_interval(uint32_t minutes) { renewal_interval_ = minutes; }

  void set_activation_count_sensor(sensor::Sensor *s) { activation_count_sensor_ = s; }
  void set_last_product_sensor(text_sensor::TextSensor *s) { last_product_sensor_ = s; }
  void set_last_client_sensor(text_sensor::TextSensor *s) { last_client_sensor_ = s; }
  void set_last_version_sensor(text_sensor::TextSensor *s) { last_version_sensor_ = s; }
  void set_last_time_sensor(text_sensor::TextSensor *s) { last_time_sensor_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Called from the KMS FreeRTOS task (via CreateResponseBase hook).
  // Only copies into a pending slot; publishing happens in loop().
  void record_activation(const char *product, const char *client, uint16_t vmaj, uint16_t vmin);

  uint32_t activation_count() const { return activation_count_; }

 protected:
  static void task_trampoline(void *param);
  void server_task();
  void install_hook();
  void publish_pending();

  uint16_t port_{1688};
  uint32_t activation_interval_{120};
  uint32_t renewal_interval_{10080};

  int listen_fd_{-1};
  bool running_{false};
  uint32_t activation_count_{0};

  sensor::Sensor *activation_count_sensor_{nullptr};
  text_sensor::TextSensor *last_product_sensor_{nullptr};
  text_sensor::TextSensor *last_client_sensor_{nullptr};
  text_sensor::TextSensor *last_version_sensor_{nullptr};
  text_sensor::TextSensor *last_time_sensor_{nullptr};

  // Pending activation filled by KMS task, consumed by loop().
  struct Pending {
    char product[48]{};
    char client[40]{};
    char version[16]{};
    char time[24]{};
    bool dirty{false};
  };
  Pending pending_{};
};

}  // namespace kms_server
}  // namespace esphome
