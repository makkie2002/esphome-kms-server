import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT
import esphome.components.sensor as sensor
import esphome.components.text_sensor as text_sensor

CODEOWNERS = ["@you"]
DEPENDENCIES = ["network"]

kms_server_ns = cg.esphome_ns.namespace("kms_server")
KmsServer = kms_server_ns.class_("KmsServer", cg.Component)

CONF_ACTIVATION_INTERVAL = "activation_interval"
CONF_RENEWAL_INTERVAL = "renewal_interval"
CONF_ACTIVATION_COUNT = "activation_count"
CONF_LAST_PRODUCT = "last_product"
CONF_LAST_CLIENT = "last_client"
CONF_LAST_VERSION = "last_version"
CONF_LAST_TIME = "last_time"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(KmsServer),
        cv.Optional(CONF_PORT, default=1688): cv.port,
        # Minutes, mirrors vlmcsd -A / -R defaults (2h / 7d).
        # KMS clients mostly ignore these on modern Windows, kept for completeness.
        cv.Optional(CONF_ACTIVATION_INTERVAL, default=120): cv.int_range(min=15, max=43200),
        cv.Optional(CONF_RENEWAL_INTERVAL, default=10080): cv.int_range(min=15, max=43200),
        cv.Optional(CONF_ACTIVATION_COUNT): sensor.sensor_schema(
            unit_of_measurement="activations",
            accuracy_decimals=0,
            state_class="total_increasing",
        ),
        cv.Optional(CONF_LAST_PRODUCT): text_sensor.text_sensor_schema(icon="mdi:monitor"),
        cv.Optional(CONF_LAST_CLIENT): text_sensor.text_sensor_schema(icon="mdi:ip-network"),
        cv.Optional(CONF_LAST_VERSION): text_sensor.text_sensor_schema(icon="mdi:information"),
        cv.Optional(CONF_LAST_TIME): text_sensor.text_sensor_schema(icon="mdi:clock"),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # NOTE: no -I flags. ESPHome's native IDF build only propagates -D/-W
    # build flags, so all vendored sources AND headers live at this top level:
    # top-level .c files are compiled into the build, and quoted sibling
    # includes resolve in the same directory. Missing IDF headers
    # (<sys/shm.h>, <sys/ipc.h>, <syslog.h>) are skipped in the vendored code
    # via __has_include (that code is compiled out by our config.h anyway).
    # Silence noisy upstream warnings, keep build log clean in HA addon.
    cg.add_build_flag("-Wno-unused-parameter")
    cg.add_build_flag("-Wno-missing-field-initializers")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_activation_interval(config[CONF_ACTIVATION_INTERVAL]))
    cg.add(var.set_renewal_interval(config[CONF_RENEWAL_INTERVAL]))

    if CONF_ACTIVATION_COUNT in config:
        sens = await sensor.new_sensor(config[CONF_ACTIVATION_COUNT])
        cg.add(var.set_activation_count_sensor(sens))
    if CONF_LAST_PRODUCT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_PRODUCT])
        cg.add(var.set_last_product_sensor(sens))
    if CONF_LAST_CLIENT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_CLIENT])
        cg.add(var.set_last_client_sensor(sens))
    if CONF_LAST_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_VERSION])
        cg.add(var.set_last_version_sensor(sens))
    if CONF_LAST_TIME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_TIME])
        cg.add(var.set_last_time_sensor(sens))
