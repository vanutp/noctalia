#pragma once

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class Label;
class INetworkService;
class TailscaleService;
struct wl_output;

// Bar indicator for VPN tunnels: NetworkManager profiles and tailscale exit
// nodes alike. Connecting happens in the control center's VPN tab, which the
// default left-click gesture opens.
class VpnWidget : public Widget {
public:
  struct Options {
    bool showLabel = false;
    bool hideWhenDisconnected = false;
  };

  VpnWidget(INetworkService* network, TailscaleService* tailscale, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::string activeTunnelName() const;
  [[nodiscard]] bool vpnConnected() const;
  [[nodiscard]] bool vpnConnecting() const;
  [[nodiscard]] bool hasAnyTunnel() const;

  INetworkService* m_network = nullptr;
  TailscaleService* m_tailscale = nullptr;
  bool m_showLabel = false;
  bool m_hideWhenDisconnected = false;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_lastName;
  bool m_lastConnected = false;
  bool m_lastConnecting = false;
  bool m_lastHasTunnel = false;
  bool m_haveLastState = false;
};
