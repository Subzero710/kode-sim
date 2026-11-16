import asyncio
import struct

import pytest

import kd
from emu_helpers import boot, console_line, firmware, frame_where, no_panic
from screen import Screen

pytestmark = pytest.mark.emu


@pytest.fixture(scope="module")
def display_lvgl(exe, kode_sdk):
    return firmware(exe, kode_sdk / "examples" / "display_lvgl", sdk=kode_sdk)


async def collect(k, screen, seconds):
    loop = asyncio.get_running_loop()
    end = loop.time() + seconds
    while loop.time() < end:
        try:
            _, p = await frame_where(k, lambda t, p: t == kd.FB_RECT, end - loop.time())
        except (asyncio.TimeoutError, TimeoutError):
            return
        screen.apply(p)


@pytest.fixture(scope="module")
def display_lvgl_board(exe, kode_sdk):
    return firmware(exe, kode_sdk / "examples" / "display_lvgl", sdk=kode_sdk, board=True)


async def tap(k, x, y):
    await k.send(kd.TOUCH, struct.pack("<BHH", 1, x, y))
    await asyncio.sleep(0.15)
    await k.send(kd.TOUCH, struct.pack("<BHH", 0, x, y))


async def test_display_lvgl_draws(exe, display_lvgl, tmp_path):
    k = await boot(exe, tmp_path, display_lvgl)
    try:
        await frame_where(k, lambda t, p: t == kd.HELLO, 30)
        screen = Screen()
        _, p = await frame_where(k, lambda t, p: t == kd.FB_RECT, 60)
        screen.apply(p)
        await collect(k, screen, 5)
        assert screen.lit_fraction() > 0.01, f"{screen.rects} rects, all dark"
        before = bytes(screen.pixels)
        await tap(k, 205, 251)
        await collect(k, screen, 3)
        assert bytes(screen.pixels) != before, "a tap did not change the screen"
        before = bytes(screen.pixels)
        await k.send(kd.BUTTONS, bytes([kd.BTN_TOP]))
        await asyncio.sleep(0.15)
        await k.send(kd.BUTTONS, bytes([0]))
        await collect(k, screen, 3)
        assert bytes(screen.pixels) != before, "TOP did not change the screen"
        no_panic(k)
    finally:
        await k.stop()


async def test_a_small_change_crosses_the_wire_small(exe, display_lvgl, tmp_path):
    """The UART is the simulator's bottleneck: every byte the firmware pushes
    through it is emulated, so a tap that changes a label must cost a strip
    of the screen, not the whole of it."""
    k = await boot(exe, tmp_path, display_lvgl)
    try:
        await frame_where(k, lambda t, p: t == kd.HELLO, 30)
        screen = Screen()
        await collect(k, screen, 8)
        before = bytes(screen.pixels)
        sent = await k.uart_bytes()
        await tap(k, 205, 251)
        await collect(k, screen, 3)
        assert bytes(screen.pixels) != before, "a tap did not change the screen"
        full = 410 * 502 * 2
        crossed = await k.uart_bytes() - sent
        assert crossed < full // 4, f"{crossed} bytes for a tap"
        no_panic(k)
    finally:
        await k.stop()


async def test_a_board_build_runs_unchanged(exe, display_lvgl_board, tmp_path):
    """A binary built for the board, no kemu in it: esp-emu's MIPI-DSI host
    shows what its own esp_lcd draws, the panel is lit from its DCS
    commands, a tap reaches it, and its console is its USB-Serial/JTAG."""
    k = await boot(exe, tmp_path, display_lvgl_board)
    try:
        _, lit = await frame_where(k, lambda t, p: t == kd.PANEL and p[0] == 1, 60)
        screen = Screen()
        await collect(k, screen, 8)
        assert screen.lit_fraction() > 0.01, f"{screen.rects} rects, all dark"
        assert not any(t == kd.HELLO for t, _ in k.backlog), "a board build said kemu's HELLO"
        before = bytes(screen.pixels)
        await tap(k, 205, 251)
        await collect(k, screen, 3)
        assert bytes(screen.pixels) != before, "a tap did not change the screen"
        await k.console_in("ping")
        await console_line(k, r">\|pong", 20)
        no_panic(k)
    finally:
        await k.stop()
