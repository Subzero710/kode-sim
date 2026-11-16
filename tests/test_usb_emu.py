"""USB through a real P4 firmware, ESP-TinyUSB 2.2.0 and the real esp-emu.

No fake_emu fixture is used. The direct variant links the same components
but owns its task/descriptors, detecting accidental extraction of ESP's API.
"""
import asyncio
import json
import subprocess
import re
import shlex
import shutil
from pathlib import Path

import pytest

import kd
from emu_helpers import boot, console_line, firmware, no_panic
from conftest import CACHE
from sim import next_json, ws

pytestmark = pytest.mark.emu
FIXTURE = Path(__file__).parent / "fixtures" / "usb"


@pytest.fixture(scope="module", params=[(False, None), (True, None), (True, "0.19.0~3")],
                ids=["esp_tinyusb", "direct", "direct_0_19"])
def usb_firmware(request, exe, kode_sdk):
    direct, version = request.param
    name = (f"direct_{version}" if version else "direct") if direct else "esp_tinyusb"
    source = CACHE / "usb-fixtures" / name
    shutil.copytree(FIXTURE, source, dirs_exist_ok=True)
    if direct:
        with (source / "sdkconfig.defaults").open("a") as f:
            f.write("\nCONFIG_USB_TEST_DIRECT=y\n")
    if version:
        with (source / "main" / "idf_component.yml").open("a") as f:
            f.write(f'  espressif/tinyusb: "=={version}"\n')
    fw = firmware(exe, source, sdk=kode_sdk)
    verify_linkage(fw, direct)
    return fw, direct


def component_versions(fw):
    description = json.loads((fw["elf"].parent / "project_description.json").read_text())
    lock = (Path(description["project_path"]) / "dependencies.lock").read_text()
    versions = {}
    for name in ("espressif/esp_tinyusb", "espressif/tinyusb"):
        section = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  \S|\Z)", lock, re.M | re.S)
        assert section, f"{name} absent from the resolved lockfile"
        versions[name] = re.search(r"^    version: ['\"]?([^'\"\s]+)", section[1], re.M)[1]
    assert versions["espressif/esp_tinyusb"] == "2.2.0", versions
    return versions


def verify_linkage(fw, direct):
    """Check the produced binary, including the weak same-object DCD call."""
    cache = (fw["elf"].parent / "CMakeCache.txt").read_text()
    def tool(name):
        match = re.search(rf"^CMAKE_{name}:[^=]+=(.+)$", cache, re.M)
        assert match, f"CMAKE_{name} missing from the build cache"
        return match[1]
    symbols = subprocess.check_output([tool("NM"), "-g", str(fw["elf"])], text=True)
    assert re.search(r" T dcd_deinit$", symbols, re.M), "TinyUSB's weak DCD deinit was not overridden"
    disasm = subprocess.check_output([tool("OBJDUMP"), "-d", "--disassemble=tud_deinit", str(fw["elf"])], text=True)
    assert "<dcd_deinit>" in disasm, disasm
    assert "<__wrap_dcd_int_disable>" in disasm, disasm
    event = subprocess.check_output([tool("OBJDUMP"), "-d", "--disassemble=dcd_event_handler", str(fw["elf"])], text=True)
    assert "<__wrap_xQueueGenericSend>" in event, event
    assert not re.search(r" T dcd_init$", symbols, re.M), "Hardware DWC2 driver was linked"
    task = subprocess.check_output([tool("OBJDUMP"), "-d", "--disassemble=__wrap_tud_task_ext", str(fw["elf"])], text=True)
    assert "<tud_task_ext>" in task, task
    assert re.search(r" T __wrap_vTaskDelete$", symbols, re.M), "Consumer deletion barrier not linked"
    if not direct:
        stop = subprocess.check_output([tool("OBJDUMP"), "-d", "--disassemble=tinyusb_task_stop", str(fw["elf"])], text=True)
        assert "<__wrap_vTaskDelete>" in stop, stop
    real_driver = bool(re.search(r" T tinyusb_driver_install$", symbols, re.M))
    assert real_driver != direct, "ESP-TinyUSB must link only for users of its API"
    if not direct:
        assert re.search(r" T tinyusb_task_start$", symbols, re.M), "Official ESP-TinyUSB task was not linked"


def queue_depth(fw):
    # Read the exact TinyUSB translation unit's preprocessor configuration;
    # its queue default is private to usbd.c and is not an application macro.
    commands = json.loads((fw["elf"].parent / "compile_commands.json").read_text())
    entry = next(c for c in commands if Path(c["file"]).name == "usbd.c")
    args = shlex.split(entry["command"])
    output = args.index("-o")
    del args[output:output + 2]
    args.remove("-c")
    defines = subprocess.check_output([*args, "-E", "-dM"], cwd=entry["directory"], text=True)
    value = re.search(r"^#define CFG_TUD_TASK_QUEUE_SZ (.+)$", defines, re.M)
    assert value, "TinyUSB event queue configuration not found"
    return int(value[1], 0)


async def result(k, command, expected="ESP_OK", timeout=90):
    """Only the answer to this command, including repeated identical commands."""
    answer = asyncio.get_running_loop().create_future()
    lines = []

    def receive(line):
        lines.append(line)
        if line.startswith(f"USB result {command} ") and not answer.done():
            answer.set_result(line)

    k.on_line.append(receive)
    try:
        await k.console_in(command)
        line = await asyncio.wait_for(answer, timeout)
        assert line == f"USB result {command} {expected}", "\n".join(lines)
        return lines
    finally:
        k.on_line.remove(receive)


async def cable(w, command, stage):
    await w.send_str(json.dumps({"usb": {"cmd": command}}))
    return (await next_json(w, lambda j: j.get("usb", {}).get("stage") == stage, timeout=120))["usb"]


async def keyboard(w):
    u = (await next_json(w, lambda j: j.get("usb", {}).get("stage") == "keyboard", timeout=120))["usb"]
    assert u["cable"] and not u["error"]
    assert u["device"] == {"vid": "303a", "pid": "4004", "manufacturer": "kode-sim",
                           "product": "ESP-TinyUSB lifecycle", "class": "keyboard"}
    assert any("SET_CONFIGURATION 1" in line and "0x81" in line for line in u["log"])
    return u


async def typed(k, w):
    await w.send_str(json.dumps({"usb": {"cmd": "clear_typed"}}))
    await next_json(w, lambda j: j.get("usb", {}).get("typed") == "")
    await result(k, "report")
    await next_json(w, lambda j: j.get("usb", {}).get("typed") == "a", timeout=60)


async def test_real_stack_enumerates_reconnects_and_reinstalls(exe, usb_firmware, tmp_path):
    fw, direct = usb_firmware
    k = await boot(exe, tmp_path, fw)
    w = None
    try:
        await console_line(k, r"USB result install ESP_OK", 120)
        versions = await console_line(k, r"USB versions idf=v6\.1(?:[ .-]|$)", 30)
        resolved = component_versions(fw)["espressif/tinyusb"].split("~", 1)[0]
        assert f"tinyusb={resolved}" in versions, versions
        port_line = await console_line(k, r"USB port TINYUSB_PORT_FULL_SPEED_0=\d+", 30)
        port = re.search(r"=(\d+)", port_line)[1]
        w = await ws(k.sim)
        await w.send_str(json.dumps({"usb": {"cmd": "plug"}}))
        await keyboard(w)
        await console_line(k, rf"USB attached 1 rhport={port} speed=full configuration=1", 30)
        await typed(k, w)
        await result(k, "install", "ESP_ERR_INVALID_STATE")
        await result(k, "bad_phy", "ESP_ERR_INVALID_ARG")
        await cable(w, "unplug", "no_cable")
        await console_line(k, rf"USB detached 1 rhport={port}", 60)
        await w.send_str(json.dumps({"usb": {"cmd": "plug"}}))
        await keyboard(w)
        await console_line(k, rf"USB attached 2 rhport={port} speed=full configuration=1", 60)
        await typed(k, w)

        await result(k, "mask")
        await cable(w, "reset", "waiting")
        await result(k, "unmask")
        await keyboard(w)
        await typed(k, w)

        await result(k, "disconnect")
        await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
        await result(k, "connect")
        await keyboard(w)
        await typed(k, w)

        # A deferred reset/SETUP must disappear at teardown, including when
        # no PHY exists (ESP-TinyUSB's skip_setup branch below).
        await result(k, "mask")
        await k.send(kd.USB_BUS, bytes([4]))
        await k.send(kd.USB_SETUP, bytes([0x80, 6, 0, 1, 0, 0, 18, 0]))
        await result(k, "uninstall")
        await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
        await result(k, "uninstall", "ESP_ERR_INVALID_STATE")
        await k.send(kd.USB_SETUP, bytes([0x80, 6, 0, 1, 0, 0, 18, 0]))
        await k.send(kd.USB_OUT, bytes([0, 0]))
        await k.send(kd.USB_BUS, bytes([2]))
        await k.send(kd.USB_BUS, bytes([3]))
        await result(k, "unmask")
        await result(k, "install")
        await keyboard(w)
        await typed(k, w)

        if not direct:
            await result(k, "uninstall")
            await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
            await result(k, "invalid", "ESP_ERR_INVALID_ARG")
            await result(k, "bad_task", "ESP_ERR_INVALID_ARG")
            await result(k, "high_speed", "ESP_ERR_NOT_SUPPORTED")
            await result(k, "high_speed_skip_phy", "ESP_ERR_NOT_SUPPORTED")
            await result(k, "bad_descriptors", "ESP_ERR_TIMEOUT", timeout=180)
            lines = await result(k, "race", timeout=180)
            raced = [line.rsplit(" ", 1)[-1] for line in lines if line.startswith("USB race ")]
            assert sorted(raced) == ["ESP_ERR_INVALID_STATE", "ESP_OK"], lines
            await keyboard(w)
            await typed(k, w)
            await result(k, "uninstall")
            await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
            await result(k, "skip_phy")
            await keyboard(w)
            await result(k, "install", "ESP_ERR_INVALID_STATE")
            await typed(k, w)
            await result(k, "uninstall")
            await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
            await result(k, "install")
            await keyboard(w)

        if direct:
            # Force a blocked real dcd_event_handler: the application's own
            # task is stopped with a full queue before tusb_deinit(). The
            # emulator must cancel pending IRQ delivery without hanging or
            # sending into the queue after the stack frees it.
            depth = queue_depth(fw)
            await result(k, "pause_consumer")
            for _ in range(depth + 2):
                await k.send(kd.USB_SETUP, bytes([0x80, 6, 0, 1, 0, 0, 18, 0]))
                await result(k, "yield")
            await result(k, "uninstall")
            await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
            await result(k, "install")
            await keyboard(w)
            await typed(k, w)

        # Exercise the delivery/teardown overlap repeatedly with a live
        # consumer, without masking interrupts explicitly first.
        for _ in range(3):
            await result(k, "report")
            await result(k, "uninstall")
            await next_json(w, lambda j: j.get("usb", {}).get("stage") == "waiting")
            await result(k, "install")
            await keyboard(w)
        no_panic(k)
    finally:
        if w is not None:
            await w.close()
            await w.session.close()
        await k.stop()
