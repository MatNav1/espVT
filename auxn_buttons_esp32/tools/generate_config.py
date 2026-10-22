#!/usr/bin/env python3
"""Generate the ESP32 pin table and AUX-N object pool from config/buttons.json."""

import json
import pathlib
import struct


try:
    Import("env")  # Provided by PlatformIO/SCons.
    PROJECT_DIR = pathlib.Path(env["PROJECT_DIR"])
except NameError:
    PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
CONFIG_PATH = PROJECT_DIR / "config" / "buttons.json"
GENERATED_DIR = PROJECT_DIR / "src" / "generated"

# Pins with internal pull-ups that are practical for buttons on ESP32-WROOM.
# Boot strapping pins and the UART/CAN pins used by this project are omitted.
ALLOWED_BUTTON_PINS = {4, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33}

NULL_OBJECT_ID = 0xFFFF
WORKING_SET = 0
MAIN_MASK = 1
TITLE_STRING = 2
TITLE_FONT = 3
DESIGNATOR_FONT = 4
BUTTON_ID_BASE = 200
DESIGNATOR_ID_BASE = 400

TYPE_WORKING_SET = 0
TYPE_DATA_MASK = 1
TYPE_OUTPUT_STRING = 11
TYPE_FONT_ATTRIBUTES = 23
TYPE_AUXILIARY_INPUT_TYPE_2 = 32
FUNCTION_TYPE_BOOLEAN_MOMENTARY = 2


def u8(value):
    return struct.pack("<B", value & 0xFF)


def u16(value):
    return struct.pack("<H", value & 0xFFFF)


def i16(value):
    return struct.pack("<h", value)


def working_set():
    return (u16(WORKING_SET) + u8(TYPE_WORKING_SET) + u8(0) + u8(0) +
            u16(MAIN_MASK) + u8(1) + u8(0) + u8(0) +
            u16(MAIN_MASK) + i16(0) + i16(0))


def data_mask():
    return (u16(MAIN_MASK) + u8(TYPE_DATA_MASK) + u8(1) + u16(NULL_OBJECT_ID) +
            u8(1) + u8(0) + u16(TITLE_STRING) + i16(10) + i16(10))


def output_string(object_id, width, height, font_id, text, justify=0):
    encoded = text.encode("ascii")
    return (u16(object_id) + u8(TYPE_OUTPUT_STRING) + u16(width) + u16(height) +
            u8(1) + u16(font_id) + u8(0) + u16(NULL_OBJECT_ID) + u8(justify) +
            u16(len(encoded)) + encoded + u8(0))


def font(object_id, size):
    return u16(object_id) + u8(TYPE_FONT_ATTRIBUTES) + u8(0) + u8(size) + u8(0) + u8(0) + u8(0)


def aux_input(object_id, designator_id):
    return (u16(object_id) + u8(TYPE_AUXILIARY_INPUT_TYPE_2) + u8(0) +
            u8(FUNCTION_TYPE_BOOLEAN_MOMENTARY) + u8(1) +
            u16(designator_id) + i16(0) + i16(0))


def load_buttons():
    data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    buttons = data.get("buttons", [])
    if not 1 <= len(buttons) <= 32:
        raise ValueError("buttons.json must contain between 1 and 32 buttons")

    seen = set()
    for index, button in enumerate(buttons, 1):
        pin = button.get("pin")
        label = button.get("label", f"B{index}")
        if pin not in ALLOWED_BUTTON_PINS:
            raise ValueError(f"GPIO{pin} is not allowed for a pulled-up button")
        if pin in seen:
            raise ValueError(f"GPIO{pin} is used more than once")
        try:
            label.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError(f"button label {label!r} must use ASCII characters") from exc
        if not 1 <= len(label) <= 6:
            raise ValueError(f"button label {label!r} must be 1..6 characters")
        seen.add(pin)
        button["label"] = label
    return buttons


def generate():
    buttons = load_buttons()
    button_ids = [BUTTON_ID_BASE + i for i in range(len(buttons))]

    pool = bytearray()
    pool += working_set()
    pool += data_mask()
    pool += output_string(TITLE_STRING, 180, 24, TITLE_FONT, "AUX-N Buttons")
    pool += font(TITLE_FONT, 3)
    pool += font(DESIGNATOR_FONT, 2)  # 8x12, fits labels up to 6 chars in 60 px
    for index, button in enumerate(buttons):
        designator_id = DESIGNATOR_ID_BASE + index
        pool += output_string(designator_id, 60, 60, DESIGNATOR_FONT,
                              button["label"], justify=0b0001)
        pool += aux_input(button_ids[index], designator_id)

    GENERATED_DIR.mkdir(parents=True, exist_ok=True)
    config_lines = [
        "// Generated from config/buttons.json. Do not edit.",
        "#pragma once",
        "#include <stdint.h>",
        f"static constexpr uint8_t NUM_AUX_BUTTONS = {len(buttons)};",
        "static constexpr uint8_t BUTTON_PINS[NUM_AUX_BUTTONS] = {" +
        ", ".join(str(item["pin"]) for item in buttons) + "};",
        "static constexpr uint16_t AUX_BUTTON_IDS[NUM_AUX_BUTTONS] = {" +
        ", ".join(str(item) for item in button_ids) + "};",
        "",
    ]
    (GENERATED_DIR / "device_config.hpp").write_text("\n".join(config_lines), encoding="ascii")

    byte_lines = []
    for offset in range(0, len(pool), 16):
        byte_lines.append("    " + ", ".join(f"0x{x:02X}" for x in pool[offset:offset + 16]) + ",")
    pool_lines = [
        "// Generated from config/buttons.json. Do not edit.",
        "#pragma once",
        "#include <stdint.h>",
        f"static constexpr uint32_t AUX_N_POOL_SIZE = {len(pool)};",
        "static const uint8_t AUX_N_POOL_DATA[] = {",
        *byte_lines,
        "};",
        "",
    ]
    (GENERATED_DIR / "aux_n_pool_data.hpp").write_text("\n".join(pool_lines), encoding="ascii")
    (GENERATED_DIR / "aux_n_pool.iop").write_bytes(pool)
    print(f"Generated AUX-N configuration for {len(buttons)} buttons")


if __name__ == "__main__":
    generate()
else:
    # PlatformIO/SCons pre-build hook.
    generate()
