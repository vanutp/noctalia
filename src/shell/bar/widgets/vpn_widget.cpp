#include "shell/bar/widgets/vpn_widget.h"

#include "dbus/network/inetwork_service.h"
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

bool VpnWidget::vpnActive() const {
  return (m_network != nullptr && m_network->state().vpnActive)
      || (m_tailscale != nullptr && m_tailscale->exitNodeActive());
}

bool VpnWidget::hasAnyTunnel() const {
  return (m_network != nullptr && !m_network->vpnConnections().empty())
      || (m_tailscale != nullptr && !m_tailscale->exitNodes().empty());
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

  const bool active = vpnActive();
  const bool hasTunnel = hasAnyTunnel();
  const std::string name = activeTunnelName();
  if (m_haveLastState && active == m_lastActive && hasTunnel == m_lastHasTunnel && name == m_lastName) {
    return;
  }
  m_lastActive = active;
  m_lastHasTunnel = hasTunnel;
  m_lastName = name;
  m_haveLastState = true;

  // Nothing to connect to means nothing to say: the widget collapses rather than
  // sitting in the bar as a permanently dead icon.
  const bool showWidget = hasTunnel && (!m_hideWhenDisconnected || active);
  if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
    requestUpdate();
  }
  if (!showWidget) {
    static_cast<InputArea*>(rootNode)->clearTooltip();
    return;
  }

  m_glyph->setGlyph(active ? "shield-check" : "shield-off");
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  if (m_label != nullptr) {
    m_label->setText(active ? name : std::string{});
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  std::vector<TooltipRow> rows;
  rows.push_back({i18n::tr("bar.widgets.network.vpn"), active ? name : i18n::tr("bar.widgets.network.not-connected")});
  static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));

  requestRedraw();
}
