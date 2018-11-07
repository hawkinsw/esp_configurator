#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

void server_task(void *parameters) {
	struct sockaddr_in server_addr, client_addr;
	socklen_t client_addr_len;
	int server_fd = 0;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(8080);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

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

	printf("LOGS: No errors, starting server!\n");
	while (1) {
		int client = 0;
		if (0 < (client = accept(server_fd,
		                         (struct sockaddr*)&client_addr,
		                         &client_addr_len))) {
			printf("Client connected!\n");
			close(client);
		} else {
			printf("accept() returned with an error: %d\n", client);
		}
	}
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_AP_START:
        {
            /*
             * I want to get some information about the dhcp
             * server.
             */
            tcpip_adapter_dhcp_status_t dhcps_status = 0;
            if (tcpip_adapter_dhcps_get_status(TCPIP_ADAPTER_IF_AP, &dhcps_status) == ESP_OK) {
                if (dhcps_status == TCPIP_ADAPTER_DHCP_INIT) {
                    printf("LOG: DHCP Server initializing.\n");
                } else if (dhcps_status == TCPIP_ADAPTER_DHCP_STARTED) {
                    printf("LOG: DHCP Server started.\n");
    xTaskCreate(&server_task,"tcp_server",4096,NULL,5,NULL);
#define GET 1
                } else if (dhcps_status == TCPIP_ADAPTER_DHCP_STOPPED) {
                    printf("LOG: DHCP Server stopped.\n");
                } else if (dhcps_status == TCPIP_ADAPTER_DHCP_STATUS_MAX) {
                    printf("LOG: DHCP Server max status.\n");
                }
            }
            break;
        }
        default: 
        {
            break;
        }
    }
    return ESP_OK;
}

void app_main(void)
{
    nvs_flash_init();
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_config = { .ap = {
        .ssid = "TestingESP",
        .password = "No password.",
        .ssid_len = 0,
        .channel = 6,
        .authmode = WIFI_AUTH_WPA2_PSK,
        .ssid_hidden = 0,
        .max_connection = 4,
        .beacon_interval = 100
    } };
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );


    while (true) {
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
}

