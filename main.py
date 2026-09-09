"""
Orpheus Pico -- LED status display
====================================
Blue LED   : blinks steadily whenever the Pico is running (heartbeat).
             Blinks FAST if it hasn't heard from the ESP32 in a while
             (link lost / ESP32 off / wiring problem).
Red/Yellow/Green : whichever one matches the current temperature "level"
             sent by the ESP32 blinks; the other two stay off.
RGB LED      : smooth rainbow animation, toggled with the button.

Save this file as main.py on the Pico so it runs automatically on boot
(e.g. with Thonny: File > Save As > Raspberry Pi Pico > main.py).

============ CONFIRMED WIRING (Orpheus Pico, labeled IO0-IO5 on silkscreen) ============
  LEDs (each through a ~220-330 ohm resistor to GND):
    Blue   -> IO2 / GP2
    Red    -> IO3 / GP3
    Yellow -> IO4 / GP4
    Green  -> IO5 / GP5
    RGB LED  -> IO24 / GP24 (single-wire NeoPixel-style LED)
    Button   -> IO23 / GP23 (connect to GND when pressed)

  UART0 link to the ESP32-C3:
    IO0/GP0 (UART0 TX) -> ESP32 RX (GPIO20)
    IO1/GP1 (UART0 RX) <- ESP32 TX (GPIO21)
    GND (3rd pin)      -- ESP32 G   <-- REQUIRED, even with separate power
                                        supplies for each board.
=========================================================================================
"""

from machine import Pin, UART
import neopixel
import time

led_blue = Pin(2, Pin.OUT)
led_red = Pin(3, Pin.OUT)
led_yellow = Pin(4, Pin.OUT)
led_green = Pin(5, Pin.OUT)
rgb_led = neopixel.NeoPixel(Pin(24), 1)
button = Pin(23, Pin.IN, Pin.PULL_UP)

uart = UART(0, baudrate=115200, tx=Pin(0), rx=Pin(1))

LINK_TIMEOUT_MS = 10_000     # consider the ESP32 "gone" after this long
HEARTBEAT_MS = 500           # blue blink period when link is fine
HEARTBEAT_FAST_MS = 150      # blue blink period when link is lost
LEVEL_BLINK_MS = 400         # red/yellow/green blink period
RGB_UPDATE_MS = 20           # animation update interval
BUTTON_DEBOUNCE_MS = 40

current_level = None
last_rx_ms = time.ticks_ms()
rx_buf = b""
rgb_enabled = False


def set_rgb_color(color):
    rgb_led[0] = color
    rgb_led.write()


def rainbow_color(position):
    """Return a smooth RGB color for a hue position from 0 through 1535."""
    position %= 1536
    section = position // 256
    offset = position % 256
    if section == 0:
        return (255, offset, 0)
    if section == 1:
        return (255 - offset, 255, 0)
    if section == 2:
        return (0, 255, offset)
    if section == 3:
        return (0, 255 - offset, 255)
    if section == 4:
        return (offset, 0, 255)
    return (255, 0, 255 - offset)


def parse_line(line):
    """Parse a line like: LEVEL:GREEN,TEMP:23.45"""
    global current_level, last_rx_ms
    try:
        fields = dict(part.split(":", 1) for part in line.strip().split(","))
        level = fields.get("LEVEL")
        if level in ("GREEN", "YELLOW", "RED"):
            current_level = level
            last_rx_ms = time.ticks_ms()
    except Exception:
        pass  # ignore malformed / partial lines


def poll_uart():
    global rx_buf
    if uart.any():
        rx_buf += uart.read()
        while b"\n" in rx_buf:
            line, rx_buf = rx_buf.split(b"\n", 1)
            parse_line(line.decode("utf-8", "ignore"))


blue_on = False
last_blue_toggle = time.ticks_ms()
level_on = False
last_level_toggle = time.ticks_ms()
button_state = button.value()
last_button_change_ms = time.ticks_ms()
rgb_position = 0
last_rgb_update_ms = time.ticks_ms()
set_rgb_color((0, 0, 0))

while True:
    poll_uart()

    now = time.ticks_ms()
    link_lost = time.ticks_diff(now, last_rx_ms) > LINK_TIMEOUT_MS

    # Toggle the RGB animation on a debounced button press.
    button_value = button.value()
    if button_value != button_state:
        if time.ticks_diff(now, last_button_change_ms) >= BUTTON_DEBOUNCE_MS:
            previous_button_state = button_state
            button_state = button_value
            last_button_change_ms = now
            if previous_button_state == 1 and button_state == 0:
                rgb_enabled = not rgb_enabled
                if not rgb_enabled:
                    set_rgb_color((0, 0, 0))

    if rgb_enabled and time.ticks_diff(now, last_rgb_update_ms) >= RGB_UPDATE_MS:
        rgb_position = (rgb_position + 4) % 1536
        set_rgb_color(rainbow_color(rgb_position))
        last_rgb_update_ms = now

    # Blue heartbeat -- always blinking, faster if the link is down
    heartbeat_period = HEARTBEAT_FAST_MS if link_lost else HEARTBEAT_MS
    if time.ticks_diff(now, last_blue_toggle) >= heartbeat_period:
        blue_on = not blue_on
        led_blue.value(blue_on)
        last_blue_toggle = now

    # Shared blink clock for the level LEDs
    if time.ticks_diff(now, last_level_toggle) >= LEVEL_BLINK_MS:
        level_on = not level_on
        last_level_toggle = now

    active_level = None if link_lost else current_level
    led_red.value(1 if (active_level == "RED" and level_on) else 0)
    led_yellow.value(1 if (active_level == "YELLOW" and level_on) else 0)
    led_green.value(1 if (active_level == "GREEN" and level_on) else 0)

    time.sleep_ms(20)
