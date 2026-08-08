#pragma once

#include "dbus/network/network_types.h"
#include "shell/control_center/tab.h"

#include <memory>
#include <string>
#include <vector>

class Flex;
class ScrollView;
class INetworkService;
class TailscaleService;
struct TailscaleExitNode;

class VpnTab : public Tab {
public:
  VpnTab(INetworkService* network, TailscaleService* tailscale);

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void rebuildList(Renderer& renderer);
  [[nodiscard]] std::string
  structureKey(const std::vector<VpnConnectionInfo>& vpns, const std::vector<TailscaleExitNode>& exitNodes) const;

  INetworkService* m_network = nullptr;
  TailscaleService* m_tailscale = nullptr;

  Flex* m_rootLayout = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0f;
  bool m_active = false;
};
