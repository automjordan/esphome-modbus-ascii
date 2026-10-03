import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ADDRESS, CONF_FLOW_CONTROL_PIN, CONF_ID

DEPENDENCIES = ["uart"]
MULTI_CONF = True

CONF_COMMAND_THROTTLE = "command_throttle"
CONF_RESPONSE_TIMEOUT = "response_timeout"

modbus_ascii_ns = cg.esphome_ns.namespace("modbus_ascii")
ModbusAscii = modbus_ascii_ns.class_(
    "ModbusAscii", cg.PollingComponent, uart.UARTDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModbusAscii),
            cv.Optional(CONF_ADDRESS, default=1): cv.int_range(min=1, max=247),
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(
                CONF_COMMAND_THROTTLE, default="200ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RESPONSE_TIMEOUT, default="500ms"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.polling_component_schema("10s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_command_throttle(config[CONF_COMMAND_THROTTLE]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
