/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define POLL_MS 5
#define REFRESH_MS 50
#define RECONNECT_MS 1000
#define WIFI_TIMEOUT_MS 40000
#define DISCOVERY_PORT 5005
#define HOST_LEASE_MS 4000
static const uint8_t discovery_query[] = {'H', 'W', '?', 1};
static const uint8_t host_magic[] = {'H', 'W', 'H', 1};
#define SERVICE_UUID BT_UUID_DECLARE_16(0xAABB)
#define COMMAND_UUID BT_UUID_DECLARE_16(0x1AA1)
#define STATUS_UUID BT_UUID_DECLARE_16(0x1BB1)

struct wifi_credentials
{
    char ssid[33];
    char password[64];
};

static const struct gpio_dt_spec button_a = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button_b = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct wifi_credentials stored;
static struct wifi_credentials pending;
static size_t ssid_used;
static size_t password_used;
static K_MUTEX_DEFINE(provision_lock);
K_MSGQ_DEFINE(credentials_queue, sizeof(struct wifi_credentials), 1, 4);
static atomic_t provision_status = ATOMIC_INIT(0);
static atomic_t wifi_connected = ATOMIC_INIT(0);
static atomic_t ipv4_ready = ATOMIC_INIT(0);
static atomic_t dhcp_started = ATOMIC_INIT(0);
static atomic_t connect_failed = ATOMIC_INIT(0);
static struct net_mgmt_event_callback wifi_events;
static struct net_mgmt_event_callback ipv4_events;
static struct net_if *wifi_iface;

static int credentials_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    if (strcmp(name, "credentials") != 0 || len != sizeof(stored))
    {
        printk("Stored Wi-Fi record rejected: length %u\n", (unsigned int)len);
        return -EINVAL;
    }

    int count = read_cb(cb_arg, &stored, sizeof(stored));

    if (count != sizeof(stored))
    {
        memset(&stored, 0, sizeof(stored));
        return -EINVAL;
    }

    stored.ssid[sizeof(stored.ssid) - 1] = '\0';
    stored.password[sizeof(stored.password) - 1] = '\0';
    printk("Wi-Fi credentials restored\n");

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(hid_settings, "hid", NULL, credentials_set, NULL, NULL);

static bool append_chunk(char *target, size_t capacity, size_t *used,
                         const uint8_t *data, size_t len)
{
    if (len < 2)
    {
        return false;
    }

    size_t offset = data[1];
    size_t payload = len - 2;

    if (offset == 0)
    {
        memset(target, 0, capacity);
        *used = 0;
    }

    if (offset != *used || offset + payload >= capacity)
    {
        return false;
    }

    memcpy(target + offset, data + 2, payload);
    *used += payload;
    target[*used] = '\0';

    return true;
}

static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    const uint8_t *data = buf;
    bool valid = false;

    if (offset != 0 || len == 0 || len > 20)
    {
        atomic_set(&provision_status, 4);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    k_mutex_lock(&provision_lock, K_FOREVER);

    switch (data[0])
    {
        case 1:
            valid = append_chunk(pending.ssid, sizeof(pending.ssid), &ssid_used, data, len);
            break;
        case 2:
            valid = append_chunk(pending.password, sizeof(pending.password), &password_used,
                                 data, len);
            break;
        case 3:
            valid = len == 1 && ssid_used > 0 && password_used >= 8 &&
                    k_msgq_put(&credentials_queue, &pending, K_NO_WAIT) == 0;

            if (valid)
            {
                memset(&pending, 0, sizeof(pending));
                ssid_used = password_used = 0;
                atomic_set(&provision_status, 1);
            }

            break;
        default:
            break;
    }

    k_mutex_unlock(&provision_lock);

    if (!valid)
    {
        atomic_set(&provision_status, 4);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    uint8_t status = atomic_get(&provision_status);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status, sizeof(status));
}

BT_GATT_SERVICE_DEFINE(provision_service,
    BT_GATT_PRIMARY_SERVICE(SERVICE_UUID),
    BT_GATT_CHARACTERISTIC(COMMAND_UUID, BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_command, NULL),
    BT_GATT_CHARACTERISTIC(STATUS_UUID, BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ, read_status, NULL, NULL));

static const struct bt_data advertising[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0xBB, 0xAA),
};
static const struct bt_data scan_response[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
        sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_advertising(void)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising, ARRAY_SIZE(advertising),
                              scan_response, ARRAY_SIZE(scan_response));

    if (err)
    {
        printk("BLE advertising failed: %d\n", err);
    }
    else
    {
        printk("BLE advertising as %s\n", CONFIG_BT_DEVICE_NAME);
    }
}

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
    if (err == 0)
    {
        /* This inputless board uses Just Works encryption and bonding. */
        printk("BLE connected; security request: %d\n",
               bt_conn_set_security(conn, BT_SECURITY_L2));
    }
    else
    {
        printk("BLE connection failed: %u\n", err);
    }
}

static void ble_security_changed(struct bt_conn *conn, bt_security_t level,
                                 enum bt_security_err err)
{
    ARG_UNUSED(conn);
    printk("BLE security level %u, error %u\n", level, err);
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    printk("BLE disconnected: 0x%02x\n", reason);
    k_mutex_lock(&provision_lock, K_FOREVER);
    memset(&pending, 0, sizeof(pending));
    ssid_used = password_used = 0;
    k_mutex_unlock(&provision_lock);
}

BT_CONN_CB_DEFINE(ble_callbacks) = {
    .connected = ble_connected,
    .disconnected = ble_disconnected,
    .security_changed = ble_security_changed,
    .recycled = start_advertising,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    ARG_UNUSED(conn);
    printk("BLE pairing complete; bonded=%u\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    ARG_UNUSED(conn);
    printk("BLE pairing failed: %u\n", reason);
}

static struct bt_conn_auth_info_cb pairing_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

static void network_event(struct net_mgmt_event_callback *cb, uint64_t event,
                          struct net_if *iface)
{
    printk("Network event 0x%llx, Wi-Fi interface match %u\n",
           (unsigned long long)event, iface == wifi_iface);

    if (iface != wifi_iface)
    {
        return;
    }

    if (event == NET_EVENT_WIFI_CONNECT_RESULT)
    {
        const struct wifi_status *result = cb->info;

        if (result == NULL || result->status != 0)
        {
            printk("Wi-Fi association failed\n");
            atomic_set(&connect_failed, 1);
            atomic_set(&wifi_connected, 0);
            atomic_set(&ipv4_ready, 0);
        }
        else
        {
            printk("Wi-Fi associated\n");
            atomic_set(&wifi_connected, 1);
        }
    }
    else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT)
    {
        atomic_set(&wifi_connected, 0);
        atomic_set(&ipv4_ready, 0);
        atomic_set(&dhcp_started, 0);
    }
    else if (event == NET_EVENT_IPV4_ADDR_ADD || event == NET_EVENT_IPV4_DHCP_BOUND)
    {
        atomic_set(&ipv4_ready, 1);
        atomic_set(&provision_status, 2);

        struct net_in_addr *address = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
        char text[NET_IPV4_ADDR_LEN];

        if (address != NULL && zsock_inet_ntop(AF_INET, address, text, sizeof(text)) != NULL)
        {
            printk("Wi-Fi IPv4 address: %s\n", text);
        }
    }
}

static int start_wifi(const struct wifi_credentials *credentials)
{
    struct wifi_connect_req_params params = {
        .channel = WIFI_CHANNEL_ANY,
        .security = WIFI_SECURITY_TYPE_PSK,
        .mfp = WIFI_MFP_DISABLE,
        .ssid = (const uint8_t *)credentials->ssid,
        .ssid_length = strlen(credentials->ssid),
        .psk = (const uint8_t *)credentials->password,
        .psk_length = strlen(credentials->password),
    };

    atomic_set(&connect_failed, 0);
    atomic_set(&ipv4_ready, 0);
    atomic_set(&provision_status, 1);

    return net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &params, sizeof(params));
}

static uint8_t button_state(void)
{
    /* gpio_pin_get_dt returns logical active state, including ACTIVE_LOW. */
    return (gpio_pin_get_dt(&button_a) > 0 ? 1 : 0) |
           (gpio_pin_get_dt(&button_b) > 0 ? 2 : 0);
}

int main(void)
{
    if (!gpio_is_ready_dt(&button_a) || !gpio_is_ready_dt(&button_b) ||
        gpio_pin_configure_dt(&button_a, GPIO_INPUT) != 0 ||
        gpio_pin_configure_dt(&button_b, GPIO_INPUT) != 0)
    {
        printk("Button GPIO setup failed\n");
        return 0;
    }

    wifi_iface = net_if_get_first_wifi();

    if (wifi_iface == NULL)
    {
        printk("No Wi-Fi interface\n");
        return 0;
    }

    net_mgmt_init_event_callback(&wifi_events, network_event,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_events);
    net_mgmt_init_event_callback(&ipv4_events, network_event,
        NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&ipv4_events);

    if (settings_subsys_init() != 0 || bt_enable(NULL) != 0 || settings_load() != 0 ||
        bt_conn_auth_info_cb_register(&pairing_callbacks) != 0)
    {
        printk("BLE or settings initialization failed\n");
        return 0;
    }

    start_advertising();
    printk("Saved Wi-Fi profile present: %u\n", stored.ssid[0] != '\0');

    if (stored.ssid[0] != '\0')
    {
        printk("Wi-Fi connect request: %d\n", start_wifi(&stored));
    }

    struct sockaddr_in host = {.sin_family = AF_INET};
    bool have_host = false;
    int discovery_sock = -1;
    int64_t host_seen = 0;
    int64_t last_query = -1000;
    int64_t last_discovery_open = -1000;
    int sock = -1;
    uint8_t packet[6] = {'H', 'W', 1, 0, 0, 0};
    uint8_t last_state = 0xff;
    int64_t last_sent = 0;
    int64_t last_retry = k_uptime_get();
    int64_t connect_started = last_retry;
    int64_t last_diagnostic = last_retry;
    bool was_ready = false;

    for (;;)
    {
        struct wifi_credentials received;

        if (k_msgq_get(&credentials_queue, &received, K_NO_WAIT) == 0)
        {
            if (discovery_sock >= 0)
            {
                zsock_close(discovery_sock);
                discovery_sock = -1;
            }

            have_host = false;

            if (sock >= 0)
            {
                zsock_close(sock);
                sock = -1;
            }

            if (atomic_get(&wifi_connected))
            {
                if (atomic_get(&dhcp_started))
                {
                    net_dhcpv4_stop(wifi_iface);
                }

                (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, wifi_iface, NULL, 0);
            }

            atomic_set(&wifi_connected, 0);
            atomic_set(&ipv4_ready, 0);
            atomic_set(&dhcp_started, 0);
            stored = received;
            memset(&received, 0, sizeof(received));

            int save_err = settings_save_one("hid/credentials", &stored, sizeof(stored));

            printk("Wi-Fi credential save: %d\n", save_err);
            connect_started = k_uptime_get();
            last_retry = connect_started;

            int err = start_wifi(&stored);

            printk("Wi-Fi connect request: %d\n", err);

            if (err != 0)
            {
                atomic_set(&connect_failed, 1);
            }

            last_state = 0xff;
        }

        int64_t now = k_uptime_get();
        bool ready = atomic_get(&ipv4_ready) != 0;

        if (!ready && now - last_diagnostic >= 5000)
        {
            struct wifi_iface_status status = {0};
            int err = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi_iface,
                               &status, sizeof(status));

            printk("Wi-Fi query %d, state %d, associated %ld, up %u, dormant %u, DHCP %d\n",
                   err, status.state, atomic_get(&wifi_connected),
                   net_if_is_up(wifi_iface), net_if_is_dormant(wifi_iface),
                   wifi_iface->config.dhcpv4.state);

            if (err == 0 && status.state == WIFI_STATE_COMPLETED)
            {
                atomic_set(&wifi_connected, 1);
            }

            last_diagnostic = now;
        }

        if (atomic_get(&wifi_connected) && atomic_cas(&dhcp_started, 0, 1))
        {
            net_if_dormant_off(wifi_iface);
            net_dhcpv4_start(wifi_iface);
            printk("IPv4 DHCP started\n");
        }

        if (!ready && was_ready)
        {
            if (sock >= 0)
            {
                zsock_close(sock);
                sock = -1;
            }

            if (discovery_sock >= 0)
            {
                zsock_close(discovery_sock);
                discovery_sock = -1;
            }

            have_host = false;
        }

        was_ready = ready;

        if (stored.ssid[0] != '\0' && !ready && !atomic_get(&wifi_connected) &&
            (atomic_get(&connect_failed) || now - connect_started > WIFI_TIMEOUT_MS) &&
            now - last_retry >= RECONNECT_MS)
        {
            atomic_set(&provision_status, 3);
            last_retry = now;
            connect_started = now;

            if (start_wifi(&stored) != 0)
            {
                atomic_set(&connect_failed, 1);
            }
        }

        if (ready)
        {
            if (have_host && now - host_seen >= HOST_LEASE_MS)
            {
                have_host = false;
                printk("HID host expired; searching again\n");
            }

            if (discovery_sock < 0 && now - last_discovery_open >= 1000)
            {
                last_discovery_open = now;
                discovery_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

                if (discovery_sock >= 0)
                {
                    struct sockaddr_in local = {
                        .sin_family = AF_INET,
                        .sin_port = htons(DISCOVERY_PORT),
                    };

                    /* Zephyr's native UDP stack permits broadcast without SO_BROADCAST. */
                    if (zsock_bind(discovery_sock, (struct sockaddr *)&local, sizeof(local)) < 0)
                    {
                        printk("HID discovery bind failed: %d\n", errno);
                        zsock_close(discovery_sock);
                        discovery_sock = -1;
                    }
                    else
                    {
                        last_query = now - 1000;
                        printk("HID host discovery listening on UDP %u\n", DISCOVERY_PORT);
                    }
                }
                else
                {
                    printk("HID discovery socket failed: %d\n", errno);
                }
            }

            if (discovery_sock >= 0)
            {
                if ((!have_host || now - host_seen >= 1500) && now - last_query >= 1000)
                {
                    struct sockaddr_in probe = host;

                    probe.sin_port = htons(DISCOVERY_PORT);

                    if (!have_host)
                    {
                        probe.sin_addr.s_addr = INADDR_BROADCAST;
                    }

                    if (zsock_sendto(discovery_sock, discovery_query, sizeof(discovery_query),
                                     MSG_DONTWAIT, (struct sockaddr *)&probe, sizeof(probe)) < 0)
                    {
                        printk("HID discovery probe failed: %d\n", errno);
                    }

                    last_query = now;
                }

                /* Bound receive work so discovery never stalls button polling. */
                for (int i = 0; i < 4; ++i)
                {
                    uint8_t announcement[64];
                    struct sockaddr_in source;
                    socklen_t source_length = sizeof(source);
                    int size = zsock_recvfrom(discovery_sock, announcement, sizeof(announcement),
                                              MSG_DONTWAIT, (struct sockaddr *)&source, &source_length);

                    if (size < 0)
                    {
                        break;
                    }

                    if (size != 8 || memcmp(announcement, host_magic, sizeof(host_magic)) != 0 ||
                        announcement[6] != 0 || announcement[7] != 0 ||
                        source.sin_port != htons(DISCOVERY_PORT) || source.sin_addr.s_addr == 0)
                    {
                        continue;
                    }

                    uint16_t port = ((uint16_t)announcement[4] << 8) | announcement[5];

                    if (port == 0)
                    {
                        continue;
                    }

                    if (!have_host || (host.sin_addr.s_addr == source.sin_addr.s_addr &&
                                       host.sin_port == htons(port)))
                    {
                        if (!have_host)
                        {
                            last_state = 0xff;
                            printk("HID host discovered on UDP port %u\n", port);
                        }

                        host = source;
                        host.sin_port = htons(port);
                        host_seen = now;
                        have_host = true;
                    }
                }
            }
        }

        if (ready && have_host)
        {
            if (sock < 0)
            {
                sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            }

            uint8_t state = button_state();

            if (sock >= 0 && (state != last_state || now - last_sent >= REFRESH_MS))
            {
                packet[3]++;
                packet[4] = state;
                int sent = zsock_sendto(sock, packet, sizeof(packet), MSG_DONTWAIT,
                                        (const struct sockaddr *)&host, sizeof(host));

                if (sent == sizeof(packet))
                {
                    last_state = state;
                    last_sent = now;
                }
                else
                {
                    zsock_close(sock);
                    sock = -1;
                }
            }
        }

        k_sleep(K_MSEC(POLL_MS));
    }
}
