import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["esp32"]

CONF_REPORT_TYPE = "report_type"
CONF_LOG_RAW_REPORTS = "log_raw_reports"
CONF_ON_TOUCH_START = "on_touch_start"
CONF_ON_TOUCH_END = "on_touch_end"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"

muteme_ns = cg.esphome_ns.namespace("muteme")
MuteMeComponent = muteme_ns.class_("MuteMeComponent", cg.Component)

TouchStartTrigger = muteme_ns.class_("TouchStartTrigger", automation.Trigger.template())
TouchEndTrigger = muteme_ns.class_("TouchEndTrigger", automation.Trigger.template())
ConnectedTrigger = muteme_ns.class_("ConnectedTrigger", automation.Trigger.template())
DisconnectedTrigger = muteme_ns.class_("DisconnectedTrigger", automation.Trigger.template())

_TRIGGERS = {
    CONF_ON_TOUCH_START: TouchStartTrigger,
    CONF_ON_TOUCH_END: TouchEndTrigger,
    CONF_ON_CONNECTED: ConnectedTrigger,
    CONF_ON_DISCONNECTED: DisconnectedTrigger,
}


def _require_esp_idf(config):
    # ESPHome 2026.x removed cv.only_with_esp_idf; check the framework directly.
    from esphome.core import CORE

    using_idf = getattr(CORE, "using_esp_idf", None)
    if using_idf is False:
        raise cv.Invalid("muteme needs the ESP-IDF framework (esp32: framework: type: esp-idf)")
    return config


def _only_s2_s3(config):
    # USB host needs the ESP32-S2/S3 OTG peripheral.
    try:
        from esphome.components.esp32 import get_esp32_variant
        from esphome.components.esp32.const import VARIANT_ESP32S2, VARIANT_ESP32S3

        if get_esp32_variant() not in (VARIANT_ESP32S2, VARIANT_ESP32S3):
            raise cv.Invalid("muteme needs an ESP32-S2 or ESP32-S3 (USB OTG host)")
    except ImportError:
        pass
    return config


_schema = {
    cv.GenerateID(): cv.declare_id(MuteMeComponent),
    cv.Optional(CONF_REPORT_TYPE, default="feature"): cv.one_of(
        "feature", "output", lower=True
    ),
    cv.Optional(CONF_LOG_RAW_REPORTS, default=True): cv.boolean,
}
for _key, _trig in _TRIGGERS.items():
    _schema[cv.Optional(_key)] = automation.validate_automation(
        {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(_trig)}
    )

CONFIG_SCHEMA = cv.All(
    cv.Schema(_schema).extend(cv.COMPONENT_SCHEMA),
    _require_esp_idf,
    _only_s2_s3,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_feature_report(config[CONF_REPORT_TYPE] == "feature"))
    cg.add(var.set_log_raw(config[CONF_LOG_RAW_REPORTS]))

    for key in _TRIGGERS:
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)

    # Newer ESPHome builds leave unused ESP-IDF components out; pull the USB host one back in.
    try:
        from esphome.components.esp32 import include_builtin_idf_component

        include_builtin_idf_component("usb")
    except ImportError:
        pass
