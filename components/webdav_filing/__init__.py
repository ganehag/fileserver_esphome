# SPDX-License-Identifier: MIT
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import socket
from esphome.components.esp32 import (
    include_builtin_idf_component,
    require_fatfs,
    require_vfs_dir,
    set_idf_sdkconfig_default,
)
from esphome.const import CONF_AUTH, CONF_ID, CONF_PASSWORD, CONF_TYPE, CONF_USERNAME

DEPENDENCIES = ["wifi"]

DOMAIN = "webdav_filing"

CONF_HTTP = "http"
CONF_STORAGE = "storage"
CONF_SD = "sd"

CONF_PORT = "port"
CONF_BASE_PATH = "base_path"

# max_open_sockets counts client sessions only; HTTPD adds three internal
# sockets (listen plus two control sockets). Music Assistant probes many files
# in parallel, so the old four client sockets made some GETs time out while
# others worked. One worker per client socket keeps GETs off the HTTPD loop.
HTTPD_MAX_OPEN_SOCKETS = 12
HTTPD_BACKLOG_CONN = 16
HTTPD_MAX_URI_LEN = 4096
HTTPD_MAX_REQ_HDR_LEN = 8192

CONF_BUS = "bus"
CONF_CLK_PIN = "clk_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_CS_PIN = "cs_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_D0_PIN = "d0_pin"
CONF_D1_PIN = "d1_pin"
CONF_D2_PIN = "d2_pin"
CONF_D3_PIN = "d3_pin"
CONF_FREQUENCY = "frequency"
CONF_SLOT = "slot"
CONF_WIDTH = "width"
CONF_PWR_LDO_CHANNEL = "pwr_ldo_channel"

SD_BUS_SPI = "spi"
SD_BUS_SDMMC = "sdmmc"

AUTH_TYPE_BASIC = "basic"
AUTH_TYPE_DIGEST = "digest"

webdav_filing_ns = cg.esphome_ns.namespace("webdav_filing")
WebDAVFiling = webdav_filing_ns.class_("WebDAVFiling", cg.Component)

PIN_SCHEMA = pins.internal_gpio_output_pin_number


def validate_base_path(value):
    """Normalize an HTTP-visible mount prefix such as /dav."""
    if not isinstance(value, str):
        raise cv.Invalid("base_path must be a string")
    if value in ("", "/"):
        return "/"
    if not value.startswith("/") or len(value) > 128:
        raise cv.Invalid("base_path must start with / and be at most 128 characters")
    parts = value.split("/")[1:]
    if any(part in ("", ".", "..") for part in parts):
        raise cv.Invalid("base_path contains an empty or relative path segment")
    if any(not (part.isalnum() or all(c in "-_." for c in part)) for part in parts):
        raise cv.Invalid("base_path segments may only contain letters, digits, -, _, and .")
    return "/" + "/".join(parts)


def validate_sd_bus(config):
    """Fill in bus-specific SD defaults and check the required pins."""
    sd = config[CONF_STORAGE][CONF_SD]
    if sd[CONF_BUS] == SD_BUS_SPI:
        sd.setdefault(CONF_CLK_PIN, 18)
        sd.setdefault(CONF_MISO_PIN, 19)
        sd.setdefault(CONF_MOSI_PIN, 23)
        sd.setdefault(CONF_CS_PIN, 5)
        return config

    # Waveshare ESP32-P4 boards wire the microSD card to SDMMC slot 0's
    # dedicated pins; C6 Wi-Fi uses slot 1 through esp32_hosted.
    sd.setdefault(CONF_CLK_PIN, 43)
    sd.setdefault(CONF_CMD_PIN, 44)
    sd.setdefault(CONF_D0_PIN, 39)
    if sd[CONF_WIDTH] == 4:
        sd.setdefault(CONF_D1_PIN, 40)
        sd.setdefault(CONF_D2_PIN, 41)
        sd.setdefault(CONF_D3_PIN, 42)
    return config


AUTH_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TYPE, default=AUTH_TYPE_DIGEST): cv.one_of(
            AUTH_TYPE_BASIC, AUTH_TYPE_DIGEST, lower=True
        ),
        cv.Required(CONF_USERNAME): cv.All(cv.string_strict, cv.Length(min=1)),
        cv.Required(CONF_PASSWORD): cv.sensitive(
            cv.All(cv.string_strict, cv.Length(min=1))
        ),
    }
)

HTTP_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_PORT, default=8080): cv.int_range(min=1, max=65535),
        cv.Optional(CONF_BASE_PATH, default="/"): validate_base_path,
        cv.Optional(CONF_AUTH): AUTH_SCHEMA,
    }
)

SD_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BUS, default=SD_BUS_SPI): cv.one_of(SD_BUS_SPI, SD_BUS_SDMMC),
        cv.Optional(CONF_CLK_PIN): PIN_SCHEMA,
        cv.Optional(CONF_MISO_PIN): PIN_SCHEMA,
        cv.Optional(CONF_MOSI_PIN): PIN_SCHEMA,
        cv.Optional(CONF_CS_PIN): PIN_SCHEMA,
        cv.Optional(CONF_CMD_PIN): PIN_SCHEMA,
        cv.Optional(CONF_D0_PIN): PIN_SCHEMA,
        cv.Optional(CONF_D1_PIN): PIN_SCHEMA,
        cv.Optional(CONF_D2_PIN): PIN_SCHEMA,
        cv.Optional(CONF_D3_PIN): PIN_SCHEMA,
        cv.Optional(CONF_FREQUENCY, default="4MHz"): cv.frequency,
        cv.Optional(CONF_SLOT, default=0): cv.one_of(0, 1, int=True),
        cv.Optional(CONF_WIDTH, default=4): cv.one_of(1, 4, int=True),
        cv.Optional(CONF_PWR_LDO_CHANNEL, default=0): cv.int_range(min=0, max=4),
    }
)

STORAGE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SD, default={}): SD_SCHEMA,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebDAVFiling),
            cv.Optional(CONF_HTTP, default={}): HTTP_SCHEMA,
            cv.Optional(CONF_STORAGE, default={}): STORAGE_SCHEMA,
        }
    ),
    validate_sd_bus,
    cv.only_on_esp32,
    # esp_http_server needs one listening socket, its two UDP control sockets,
    # and enough client sockets for parallel media probes.
    socket.consume_sockets(1, DOMAIN, socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(HTTPD_MAX_OPEN_SOCKETS, DOMAIN, socket.SocketType.TCP),
    socket.consume_sockets(2, DOMAIN, socket.SocketType.UDP),
)


def sd_config_expression(storage):
    """Build an SdConfig value from storage.sd."""
    sd = storage[CONF_SD]
    bus = "SDMMC" if sd[CONF_BUS] == SD_BUS_SDMMC else "SPI"
    fields = [
        f"esphome::webdav_filing::SdBus::{bus}",
        str(sd.get(CONF_CLK_PIN, -1)),
        str(sd.get(CONF_MISO_PIN, -1)),
        str(sd.get(CONF_MOSI_PIN, -1)),
        str(sd.get(CONF_CS_PIN, -1)),
        str(sd.get(CONF_CMD_PIN, -1)),
        str(sd.get(CONF_D0_PIN, -1)),
        str(sd.get(CONF_D1_PIN, -1)),
        str(sd.get(CONF_D2_PIN, -1)),
        str(sd.get(CONF_D3_PIN, -1)),
        str(sd[CONF_WIDTH]),
        str(sd[CONF_SLOT]),
        str(sd[CONF_PWR_LDO_CHANNEL]),
        str(int(sd[CONF_FREQUENCY])),
    ]
    return cg.RawExpression(
        "esphome::webdav_filing::SdConfig{" + ", ".join(fields) + "}"
    )


async def to_code(config):
    # ESPHome excludes these IDF components by default; opt in explicitly.
    include_builtin_idf_component("esp_http_server")
    include_builtin_idf_component("fatfs")
    include_builtin_idf_component("esp_driver_gpio")
    include_builtin_idf_component("esp_driver_spi")
    include_builtin_idf_component("esp_driver_sdspi")
    include_builtin_idf_component("esp_driver_sdmmc")
    include_builtin_idf_component("sdmmc")
    require_fatfs()
    require_vfs_dir()
    cg.add_define("WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS", HTTPD_MAX_OPEN_SOCKETS)
    cg.add_define("WEBDAV_FILING_HTTPD_BACKLOG_CONN", HTTPD_BACKLOG_CONN)
    cg.add_define("WEBDAV_FILING_HTTPD_MAX_URI_LEN", HTTPD_MAX_URI_LEN)
    cg.add_define("WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN", HTTPD_MAX_REQ_HDR_LEN)
    set_idf_sdkconfig_default("CONFIG_HTTPD_MAX_URI_LEN", HTTPD_MAX_URI_LEN)
    set_idf_sdkconfig_default("CONFIG_HTTPD_MAX_REQ_HDR_LEN", HTTPD_MAX_REQ_HDR_LEN)

    http = config[CONF_HTTP]
    storage = config[CONF_STORAGE]
    var = cg.new_Pvariable(
        config[CONF_ID],
        http[CONF_PORT],
        sd_config_expression(storage),
    )
    await cg.register_component(var, config)
    cg.add(var.set_base_path(http[CONF_BASE_PATH]))
    if (auth := http.get(CONF_AUTH)) is not None:
        cg.add(
            var.set_auth(
                auth[CONF_USERNAME],
                auth[CONF_PASSWORD],
                auth[CONF_TYPE] == AUTH_TYPE_DIGEST,
            )
        )
