/* Physical input interface (#13): named UI/settings actions callable from
 * the HTTP endpoint (/api/input) and from buttons/encoders wired to a
 * PCF8574 or MCP23017 I2C expander polled in the background.
 *
 * Mapping string (settings input_map, configured from the web panel):
 *   "<chip>@<hexaddr>;<entry>;<entry>..."
 *   chip:  pcf (PCF8574, 8 pins) or mcp (MCP23017, 16 pins)
 *   entry: b<pin>s=<action>   button, short press
 *          b<pin>l=<action>   button, long press (>=500 ms)
 *          e<pinA>,<pinB>=<actionCW>,<actionCCW>   quadrature encoder
 *          g<gpio>s/l=<action>          button on a direct GPIO
 *          q<gpioA>,<gpioB>=<cw>,<ccw>  encoder on direct GPIOs
 * Direct GPIOs are limited to a conservative per-board safe-pin list
 * (Guition JC8048W550: 17 and 18); the chip token is optional for
 * GPIO-only maps.
 * Example: "pcf@20;b3s=zoom_in;b3l=wake;e0,1=alt_max_up,alt_max_down"
 * Buttons/encoder pins are active-low against the pull-ups. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void input_ctl_init(void);

/* Run a named action ("zoom_in", "next_view", ...). Thread-safe; returns
 * false for an unknown name. */
bool input_ctl_dispatch(const char *action);

/* Learn mode: the most recent raw physical event ("b3s", "e0+") and how
 * long ago it fired. Returns false when nothing was pressed yet. */
bool input_ctl_last_event(char *buf, size_t n, unsigned *age_ms);

/* Comma-separated list of valid action names (static string). */
const char *input_ctl_actions(void);
