/* A boot keyboard exercised through either the real ESP-TinyUSB driver or
 * an application's own TinyUSB lifecycle. No board services are started. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device/dcd.h"

/* IDs are fixture data and are shared by the descriptor and sender. The
 * host must discover them from the descriptors, including these non-boot IDs. */
enum { TEST_REPORT_KEYBOARD = 43, TEST_REPORT_MOUSE = 207 };
#if defined(CONFIG_USB_TEST_SEPARATE)
enum { TEST_ITF_KEYBOARD, TEST_ITF_MOUSE, TEST_ITF_COUNT };
enum { TEST_INSTANCE_KEYBOARD, TEST_INSTANCE_MOUSE };
static const uint8_t keyboard_desc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t mouse_desc[] = { TUD_HID_REPORT_DESC_MOUSE() };
static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, TEST_ITF_COUNT, 0, TUD_CONFIG_DESC_LEN + TEST_ITF_COUNT * TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(TEST_ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof keyboard_desc,
                       0x81, CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(TEST_ITF_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE, sizeof mouse_desc,
                       0x82, CFG_TUD_HID_EP_BUFSIZE, 10),
};
#else
enum { TEST_INSTANCE_KEYBOARD, TEST_INSTANCE_MOUSE = TEST_INSTANCE_KEYBOARD };
static const uint8_t report_desc[] = {
#if defined(CONFIG_USB_TEST_SHARED)
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(TEST_REPORT_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(TEST_REPORT_MOUSE)),
#elif defined(CONFIG_USB_TEST_MOUSE)
    TUD_HID_REPORT_DESC_MOUSE(),
#else
    TUD_HID_REPORT_DESC_KEYBOARD(),
#endif
};
static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
#if defined(CONFIG_USB_TEST_SHARED)
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof report_desc,
                       0x81, CFG_TUD_HID_EP_BUFSIZE, 10),
#elif defined(CONFIG_USB_TEST_MOUSE)
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof report_desc,
                       0x81, CFG_TUD_HID_EP_BUFSIZE, 10),
#else
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof report_desc,
                       0x81, sizeof(hid_keyboard_report_t), 10),
#endif
};
#endif
static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID, .idProduct = 0x4004, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .bNumConfigurations = 1,
};
static const char *strings[] = { (const char[]){0x09, 0x04}, "kode-sim", "ESP-TinyUSB lifecycle" };
static unsigned mounts, unmounts;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
#if defined(CONFIG_USB_TEST_SEPARATE)
    if (instance == TEST_INSTANCE_KEYBOARD) { return keyboard_desc; }
    if (instance == TEST_INSTANCE_MOUSE) { return mouse_desc; }
    return NULL;
#else
    return instance == TEST_INSTANCE_KEYBOARD ? report_desc : NULL;
#endif
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                              uint8_t *buffer, uint16_t size)
{ (void)instance; (void)report_id; (void)type; (void)buffer; (void)size; return 0; }
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           const uint8_t *buffer, uint16_t size)
{ (void)instance; (void)report_id; (void)type; (void)buffer; (void)size; }

/* Exercise DCD calls from the virtual interrupt callback, including when an
 * uninstall overlaps a transfer completion. These calls must not wait on
 * the dispatcher's own delivery barrier or the consumer's lifecycle lock. */
void tud_event_hook_cb(uint8_t rhport, uint32_t event_id, bool in_isr)
{
    if (event_id == DCD_EVENT_XFER_COMPLETE) {
        configASSERT(!in_isr);
        dcd_int_disable(rhport);
        dcd_int_enable(rhport);
    }
}

static void mounted(uint8_t rhport)
{
    printf("USB attached %u rhport=%u speed=%s configuration=%u\n",
           ++mounts, rhport, tud_speed_get() == TUSB_SPEED_FULL ? "full" : "other", tud_mounted() ? 1U : 0U);
}
static void unmounted(uint8_t rhport)
{ printf("USB detached %u rhport=%u\n", ++unmounts, rhport); }

#ifdef CONFIG_USB_TEST_DIRECT
static usb_phy_handle_t phy;
static TaskHandle_t task;
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_desc; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return config_desc; }
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];
    if (index >= sizeof strings / sizeof strings[0]) { return NULL; }
    size_t n = index == 0 ? 1U : strlen(strings[index]);
    if (n > (sizeof desc / sizeof desc[0]) - 1U) { return NULL; }
    if (index == 0) { memcpy(&desc[1], strings[0], 2U); }
    else { for (size_t i = 0; i < n; ++i) { desc[i + 1U] = (uint8_t)strings[index][i]; } }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * (n + 1U)));
    return desc;
}
void tud_mount_cb(void) { mounted(TINYUSB_PORT_FULL_SPEED_0); }
void tud_umount_cb(void) { unmounted(TINYUSB_PORT_FULL_SPEED_0); }
static void device_task(void *arg) { (void)arg; for (;;) { tud_task(); } }
static esp_err_t install(void)
{
    if (task != NULL) { return ESP_ERR_INVALID_STATE; }
    const usb_phy_config_t cfg = {
        .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_FULL,
    };
    esp_err_t ret = usb_new_phy(&cfg, &phy);
    if (ret != ESP_OK) { return ret; }
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!tusb_init(TINYUSB_PORT_FULL_SPEED_0, &init)) {
        usb_del_phy(phy); phy = NULL; return ESP_FAIL;
    }
    if (xTaskCreate(device_task, "direct_usb", TINYUSB_DEFAULT_TASK_SIZE, NULL,
                    TINYUSB_DEFAULT_TASK_PRIO, &task) != pdPASS) {
        tusb_deinit(TINYUSB_PORT_FULL_SPEED_0);
        usb_del_phy(phy); phy = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t uninstall(void)
{
    if (task == NULL) { return ESP_ERR_INVALID_STATE; }
    vTaskDelete(task); task = NULL;
    if (!tusb_deinit(TINYUSB_PORT_FULL_SPEED_0)) { return ESP_FAIL; }
    esp_err_t ret = usb_del_phy(phy); phy = NULL; return ret;
}
#else
static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) { mounted(event->rhport); }
    if (event->id == TINYUSB_EVENT_DETACHED) { unmounted(event->rhport); }
}
static tinyusb_config_t configuration(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event);
    cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    cfg.descriptor.device = &device_desc;
    cfg.descriptor.full_speed_config = config_desc;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof strings / sizeof strings[0];
    return cfg;
}
static esp_err_t install(void) { tinyusb_config_t cfg = configuration(); return tinyusb_driver_install(&cfg); }
static esp_err_t uninstall(void) { return tinyusb_driver_uninstall(); }

static SemaphoreHandle_t race_done;
static void race_install(void *arg)
{
    esp_err_t ret = install();
    printf("USB race %u %s\n", (unsigned)(uintptr_t)arg, esp_err_to_name(ret));
    xSemaphoreGive(race_done);
    vTaskDelete(NULL);
}
#endif

static void command(const char *line)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (strcmp(line, "install") == 0) { ret = install(); }
    else if (strcmp(line, "uninstall") == 0) { ret = uninstall(); }
    else if (strcmp(line, "disconnect") == 0) { ret = tud_disconnect() ? ESP_OK : ESP_FAIL; }
    else if (strcmp(line, "connect") == 0) { ret = tud_connect() ? ESP_OK : ESP_FAIL; }
    else if (strcmp(line, "mask") == 0) { dcd_int_disable(TINYUSB_PORT_FULL_SPEED_0); ret = ESP_OK; }
    else if (strcmp(line, "unmask") == 0) { dcd_int_enable(TINYUSB_PORT_FULL_SPEED_0); ret = ESP_OK; }
#ifdef CONFIG_USB_TEST_DIRECT
    else if (strcmp(line, "pause_consumer") == 0) { vTaskSuspend(task); ret = ESP_OK; }
#endif
    else if (strcmp(line, "yield") == 0) { vTaskDelay(1); ret = ESP_OK; }
    else if (strcmp(line, "bad_phy") == 0) { ret = usb_del_phy((usb_phy_handle_t)&mounts); }
    else if (strcmp(line, "report") == 0) {
#if !defined(CONFIG_USB_TEST_MOUSE)
        uint8_t keys[6] = { HID_KEY_A };
        ret = tud_hid_n_keyboard_report(TEST_INSTANCE_KEYBOARD,
#if defined(CONFIG_USB_TEST_SHARED)
                                       TEST_REPORT_KEYBOARD,
#else
                                       0,
#endif
                                       0, keys) ? ESP_OK : ESP_FAIL;
#else
        ret = ESP_ERR_NOT_SUPPORTED;
#endif
    }
#ifndef CONFIG_USB_TEST_DIRECT
    else if (strcmp(line, "invalid") == 0) { ret = tinyusb_driver_install(NULL); }
    else if (strcmp(line, "high_speed") == 0 || strcmp(line, "high_speed_skip_phy") == 0) {
        tinyusb_config_t cfg = configuration(); cfg.port = TINYUSB_PORT_HIGH_SPEED_0;
        cfg.phy.skip_setup = strcmp(line, "high_speed_skip_phy") == 0;
        ret = tinyusb_driver_install(&cfg);
    } else if (strcmp(line, "bad_task") == 0) {
        tinyusb_config_t cfg = configuration(); cfg.task.size = 0; ret = tinyusb_driver_install(&cfg);
    } else if (strcmp(line, "bad_descriptors") == 0) {
        tinyusb_config_t cfg = configuration(); cfg.descriptor.full_speed_config = NULL;
        ret = tinyusb_driver_install(&cfg);
    } else if (strcmp(line, "skip_phy") == 0) {
        tinyusb_config_t cfg = configuration(); cfg.phy.skip_setup = true; ret = tinyusb_driver_install(&cfg);
    } else if (strcmp(line, "race") == 0) {
        for (uintptr_t i = 0; i < 2U; ++i) {
            configASSERT(xTaskCreate(race_install, "usb_race", TINYUSB_DEFAULT_TASK_SIZE,
                         (void *)i, TINYUSB_DEFAULT_TASK_PRIO, NULL) == pdPASS);
        }
        xSemaphoreTake(race_done, portMAX_DELAY); xSemaphoreTake(race_done, portMAX_DELAY);
        ret = ESP_OK;
    }
#endif
    else if (strncmp(line, "key ", 4U) == 0) {
#if !defined(CONFIG_USB_TEST_MOUSE)
        unsigned mods, key; char extra;
        if (sscanf(line + 4, "%u %u %c", &mods, &key, &extra) == 2 && mods <= UINT8_MAX && key <= UINT8_MAX) {
            uint8_t keys[6] = { (uint8_t)key };
            ret = tud_hid_n_keyboard_report(TEST_INSTANCE_KEYBOARD,
#if defined(CONFIG_USB_TEST_SHARED)
                                           TEST_REPORT_KEYBOARD,
#else
                                           0,
#endif
                                           (uint8_t)mods, keys) ? ESP_OK : ESP_FAIL;
        }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (strncmp(line, "mouse ", 6U) == 0) {
#if defined(CONFIG_USB_TEST_MOUSE) || defined(CONFIG_USB_TEST_SEPARATE) || defined(CONFIG_USB_TEST_SHARED)
        unsigned buttons; int x, y, wheel; char extra;
        if (sscanf(line + 6, "%u %d %d %d %c", &buttons, &x, &y, &wheel, &extra) == 4 && buttons <= UINT8_MAX
            && x >= INT8_MIN && x <= INT8_MAX && y >= INT8_MIN && y <= INT8_MAX && wheel >= INT8_MIN && wheel <= INT8_MAX) {
            ret = tud_hid_n_mouse_report(TEST_INSTANCE_MOUSE,
#if defined(CONFIG_USB_TEST_SHARED)
                                        TEST_REPORT_MOUSE,
#else
                                        0,
#endif
                                        (uint8_t)buttons, (int8_t)x, (int8_t)y, (int8_t)wheel, 0) ? ESP_OK : ESP_FAIL;
        }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    printf("USB result %s %s\n", line, esp_err_to_name(ret));
    fflush(stdout);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("USB versions idf=%s tinyusb=%u.%u.%u\n", esp_get_idf_version(),
           TUSB_VERSION_MAJOR, TUSB_VERSION_MINOR, TUSB_VERSION_REVISION);
#ifndef CONFIG_USB_TEST_DIRECT
    race_done = xSemaphoreCreateCounting(2U, 0U); configASSERT(race_done != NULL);
#endif
    printf("USB port TINYUSB_PORT_FULL_SPEED_0=%u\n", TINYUSB_PORT_FULL_SPEED_0);
    command("install");
    char line[64]; size_t used = 0;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1U, portMAX_DELAY) != 1) { continue; }
        if (c == '\n' || c == '\r') {
            line[used] = '\0'; if (used != 0U) { command(line); } used = 0;
        } else if (used + 1U < sizeof line) { line[used++] = c; }
    }
}
