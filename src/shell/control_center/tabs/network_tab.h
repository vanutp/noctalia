#pragma once

#include "core/timer_manager.h"
#include "dbus/network/enterprise_credentials.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AccessPointRow;
class ConnectionRow;
class Button;
class CellularRow;
class ExternalIpService;
class Flex;
class Input;
class Label;
class ModemManagerService;
class ScrollView;
class Select;
class Spinner;
class Toggle;
class INetworkService;

class NetworkTab : public Tab {
public:
  NetworkTab(
      INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp, ModemManagerService* modem
  );
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  // A backend exists and its daemon is on the bus.
  [[nodiscard]] bool networkAvailable() const noexcept;
  // Values that move without changing the list structure: the connected row's
  // addresses, the Wi-Fi toggle, the scan spinner.
  void syncLiveState(Renderer& renderer);
  void requestWirelessEnabled(bool enabled);
  void handleWirelessEnabledCompletion(std::uint64_t generation, bool success);
  void rebuildApList(Renderer& renderer);
  [[nodiscard]] int activeLinkCount() const;
  [[nodiscard]] bool isPrimaryDevice(const std::string& devicePath) const;
  // Pushes live signal values into the existing rows. Returns true if any changed.
  bool syncApRows();
  // Same for the cellular rows and the cellular toggle. Returns true if any changed.
  bool syncCellularCard();
  // GNOME-style mobile-data semantics when the network backend owns a saved gsm
  // connection (toggle reflects cellularActive); otherwise raw modem power.
  [[nodiscard]] bool cellularToggleChecked() const;
  [[nodiscard]] bool cellularToggleDisplayChecked() const;
  void requestCellularEnabled(bool enabled);
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  // Reads the enterprise form back out. Password comes from the shared field.
  [[nodiscard]] network_enterprise::EnterpriseCredentials
  collectEnterpriseCredentials(const std::string& password) const;
  // Shows a message inside the credential card; empty hides the row. The prompt
  // stays open so the user can correct what is wrong.
  void setCredentialError(const std::string& message);
  // Reason this access point cannot be joined with a password, empty when it can.
  [[nodiscard]] std::string enterpriseBlockReason(const AccessPointInfo& ap) const;
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<WiredConnectionInfo>& wired) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  ExternalIpService* m_externalIpService = nullptr;
  ModemManagerService* m_modem = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  bool m_passwordRevealed = false;
  // 802.1X form. Hidden for pre-shared-key networks, which keep the single
  // password field below it.
  Flex* m_enterpriseFields = nullptr;
  Select* m_eapSelect = nullptr;
  Select* m_phase2Select = nullptr;
  Input* m_identityInput = nullptr;
  Input* m_anonymousIdentityInput = nullptr;
  Input* m_caCertInput = nullptr;
  Input* m_domainMatchInput = nullptr;
  Label* m_credentialError = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Spinner* m_scanSpinner = nullptr;

  // Rows by identity, so the live address sync can reach every connected link
  // rather than one remembered row: two NICs can be up at once, each with its own
  // address. Cleared whenever the list is torn down.
  std::unordered_map<std::string, AccessPointRow*> m_apRows;
  std::unordered_map<std::string, ConnectionRow*> m_wiredRows;

  Toggle* m_cellularToggle = nullptr;
  std::vector<CellularRow*> m_cellularRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;

  bool m_hasPendingSecret = false;
  bool m_pendingEnterprise = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  bool m_wifiTogglePending = false;
  bool m_wifiToggleTarget = false;
  bool m_wifiToggleWriteComplete = false;
  bool m_wifiToggleTargetObserved = false;
  std::uint64_t m_wifiToggleRequestGeneration = 0;

  // A cellular request is only observable once ModemManager and NM have walked
  // the modem through enable/registration, so the switch shows the requested
  // position until then. It stays clickable: a second click just retargets.
  bool m_cellularTogglePending = false;
  bool m_cellularToggleTarget = false;
  std::chrono::steady_clock::time_point m_cellularTogglePendingSince;

  Timer m_cellularTogglePendingTimer;

  static constexpr std::chrono::seconds kCellularPendingTimeout = std::chrono::seconds(25);
};
