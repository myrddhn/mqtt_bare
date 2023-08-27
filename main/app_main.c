#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sleep.h"

#include "bmp180.h"
#include "i2cdev.h"
#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/apps/sntp.h"

#include "esp_log.h"
#include "mqtt_client.h"

//#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/adc.h"

#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_SSID					"MagentaWLAN-FTHQ"
#define WIFI_PASSWORD				"14262453896122832374"
#define WIFI_ESP_MAXIMUM_RETRY		255

#define WIFI_CONNECTED_BIT 			BIT0
#define WIFI_FAIL_BIT      			BIT1

#define GPIO_OUT_1      			GPIO_NUM_2
#define GPIO_OUT_PINS   			(1ULL << GPIO_OUT_1)

#define GPIO_IN_1					GPIO_NUM_15
#define GPIO_IN_PINS				(1ULL << GPIO_IN_1)

#define LEDC_HS_TIMER          		LEDC_TIMER_0
#define LEDC_HS_MODE           		LEDC_HIGH_SPEED_MODE
#define LEDC_HS_CH0_GPIO       		GPIO_NUM_2
#define LEDC_HS_CH0_CHANNEL    		LEDC_CHANNEL_0
#define LEDC_LS_TIMER          		LEDC_TIMER_1
#define LEDC_LS_MODE           		LEDC_LOW_SPEED_MODE
#define LEDC_TEST_DUTY				8191
#define LEDC_TEST_FADE_TIME			500
#define LEDC_TEST_CH_NUM       		1

#define INTERVALSECONDS				5

static const char *states[] = {
    [BUTTON_PRESSED]      = "pressed",
    [BUTTON_RELEASED]     = "released",
    [BUTTON_CLICKED]      = "clicked",
    [BUTTON_PRESSED_LONG] = "pressed long",
};

//static void display_task(void *args);

static const char *TAG = "MQTT_TEMP_PRESSURE";
static char buff[64];
char topic[64];
char data[64];
esp_mqtt_client_handle_t client = NULL;
int msg_id;

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

static char strftime_buf[64];
static struct tm timeinfo;
ledc_channel_config_t ledc_channel[LEDC_TEST_CH_NUM];
ledc_timer_config_t ledc_timer;

static float temp;
static uint32_t pressure;

uint16_t vss;
float vss_v;

static button_t button;
static char buttonname[] = "My Shiny Red Button";

uint8_t mac_address[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
char id[7];

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got IP:%s", ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    client = event->client;
    // int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
            ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

            snprintf(topic, 64, "TOPIC=%.*s", event->topic_len, event->topic);
            snprintf(data, 64, "DATA=%.*s", event->data_len, event->data);

            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
//        .uri = "mqtt://mqtt.darwinistic.com",
		.uri = "mqtt://darwinistic.com",
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

static void mqtt_task(void *args) {
	wifi_ap_record_t ap_info;

    while (1)
    {
		if (client != NULL) {
			if (adc_read(&vss) == ESP_OK) {
				vss_v = (float)vss / 330.0F * 6.0F;
				snprintf(buff, 63, "%.4f", vss_v);
				msg_id = esp_mqtt_client_publish(client, "/stats/vss", buff, 0, 0, 0);
				ESP_LOGI(TAG, "Published VSS value (ADC: %i == %.4fV)", vss, vss_v);
			} else {
				ESP_LOGE(TAG, "Error reading ADC value");
			}

		    esp_wifi_sta_get_ap_info(&ap_info);
			snprintf(buff, 63, "%i", ap_info.rssi);
			msg_id = esp_mqtt_client_publish(client, "/stats/rssi", buff, 0, 0, 0);
			ESP_LOGI(TAG, "Published RSSI message (RSSI: %i)", ap_info.rssi);

			msg_id = esp_mqtt_client_publish(client, "/stats/timestamp", strftime_buf, 0, 0, 0);
			ESP_LOGI(TAG, "Published timestamp");
		}
        vTaskDelay(pdMS_TO_TICKS(INTERVALSECONDS * 1000));
    }
}

static void temperature_task(void *args) {
    bmp180_dev_t dev;
    gpio_num_t SDA = GPIO_NUM_12;
    gpio_num_t SCL = GPIO_NUM_13;
    memset(&dev, 0, sizeof(bmp180_dev_t));

	bmp180_init_desc(&dev, 0, SDA, SCL);
	bmp180_init(&dev);

    while (1) {
        esp_err_t res = bmp180_measure(&dev, &temp, &pressure, BMP180_MODE_STANDARD);
        if (res != ESP_OK) {
            printf("Could not measure: %d\n", res);
        } else {
            /* float is used in printf(). you need non-default configuration in
             * sdkconfig for ESP8266, which is enabled by default for this
             * example. see sdkconfig.defaults.esp8266
             */
        	printf("Temperature: %.2f degrees Celsius; Pressure: %.2f mbar\n", temp, (double)pressure / 100.0F);
            ESP_LOGI(TAG, "Temperature: %.2f degrees Celsius; Pressure: %.2f mbar", temp, (double)pressure / 100.0F);
            snprintf(buff, 63, "%.2f" "\xc2\xb0" "C", temp);
			if (client != NULL) {
				msg_id = esp_mqtt_client_publish(client, "/home/temp/ext", buff, 0, 0, 0);
				snprintf(buff, 63, "%.2f", temp);
				msg_id = esp_mqtt_client_publish(client, "/home/temp/ext/raw", buff, 0, 0, 0);

            }
            snprintf(buff, 63, "%.2f", (double)pressure / 100.0F);
            msg_id = esp_mqtt_client_publish(client, "/home/pressure/raw", buff, 0, 0, 0);
			ESP_LOGI(TAG, "Published temperature and pressure");
        }

        esp_deep_sleep(1000 * 1000 * 60 * 10);

        vTaskDelay(pdMS_TO_TICKS(INTERVALSECONDS * 1000));
    }
}

static void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void obtain_time(void) {
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

static void sntp_task(void *arg) {
    time_t now;

    time(&now);
    localtime_r(&now, &timeinfo);

    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
    }

    // Set timezone to Eastern Standard Time and print local time
    // setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
    // tzset();

    // Set timezone to UTC +2
    //setenv("TZ", "UTC-2", 1);
    setenv("TZ", "UTC-1CEST", 1);
    tzset();

    while (1) {
        // update 'now' variable with current time
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < (2016 - 1900)) {
            ESP_LOGE(TAG, "The current date/time error");
        } else {
            //vTaskSuspendAll();
            strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %X", &timeinfo);
            ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
            //xTaskResumeAll();
        }

        ESP_LOGI(TAG, "Free heap size: %d", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(INTERVALSECONDS * 1000));
    }
}

void initialize_ledc() {
	ledc_timer.duty_resolution = LEDC_TIMER_13_BIT;
	ledc_timer.freq_hz = 5000;
	ledc_timer.speed_mode = LEDC_HS_MODE;
	ledc_timer.timer_num = LEDC_HS_TIMER;
    ledc_timer_config(&ledc_timer);

    ledc_timer.speed_mode = LEDC_LS_MODE;
	ledc_timer.timer_num = LEDC_LS_TIMER;
	ledc_timer_config(&ledc_timer);

	ledc_channel[0].channel = LEDC_HS_CH0_CHANNEL;
	ledc_channel[0].duty = 0;
	ledc_channel[0].gpio_num = LEDC_HS_CH0_GPIO;
	ledc_channel[0].speed_mode = LEDC_HS_MODE;
	ledc_channel[0].hpoint = 0;
	ledc_channel[0].timer_sel = LEDC_HS_TIMER;
    ledc_channel_config(&ledc_channel[0]);

    ledc_fade_func_install(0);
}

/*
static void gloop_task(void *args) {
	while (1) {
		ledc_set_fade_with_time(ledc_channel[0].speed_mode, ledc_channel[0].channel, LEDC_TEST_DUTY, LEDC_TEST_FADE_TIME);
		ledc_fade_start(ledc_channel[0].speed_mode, ledc_channel[0].channel, LEDC_FADE_WAIT_DONE);

		ledc_set_fade_with_time(ledc_channel[0].speed_mode, ledc_channel[0].channel, 0, LEDC_TEST_FADE_TIME);
		ledc_fade_start(ledc_channel[0].speed_mode, ledc_channel[0].channel, LEDC_FADE_WAIT_DONE);
	}
}
*/

void timheartbeattask(TimerHandle_t xTimer) {
	gpio_set_level(GPIO_OUT_1, 0);
	vTaskDelay(75 / portTICK_PERIOD_MS);

	gpio_set_level(GPIO_OUT_1, 0x01);
}

void buttoncb(button_t *btn, button_state_t state) {
	ESP_LOGI(TAG, "YAY!  The button was %s (%s), pressed: %d", states[state], (char *)btn->ctx, btn->pressed_level);
//	button_done(btn);
	button_init(btn);
}

void app_init() {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUT_PINS;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = GPIO_IN_PINS;
	io_conf.pull_down_en = 1;
	io_conf.pull_up_en = 0;
	gpio_config(&io_conf);

	gpio_set_level(GPIO_IN_1, 0x00);

	adc_config_t adc_config;
	adc_config.mode = ADC_READ_TOUT_MODE;
	adc_config.clk_div = 8; // ADC sample collection clock = 80MHz/clk_div = 10MHz
	ESP_ERROR_CHECK(adc_init(&adc_config));

//    int i2c_master_port = I2C_NUM_0;
//    i2c_config_t i2c_config = {
//        .mode = I2C_MODE_MASTER,
//        .sda_io_num = GPIO_NUM_12,
//        .scl_io_num = GPIO_NUM_13,
//        .sda_pullup_en = GPIO_PULLUP_ENABLE,
//        .scl_pullup_en = GPIO_PULLUP_ENABLE,
//        .clk_stretch_tick = 300
//    };
//    i2c_driver_install(i2c_master_port, i2c_config.mode);
//    i2c_param_config(i2c_master_port, &i2c_config);
	i2cdev_init();
    //initialize_ledc();

	button.gpio = GPIO_NUM_16;
	button.internal_pull = true;
	button.pressed_level = 1;
	button.autorepeat = false;
	button.callback = buttoncb;
	button.ctx = buttonname;
	button_init(&button);

    s_wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD
        },
    };

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
        * However these modes are deprecated and not advisable to be used. In case your Access point
        * doesn't support WPA2, these mode can be enabled by commenting below line */

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to AP SSID:%s password:%s", WIFI_SSID, WIFI_PASSWORD);
    	esp_wifi_get_mac(ESP_IF_WIFI_STA, mac_address);
    	ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
    	snprintf(id, 7, "%02X%02X%02X", mac_address[3], mac_address[4], mac_address[5]);

        mqtt_app_start();

		sprintf(topic, "TOPIC=no topic");
		sprintf(data, "DATA=no data");
		xTaskCreate(mqtt_task, "MQTT Task", 2048, NULL, 2, NULL);
	    xTaskCreate(sntp_task, "SNTP Task", 2048, NULL, 5, NULL);
	    xTaskCreate(temperature_task, "Temperature Pressure Task", 2048, NULL, 3, NULL);

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete(s_wifi_event_group);
}

void app_main(void) {
	TimerHandle_t timheartbeat;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    //esp_log_level_set("*", ESP_LOG_VERBOSE);
    esp_log_level_set("ledc", ESP_LOG_NONE);
    esp_log_level_set("gpio", ESP_LOG_VERBOSE);
    esp_log_level_set("i2c", ESP_LOG_VERBOSE);
//    esp_log_level_set("u8g2_hal", ESP_LOG_INFO);
    //esp_log_level_set("MQTT_TEMP_PRESSURE", ESP_LOG_INFO);


    app_init();
    //xTaskCreate(gloop_task, "Gloop Task", 2048, NULL, 6, NULL);
    timheartbeat = xTimerCreate("Heartbeat", 30000 / portTICK_PERIOD_MS, pdTRUE, (void *)0, timheartbeattask);
    xTimerStart(timheartbeat, 0);
}
