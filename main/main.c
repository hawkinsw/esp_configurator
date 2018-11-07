#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

wifi_scan_config_t g_scan_config = { 0 };
wifi_ap_record_t *g_access_points = NULL;
wifi_config_t g_default_ap_config = { .ap = {
  .ssid = "TestingESP",
  .password = "No password.",
  .ssid_len = 0,
  .channel = 6,
  .authmode = WIFI_AUTH_WPA2_PSK,
  .ssid_hidden = 0,
  .max_connection = 4,
  .beacon_interval = 100
} };
SemaphoreHandle_t g_xaccess_points;

uint16_t configurator_port = 8080;
uint32_t configurator_addr = INADDR_ANY;

void scanner_task(void *parameters) {
	do {
		ESP_ERROR_CHECK( esp_wifi_scan_start(&g_scan_config, false) );
		vTaskDelay(10000 / portTICK_PERIOD_MS);
	} while (true);
}

void configurator_task(void *parameters) {
	struct sockaddr_in server_addr = {}, client_addr = {};
	socklen_t client_addr_len = 0;
	int server_fd = 0;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(configurator_port);
	server_addr.sin_addr.s_addr = htonl(configurator_addr);

	if (0 > (server_fd = socket(AF_INET, SOCK_STREAM, 0))) {
		printf("ERROR: socket() failed.\n");
		return;
	}

	if (0 > (bind(server_fd, (const struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)))) {
		printf("ERROR: bind() failed.\n");
		return;
	}

	if (0 > (listen(server_fd, 1))) {
		printf("ERROR: listen() failed.\n");
		return;
	}

	printf("LOG: No errors, starting server!\n");
	while (1) {
		int client = 0;
		if (0 < (client = accept(server_fd,
		                         (struct sockaddr*)&client_addr,
		                         &client_addr_len))) {
			printf("Client connected!\n");
			while (xSemaphoreTake(g_xaccess_points, (TickType_t) 10) != pdTRUE) {
				vTaskDelay(300 / portTICK_PERIOD_MS);
			}
			write(client, "Testing\n", 7);
			close(client);
			xSemaphoreGive(g_xaccess_points);
		} else {
			printf("accept() returned with an error: %d\n", client);
		}
	}
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch (event->event_id) {
		case SYSTEM_EVENT_AP_START: {
			tcpip_adapter_dhcp_status_t dhcps_status = 0;
			if (ESP_OK == tcpip_adapter_dhcps_get_status(TCPIP_ADAPTER_IF_AP,
			                                             &dhcps_status) &&
			    TCPIP_ADAPTER_DHCP_STARTED == dhcps_status) {
				printf("LOG: DHCP Server started.\n");
				xTaskCreate(&configurator_task,"configurator_server",4096,NULL,5,NULL);
			}
			break;
		}
		case SYSTEM_EVENT_SCAN_DONE: {
			uint16_t num_stations = 0;

			while (xSemaphoreTake(g_xaccess_points, (TickType_t) 10) != pdTRUE) {
				vTaskDelay(300 / portTICK_PERIOD_MS);
			}
			
			esp_wifi_scan_get_ap_num(&num_stations);
			if (g_access_points != NULL) {
				/*
				 * free previous results.
				 */
				free(g_access_points);
			}
			if (NULL == (g_access_points = (wifi_ap_record_t*)calloc(
			                                             num_stations,
			                                             sizeof(wifi_ap_record_t)))) {
				/*
				 * Report an error.
				 */
				return ESP_OK;
			}

			if (esp_wifi_scan_get_ap_records(&num_stations,
			                                 g_access_points) != ESP_OK) {
				free(g_access_points);
				g_access_points = NULL;
			}
			printf("LOG: Retrieved %d access points.\n", num_stations);

			xSemaphoreGive(g_xaccess_points);
			break;
		}
		default: {
			break;
		}
	}
	return ESP_OK;
}

void app_main(void)
{
	g_xaccess_points = xSemaphoreCreateMutex();

	nvs_flash_init();
	tcpip_adapter_init();
	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &g_default_ap_config) );
	ESP_ERROR_CHECK( esp_wifi_start() );
	xTaskCreate(&scanner_task,"scanner_task",4096,NULL,5,NULL);

	while (true) {
		vTaskDelay(300 / portTICK_PERIOD_MS);
	}
}
