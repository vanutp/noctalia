#include "shell/control_center/tabs/vpn_tab.h"

#include "core/ui_phase.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/tabs/network_row.h"
#include "shell/panel/panel_manager.h"
#include "system/tailscale_service.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

using namespace control_center;

VpnTab::VpnTab(INetworkService* network, TailscaleService* tailscale) : m_network(network), m_tailscale(tailscale) {}

std::unique_ptr<Flex> VpnTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);

  tab->addChild(std::move(listScroll));
  return tab;
}

void VpnTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_active && m_tailscale != nullptr) {
    m_tailscale->refresh();
  }
}

void VpnTab::onClose() {
  m_rootLayout = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0f;
  m_active = false;
}

void VpnTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  rebuildList(renderer);
  m_rootLayout->layout(renderer);
}

void VpnTab::doUpdate(Renderer& renderer) { rebuildList(renderer); }

std::string VpnTab::structureKey(
    const std::vector<VpnConnectionInfo>& vpns, const std::vector<TailscaleExitNode>& exitNodes
) const {
  std::string key;
  for (const auto& vpn : vpns) {
    key += vpn.path;
    key.push_back(':');
    key += vpn.name;
    key.push_back(':');
    key += vpn.active ? '1' : '0';
    key.push_back(':');
    key += vpn.connecting ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& node : exitNodes) {
    key += node.id;
    key.push_back(':');
    key += node.name;
    key.push_back(':');
    key += node.active ? '1' : '0';
    key.push_back(':');
    key += node.online ? '1' : '0';
    key.push_back('\n');
  }
  key += "ts-busy:";
  key += (m_tailscale != nullptr && m_tailscale->busy()) ? '1' : '0';
  key += "\nts-up:";
  key += (m_tailscale != nullptr && m_tailscale->running()) ? '1' : '0';
  key += "\nts-ip:";
  key += m_tailscale != nullptr ? m_tailscale->selfIp() : std::string{};
  return key;
}

void VpnTab::rebuildList(Renderer& renderer) {
  uiAssertNotRendering("VpnTab::rebuildList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0f) {
    return;
  }

  static const std::vector<VpnConnectionInfo> kNoVpns;
  static const std::vector<TailscaleExitNode> kNoExitNodes;
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : kNoVpns;
  const auto& exitNodes = m_tailscale != nullptr ? m_tailscale->exitNodes() : kNoExitNodes;
  const std::string nextStructure = structureKey(vpns, exitNodes);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;

  const float scale = contentScale();
  const float opacity = panelCardOpacity();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  // NetworkManager profiles and tailscale exit nodes go into one list: both are
  // tunnels the user picks between, and the leading icon says which is which.
  std::vector<ConnectionRowSpec> rows;
  rows.reserve(vpns.size() + exitNodes.size());
  for (const auto& vpn : vpns) {
    rows.push_back({
        .name = vpn.name,
        .glyph = "shield-lock",
        .active = vpn.active,
        .connecting = vpn.connecting,
        .onActivate =
            [this, vpn]() {
              if (m_network != nullptr) {
                m_network->activateVpnConnection(vpn);
              }
              PanelManager::instance().refresh();
            },
        .onDeactivate =
            [this, vpn]() {
              if (m_network != nullptr) {
                m_network->deactivateVpnConnection(vpn);
              }
              PanelManager::instance().refresh();
            },
    });
  }
  for (const auto& node : exitNodes) {
    rows.push_back({
        .name = node.name,
        .iconAsset = node.online ? "tailscale.svg" : "tailscale-off.svg",
        .active = node.active,
        .enabled = !m_tailscale->busy(),
        .onActivate =
            [this, node]() {
              if (m_tailscale != nullptr) {
                m_tailscale->connectExitNode(node);
              }
              PanelManager::instance().refresh();
            },
        .onDeactivate =
            [this]() {
              if (m_tailscale != nullptr) {
                m_tailscale->disconnectExitNode();
              }
              PanelManager::instance().refresh();
            },
    });
  }

  std::ranges::stable_partition(rows, [](const ConnectionRowSpec& row) { return row.active || row.connecting; });

  if (m_tailscale != nullptr && m_tailscale->available()) {
    auto tailscaleCard = ui::column({
        .configure = [scale, opacity](Flex& node) { applySectionCardStyle(node, scale, opacity); },
    });
    // The tailnet address is a property of the running backend, so it only shows
    // under the title while tailscale is up.
    const std::string selfIp = m_tailscale->running() ? m_tailscale->selfIp() : std::string{};
    auto titleColumn = ui::column(
        {.align = FlexAlign::Start, .flexGrow = 1.0F},
        ui::label({
            .text = i18n::tr("control-center.vpn.tailscale"),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    if (!selfIp.empty()) {
      titleColumn->addChild(
          ui::label({
              .text = selfIp,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    header->addChild(std::move(titleColumn));
    header->addChild(
        ui::toggle({
            .checkedImmediate = m_tailscale->running(),
            .enabled = !m_tailscale->busy(),
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_tailscale != nullptr) {
                m_tailscale->setEnabled(checked);
              }
              PanelManager::instance().refresh();
            },
        })
    );
    tailscaleCard->addChild(std::move(header));
    m_list->addChild(std::move(tailscaleCard));
  }

  if (!rows.empty()) {
    auto card = ui::column({
        .configure = [scale, opacity](Flex& node) { applySectionCardStyle(node, scale, opacity); },
    });
    card->addChild(makeCardHeaderRow(i18n::tr("control-center.vpn.connections"), scale));
    for (auto& row : rows) {
      card->addChild(std::make_unique<ConnectionRow>(renderer, scale, std::move(row)));
    }
    m_list->addChild(std::move(card));
  }

  m_list->layout(renderer);
}
