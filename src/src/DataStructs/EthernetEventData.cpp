#include "../DataStructs/EthernetEventData.h"

#if FEATURE_ETHERNET

#include "../ESPEasyCore/ESPEasy_Log.h"
#include "../Globals/Settings.h"
#include "../Helpers/Networking.h"

#include <ETH.h>

// Bit numbers for Eth status
#define ESPEASY_ETH_CONNECTED               0
#define ESPEASY_ETH_GOT_IP                  1
#define ESPEASY_ETH_SERVICES_INITIALIZED    2


#if FEATURE_USE_IPV6
#include <esp_netif.h>

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Private functions ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

esp_netif_t* get_esp_interface_netif(esp_interface_t interface);
#endif



bool EthernetEventData_t::EthConnectAllowed() const {
  if (!ethConnectAttemptNeeded) return false;
  if (last_eth_connect_attempt_moment.isSet()) {
    // TODO TD-er: Make this time more dynamic.
    if (!last_eth_connect_attempt_moment.timeoutReached(10000)) {
      return false;
    }
  }
  return true;
}

bool EthernetEventData_t::unprocessedEthEvents() const {
  if (processedConnect && processedDisconnect && processedGotIP && processedDHCPTimeout
#if FEATURE_USE_IPV6
      && processedGotIP6
#endif
  )
  {
    return false;
  }
  return true;
}

void EthernetEventData_t::clearAll() {
  lastDisconnectMoment.clear();
  lastConnectMoment.clear();
  lastGetIPmoment.clear();
  last_eth_connect_attempt_moment.clear();

  lastEthResetMoment.setNow();
  eth_considered_stable = false;

  // Mark all flags to default to prevent handling old events.
  processedConnect          = true;
  processedDisconnect       = true;
  processedGotIP            = true;
  #if FEATURE_USE_IPV6
  processedGotIP6           = true;
  #endif
  processedDHCPTimeout      = true;
  ethConnectAttemptNeeded  = true;
  dns0_cache = IPAddress();
  dns1_cache = IPAddress();
}

void EthernetEventData_t::markEthBegin() {
  lastDisconnectMoment.clear();
  lastConnectMoment.clear();
  lastGetIPmoment.clear();
  last_eth_connect_attempt_moment.setNow();
  eth_considered_stable = false;
  ethConnectInProgress  = true;
  ++eth_connect_attempt;
}

bool EthernetEventData_t::EthDisconnected() const {
  return ethStatus == ESPEASY_ETH_DISCONNECTED;
}

bool EthernetEventData_t::EthGotIP() const {

  return bitRead(ethStatus, ESPEASY_ETH_GOT_IP);
}

bool EthernetEventData_t::EthConnected() const {
  return bitRead(ethStatus, ESPEASY_ETH_CONNECTED);
}

bool EthernetEventData_t::EthServicesInitialized() const {
  return bitRead(ethStatus, ESPEASY_ETH_SERVICES_INITIALIZED);
}

void EthernetEventData_t::setEthDisconnected() {
  processedConnect          = true;
  processedDisconnect       = true;
  processedGotIP            = true;
  #if FEATURE_USE_IPV6
  processedGotIP6           = true;
  #endif
  processedDHCPTimeout      = true;

  ethStatus = ESPEASY_ETH_DISCONNECTED;
}

void EthernetEventData_t::setEthGotIP() {
  bitSet(ethStatus, ESPEASY_ETH_GOT_IP);
  setEthServicesInitialized();
}

void EthernetEventData_t::setEthConnected() {
  bitSet(ethStatus, ESPEASY_ETH_CONNECTED);
  setEthServicesInitialized();
}

bool EthernetEventData_t::setEthServicesInitialized() {
  if (!unprocessedEthEvents() && !EthServicesInitialized()) {
    if (EthGotIP() && EthConnected()) {
      if (valid_DNS_address(ETH.dnsIP(0))) {
        dns0_cache = ETH.dnsIP(0);
      }
      if (valid_DNS_address(ETH.dnsIP(1))) {
        dns1_cache = ETH.dnsIP(1);
      }

      #ifndef BUILD_NO_DEBUG
      addLog(LOG_LEVEL_DEBUG, F("Eth : Eth services initialized"));
      #endif
      bitSet(ethStatus, ESPEASY_ETH_SERVICES_INITIALIZED);
      ethConnectInProgress = false;
      return true;
    }
  }
  return false;
}

void EthernetEventData_t::markGotIP() {
  lastGetIPmoment.setNow();

  // Create the 'got IP event' so mark the ethStatus to not have the got IP flag set
  // This also implies the services are not fully initialized.
  bitClear(ethStatus, ESPEASY_ETH_GOT_IP);
  bitClear(ethStatus, ESPEASY_ETH_SERVICES_INITIALIZED);
  processedGotIP = false;
}

#if FEATURE_USE_IPV6
void EthernetEventData_t::markGotIPv6(const IPAddress& ip6) {
  processedGotIP6 = false;
  unprocessed_IP6 = ip6;
}
#endif


void EthernetEventData_t::markLostIP() {
  bitClear(ethStatus, ESPEASY_ETH_GOT_IP);
  bitClear(ethStatus, ESPEASY_ETH_SERVICES_INITIALIZED);
  lastGetIPmoment.clear();
  processedGotIP = false;
}

void EthernetEventData_t::markDisconnect() {
  lastDisconnectMoment.setNow();

  if (last_eth_connect_attempt_moment.isSet() && !lastConnectMoment.isSet()) {
    // There was an unsuccessful connection attempt
    lastConnectedDuration_us = last_eth_connect_attempt_moment.timeDiff(lastDisconnectMoment);
  } else {
    lastConnectedDuration_us = lastConnectMoment.timeDiff(lastDisconnectMoment);
  }
  lastConnectMoment.clear();
  processedDisconnect  = false;
#if ESP_IDF_VERSION_MAJOR >= 5
  WiFi.STA.setDefault();
#endif
}

void EthernetEventData_t::markConnected() {
  lastConnectMoment.setNow();
  processedConnect    = false;
#if ESP_IDF_VERSION_MAJOR >= 5
  ETH.setDefault();
#endif

#if FEATURE_USE_IPV6
  if (Settings.EnableIPv6()) {
    ETH.enableIPv6(true);
  }
#endif
}

String EthernetEventData_t::ESPEasyEthStatusToString() const {
  String log;
  if (EthDisconnected()) {
    log = F("DISCONNECTED");
  } else {
    if (EthConnected()) {
      log += F("Conn. ");
    }
    if (EthGotIP()) {
      log += F("IP ");
    }
    if (EthServicesInitialized()) {
      log += F("Init");
    }
  }
  return log;
}

#endif
