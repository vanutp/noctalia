#pragma once

#include "core/timer_manager.h"
#include "dbus/network/inetwork_service.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class NetworkManagerService : public INetworkService {
  struct CompetingLink;
  struct PromotionScan;
  struct RouteMetrics;

public:
  using ChangeCallback = std::function<void(const NetworkState&, NetworkChangeOrigin)>;

  explicit NetworkManagerService(SystemBus& bus);
  ~NetworkManagerService() override;

  NetworkManagerService(const NetworkManagerService&) = delete;
  NetworkManagerService& operator=(const NetworkManagerService&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override;

  // Follows the org.freedesktop.NetworkManager bus name: NetworkManager may start after the shell or
  // restart mid-session. Nothing is sent to it while it is off the bus.
  [[nodiscard]] bool available() const noexcept override { return m_nm != nullptr; }
  [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept override { return m_hasStateSnapshot; }
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_accessPoints; }
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
    return m_vpnConnections;
  }
  [[nodiscard]] const std::vector<WiredConnectionInfo>& wiredConnections() const noexcept override {
    return m_wiredConnections;
  }

  // Trigger a Wi-Fi scan on every wifi device. Results arrive via PropertiesChanged.
  void requestScan() override;

  // Activate a saved connection for the given access point, or create an
  // in-memory profile for a new network and persist it after activation succeeds.
  // NM picks the matching saved connection automatically when the first argument is "/".
  // Returns false only on an immediate D-Bus error.
  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;

  [[nodiscard]] bool supportsEnterprise() const noexcept override { return true; }
  bool activateEnterpriseAccessPoint(
      const AccessPointInfo& ap, const network_enterprise::EnterpriseCredentials& credentials
  ) override;

  // Activate / deactivate a saved VPN connection profile. Deactivate also
  // aborts a connection that is stuck activating.
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override;
  [[nodiscard]] bool canActivateWiredConnection() const noexcept override;
  bool activateWiredConnection() override;
  [[nodiscard]] bool canActivateCellularConnection() const noexcept override;
  bool activateCellularConnection() override;
  bool deactivateCellularConnection() override;

  // Activate / deactivate one saved wired profile. Deactivation goes through
  // Device.Disconnect so autoconnect does not immediately bring it back up.
  bool activateWiredConnection(const WiredConnectionInfo& wired) override;
  bool deactivateWiredConnection(const WiredConnectionInfo& wired) override;

  // Enable / disable the Wi-Fi radio.
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) override;

  // Disconnect the active physical connection.
  void disconnect() override;

  // Disconnect the wifi device this access point belongs to, leaving any other
  // link (wired, VPN) untouched.
  bool disconnectAccessPoint(const AccessPointInfo& ap) override;

  // Hand the default route to an already-connected link.
  bool makePrimary(const AccessPointInfo& ap) override;
  bool makePrimary(const WiredConnectionInfo& wired) override;

  // Delete every saved connection whose 802-11-wireless SSID matches.
  void forgetSsid(const std::string& ssid) override;

  // Whether any saved connection matches the SSID (uses cached snapshot refreshed on every refresh()).
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override;

private:
  void refreshAccessPoints(std::function<void()> onComplete);
  void refreshSavedConnections(std::function<void()> onComplete);
  // Rebuilds the VPN profile list and, from one pass over NM's active
  // connections, the derived flags those profiles share with cellular
  // (m_anyVpnConnected, m_anyCellularActive).
  void refreshVpnAndActiveConnections(std::function<void()> onComplete);
  void refreshLinkDetails(std::function<void()> onComplete);
  void reconcileVpnActiveWatchers(const std::set<std::string>& activePaths);
  void finishSavedConnections(
      std::vector<std::string>& ssids, std::vector<WiredConnectionInfo>& wiredConnections,
      std::vector<std::string>& cellularConnectionPaths, std::function<void()> onComplete
  );
  void disconnectWiredActiveConnection(const std::string& activePath, const std::string& devicePath);
  bool makeProfilePrimary(const std::string& profilePath, const std::string& devicePath);
  // Every active non-tunnel link except the excluded profile, with the metrics its
  // IPv4 and IPv6 default routes currently hold, so a promotion can undercut the
  // real winner of each family instead of a guess.
  void
  collectCompetingLinks(const std::string& excludeProfilePath, std::function<void(std::vector<CompetingLink>)> done);
  void applyRouteMetric(
      const std::string& profilePath, const std::string& devicePath, const RouteMetrics& metrics,
      std::function<void()> onDone
  );
  // Re-applying a profile drops and rebuilds its link, so its address only exists
  // once activation finishes. Watch for that and refresh, or the row keeps showing
  // the empty mid-activation snapshot until something else forces a re-read.
  void watchReactivation(const std::string& activePath);
  void finishRefreshAccessPoints(std::vector<AccessPointInfo>& aps, std::function<void()> onComplete);
  bool addAndActivateAccessPoint(
      const AccessPointInfo& ap, const std::optional<std::string>& psk,
      const std::optional<network_enterprise::EnterpriseCredentials>& credentials = std::nullopt
  );
  void watchPendingAccessPointActivation(
      const std::string& ssid, const std::string& connectionPath, const std::string& activePath
  );
  void handlePendingAccessPointActivationState(const std::string& activePath, std::uint32_t state);
  void persistConnectionToDisk(const std::string& connectionPath, const std::string& ssid);
  void deleteUnsavedConnection(const std::string& connectionPath, const std::string& ssid);
  // Async rebind pipeline. All proxy destruction happens in async reply
  // context, never inside a proxy's own signal handler.
  void requestRebind();
  // allowActivatedAsPrimary: when false (no NM PrimaryConnection yet), a fully
  // activated physical device must not be reported as the connected primary — it
  // may be a bridge/bond slave or a link that has not become the default route.
  // Only mid-activation links are surfaced (resolving state).
  void resolvePhysicalPrimary(
      bool allowActivatedAsPrimary, std::function<void(std::string connectionPath, std::string devicePath)> done
  );
  void adoptActiveConnection(const std::string& connectionPath, const std::string& devicePath);
  void rebindActiveDevice(const std::string& devicePath);
  void rebindActiveAccessPoint(const std::string& apPath);
  void ensureWifiDeviceSubscribed(const std::string& devicePath);
  void
  collectWifiDevices(std::function<void(std::vector<std::string> devicePaths, std::int64_t lastScanBaseline)> done);
  void tryActivateWiredConnection(std::shared_ptr<std::vector<std::string>> candidates, std::size_t index);
  void tryActivateCellularConnection(std::shared_ptr<std::vector<std::string>> candidates, std::size_t index);
  // Shared deactivate-by-profile-paths machinery used by the VPN and cellular
  // toggles. Deactivates active (or stuck-activating) connections whose profile
  // path is in the set. Returns false only on an immediate dispatch error.
  bool deactivateConnectionsByProfilePaths(const std::set<std::string>& profilePaths, std::string_view kindTag);
  // Subscribe to the running NetworkManager and read its state.
  void attach();
  // Drop every proxy, cached object path, and derived state of the instance that left.
  void detach();
  void readStateAsync(std::function<void(NetworkState)> onComplete);
  [[nodiscard]] NetworkChangeOrigin consumeWirelessEnabledChangeOrigin(bool enabled);
  void beginScan(std::int64_t lastScanBaseline);
  void endScan();

  struct PendingAccessPointActivation;

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_busDaemon; // NameOwnerChanged watch for NetworkManager.
  std::unique_ptr<sdbus::IProxy> m_nm;
  std::unique_ptr<sdbus::IProxy> m_activeConnection;
  std::unique_ptr<sdbus::IProxy> m_activeDevice;
  std::unique_ptr<sdbus::IProxy> m_activeAp;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_wifiDevices;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_vpnActiveWatchers;
  std::string m_activeConnectionPath;
  std::string m_activeDevicePath;
  std::string m_activeApPath;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  std::vector<VpnConnectionInfo> m_vpnConnections;
  std::vector<std::string> m_savedSsids;
  std::vector<WiredConnectionInfo> m_wiredConnections;
  std::vector<std::string> m_savedCellularConnectionPaths;
  // Profile paths of every connection currently activating or activated, refreshed
  // by the VPN active-connection scan and applied to m_wiredConnections on emit.
  std::set<std::string> m_activeProfilePaths;
  // Wired profiles a present ethernet device could activate, and the address every
  // active link holds, keyed both by profile (for wired rows) and by device (for
  // access points). All joined into the published lists on emit.
  std::set<std::string> m_wiredAvailableProfilePaths;
  std::map<std::string, std::string> m_ipv4ByProfilePath;
  std::map<std::string, std::string> m_ipv4ByDevicePath;
  std::map<std::string, std::string> m_devicePathByProfilePath;
  std::map<std::string, std::string> m_profilePathByDevicePath;
  // Watcher for the link a promotion re-activated. Replaced only from async reply
  // context, never torn down inside its own handler.
  std::unique_ptr<sdbus::IProxy> m_reactivationWatcher;
  std::unordered_map<std::string, std::unique_ptr<PendingAccessPointActivation>> m_pendingApActivations;
  // Finished activations whose proxy may still be executing its own handler;
  // freed at the next refresh completion (an async reply context).
  std::vector<std::unique_ptr<PendingAccessPointActivation>> m_retiredApActivations;
  std::shared_ptr<int> m_lifetimeToken;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
  bool m_rebindInFlight = false;
  bool m_rebindQueued = false;
  bool m_emitOnNextRefresh = false;
  bool m_scanning = false;
  bool m_anyVpnConnected = false;
  bool m_anyCellularActive = false;
  std::int64_t m_scanBaselineLastScan = 0;
  Timer m_scanTimeoutTimer;
  std::uint64_t m_scanGeneration = 0;
  std::optional<bool> m_pendingLocalWirelessEnabled;
  bool m_hasStateSnapshot = false;
  ChangeCallback m_changeCallback;

  static constexpr std::chrono::seconds kScanTimeout = std::chrono::seconds(30);
};
