import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor

from . import ModbusAscii, modbus_ascii_ns

DEPENDENCIES = ["modbus_ascii"]

CONF_MODBUS_ASCII_ID = "modbus_ascii_id"
CONF_REGISTER = "register"
CONF_SIGNED = "signed"
CONF_PASSIVE = "passive"

ModbusAsciiSensor = modbus_ascii_ns.class_("ModbusAsciiSensor", sensor.Sensor)

CONFIG_SCHEMA = sensor.sensor_schema(ModbusAsciiSensor).extend(
    {
        cv.GenerateID(CONF_MODBUS_ASCII_ID): cv.use_id(ModbusAscii),
        cv.Required(CONF_REGISTER): cv.uint16_t,
        cv.Optional(CONF_SIGNED, default=False): cv.boolean,
        # passive: non interrogato, aggiornato solo dal traffico altrui
        cv.Optional(CONF_PASSIVE, default=False): cv.boolean,
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    parent = await cg.get_variable(config[CONF_MODBUS_ASCII_ID])
    cg.add(var.set_register(config[CONF_REGISTER]))
    cg.add(var.set_signed(config[CONF_SIGNED]))
    cg.add(var.set_passive(config[CONF_PASSIVE]))
    cg.add(parent.register_sensor(var))
