#include "DoorServer.h"
// #include "esp_http_server.h"

// DoorServer doorServer;

// WebServer server(80);

// WiFiClient micClient;

void DoorServer::init() {

    String current_ssid = camConfig.getWiFiSSID(ssid);
    String current_pass = camConfig.getWiFiPassword(password);

    Serial.printf("[TRACE] WiFi connecting | target_ssid: %s\n", current_ssid.c_str());
    WiFi.setHostname(dns_cam);
    WiFi.mode(WIFI_STA);
    WiFi.begin(current_ssid.c_str(), current_pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[TRACE] WiFi connected | IP: %s\n", dns_cam);
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Door-Setup", "12345678");
        Serial.printf("[TRACE] AP Mode Started | IP: %s\n", dns_cam);
    }

    if (MDNS.begin(dns_cam)) {
        Serial.printf("[TRACE] mDNS started: http://%s.local\n", dns_cam);
    }

}
