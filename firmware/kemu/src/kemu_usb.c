/**
 * @file kemu_usb.c
 * @brief The USB-OTG port: a TinyUSB device controller with the Mac as the host.
 *
 * Every app that is a USB device (SD Reader, BadUSB, Air Mouse, Webcam) runs
 * the real TinyUSB: usb_new_phy() for the PHY, tusb_init(), then the class
 * callbacks. Under esp-emu the OTG controller is absent, so TinyUSB's device
 * controller driver (dcd_*) is what kemu replaces, with the model in
 * models/usbdcd.c: the stack, its descriptors, its classes and the app above
 * run unchanged, and the "bus" is the page. The page's computer (kode-sim's
 * usbhost.rs) enumerates the device, mounts its disk, reads its keyboard.
 *
 * usb_new_phy() and usb_del_phy() answer at once: there is no PHY to route,
 * and the app's own writes to the routing bits land in registers esp-emu
 * keeps for itself.
 *
 * tinyusb_driver_install() - esp_tinyusb, which kodeOS uses for its dock
 * port - still answers "not supported": kodeOS says so at once instead of
 * waiting five seconds for a stack that would never start.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kemu_link.h"
#include "kemu_usb.h"
#include "kemu_usbdcd.h"

#ifdef KEMU_USB_TINYUSB
#include "tusb.h"
#include "device/dcd.h"

/* Leave the stack unlinked if it is installed but unused by the firmware. */
#pragma weak dcd_event_handler

/* Linker wrappers need an explicit compile-time check against TinyUSB's headers. */
#define CHECK_WRAP(sym) extern __typeof__(sym) __wrap_ ## sym
CHECK_WRAP(dcd_init);
CHECK_WRAP(dcd_deinit);
CHECK_WRAP(dcd_int_handler);
CHECK_WRAP(dcd_int_enable);
CHECK_WRAP(dcd_int_disable);
CHECK_WRAP(dcd_set_address);
CHECK_WRAP(dcd_remote_wakeup);
CHECK_WRAP(dcd_connect);
CHECK_WRAP(dcd_disconnect);
CHECK_WRAP(dcd_sof_enable);
CHECK_WRAP(dcd_edpt0_status_complete);
CHECK_WRAP(dcd_edpt_open);
CHECK_WRAP(dcd_edpt_close_all);
#if (TUSB_VERSION_NUMBER < 2000 && defined(TUP_DCD_EDPT_ISO_ALLOC)) || \
    (TUSB_VERSION_NUMBER >= 2000 && !defined(TUP_DCD_EDPT_CLOSE_API))
CHECK_WRAP(dcd_edpt_iso_alloc);
CHECK_WRAP(dcd_edpt_iso_activate);
#else
CHECK_WRAP(dcd_edpt_close);
#endif
CHECK_WRAP(dcd_edpt_xfer);
CHECK_WRAP(dcd_edpt_xfer_fifo);
CHECK_WRAP(dcd_edpt_stall);
CHECK_WRAP(dcd_edpt_clear_stall);
#undef CHECK_WRAP
#endif

static kemu_usb_t s_m;
static SemaphoreHandle_t s_lock;
static bool s_up;
/* dcd_int_enable()/dcd_int_disable(): the controller's interrupt. Off, nothing reaches the stack
 * (the events wait in the model): tud_deinit() turns it off before it deletes its event queue,
 * and an event sent after that would be a send on a queue that is gone. */
static volatile bool s_ints;

/* The model's sends: called with the lock held, from whichever task is in it. */
static void m_in(void *ctx, uint8_t ep_addr, const uint8_t *p, uint32_t len)
{
    (void)ctx;
    kemu_send2(KEMU_T_USB_IN, &ep_addr, 1U, p, len);
}

static void m_dev(void *ctx, uint8_t kind, uint8_t a, uint16_t b)
{
    (void)ctx;
    uint8_t const f[4] = {kind, a, (uint8_t)(b & 0xFFU), (uint8_t)(b >> 8)};
    kemu_send(KEMU_T_USB_DEV, f, sizeof f);
}

static void m_ack(void *ctx, uint8_t ep, uint32_t bytes)
{
    (void)ctx;
    uint8_t const f[5] = {ep, (uint8_t)bytes, (uint8_t)(bytes >> 8), (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24)};
    kemu_send(KEMU_T_USB_ACK, f, sizeof f);
}

static void ensure(void)
{
    if (!s_up) {
        s_lock = xSemaphoreCreateMutex();
        kemu_usb_init(&s_m, (kemu_usb_out_t){m_in, m_dev, m_ack}, NULL);
        s_up = true;
    }
}

/* What the model queued goes to the stack, outside the lock: the stack's queue may block. */
static void deliver(void)
{
#ifdef KEMU_USB_TINYUSB
    kemu_usb_ev_t events[KEMU_USB_EV_MAX];
    uint32_t const count = s_ints ? kemu_usb_take_events(&s_m, events, KEMU_USB_EV_MAX) : 0U;
#endif
    xSemaphoreGive(s_lock);
#ifdef KEMU_USB_TINYUSB
    if (dcd_event_handler == NULL) { return; }
    for (uint32_t i = 0; i < count; i++) {
        const kemu_usb_ev_t *event = &events[i];
        switch (event->kind) {
        case KEMU_USB_EV_BUS_RESET: dcd_event_bus_reset(0, TUSB_SPEED_FULL, false); break;
        case KEMU_USB_EV_UNPLUGGED: dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false); break;
        case KEMU_USB_EV_SUSPEND: dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false); break;
        case KEMU_USB_EV_RESUME: dcd_event_bus_signal(0, DCD_EVENT_RESUME, false); break;
        case KEMU_USB_EV_SETUP: dcd_event_setup_received(0, event->setup, false); break;
        case KEMU_USB_EV_XFER:
            dcd_event_xfer_complete(0, event->ep_addr, event->len,
                                    event->ok ? XFER_RESULT_SUCCESS : XFER_RESULT_FAILED, false);
            break;
        }
    }
#endif
}

#define WITH_MODEL(call) do { ensure(); xSemaphoreTake(s_lock, portMAX_DELAY); call; deliver(); } while (0)

/* ---- the Mac's frames ---- */

static void on_bus(const uint8_t *p, uint32_t len)
{
    if (len >= 1U) {
        WITH_MODEL(kemu_usb_bus(&s_m, p[0]));
    }
}

static void on_setup(const uint8_t *p, uint32_t len)
{
    if (len >= 8U) {
        WITH_MODEL(kemu_usb_setup(&s_m, p));
    }
}

static void on_out(const uint8_t *p, uint32_t len)
{
    if (len >= 1U) {
        WITH_MODEL((void)kemu_usb_out_data(&s_m, p[0], p + 1, len - 1U));
    }
}

void kemu_usb_start(void)
{
    ensure();
    kemu_on(KEMU_T_USB_BUS, on_bus);
    kemu_on(KEMU_T_USB_SETUP, on_setup);
    kemu_on(KEMU_T_USB_OUT, on_out);
}

/* ---- esp_tinyusb, and the PHY ---- */

esp_err_t __wrap_tinyusb_driver_install(const void *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

static int s_phy;

esp_err_t __wrap_usb_new_phy(const void *conf, void **handle)
{
    (void)conf;
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = &s_phy;
    return ESP_OK;
}

esp_err_t __wrap_usb_del_phy(void *handle)
{
    (void)handle;
    s_ints = false;
    WITH_MODEL(kemu_usb_dcd_deinit(&s_m));
    return ESP_OK;
}

/* ---- TinyUSB's device controller driver ---- */

#ifdef KEMU_USB_TINYUSB
bool __wrap_dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rhport; (void)rh_init;
    bool ok = false;
    /* dcd_init() of the real (DWC2) driver ends by raising the pull-up: the stack never
     * calls dcd_connect() itself unless tud_connect() asks again after a tud_disconnect(). */
    WITH_MODEL({ ok = kemu_usb_dcd_init(&s_m); if (ok) { kemu_usb_dcd_connect(&s_m); } });
    return ok;
}

bool __wrap_dcd_deinit(uint8_t rhport)
{
    (void)rhport;
    WITH_MODEL({ s_ints = false; kemu_usb_dcd_deinit(&s_m); });
    return true;
}

void __wrap_dcd_int_handler(uint8_t rhport) { (void)rhport; }
void __wrap_dcd_int_enable(uint8_t rhport)
{
    (void)rhport;
    s_ints = true;
    WITH_MODEL((void)0);   /* what waited goes now */
}

void __wrap_dcd_int_disable(uint8_t rhport)
{
    (void)rhport;
    s_ints = false;
}
void __wrap_dcd_remote_wakeup(uint8_t rhport) { (void)rhport; }
void __wrap_dcd_sof_enable(uint8_t rhport, bool en) { (void)rhport; (void)en; }
void __wrap_dcd_edpt0_status_complete(uint8_t rhport, const tusb_control_request_t *request) { (void)rhport; (void)request; }

void __wrap_dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_set_address(&s_m, dev_addr));
}

void __wrap_dcd_connect(uint8_t rhport)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_connect(&s_m));
}

void __wrap_dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_disconnect(&s_m));
}

bool __wrap_dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *d)
{
    (void)rhport;
    bool ok = false;
    if (d == NULL) { return false; }
    WITH_MODEL(ok = kemu_usb_dcd_edpt_open(&s_m, d->bEndpointAddress,
        tu_edpt_packet_size(d), d->bmAttributes.xfer));
    return ok;
}

bool __wrap_dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size)
{
    (void)rhport;
    bool ok = false;
    WITH_MODEL(ok = kemu_usb_dcd_edpt_open(&s_m, ep_addr, largest_packet_size, 1U));
    return ok;
}

bool __wrap_dcd_edpt_iso_activate(uint8_t rhport, const tusb_desc_endpoint_t *d)
{
    return __wrap_dcd_edpt_open(rhport, d);
}

void __wrap_dcd_edpt_close(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_edpt_close(&s_m, ep_addr));
}

void __wrap_dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_edpt_close_all(&s_m));
}

bool __wrap_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes
#if TUSB_VERSION_NUMBER >= 2100
    , bool is_isr
#endif
)
{
    (void)rhport;
#if TUSB_VERSION_NUMBER >= 2100
    if (is_isr) { return false; }  /* The virtual controller uses task-context I/O. */
#endif
    bool ok = false;
    WITH_MODEL(ok = kemu_usb_dcd_edpt_xfer(&s_m, ep_addr, buffer, total_bytes));
    return ok;
}

bool __wrap_dcd_edpt_xfer_fifo(uint8_t rhport, uint8_t ep_addr, tu_fifo_t *ff, uint16_t total_bytes
#if TUSB_VERSION_NUMBER >= 2100
    , bool is_isr
#endif
)
{
    (void)rhport; (void)ep_addr; (void)ff; (void)total_bytes;
#if TUSB_VERSION_NUMBER >= 2100
    (void)is_isr;
#endif
    return false;           /* the controller has no dedicated hardware FIFOs */
}

void __wrap_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_edpt_stall(&s_m, ep_addr));
}

void __wrap_dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    WITH_MODEL(kemu_usb_dcd_edpt_clear_stall(&s_m, ep_addr));
}

/* These calls in usbd.c resolve to its weak defaults unless we define them
 * strongly. The linker only wraps references from outside that object. */
bool dcd_deinit(uint8_t rhport) { return __wrap_dcd_deinit(rhport); }
void dcd_connect(uint8_t rhport) { __wrap_dcd_connect(rhport); }
void dcd_disconnect(uint8_t rhport) { __wrap_dcd_disconnect(rhport); }
#endif
