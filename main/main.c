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
#include <string.h>

#define LOG(args...) printf("LOG: " args)
#define ERROR(args...) printf("ERROR: " args)

/*
 * Globals required to track access points
 * accross tasks.
 */
wifi_scan_config_t g_scan_config = { 0 };
uint16_t g_num_access_points = 0;
wifi_ap_record_t *g_access_points = NULL;
SemaphoreHandle_t g_xaccess_points;

/*
 * Our default AP configuration to which
 * users can associate to configure.
 */
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

/*
 * Globals required for configuring the
 * configurator task.
 */
bool g_configurator_task_running;
uint16_t configurator_port = 8080;
uint32_t configurator_addr = INADDR_ANY;

esp_err_t event_handler(void *, system_event_t *);

/*
 * Initialize WiFi.
 */
void wifi_initialize() {
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
}

/*
 * Deinitialize WiFi.
 */
void wifi_deinitialize() {
	ESP_ERROR_CHECK( esp_wifi_deinit());
}

/*
 * Start WiFi.
 */
void wifi_start() {
	ESP_ERROR_CHECK( esp_wifi_start() );
}

/*
 * Stop WiFi.
 */
void wifi_stop() {
	ESP_ERROR_CHECK( esp_wifi_stop() );
}

/*
 * Initiate a scan of the nearby APs.
 */
void wifi_scan() {
	ESP_ERROR_CHECK( esp_wifi_scan_start(&g_scan_config, false) );
}

/*
 * Tell the WiFi to connect to the configured AP.
 */
void wifi_connect() {
	ESP_ERROR_CHECK( esp_wifi_connect() );
}

/*
 * Configure the WiFi to be an AP according to the
 * default config (defined above).
 */
void wifi_configure_as_access_point() {
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &g_default_ap_config) );
}

/*
 * Configure the WiFi to be a client.
 *
 * Input: ssid: The ssid with which to associate.
 *        password: The ssid password.
 */
void wifi_configure_as_client(char *ssid, char *password) {
	wifi_config_t wifi_client_config = { 0 };
	strncpy(&wifi_client_config.sta.ssid, ssid, strlen(ssid));
	strncpy(&wifi_client_config.sta.password, password, strlen(password));
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_client_config) );
}

/*
 * Configure the WiFi to be a client.
 *
 * Input: ssid: The ssid with which to associate.
 *        password: The ssid password.
 */
void wifi_connect_as_access_point(bool deinitialize) {
	if (deinitialize) {
		wifi_stop();
		wifi_deinitialize();
	}
	wifi_initialize();
	wifi_configure_as_access_point();
	wifi_start();
	wifi_scan();
}

/*
 * Configure the WiFi to be a client.
 *
 * Input: ssid: The ssid with which to associate.
 *        password: The ssid password.
 */
void wifi_connect_to_access_point(bool deinitialize, char *ssid, char *passphrase) {
	if (deinitialize) {
		wifi_stop();
		wifi_deinitialize();
	}
	wifi_initialize();
	wifi_configure_as_client(ssid, passphrase);
	wifi_start();
}

/*
 * Initialize the required ESP32 subsystems.
 */
void initialize_subsystems() {
	nvs_flash_init();
	tcpip_adapter_init();
	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
}

/*
 * Spin until we can acquire a semaphor.
 *
 * Input: sem: The semaphor to acquire.
 */
void sem_acquire(SemaphoreHandle_t sem) {
	while (xSemaphoreTake(sem, (TickType_t) 10) != pdTRUE) {
		vTaskDelay(300 / portTICK_PERIOD_MS);
	}
}
/*
 * Give up a semaphore that we took.
 *
 * Input: sem: The semaphor to cede.
 */
void sem_give(SemaphoreHandle_t sem) {
	xSemaphoreGive(sem);
}
/*
 * Read a client's command from the network.
 *
 * Input: fd: The descriptor of the client's network connection.
 * Output: A pointer to the client's command; NULL if error. The
 *         command is limited to a maximum of 80 characters and
 *         is always NULL terminated.
 *
 * Note: The caller must free memory if the return value is not NULL.
 */
#define MAX_CONFIGURATOR_COMMAND_LEN 80
char *read_configurator_command(int fd) {
	char *command = NULL;
	char command_char;
	int command_len = 0;

	command = (char*)calloc(MAX_CONFIGURATOR_COMMAND_LEN+1, sizeof(char));

	if (command == NULL)
		return NULL;

	while (0<read(fd, &command_char, 1) && command_char != '!') {
		command[command_len] = command_char;

		if (++command_len >= MAX_CONFIGURATOR_COMMAND_LEN)
			break;
	}
	return command;
}

/*
 * A function that implements a network server to handle
 * configuration commands from a client. This function will
 * only return in error scenarios. Under normal circumstances,
 * it will run until another task signals it to stop by setting
 * g_configurator_task_running to false. Usually, that signal
 * comes from within this function.
 *
 * Input: parameters: Disregarded.
 */
void configurator_impl(void *parameters) {
	struct sockaddr_in server_addr = {}, client_addr = {};
	socklen_t client_addr_len = 0;
	int server = 0;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(configurator_port);
	server_addr.sin_addr.s_addr = htonl(configurator_addr);

	if (0 > (server = socket(AF_INET, SOCK_STREAM, 0))) {
		ERROR("socket() failed.\n");
		return;
	}

	if (0 > (bind(server, (const struct sockaddr*)&server_addr,
	                       sizeof(struct sockaddr_in)))) {
		ERROR("bind() failed.\n");
		return;
	}

	if (0 > (listen(server, 1))) {
		ERROR("listen() failed.\n");
		return;
	}

	LOG("No errors, starting server!\n");
	while (g_configurator_task_running) {
		int client = 0;
		/*
		 * Wait until there is a client to handle.
		 */
		if (0 < (client = accept(server,
		                         (struct sockaddr*)&client_addr,
		                         &client_addr_len))) {
			LOG("Client connected!\n");

			char *command = read_configurator_command(client);
			if (command) {
				LOG("Client command: [%s]\n", command);
				/*
				 * The client wants to know what APs are in range.
				 */
				if (!strcmp("list", command)) {
					/*
					 * We are sharing access to the list of access points
					 * with the task that is writing the information. So,
					 * we have to acquire the lock here.
					 */
					sem_acquire(g_xaccess_points);
					for (uint16_t i = 0; i<g_num_access_points; i++) {
						/*
						 * In real life, we would have a wrapped version of write()
						 * that guarantees that we write()d all the bytes.
						 */
						write(client, g_access_points[i].ssid,
						              strlen((const char*)g_access_points[i].ssid));
						write(client, ",\n", 2);
					}
					/*
					 * Release the lock, of course.
					 */
					sem_give(g_xaccess_points);

				/*
				 * The client want to rescan for APs.
				 */
				} else if (!strcmp("scan", command)) {
					wifi_scan();
				/*
				 * The client wants to direct the device to connect
				 * to a particular network.
				 */
				} else if (!strncmp("connect:", command, 8)) {
					char *configuration = &command[8];
					char *ssid = NULL, *password = NULL;

					close(client);
					client = -1;

					ssid = strtok(configuration, ":");
					password = strtok(NULL, ":");

					LOG("ssid: %s\n", ssid);
					LOG("password: %s\n", password);

					/*
					 * We tell the WiFi to try to connect and stop
					 * ourself. We may restart again later if the
					 * client gave us bogus data.
					 */
					wifi_connect_to_access_point(true, ssid, password);
					g_configurator_task_running = false;
				/*
				 * The client was confused.
				 */
				} else {
					write(client, "Unknown command!\n", strlen("Unknown command!\n"));
				}
			} else {
				ERROR("Could not allocate space for client command.\n");
			}
			/*
			 * If there was a command string allocated, we have to free it.
			 */
			if (command != NULL)
					free(command);
			/*
			 * Close the client connection. Remember that we may have had to
			 * close early because we switched modes.
			 */
			if (client>0)
				close(client);
		} else {
			ERROR("accept() returned with an error: %d\n", client);
		}
	}
	/*
	 * Close the server socket and terminate ourself.
	 */
	LOG("Ending the configurator_impl function.\n");
	close(server);
	vTaskDelete(NULL);
}

/*
 * An event handler for WiFi and TCP/IP events emitted
 * by the operating system.
 *
 * Input: ctx: ignored.
 *        event: A handle to the event about which we are
 *               being notified.
 */
esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch (event->event_id) {
		case SYSTEM_EVENT_STA_START: {
			/*
			 * This means that we started as a station. But, since
			 * we could potentially be in STA+AP mode, we need to
			 * make sure that we are only a STA at this point before
			 * we attempt to connect to the AP requested by the user.
			 */
			wifi_mode_t mode;
			ESP_ERROR_CHECK( esp_wifi_get_mode(&mode) );
			if (mode == WIFI_MODE_STA) {
				LOG("Station started as STA (only).\n");
				wifi_connect();
			}
			else
				LOG("Station started as AP/STA.\n");
			break;
		}
		case SYSTEM_EVENT_STA_DISCONNECTED: {
			/*
			 * We were disconnected from an access point.
			 * This signals that we got a bad connection string from the client.
			 * Go back to being an access point.
			 */
			LOG("We were disconnected from the station.\n");
			wifi_connect_as_access_point(true);
			break;
		}
		case SYSTEM_EVENT_AP_START: {
			/*
			 * This signals that we started as an access point. As long
			 * as our DHCP server is running, then we can start accepting
			 * connections from clients.
			 */
			tcpip_adapter_dhcp_status_t dhcps_status = 0;
			if (ESP_OK == tcpip_adapter_dhcps_get_status(TCPIP_ADAPTER_IF_AP,
			                                             &dhcps_status) &&
			    TCPIP_ADAPTER_DHCP_STARTED == dhcps_status) {
				LOG("DHCP Server started.\n");
				if (g_configurator_task_running == true) {
					LOG("ERROR: configurator_impl is already running.\n");
				} else {
					g_configurator_task_running = true;
					xTaskCreate(&configurator_impl,"configurator_impl",4096,NULL,5,NULL);
				}
			}
			break;
		}
		case SYSTEM_EVENT_SCAN_DONE: {
			/*
			 * The WiFi finished scanning for access points. We update
			 * the list. Since access to the list is shared, we have to
			 * get the semaphore first.
			 */
			sem_acquire(g_xaccess_points);
			esp_wifi_scan_get_ap_num(&g_num_access_points);
			if (g_access_points != NULL) {
				/*
				 * free previous results.
				 */
				free(g_access_points);
			}
			if (NULL == (g_access_points = (wifi_ap_record_t*)calloc(
			                                             g_num_access_points,
			                                             sizeof(wifi_ap_record_t)))) {
				ERROR("No memory available to store scanned APs.\n");
				return ESP_OK;
			}

			if (esp_wifi_scan_get_ap_records(&g_num_access_points,
			                                 g_access_points) != ESP_OK) {
				ERROR("Could not get the list of scanned APs from the OS.\n");
				free(g_access_points);
				g_access_points = NULL;
			}
			LOG("Retrieved %d access points.\n", g_num_access_points);
			/*
			 * We are done updating the list; relinquish the lock.
			 */
			sem_give(g_xaccess_points);
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
	/*
	 * Create a semaphore that will restrict access to the
	 * shared list of nearby APs.
	 */
	g_xaccess_points = xSemaphoreCreateMutex();

	initialize_subsystems();

	/*
	 * Start by becoming an AP to which clients will connect
	 * to begin configuration of the device.
	 */
	wifi_connect_as_access_point(false);

	/*
	 * Let the runloop do its thing.
	 */
	while (true) {
		vTaskDelay(300 / portTICK_PERIOD_MS);
	}
}
