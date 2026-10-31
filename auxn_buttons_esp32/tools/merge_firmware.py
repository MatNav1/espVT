#!/usr/bin/env python3
"""Create one flashable ESP32 image after a successful PlatformIO build."""

import pathlib
import subprocess


Import("env")  # Provided by PlatformIO/SCons.


def merge_firmware(source, target, env):
    project_dir = pathlib.Path(env["PROJECT_DIR"])
    build_dir = pathlib.Path(env.subst("$BUILD_DIR"))
    output_dir = project_dir.parent / "releases"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_file = output_dir / "espVT-AUX-N-ESP32U.bin"
    esptool = pathlib.Path(env.PioPlatform().get_package_dir("tool-esptoolpy")) / "esptool.py"

    command = [
        env.subst("$PYTHONEXE"),
        str(esptool),
        "--chip", "esp32",
        "merge_bin",
        "-o", str(output_file),
        "--flash_mode", "dio",
        "--flash_freq", "40m",
        "--flash_size", "4MB",
        "0x1000", str(build_dir / "bootloader.bin"),
        "0x8000", str(build_dir / "partitions.bin"),
        "0x10000", str(build_dir / "firmware.bin"),
    ]
    subprocess.run(command, check=True)
    print(f"Merged AUX-N firmware: {output_file}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)
