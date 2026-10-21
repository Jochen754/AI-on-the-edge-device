// Ethernet (W5500 via SPI) implementation of the network interface declared in connect_wlan.h.
// Used instead of connect_wlan.cpp on boards which define NETWORK_ETHERNET (e.g. BOARD_ESP32_S3_ETH).
// Hostname, static IP, gateway, netmask and DNS are read from wlan.ini like in WLAN mode,
// SSID / password are not needed.
#include "../../include/defines.h"

#ifdef NETWORK_ETHERNET
#include "connect_wlan.h"

#include <string.h>
#include <sstream>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include <netdb.h>

#ifdef ENABLE_MQTT
    #include "interface_mqtt.h"
#endif //ENABLE_MQTT

#include "ClassLogFile.h"
#include "read_wlanini.h"
#include "statusled.h"

#include "../esp-protocols/components/mdns/include/mdns.h"

static const char *TAG = "ETH";

static bool ETHConnected = false;
static esp_netif_t *eth_netif = NULL;
static esp_eth_handle_t eth_handle = NULL;
static esp_eth_netif_glue_handle_t eth_glue = NULL;
static std::string ethName = "Ethernet";


static void strinttoip4(const char *ip, int &a, int &b, int &c, int &d)
{
    std::string zw = std::string(ip);
    std::stringstream s(zw);
    char ch; //to temporarily store the '.'
    s >> a >> ch >> b >> ch >> c >> ch >> d;
}


std::string* getIPAddress()
{
    return &wlan_config.ipaddress;
}


std::string* getSSID()
{
    return &ethName; // No SSID on Ethernet
}


static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_START) {
        ETHConnected = false;
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Ethernet started");
    }
    else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_CONNECTED) {
        uint8_t mac_addr[6] = {0};
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Link up, MAC: " + std::string(mac));
    }
    else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_DISCONNECTED) {
        ETHConnected = false;
        LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Link down (cable unplugged or switch/PoE off)");
        StatusLED(WLAN_CONN, 1, false);
    }
    else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_STOP) {
        ETHConnected = false;
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Ethernet stopped");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ETHConnected = true;

        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        wlan_config.ipaddress = std::string(ip4addr_ntoa((const ip4_addr*) &event->ip_info.ip));
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Assigned IP: " + wlan_config.ipaddress);

        #ifdef ENABLE_MQTT
            if (getMQTTisEnabled()) {
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                MQTT_Init();    // Init when network is getting connected
            }
        #endif //ENABLE_MQTT
    }
}


// Same name as the WLAN variant so the rest of the firmware does not need to know the network type
esp_err_t wifi_init_sta(void)
{
    esp_err_t retval = esp_netif_init();
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_netif_init: Error: "  + std::to_string(retval));
        return retval;
    }

    retval = esp_event_loop_create_default();
    if (retval != ESP_OK && retval != ESP_ERR_INVALID_STATE) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_event_loop_create_default: Error: "  + std::to_string(retval));
        return retval;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);

    // W5500 interrupt line needs the GPIO ISR service
    retval = gpio_install_isr_service(0);
    if (retval != ESP_OK && retval != ESP_ERR_INVALID_STATE) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "gpio_install_isr_service: Error: "  + std::to_string(retval));
        return retval;
    }

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = ETH_PIN_MISO;
    buscfg.mosi_io_num = ETH_PIN_MOSI;
    buscfg.sclk_io_num = ETH_PIN_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    retval = spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "spi_bus_initialize: Error: "  + std::to_string(retval));
        return retval;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000;
    devcfg.queue_size = 20;
    devcfg.spics_io_num = ETH_PIN_CS;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
    w5500_config.int_gpio_num = ETH_PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = ETH_PIN_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (mac == NULL || phy == NULL) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "W5500 driver could not be created");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    retval = esp_eth_driver_install(&eth_config, &eth_handle);
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_eth_driver_install: Error: "  + std::to_string(retval) +
                                                " (W5500 not found? Check board / SPI wiring)");
        return retval;
    }

    // The W5500 has no own MAC address: use the one derived from the ESP32 efuse
    uint8_t eth_mac[6];
    esp_read_mac(eth_mac, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);

    eth_glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(eth_netif, eth_glue);

    if (!wlan_config.ipaddress.empty() && !wlan_config.gateway.empty() && !wlan_config.netmask.empty())
    {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Manual interface config -> IP: " + wlan_config.ipaddress + ", Gateway: " +
                                                std::string(wlan_config.gateway) + ", Netmask: " + std::string(wlan_config.netmask));
        esp_netif_dhcpc_stop(eth_netif);	// Stop DHCP service

        esp_netif_ip_info_t ip_info;
        int a, b, c, d;
        strinttoip4(wlan_config.ipaddress.c_str(), a, b, c, d);
        IP4_ADDR(&ip_info.ip, a, b, c, d);	// Set static IP address

        strinttoip4(wlan_config.gateway.c_str(), a, b, c, d);
        IP4_ADDR(&ip_info.gw, a, b, c, d);	// Set gateway

        strinttoip4(wlan_config.netmask.c_str(), a, b, c, d);
        IP4_ADDR(&ip_info.netmask, a, b, c, d);	// Set netmask

        esp_netif_set_ip_info(eth_netif, &ip_info);	// Set static IP configuration

        if (wlan_config.dns.empty()) {
            LogFile.WriteToFile(ESP_LOG_INFO, TAG, "No DNS server, use gateway");
            wlan_config.dns = wlan_config.gateway;
        }
        else {
            LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Manual interface config -> DNS: " + wlan_config.dns);
        }

        esp_netif_dns_info_t dns_info;
        ip4_addr_t ip;
        ip.addr = esp_ip4addr_aton(wlan_config.dns.c_str());
        ip_addr_set_ip4_u32(&dns_info.ip, ip.addr);

        retval = esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (retval != ESP_OK) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_netif_set_dns_info: Error: "  + std::to_string(retval));
            return retval;
        }
    }
    else {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Automatic interface config --> Use DHCP service");
    }

    retval = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_event_handler_instance_register - ETH_ANY: Error: "  + std::to_string(retval));
        return retval;
    }

    retval = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event_handler, NULL, NULL);
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_event_handler_instance_register - GOT_IP: Error: "  + std::to_string(retval));
        return retval;
    }

    if (!wlan_config.hostname.empty())
    {
        retval = esp_netif_set_hostname(eth_netif, wlan_config.hostname.c_str());
        if (retval != ESP_OK) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to set hostname! Error: " + std::to_string(retval));
        }
        else {
            LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Set hostname to: " + wlan_config.hostname);
        }
    }

    retval = esp_eth_start(eth_handle);
    if (retval != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_eth_start: Error: "  + std::to_string(retval));
        return retval;
    }

    if (!wlan_config.hostname.empty())
    {
        //initialize mDNS service
        retval = mdns_init();
        if (retval != ESP_OK) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "mdns_init failed! Error: "  + std::to_string(retval));
        } else {
            //set mdns hostname
            mdns_hostname_set(wlan_config.hostname.c_str());
        }
    }

    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Init successful");
    return ESP_OK;
}


int get_WIFI_RSSI()
{
    return 0; // Not applicable for a wired connection
}


std::string* getHostname() {
    return &wlan_config.hostname;
}


bool getWIFIisConnected()
{
    return ETHConnected;
}


void WIFIDestroy()
{
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, event_handler);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, event_handler);

    if (eth_handle != NULL) {
        esp_eth_stop(eth_handle);
    }
}

#endif // NETWORK_ETHERNET
