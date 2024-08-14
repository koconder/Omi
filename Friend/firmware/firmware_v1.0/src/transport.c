#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/gpio.h>
#include "transport.h"
#include "config.h"
#include "utils.h"
#include "btutils.h"
#include "lib/battery/battery.h"

LOG_MODULE_REGISTER(transport, CONFIG_LOG_DEFAULT_LEVEL);

extern bool is_connected;

//
// Internal
//

static struct bt_conn_cb _callback_references;

static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t audio_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);
static ssize_t audio_codec_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t dfu_control_point_write_handler(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

//
// Service and Characteristic
//

 static void button_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t button_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);
static struct gpio_callback button_cb_data;

struct gpio_dt_spec d4_pin = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin=4, .dt_flags = GPIO_OUTPUT_ACTIVE}; //3.3
struct gpio_dt_spec d5_pin_input = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin=5, .dt_flags = GPIO_INT_EDGE_RISING};

static struct bt_uuid_128 button_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x23BA7924,0x0000,0x1000,0x7450,0x346EAC492E92));
static struct bt_uuid_128 button_uuid_x = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x23BA7925 ,0x0000,0x1000,0x7450,0x346EAC492E92));

static struct bt_gatt_attr button_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&button_uuid),
    BT_GATT_CHARACTERISTIC(&button_uuid_x.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, button_data_read_characteristic, NULL, NULL),
    BT_GATT_CCC(button_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service button_service = BT_GATT_SERVICE(button_service_attr);

static void button_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_INF("Handler\n");

}



// Audio service with UUID 19B10000-E8F2-537E-4F6C-D104768A1214
// exposes following characteristics:
// - Audio data (UUID 19B10001-E8F2-537E-4F6C-D104768A1214) to send audio data (read/notify)
// - Audio codec (UUID 19B10002-E8F2-537E-4F6C-D104768A1214) to send audio codec type (read)
// TODO: The current audio service UUID seems to come from old Intel sample code,
// we should change it to UUID 814b9b7c-25fd-4acd-8604-d28877beee6d
static struct bt_uuid_128 audio_service_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_data_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10001, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_format_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10002, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

static struct bt_gatt_attr audio_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&audio_service_uuid),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_data_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, audio_data_read_characteristic, NULL, NULL),
    BT_GATT_CCC(audio_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_format_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, audio_codec_read_characteristic, NULL, NULL),
    // BT_GATT_CHARACTERISTIC(&button_uuid_x.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, button_data_read_characteristic, NULL, NULL),
    // BT_GATT_CCC(button_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service audio_service = BT_GATT_SERVICE(audio_service_attr);

// Nordic Legacy DFU service with UUID 00001530-1212-EFDE-1523-785FEABCD123
// exposes following characteristics:
// - Control point (UUID 00001531-1212-EFDE-1523-785FEABCD123) to start the OTA update process (write/notify)
static struct bt_uuid_128 dfu_service_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001530, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));
static struct bt_uuid_128 dfu_control_point_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001531, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));

static struct bt_gatt_attr dfu_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&dfu_service_uuid),
    BT_GATT_CHARACTERISTIC(&dfu_control_point_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_WRITE, NULL, dfu_control_point_write_handler, NULL),
    BT_GATT_CCC(dfu_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service dfu_service = BT_GATT_SERVICE(dfu_service_attr);





// Advertisement data
static const struct bt_data bt_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_UUID128_ALL, audio_service_uuid.val, sizeof(audio_service_uuid.val)),
    BT_DATA(BT_DATA_NAME_COMPLETE, "Friend", sizeof("Friend") - 1),
};

// Scan response data
static const struct bt_data bt_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
    BT_DATA(BT_DATA_UUID128_ALL, dfu_service_uuid.val, sizeof(dfu_service_uuid.val)),
};

//
// State and Characteristics
//

struct bt_conn *current_connection = NULL;
uint16_t current_mtu = 0;
uint16_t current_package_index = 0;




static uint32_t current_button_time = 0;
static uint32_t previous_button_time = 0;

const int max_debounce_interval = 700;
static bool was_pressed = false;

//
// button
//
void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
    current_button_time = k_cycle_get_32();
	if (current_button_time - previous_button_time < max_debounce_interval) { //too low!
	}
	else { //right...    
        int temp = gpio_pin_get_raw(dev,d5_pin_input.pin);
        if (temp) {
            was_pressed = true;
        }
        else {
            was_pressed = false;
        }
	}
	previous_button_time = current_button_time;
}
#define BUTTON_CHECK_INTERVAL 40 // 0.04 seconds, 25 Hz

void check_button_level(struct k_work *work_item);

K_WORK_DELAYABLE_DEFINE(button_work, check_button_level);

typedef enum {
    IDLE, 
    ONE_PRESS,
    TWO_PRESS,
    GRACE
} FSM_STATE_T;


#define DEFAULT_STATE 0
#define SINGLE_TAP 1
#define DOUBLE_TAP 2
#define LONG_TAP 3
#define BUTTON_PRESS 4
#define BUTTON_RELEASE 5


// 4 is button down, 5 is button up
static FSM_STATE_T current_button_state = IDLE;
static uint32_t inc_count_1 = 0;
static uint32_t inc_count_0 = 0;


static int final_button_state[2] = {0,0};
const static int threshold = 10;


static void reset_count() {
    inc_count_0 = 0;
    inc_count_1 = 0;
}
static void notify_press() {
    final_button_state[0] = BUTTON_PRESS;
    LOG_INF("pressed");
    bt_gatt_notify(current_connection, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
}

static void notify_unpress() {
    final_button_state[0] = BUTTON_RELEASE; 
    LOG_INF("unpressed");
    bt_gatt_notify(current_connection, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));  
}

static void notify_tap() {
      final_button_state[0] = SINGLE_TAP;
    LOG_INF("tap\n");
    bt_gatt_notify(current_connection, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));  
}

static void notify_double_tap() {
      final_button_state[0] = DOUBLE_TAP; //button press
    LOG_INF("double tap\n");
    bt_gatt_notify(current_connection, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));  
}

static void notify_long_tap() {
    final_button_state[0] = LONG_TAP; //button press
    LOG_INF("long tap\n");
    bt_gatt_notify(current_connection, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));  
}

#define LONG_PRESS_INTERVAL 50
#define SINGLE_PRESS_INTERVAL 2
void check_button_level(struct k_work *work_item) {
     //insert the current button state here
    int state_ = was_pressed ? 1 : 0;
    if (current_button_state == IDLE) {

        if (state_ == 0) {
            //Do nothing!
        }

        else if (state_ == 1) {
            //Also do nothing, but transition to the next state
            notify_press();
            current_button_state = ONE_PRESS;
        }

    }

    else if (current_button_state == ONE_PRESS) {

        if (state_ == 0) {
            
            if(inc_count_0 == 0) {
            notify_unpress();
            }
            inc_count_0++; //button is unpressed
            if (inc_count_0 > SINGLE_PRESS_INTERVAL) {
                //If button is not pressed for a little while....... 
                //transition to Two_press. button could be a single or double tap
                current_button_state = TWO_PRESS;
                reset_count();          
            }
        }
        if (state_ == 1) {
            inc_count_1++; //button is pressed

            if (inc_count_1 > LONG_PRESS_INTERVAL) {
                //If button is pressed for a long time.......
                notify_long_tap();
                //Fire the long mode notify and enter a grace period
                current_button_state = GRACE;
                reset_count();
            }

        }

    }

    else if (current_button_state == TWO_PRESS) {

        if (state_ == 0) {
             
                if (inc_count_1 > 0) { // if button has been pressed......
                notify_unpress();
                notify_double_tap();
                
                //Fire the notify and enter a grace period
                current_button_state = GRACE;
                reset_count();
             }
             //single button press
            else if (inc_count_0 > 10){
                notify_tap(); //Fire the notify and enter a grace period
                current_button_state = GRACE;
                reset_count();

             }
             else {
                inc_count_0++; //not pressed
             }
        }
        else if (state_ == 1 ) {
            if (inc_count_1 == 0) {
                notify_press();
                inc_count_1++;
            }
            if (inc_count_1 > threshold) {
                notify_long_tap();
                //Fire the notify and enter a grace period
                current_button_state = GRACE;
                reset_count();
            }
        }
    }

    else if (current_button_state == GRACE) {
        if (state_ == 0) {
            if (inc_count_0 == 0 && (inc_count_1 > 0)) {
            notify_unpress();
            }
            inc_count_0++;
            if (inc_count_0 > 10) {
            current_button_state = IDLE;
            reset_count();
            }
        }
        else if (state_ == 1) {
              inc_count_1++;
        }
    }

    k_work_reschedule(&button_work, K_MSEC(BUTTON_CHECK_INTERVAL));

}


static ssize_t button_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
     LOG_INF("button_data_read_characteristic\n");
     LOG_INF("was_pressed: %d\n", was_pressed);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &was_pressed, sizeof(was_pressed));

}


static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY)
    {
        LOG_INF("Client subscribed for notifications");
    }
    else if (value == 0)
    {
        LOG_INF("Client unsubscribed from notifications");
    }
    else
    {
        LOG_INF("Invalid CCC value: %u", value);
    }
}

static ssize_t audio_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("audio_data_read_characteristic");
    return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
}

static ssize_t audio_codec_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    uint8_t value[1] = {CODEC_ID};
    LOG_DBG("audio_codec_read_characteristic %d", CODEC_ID);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

//
// DFU Service Handlers
//

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY)
    {
        LOG_INF("Client subscribed for notifications");
    }
    else if (value == 0)
    {
        LOG_INF("Client unsubscribed from notifications");
    }
    else
    {
        LOG_INF("Invalid CCC value: %u", value);
    }
}

static ssize_t dfu_control_point_write_handler(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_INF("dfu_control_point_write_handler");
    if (len == 1 && ((uint8_t *)buf)[0] == 0x06)
    {
        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    }
    else if (len == 2 && ((uint8_t *)buf)[0] == 0x01)
    {
        uint8_t notification_value = 0x10;
        bt_gatt_notify(conn, attr, &notification_value, sizeof(notification_value));

        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    }
    return len;
}

//
// Battery Service Handlers
//

#define BATTERY_REFRESH_INTERVAL 15000 // 15 seconds

void broadcast_battery_level(struct k_work *work_item);

K_WORK_DELAYABLE_DEFINE(battery_work, broadcast_battery_level);

void broadcast_battery_level(struct k_work *work_item) {
    uint16_t battery_millivolt;
    uint8_t battery_percentage;
    if (battery_get_millivolt(&battery_millivolt) == 0 &&
        battery_get_percentage(&battery_percentage, battery_millivolt) == 0) {


        LOG_INF("Battery at %d mV (capacity %d%%)\n", battery_millivolt, battery_percentage);


        // Use the Zephyr BAS function to set (and notify) the battery level
        int err = bt_bas_set_battery_level(battery_percentage);
        if (err) {
            LOG_ERR("Error updating battery level: %d", err);
        }
    } else {
        LOG_ERR("Failed to read battery level");
    }

    k_work_reschedule(&battery_work, K_MSEC(BATTERY_REFRESH_INTERVAL));
}

//
// Connection Callbacks
//

static void _transport_connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info = {0};

    err = bt_conn_get_info(conn, &info);
    if (err)
    {
        LOG_ERR("Failed to get connection info (err %d)", err);
        return;
    }

    LOG_INF("bluetooth activated\n");

    current_connection = bt_conn_ref(conn);
    current_mtu = info.le.data_len->tx_max_len;
    LOG_INF("Transport connected");
    LOG_DBG("Interval: %d, latency: %d, timeout: %d", info.le.interval, info.le.latency, info.le.timeout);
    LOG_DBG("TX PHY %s, RX PHY %s", phy2str(info.le.phy->tx_phy), phy2str(info.le.phy->rx_phy));
    LOG_DBG("LE data len updated: TX (len: %d time: %d) RX (len: %d time: %d)", info.le.data_len->tx_max_len, info.le.data_len->tx_max_time, info.le.data_len->rx_max_len, info.le.data_len->rx_max_time);

    k_work_schedule(&battery_work, K_MSEC(BATTERY_REFRESH_INTERVAL));
    k_work_schedule(&button_work, K_MSEC(BUTTON_CHECK_INTERVAL));

    is_connected = true;
}

static void _transport_disconnected(struct bt_conn *conn, uint8_t err)
{
    is_connected = false;

    LOG_INF("Transport disconnected");
    bt_conn_unref(conn);
    current_connection = NULL;
    current_mtu = 0;
}

static bool _le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
    LOG_INF("Transport connection parameters update request received.");
    LOG_DBG("Minimum interval: %d, Maximum interval: %d", param->interval_min, param->interval_max);
    LOG_DBG("Latency: %d, Timeout: %d", param->latency, param->timeout);

    return true;
}

static void _le_param_updated(struct bt_conn *conn, uint16_t interval,
                              uint16_t latency, uint16_t timeout)
{
    LOG_INF("Connection parameters updated.");
	LOG_DBG("[ interval: %d, latency: %d, timeout: %d ]", interval, latency, timeout);
}

static void _le_phy_updated(struct bt_conn *conn,
                            struct bt_conn_le_phy_info *param)
{
    LOG_DBG("LE PHY updated: TX PHY %s, RX PHY %s",
           phy2str(param->tx_phy), phy2str(param->rx_phy));
}

static void _le_data_length_updated(struct bt_conn *conn,
                                    struct bt_conn_le_data_len_info *info)
{
    LOG_DBG("LE data len updated: TX (len: %d time: %d)"
           " RX (len: %d time: %d)",
           info->tx_max_len,
           info->tx_max_time, info->rx_max_len, info->rx_max_time);
    current_mtu = info->tx_max_len;
}

static struct bt_conn_cb _callback_references = {
    .connected = _transport_connected,
    .disconnected = _transport_disconnected,
    .le_param_req = _le_param_req,
    .le_param_updated = _le_param_updated,
    .le_phy_updated = _le_phy_updated,
    .le_data_len_updated = _le_data_length_updated,
};

//
// Ring Buffer
//

#define NET_BUFFER_HEADER_SIZE 3
#define RING_BUFFER_HEADER_SIZE 2
static uint8_t tx_queue[NETWORK_RING_BUF_SIZE * (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)];
static uint8_t tx_buffer[CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE];
static uint8_t tx_buffer_2[CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE];
static uint32_t tx_buffer_size = 0;
static struct ring_buf ring_buf;

static bool write_to_tx_queue(uint8_t *data, size_t size)
{
    if (size > CODEC_OUTPUT_MAX_BYTES)
    {
        return false;
    }

    // Copy data (TODO: Avoid this copy)
    tx_buffer_2[0] = size & 0xFF;
    tx_buffer_2[1] = (size >> 8) & 0xFF;
    memcpy(tx_buffer_2 + RING_BUFFER_HEADER_SIZE, data, size);

    // Write to ring buffer
    int written = ring_buf_put(&ring_buf, tx_buffer_2, (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)); // It always fits completely or not at all
    if (written != CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)
    {
        return false;
    }
    else
    {
        return true;
    }
}

static bool read_from_tx_queue()
{

    // Read from ring buffer
    // memset(tx_buffer, 0, sizeof(tx_buffer));
    tx_buffer_size = ring_buf_get(&ring_buf, tx_buffer, (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)); // It always fits completely or not at all
    if (tx_buffer_size != (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE))
    {
        LOG_WRN("Failed to read from ring buffer %d", tx_buffer_size);
        return false;
    }

    // Adjust size
    tx_buffer_size = tx_buffer[0] + (tx_buffer[1] << 8);

    return true;
}

//
// Pusher
//

// Thread
K_THREAD_STACK_DEFINE(pusher_stack, 1024);
static struct k_thread pusher_thread;
static uint16_t packet_next_index = 0;
static uint8_t pusher_temp_data[CODEC_OUTPUT_MAX_BYTES + NET_BUFFER_HEADER_SIZE];

static bool push_to_gatt(struct bt_conn *conn)
{
    // Read data from ring buffer
    if (!read_from_tx_queue())
    {
        return false;
    }

    // Push each frame
    uint8_t *buffer = tx_buffer + RING_BUFFER_HEADER_SIZE;
    uint32_t offset = 0;
    uint8_t index = 0;
    while (offset < tx_buffer_size)
    {
        // Recombine packet
        uint32_t id = packet_next_index++;
        uint32_t packet_size = MIN(current_mtu - NET_BUFFER_HEADER_SIZE, tx_buffer_size - offset);
        pusher_temp_data[0] = id & 0xFF;
        pusher_temp_data[1] = (id >> 8) & 0xFF;
        pusher_temp_data[2] = index;
        memcpy(pusher_temp_data + NET_BUFFER_HEADER_SIZE, buffer + offset, packet_size);
        offset += packet_size;
        index++;

        while (true)
        {
            // Try send notification
            int err = bt_gatt_notify(conn, &audio_service.attrs[1], pusher_temp_data, packet_size + NET_BUFFER_HEADER_SIZE);

            // Log failure
            if (err)
            {
                LOG_DBG("bt_gatt_notify failed (err %d)", err);
                LOG_DBG("MTU: %d, packet_size: %d", current_mtu, packet_size + NET_BUFFER_HEADER_SIZE);
                k_sleep(K_MSEC(1));
            }

            // Try to send more data if possible
            if (err == -EAGAIN || err == -ENOMEM)
            {
                continue;
            }

            // Break if success
            break;
        }
    }

    return true;
}

void pusher(void)
{
    while (1)
    {

        //
        // Load current connection
        //

        struct bt_conn *conn = current_connection;
        bool use_gatt = true;
        if (conn)
        {
            conn = bt_conn_ref(conn);
        }
        bool valid = true;
        if (current_mtu < MINIMAL_PACKET_SIZE)
        {
            valid = false;
        }
        else if (!conn)
        {
            valid = false;
        }
        else
        {
            valid = bt_gatt_is_subscribed(conn, &audio_service.attrs[1], BT_GATT_CCC_NOTIFY); // Check if subscribed
        }

        // If no valid mode exists - discard whole buffer
        if (!valid)
        {
            ring_buf_reset(&ring_buf);
            k_sleep(K_MSEC(10));
        }

        // Handle GATT
        if (use_gatt && valid)
        {
            bool sent = push_to_gatt(conn);
            if (!sent)
            {
                k_sleep(K_MSEC(50));
            }
        }

        if (conn)
        {
            bt_conn_unref(conn);
        }
    }
}

//
// Public functions
//

int transport_start()
{
    // Configure callbacks
    bt_conn_cb_register(&_callback_references);

    // Enable Bluetooth
    int err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Transport bluetooth init failed (err %d)", err);
        return err;
    }
    LOG_INF("Transport bluetooth initialized");

    	if (gpio_is_ready_dt(&d4_pin)) {
		LOG_INF("D4 Pin ready\n");
	}
    	else {
		LOG_INF("Error setting up D4 Pin\n");
	}

	if (gpio_pin_configure_dt(&d4_pin, GPIO_OUTPUT_ACTIVE) < 0) {
		LOG_INF("Error setting up D4 Pin Voltage\n");
	}
	else {
		LOG_INF("D4 ready to transmit voltage\n");
	}
	if (gpio_is_ready_dt(&d5_pin_input)) {
		LOG_INF("D5 Pin ready\n");
	}
	else {
		LOG_INF("D5 Pin not ready\n");
	}

	int err2 = gpio_pin_configure_dt(&d5_pin_input,GPIO_INPUT);

	if (err2 != 0) {
		LOG_INF("Error setting up D5 Pin\n");
		return 0;
	}
	else {
		LOG_INF("D5 ready\n");
	}
	err2 =  gpio_pin_interrupt_configure_dt(&d5_pin_input,GPIO_INT_EDGE_BOTH);

	if (err2 != 0) {
		LOG_INF("D5 unable to detect button presses\n");
		return 0;
	}
	else {
		LOG_INF("D5 ready to detect button presses\n");
	}


    gpio_init_callback(&button_cb_data, button_pressed, BIT(d5_pin_input.pin));
	gpio_add_callback(d5_pin_input.port, &button_cb_data);
    // Start advertising
    bt_gatt_service_register(&button_service);
    bt_gatt_service_register(&audio_service);
    bt_gatt_service_register(&dfu_service);
    err = bt_le_adv_start(BT_LE_ADV_CONN, bt_ad, ARRAY_SIZE(bt_ad), bt_sd, ARRAY_SIZE(bt_sd));

    if (err)
    {
        LOG_ERR("Transport advertising failed to start (err %d)", err);
        return err;
    }
    else
    {
        LOG_INF("Advertising successfully started");
    }

    int battErr = 0;

	battErr |= battery_init();
	battErr |= battery_charge_start();

	if (battErr)
	{
		LOG_ERR("Battery init failed (err %d)", battErr);
	}
	else
	{
		LOG_INF("Battery initialized");
	}

    // Start pusher
    ring_buf_init(&ring_buf, sizeof(tx_queue), tx_queue);
    k_thread_create(&pusher_thread, pusher_stack, K_THREAD_STACK_SIZEOF(pusher_stack), (k_thread_entry_t)pusher, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);

    return 0;
}

struct bt_conn *get_current_connection()
{
    return current_connection;
}

int broadcast_audio_packets(uint8_t *buffer, size_t size)
{
    while (!write_to_tx_queue(buffer, size))
    {
        k_sleep(K_MSEC(1));
    }
    return 0;
}
