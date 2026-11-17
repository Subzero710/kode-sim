/**
 * @file kemu_radio.c
 * @brief The radio an app talks to, without the ESP32-C5 behind it.
 *
 * The Kode Dot's WiFi and BLE are the C5's, reached over ESP-Hosted; the
 * simulator has no C5, so its builds leave AO_Radio out (CONFIG_KODEOS_RADIO
 * =n in sdkconfig.emu), and with it the requests an app makes. These stand
 * in for the four an app asks first, in the radio's own signals:
 *
 *  - with the C5 alive (the default), wifi_start_request() brings up the
 *    network kemu_net.c gives the simulator and, once DHCP has answered,
 *    publishes RADIO_STATE_SIG KRADIO_UP and WIFI_STATE_SIG GOT_IP with that
 *    address, on a network called "kode-sim" - a board that joined its
 *    WiFi. The app's lwIP sockets carry its traffic;
 *  - with the C5 dead (`kode-sim run --no-wifi`, or a RADIO frame of 0),
 *    they answer as AO_Radio does from Radio_failed: KRADIO_FAILED, then
 *    WIFI_STATE_SIG with no address and the wish kept in `enabled`. A link
 *    that was up and loses its C5 says the same.
 *
 * Either way the microSD is mounted first, as AO_Radio has AO_Storage do
 * around the link's lifetime (radio_restore_plain_sd(), and the hosted mount
 * before Radio_up): an app that asks about the card once the radio has
 * settled finds it there, ahead in AO_Storage's queue.
 *
 * The Mac says which with a RADIO frame after every HELLO. Weak: a build
 * with the real AO_Radio keeps the real ones.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "kaos.h"

#include "kemu_link.h"
#include "kemu_net.h"
#include "kemu_proto.h"
#include "kemu_radio.h"

/* kdrv_wifi_state_t: KDRV_WIFI_IDLE, ..., KDRV_WIFI_GOT_IP */
#define WIFI_IDLE   0U
#define WIFI_GOT_IP 2U
#define NET_SSID    "kode-sim"
#define NET_RSSI    (-50)      /* a fair signal; there is no air to measure */

static volatile bool s_c5 = true;
static volatile uint8_t s_link = (uint8_t)KRADIO_OFF;   /* nobody has asked for the link yet */
static volatile uint32_t s_ip4;                         /* network byte order; 0 until DHCP */
static bool s_wifi_want;

static bool publish_link(kradio_link_t link)
{
    RadioStateEvt *const e = Q_NEW_X(RadioStateEvt, 0U, RADIO_STATE_SIG);
    bool const posted = e != NULL;
    if (e != NULL) {
        s_link = (uint8_t)link;
        e->link = (uint8_t)link;
        e->fw_known = false;
        e->fw_major = 0U;
        e->fw_minor = 0U;
        e->fw_patch = 0U;
        e->sd_hosted = false;
        QACTIVE_PUBLISH(&e->super, (void *)0);
    }
    return posted;
}

static bool publish_wifi(void)
{
    bool const on = (s_link == (uint8_t)KRADIO_UP) && (s_ip4 != 0U);
    WifiStateEvt *const e = Q_NEW_X(WifiStateEvt, 0U, WIFI_STATE_SIG);
    bool const posted = e != NULL;
    if (e != NULL) {
        e->state = (uint8_t)(on ? WIFI_GOT_IP : WIFI_IDLE);
        e->ip4 = on ? s_ip4 : 0U;
        (void)snprintf(e->ssid, sizeof e->ssid, "%s", on ? NET_SSID : "");
        e->rssi = on ? (int8_t)NET_RSSI : 0;
        e->enabled = s_wifi_want;
        QACTIVE_PUBLISH(&e->super, (void *)0);
    }
    return posted;
}

/* The card goes back on its path before the link is said to be anything. */
static void mount_card(void)
{
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT);
}

static void failed(void)
{
    mount_card();
    publish_link(KRADIO_FAILED);
    publish_wifi();
}

/* From the event loop's task, once DHCP has answered. */
static void on_ip(uint32_t ip4)
{
    s_ip4 = ip4;
    if (s_c5 && (s_link == (uint8_t)KRADIO_STARTING)) {
        mount_card();
        publish_link(KRADIO_UP);
        publish_wifi();
    }
}

/* The Mac: is there a C5 that comes up? Losing it drops a link in flight. */
static void on_radio(const uint8_t *p, uint32_t len)
{
    if (len < 1U) {
        return;
    }
    s_c5 = (p[0] != 0U);
    if (!s_c5 && ((s_link == (uint8_t)KRADIO_UP) || (s_link == (uint8_t)KRADIO_STARTING))) {
        failed();
    }
}

void kemu_radio_start(void)
{
    kemu_on(KEMU_T_RADIO, on_radio);
}

__attribute__((weak)) bool radio_state_request(void)
{
    return publish_link((kradio_link_t)s_link);
}

__attribute__((weak)) bool wifi_start_request(void)
{
    s_wifi_want = true;
    if (s_link == (uint8_t)KRADIO_UP) {
        return publish_wifi();
    }
    if (!publish_link(KRADIO_STARTING)) {
        return false;
    }
    if (!s_c5 || !kemu_net_start(on_ip)) {
        failed();            /* where the board takes the C5's bring-up timeout */
    } else if (s_ip4 != 0U) {
        on_ip(s_ip4);        /* a lease from before: the link is back at once */
    }
    return true;
}

__attribute__((weak)) bool wifi_state_request(void)
{
    return publish_wifi();
}

/* No answer on the board either: the wish only matters on a connected station. */
__attribute__((weak)) bool wifi_fast_request(bool on)
{
    (void)on;
    return true;
}
