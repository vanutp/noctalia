#include "shell/bar/widgets/vpn_widget.h"

#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/tailscale_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

VpnWidget::VpnWidget(INetworkService* network, TailscaleService* tailscale, wl_output* /*output*/, Options options)
    : m_network(network), m_tailscale(tailscale), m_showLabel(options.showLabel),
      m_hideWhenDisconnected(options.hideWhenDisconnected) {}

// A NetworkManager tunnel first, otherwise the tailscale exit node — both route
// traffic elsewhere, so either one lights the indicator.
std::string VpnWidget::activeTunnelName() const {
  if (m_network != nullptr) {
    for (const auto& vpn : m_network->vpnConnections()) {
      if (vpn.active && !vpn.name.empty()) {
        return vpn.name;
      }
    }
  }
  return m_tailscale != nullptr ? m_tailscale->activeExitNodeName() : std::string{};
}

bool VpnWidget::vpnConnected() const {
  return (m_network != nullptr && m_network->state().vpnConnected)
      || (m_tailscale != nullptr && m_tailscale->exitNodeActive());
}

// A NetworkManager tunnel that is activating: the indicator stays off until the
// tunnel actually carries traffic.
bool VpnWidget::vpnConnecting() const {
  return m_network != nullptr && m_network->state().vpnActive && !m_network->state().vpnConnected;
}

bool VpnWidget::hasAnyTunnel() const {
  return (m_network != nullptr && !m_network->vpnConnections().empty())
      || (m_tailscale != nullptr && m_tailscale->available());
}

void VpnWidget::create() {
  auto area = ui::inputArea({});

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "shield-off",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  if (m_showLabel) {
    area->addChild(
        ui::label({
            .out = &m_label,
            .fontSize = Style::fontSizeBody * m_contentScale,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
        })
    );
  }

  setRoot(std::move(area));
}

void VpnWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }
  syncState(renderer);

  m_glyph->measure(renderer);

  float totalWidth = m_glyph->width();
  float contentHeight = m_glyph->height();
  if (m_label != nullptr) {
    m_label->measure(renderer);
    if (m_label->width() > 0.0f) {
      contentHeight = std::max(contentHeight, m_label->height());
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((contentHeight - m_label->height()) * 0.5f));
      totalWidth = m_label->x() + m_label->width();
    }
  }
  m_glyph->setPosition(0.0f, std::round((contentHeight - m_glyph->height()) * 0.5f));
  rootNode->setSize(totalWidth, contentHeight);
}

void VpnWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void VpnWidget::syncState(Renderer& renderer) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  const bool connected = vpnConnected();
  const bool connecting = vpnConnecting();
  const bool hasTunnel = hasAnyTunnel();
  const std::string name = activeTunnelName();
  if (m_haveLastState
      && connected == m_lastConnected
      && connecting == m_lastConnecting
      && hasTunnel == m_lastHasTunnel
      && name == m_lastName) {
    return;
  }
  m_lastConnected = connected;
  m_lastConnecting = connecting;
  m_lastHasTunnel = hasTunnel;
  m_lastName = name;
  m_haveLastState = true;

  // Nothing to connect to means nothing to say: the widget collapses rather than
  // sitting in the bar as a permanently dead icon.
  const bool showWidget = hasTunnel && (!m_hideWhenDisconnected || connected || connecting);
  if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
    requestUpdate();
  }
  if (!showWidget) {
    static_cast<InputArea*>(rootNode)->clearTooltip();
    return;
  }

  if (connected) {
    m_glyph->setGlyph(network_display::vpnGlyph());
  } else {
    m_glyph->setGlyph(connecting ? network_display::vpnConnectingGlyph() : "shield-off");
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  if (m_label != nullptr) {
    m_label->setText(connected ? name : std::string{});
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  std::string status = i18n::tr("bar.widgets.network.not-connected");
  if (connected) {
    status = name;
  } else if (connecting) {
    status = i18n::tr("bar.widgets.network.connecting");
  }
  std::vector<TooltipRow> rows;
  rows.push_back({i18n::tr("bar.widgets.network.vpn"), std::move(status)});
  static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));

  requestRedraw();
}
