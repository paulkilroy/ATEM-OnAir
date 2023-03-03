#include <Arduino.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <WiFi.h>

#include <DNSServer.h>  // For captive portal DNS redirect

#include <EEPROM.h>
#include <ATEMmin.h>

// For OLIMEX ESP32-POE
// Need to move these to HWRev if there is another ethernet based device
#define ETH_CLK_MODE ETH_CLOCK_GPIO17_OUT
#define ETH_PHY_POWER 12

// Important this is after the ETH_* defines to avoid redefining warnings
#include <ETH.h>

#define AP_SSID "onair-setup"

#define PIN_LIGHTS 15


//Initialize global variables
DNSServer dnsServer;
bool WiFi_Worked = false;
ATEMmin atemSwitcher;
bool InSimulator = false;
ulong pTime = 0;
boolean lightUp = false;
bool foundATEM = false;

String getSSID() {
  return WiFi.SSID();
}

bool ethUp() {
  return WiFiGenericClass::getStatusBits() & ETH_CONNECTED_BIT;
}

bool wifiUp() {
  return WiFiGenericClass::getStatusBits() & STA_CONNECTED_BIT;
}

bool hotspotUp() {
  return WiFiGenericClass::getStatusBits() & AP_STARTED_BIT;
}

bool networkUp() {
  return ethUp() || wifiUp();
}

const char* getHostname() {
  if ( ethUp() ) {
    return ETH.getHostname();
  } else {
    return WiFi.getHostname();
  }
}

IPAddress localIP() {
  if ( ethUp() ) {
    return ETH.localIP();
  } else {
    return WiFi.localIP();
  }
}

IPAddress subnetMask() {
  if ( ethUp() ) {
    return ETH.subnetMask();
  } else {
    return WiFi.subnetMask();
  }
}

IPAddress gatewayIP() {
  if ( ethUp() ) {
    return ETH.gatewayIP();
  } else {
    return WiFi.gatewayIP();
  }
}

void discoverATEM(const char info[]) {
  // _switcher_ctrl._udp.local

  log_i("%s ATEM IP not initialized - Atempting to auto discover atem", info);
  //int n = MDNS.queryService("switcher_ctrl", "udp");
  int n = MDNS.queryService("blackmagic", "tcp");
  if ( n == 0 ) {
    log_i("%s No ATEM services found", info);
  } else {
    // This is build for uncomplicated setups with only one ATEM.. should be fine
    log_i("%s %d ATEM service(s) found - choosing #1: %s", info, n, MDNS.IP(0).toString());

    atemSwitcher.begin(MDNS.IP(0));
    atemSwitcher.connect();
    foundATEM = true;
  }
}

void networkSetup(const char info[]) {
  if ( !MDNS.begin(getHostname()) ) {
    log_i("%s mDNS error [%s]", info, getHostname());
  } else {
    log_i("%s mDNS responder started: http://%s.local", info, getHostname());
  }

  discoverATEM(info);
}

// Ideas to fix POE
//  -Turn on ESP debugging to see whats happening there -- doesn't matter, I can't see it!
//  -Add a sleep after ETH.begin() -- Already have a 1 sec sleep
//  -Move ETH.setHostname() to setup() -- OLIMEX example does it in ETH_START
//  Worked -Maybe move STA stop/reconnect / stop AHEAD to ETH_CONNECTED from ETH_GOT_IP
//  -Maybe move STA reconnect and/or setHostname BACK from WIFI_START to WIFI_GOT_IP
//  -Before I enter the loop, ETH.begin() then sleep 2 sec (start with 10) then look see if I have an IP address by checking bits
//  -Connect serial port to board without power like WT-ETH0 U0TXD / U0RXD
void WiFiEvent(WiFiEvent_t event) {
  switch ( event ) {
  case ARDUINO_EVENT_ETH_START:
    log_i("%d ETH_START", WiFi.getStatusBits());
    // TODO Should I just set this in startup() ?? Why keep setting it
    // How often does this get called?
    // ETH.setHostname(AP_SSID);
    break;
  case ARDUINO_EVENT_ETH_CONNECTED:
    log_i("%d ETH_CONNECTED", WiFi.getStatusBits());

    // Grasping.. THIS WORKED -- Stopped ETH competing with Wifi.. see if it still works after
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_OFF);
    break;
  case ARDUINO_EVENT_ETH_GOT_IP:
    log_i("%d ETH_GOT_IP", WiFi.getStatusBits());
    log_i("ETH_GOT_IP Hostname: %s IP: %s", ETH.getHostname(), ETH.localIP().toString().c_str());
    WiFi.setAutoReconnect(false);
    // TODO check status bits to see if I really need to turn it off???
    WiFi.mode(WIFI_OFF);
    networkSetup("ETH_GOT_IP");
    break;
  case ARDUINO_EVENT_ETH_DISCONNECTED:
    log_i("%d ETH_DISCONNECTED", WiFi.getStatusBits());
    /* TODO fix this back!
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    */
    break;
  case ARDUINO_EVENT_ETH_STOP:
    log_i("%d ETH_STOP", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_READY:
    log_i("%d WIFI_READY", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_SCAN_DONE:
    log_i("%d WIFI_SCAN_DONE", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_STA_START:
    log_i("%d WIFI_SCAN_START", WiFi.getStatusBits());
    WiFi.setHostname(AP_SSID);
    WiFi.setAutoReconnect(true);
    break;
  case ARDUINO_EVENT_WIFI_STA_STOP:
    log_i("%d WIFI_STA_STOP", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    log_i("%d WIFI_STA_CONNECTION", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
  {
    log_i("%d WIFI_STA_DISCONNECTED", WiFi.getStatusBits());
    /*
    if (WiFi_Worked) {
      // Keep it simple and avoid AP_STA mode, its very slow and inconsistent. If your wifi suddnely blew up
      // and you need to reconfigure, just restart the device
      log_i("WIFI_STA_DISCONNECT-1: WiFi was connected so waiting for a reconnect, do not go into AP mode");
    } else if (ethUp()) {
      log_i("WIFI_STA_DISCONNECT-2 - ETH is UP, No need for AP");
    } else {
      log_i("WIFI_STA_DISCONNECT-3 - Starting AP");
      // PSK SIM dnsServer.start(53, "*", WiFi.softAPIP());
      Serial.printf("WIFI_STA_DISCONNECT - AP Mode - SSID for web config: [%s]\n", AP_SSID);
      WiFi.softAP(AP_SSID);
      WiFi.mode(WIFI_AP);  // Enable softAP to access web interface in case of no WiFi
    }*/
    break;
  }
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    log_i("%d WIFI_STA_GOT_IP", WiFi.getStatusBits());

    log_i("WIFI_STA_GOT_IP Hostname: %s IP: %s", WiFi.getHostname(), WiFi.localIP().toString().c_str());
    // Needed? WiFi.mode(WIFI_STA);  // Disable softAP if connection is successful
    networkSetup("WIFI_STA_GOT_IP");
    WiFi_Worked = true;
    break;
  case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
    log_i("%d WIFI_AP_STAIPASSIGNED", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    log_i("%d WIFI_AP_START", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    log_i("%d WIFI_AP_STOP", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    log_i("%d WIFI_AP_STACONNECTED", WiFi.getStatusBits());
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    log_i("%d WIFI_AP_STADISCONNECTED", WiFi.getStatusBits());
    break;
  default:
    break;
  }
}

//Perform initial setup on power on
void setup() {
  //Start Serial
  Serial.begin(115200);
  delay(5000);

  log_i("######################## Serial Started");

  log_i("Getting MAC");
  if ( WiFi.macAddress() == "24:0A:C4:00:01:10" ) {
    InSimulator = true;
  }
  log_i("InSimulator: %d", InSimulator);

  // onEvent needs to be the first thing so we can trigger actions off of WiFi Events
  WiFi.onEvent(WiFiEvent);

  if ( !InSimulator ) {
    log_i("Starting ETH");
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER);
    ETH.setHostname(AP_SSID);
    delay(1000);

    // Put WiFi into station mode and make it connect to saved network
    // movedup WiFi.mode(WIFI_STA);
    //log_i("Attempting connection to WiFi Network name (SSID): [%d]", getSSID());
    //WiFi.begin();

    //delay(1000); // Wait to stabalize so I get the ETH_IP event


  } else {
    WiFi.begin("Wokwi-GUEST", "", 6);
  }

  pinMode(PIN_LIGHTS, OUTPUT);

  log_i("Done");
}

void loop() {

  if (  WiFiGenericClass::getStatusBits() & ETH_HAS_IP_BIT ) {
    if ( !foundATEM ) {
      discoverATEM("LOOP");
    } else {
      atemSwitcher.runLoop();
    }

    if ( atemSwitcher.getStreamStreaming() ) {
      // light up
      digitalWrite(PIN_LIGHTS, HIGH);
    } else if ( atemSwitcher.getStreamConnecting() || atemSwitcher.getStreamStopping() ) {
      // Blink
      if ( millis() - pTime > 200 ) {
        lightUp = !lightUp;
        Serial.printf("changing light status bbb: %d\n", lightUp);
        digitalWrite(PIN_LIGHTS, lightUp ? HIGH : LOW);
        pTime = millis();
      }
    } else {
      digitalWrite(PIN_LIGHTS, LOW);
    }
  } else {
    digitalWrite(PIN_LIGHTS, LOW);
  }
}


