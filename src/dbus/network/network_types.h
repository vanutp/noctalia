#pragma once

#include "dbus/network/network_manager_security.h"

#include <cstdint>
#include <string>

struct AccessPointInfo {
  std::string path;       // Backend access point object path.
  std::string devicePath; // Backend device object path this AP belongs to.
  std::string ssid;
  std::string ipv4;          // address of the device carrying this AP while connected
  std::uint8_t strength = 0; // 0..100
  bool secured = false;
  // How the AP wants to be authenticated, derived from its RSN flags. Drives both
  // the credential form the UI shows and the NM key-mgmt value we write. A single
  // enum rather than parallel bools so contradictory states cannot be built.
  network_manager_security::KeyManagement keyManagement = network_manager_security::KeyManagement::Psk;
  bool active = false;

  [[nodiscard]] bool isEnterprise() const noexcept { return network_manager_security::isEnterprise(keyManagement); }

  // True when the UI should collect credentials before connecting. OWE stays
  // secured (lock icon) but has no password to ask for.
  [[nodiscard]] bool requiresCredentials() const noexcept {
    return network_manager_security::requiresCredentials(secured, keyManagement);
  }

  bool operator==(const AccessPointInfo&) const = default;
};

struct VpnConnectionInfo {
  std::string path; // Backend settings connection object path.
  std::string name;
  bool active = false;

  bool operator==(const VpnConnectionInfo&) const = default;
};

struct WiredConnectionInfo {
  std::string path;       // Backend settings connection object path.
  std::string devicePath; // Device carrying the profile while active; empty otherwise.
  std::string name;
  std::string ipv4; // address of the link while active; empty otherwise
  bool active = false;
  bool virtualLink = false; // bridge, bond, team or vlan rather than a plain NIC

  bool operator==(const WiredConnectionInfo&) const = default;
};

enum class NetworkConnectivity {
  Unknown = 0,
  None = 1,
  Wired = 2,
  Wireless = 3,
  Cellular = 4,
};

struct NetworkState {
  NetworkConnectivity kind = NetworkConnectivity::Unknown;
  bool connected = false;
  bool resolving = false; // active connection is activating, not yet connected
  bool wirelessEnabled = false;
  bool scanning = false;
  bool vpnActive = false;          // a VPN connection is active or activating
  bool vpnConnected = false;       // a VPN tunnel is fully activated (routes applied)
  bool cellularActive = false;     // a cellular (gsm) connection is active or activating, primary or not
  std::string ssid;                // Wi-Fi only
  std::string ipv4;                // dotted-quad of first address; empty if none
  std::string interfaceName;       // e.g. "wlan0", "eth0"
  std::string primaryDevicePath;   // device carrying the default route; empty if none
  std::uint8_t signalStrength = 0; // 0..100, Wi-Fi only
  // Operating frequency of the associated BSS. Wi-Fi only; 0 when the backend
  // does not report one (iwd).
  std::uint32_t frequencyMhz = 0;

  bool operator==(const NetworkState&) const = default;
};

enum class NetworkChangeOrigin : std::uint8_t {
  External,
  Noctalia,
};
