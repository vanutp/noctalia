#include "shell/control_center/tabs/network_tab.h"

#include "core/ui_phase.h"
#include "dbus/modem/modem_manager_service.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/tabs/network_row.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  // Label and wire value in one row, so the visible dropdown order and the value
  // sent to NetworkManager cannot drift apart.
  struct EapOption {
    std::string_view label;
    network_enterprise::EapMethod method;
  };

  struct Phase2Option {
    std::string_view label;
    network_enterprise::Phase2Auth auth;
  };

  constexpr std::array kEapOptions{
      EapOption{"PEAP", network_enterprise::EapMethod::Peap},
      EapOption{"TTLS", network_enterprise::EapMethod::Ttls},
  };

  // MSCHAPv2 first: it is the only inner method PEAP accepts, and PEAP is the
  // default EAP method.
  constexpr std::array kPhase2Options{
      Phase2Option{"MSCHAPv2", network_enterprise::Phase2Auth::MsChapV2},
      Phase2Option{"PAP", network_enterprise::Phase2Auth::Pap},
      Phase2Option{"MSCHAP", network_enterprise::Phase2Auth::MsChap},
      Phase2Option{"CHAP", network_enterprise::Phase2Auth::Chap},
  };

  constexpr std::size_t phase2IndexOf(network_enterprise::Phase2Auth auth) {
    for (std::size_t index = 0U; index < kPhase2Options.size(); ++index) {
      if (kPhase2Options[index].auth == auth) {
        return index;
      }
    }
    return kPhase2Options.size();
  }

  // Both dropdowns open on their first option, and PEAP accepts MSCHAPv2 only, so
  // choosing PEAP pins the inner dropdown to this row.
  constexpr std::size_t kDefaultOptionIndex = 0U;
  constexpr std::size_t kPeapPhase2Index = phase2IndexOf(network_enterprise::Phase2Auth::MsChapV2);
  static_assert(kPeapPhase2Index < kPhase2Options.size());
  static_assert(kEapOptions[kDefaultOptionIndex].method == network_enterprise::EapMethod::Peap);
  static_assert(kPeapPhase2Index == kDefaultOptionIndex);

  template <typename Options> std::vector<std::string> optionLabels(const Options& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) {
      labels.emplace_back(option.label);
    }
    return labels;
  }

  // A Select with no selection reports npos; the first option is the documented
  // default for both dropdowns.
  template <typename Options> const auto& optionAt(const Options& options, std::size_t index) {
    return index < options.size() ? options[index] : options.front();
  }

  std::string validationMessage(network_enterprise::Validation problem) {
    switch (problem) {
    case network_enterprise::Validation::MissingIdentity:
      return i18n::tr("control-center.network.error-missing-identity");
    case network_enterprise::Validation::MissingPassword:
      return i18n::tr("control-center.network.error-missing-password");
    case network_enterprise::Validation::UnsupportedPhase2:
      return i18n::tr("control-center.network.error-unsupported-phase2");
    case network_enterprise::Validation::CaCertNotAbsolute:
      return i18n::tr("control-center.network.error-ca-not-absolute");
    case network_enterprise::Validation::Ok:
      break;
    }
    return {};
  }

  // Subtitle of a connected row: its own address, plus the WAN address when this
  // link is the one carrying the default route — the WAN address is a property of
  // that route, so it would be a lie under any other link.
  std::string
  connectedDetail(const std::string& ipv4, std::uint32_t frequencyMhz, bool isPrimary, const std::string& externalIp) {
    std::string out;
    const auto append = [&out](std::string_view part) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += part;
    };
    if (!ipv4.empty()) {
      append(ipv4);
    }
    if (const char* band = network_display::wifiFrequencyBandLabel(frequencyMhz); band != nullptr) {
      append(band);
    }
    if (isPrimary && !externalIp.empty()) {
      append(i18n::tr("control-center.network.external-ip", "ip", externalIp));
    }
    return out;
  }
  const char* cellularGlyphFor(const CellularModemInfo& modem) {
    return modem.enabled() ? network_display::cellularGlyphForSignal(modem.signalQuality)
                           : network_display::cellularOffGlyph();
  }

  std::string cellularTitleFor(const CellularModemInfo& modem) {
    if (!modem.operatorName.empty()) {
      return modem.operatorName;
    }
    if (!modem.name.empty()) {
      return modem.name;
    }
    return i18n::tr("control-center.network.cellular");
  }

  std::string cellularDetailFor(const CellularModemInfo& modem) {
    std::string out = cellularStateText(modem.state);
    if (modem.enabled()) {
      if (const char* tech = cellularAccessTechnologyName(modem.accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
      out += "  •  " + std::to_string(static_cast<int>(modem.signalQuality)) + "%";
    }
    return out;
  }

  std::string percentText(std::uint8_t percent) { return std::to_string(static_cast<int>(percent)) + "%"; }

  std::unique_ptr<Flex> makeBucketHeaderRow(const std::string& title, float scale) {
    auto row = ui::row({
        .align = FlexAlign::Center,
    });

    auto pill = ui::row({
        .align = FlexAlign::Center,
        .paddingV = Style::spaceXs * 0.5F * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.8F),
        .radius = Style::scaledRadiusSm(scale),
    });
    pill->addChild(
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(std::move(pill));
    return row;
  }

  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_display::wifiSignalBand(a.strength);
      const int bandB = network_display::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

class AccessPointRow : public NetworkRowBase {
public:
  AccessPointRow(
      float scale, AccessPointInfo ap, bool saved, bool primary, bool showPrimaryBadge,
      std::function<void(const AccessPointInfo&)> onActivate, std::function<void(const AccessPointInfo&)> onForget,
      std::function<void()> onDisconnect, std::function<void()> onMakePrimary
  )
      : NetworkRowBase(scale, primary), m_ap(std::move(ap)), m_onActivate(std::move(onActivate)),
        m_onForget(std::move(onForget)), m_onDisconnect(std::move(onDisconnect)),
        m_onMakePrimary(std::move(onMakePrimary)) {
    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = network_display::wifiGlyphForSignal(m_ap.strength),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addTitleColumn(scale, m_ap.ssid, m_ap.active);

    if (showPrimaryBadge) {
      addPrimaryBadge(scale);
    }

    if (m_ap.secured) {
      addChild(
          ui::glyph({
              .glyph = "lock",
              .glyphSize = Style::baseGlyphSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    addChild(
        ui::label({
            .out = &m_signalValue,
            .text = percentText(m_ap.strength),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    // A row with no action still reserves the slot rather than collapsing it, so
    // the signal percent stays aligned down the whole list.
    const float actionOpacity = (m_ap.active || saved) ? 1.0F : 0.0F;
    Button* actionButton = nullptr;
    auto action = ui::button({
        .out = &actionButton,
        .glyphSize = Style::baseGlyphSize * scale,
        .variant = m_ap.active ? ButtonVariant::Destructive : ButtonVariant::Ghost,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusSm(scale),
        .opacity = actionOpacity,
    });
    if (m_ap.active) {
      action->setGlyph("plug-off");
      action->setOnClick([this]() {
        if (m_onDisconnect) {
          m_onDisconnect();
        }
      });
    } else if (saved) {
      action->setGlyph("trash");
      action->setOnClick([this]() {
        if (m_onForget) {
          m_onForget(m_ap);
        }
      });
    }
    addChild(std::move(action));

    finishRow(actionButton);
  }

  [[nodiscard]] const std::string& ssid() const noexcept { return m_ap.ssid; }

  // Refresh the values that move while a scan runs — the signal glyph and percent.
  // Everything else about the row is structural and belongs to the list key.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const AccessPointInfo& ap) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(network_display::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(ap.strength))) {
      changed = true;
    }
    m_ap.strength = ap.strength;
    return changed;
  }

private:
  // A connected row is promoted to primary rather than torn down; disconnecting
  // stayed on the button.
  void onRowClicked() override {
    if (m_ap.active) {
      if (m_onMakePrimary) {
        m_onMakePrimary();
      }
      return;
    }
    if (m_onActivate) {
      m_onActivate(m_ap);
    }
  }

  AccessPointInfo m_ap;
  std::function<void(const AccessPointInfo&)> m_onActivate;
  std::function<void(const AccessPointInfo&)> m_onForget;
  std::function<void()> m_onDisconnect;
  std::function<void()> m_onMakePrimary;
  Glyph* m_signalGlyph = nullptr;
  Label* m_signalValue = nullptr;
};

// Informational modem row: signal glyph, operator/modem name, and live status
// detail. It has no actions of its own; the card header carries the enable toggle.
class CellularRow : public Flex {
public:
  CellularRow(float scale, CellularModemInfo modem) : m_modem(std::move(modem)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = cellularGlyphFor(m_modem),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = cellularTitleFor(m_modem),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    addChild(
        ui::label({
            .out = &m_detail,
            .text = cellularDetailFor(m_modem),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  // Push live modem values into the row. Returns true when a value actually changed.
  bool syncLive(const CellularModemInfo& modem) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(cellularGlyphFor(modem))) {
      changed = true;
    }
    if (m_title != nullptr && m_title->setText(cellularTitleFor(modem))) {
      changed = true;
    }
    if (m_detail != nullptr && m_detail->setText(cellularDetailFor(modem))) {
      changed = true;
    }
    m_modem = modem;
    return changed;
  }

private:
  CellularModemInfo m_modem;
  Glyph* m_signalGlyph = nullptr;
  Label* m_title = nullptr;
  Label* m_detail = nullptr;
};

NetworkTab::NetworkTab(
    INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp, ModemManagerService* modem
)
    : m_network(network), m_secrets(secrets), m_externalIpService(externalIp), m_modem(modem) {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
}

NetworkTab::~NetworkTab() {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

std::unique_ptr<Flex> NetworkTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto passwordCard = ui::column({
      .out = &m_passwordCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity);
        card.setGap(Style::spaceMd * scale);
      },
  });

  passwordCard->addChild(
      ui::label({
          .out = &m_passwordTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  passwordCard->addChild(
      ui::label({
          .out = &m_credentialError,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 4,
          .visible = false,
      })
  );

  const auto submitFromForm = [this](const std::string& /*value*/) {
    submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
  };

  auto enterpriseFields = ui::column({
      .out = &m_enterpriseFields,
      // Flex defaults to Center on the cross axis, which collapses each field to
      // its natural width. The card itself stretches via applySectionCardStyle();
      // this nested column has to say so for itself.
      .align = FlexAlign::Stretch,
      .visible = false,
      .configure = [scale](Flex& column) { column.setGap(Style::spaceSm * scale); },
  });

  enterpriseFields->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::select({
              .out = &m_eapSelect,
              .options = optionLabels(kEapOptions),
              .selectedIndex = std::size_t{0},
              .placeholder = i18n::tr("control-center.network.eap-method"),
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*text*/) {
                    // PEAP tunnels MSCHAPv2 only. Pinning and locking the inner method
                    // beats letting the user assemble a pair that cannot authenticate
                    // and only fails later, at association time.
                    if (m_phase2Select == nullptr) {
                      return;
                    }
                    const bool pinsInnerMethod =
                        optionAt(kEapOptions, index).method == network_enterprise::EapMethod::Peap;
                    if (pinsInnerMethod) {
                      m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
                    }
                    m_phase2Select->setEnabled(!pinsInnerMethod);
                  },
          }),
          ui::select({
              .out = &m_phase2Select,
              .options = optionLabels(kPhase2Options),
              .selectedIndex = kPeapPhase2Index,
              .placeholder = i18n::tr("control-center.network.phase2-auth"),
              .enabled = false,
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
          })
      )
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_identityInput,
          .placeholder = i18n::tr("control-center.network.identity"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_anonymousIdentityInput,
          .placeholder = i18n::tr("control-center.network.anonymous-identity"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_domainMatchInput,
          .placeholder = i18n::tr("control-center.network.domain-suffix-match"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_caCertInput,
          .placeholder = i18n::tr("control-center.network.ca-certificate"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  passwordCard->addChild(std::move(enterpriseFields));

  auto inputRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({
          .out = &m_passwordInput,
          .placeholder = i18n::tr("control-center.network.password"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
      }),
      ui::button({
          .out = &m_passwordRevealButton,
          .glyph = "eye",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_passwordInput == nullptr) {
                  return;
                }
                m_passwordRevealed = !m_passwordRevealed;
                m_passwordInput->setPasswordMode(!m_passwordRevealed);
                if (m_passwordRevealButton != nullptr) {
                  m_passwordRevealButton->setGlyph(m_passwordRevealed ? "eye-off" : "eye");
                }
              },
      }),
      ui::button({
          .text = i18n::tr("control-center.network.connect"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() { submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{}); },
      }),
      ui::button({
          .text = i18n::tr("common.actions.cancel"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { cancelPasswordPrompt(); },
      })
  );

  passwordCard->addChild(std::move(inputRow));

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .contentScale = contentScale(),
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  // The password card scrolls with the list so a tall 802.1X form stays reachable.
  // m_list is a separate column because rebuildApList() clears all of its children.
  Flex* scrollContent = listScroll->content();
  scrollContent->setDirection(FlexDirection::Vertical);
  scrollContent->setAlign(FlexAlign::Stretch);
  scrollContent->setGap(Style::spaceMd * scale);
  scrollContent->addChild(std::move(passwordCard));
  scrollContent->addChild(
      ui::column({
          .out = &m_list,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      })
  );

  tab->addChild(std::move(listScroll));
  return tab;
}

std::unique_ptr<Flex> NetworkTab::createHeaderActions() { return nullptr; }

void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (!m_active) {
    return;
  }
  if (m_network != nullptr) {
    m_network->requestScan();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCellularCard();
  syncLiveState(renderer);
  m_rootLayout->layout(renderer);
}

void NetworkTab::doUpdate(Renderer& renderer) {
  syncPasswordCard();
  rebuildApList(renderer);
  // A signal percent's text changes its width, so the list has to be laid out again.
  bool listChanged = syncApRows();
  listChanged = syncCellularCard() || listChanged;
  if (listChanged && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncLiveState(renderer);
}

void NetworkTab::onClose() {
  m_rootLayout = nullptr;
  m_passwordCard = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_passwordRevealed = false;
  m_enterpriseFields = nullptr;
  m_eapSelect = nullptr;
  m_phase2Select = nullptr;
  m_identityInput = nullptr;
  m_anonymousIdentityInput = nullptr;
  m_caCertInput = nullptr;
  m_domainMatchInput = nullptr;
  m_credentialError = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_cellularTogglePending = false;
  m_cellularTogglePendingTimer.stop();
  m_apRows.clear();
  m_wiredRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_wifiTogglePending = false;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  ++m_wifiToggleRequestGeneration;
}

void NetworkTab::syncPasswordCard() {
  if (m_hasPendingSecret
      && !m_pendingAccessPoint.has_value()
      && m_secrets != nullptr
      && !m_secrets->hasPendingRequest()) {
    clearPasswordPrompt();
  }
  if (m_passwordCard == nullptr) {
    return;
  }
  m_passwordCard->setVisible(m_hasPendingSecret);
  if (m_enterpriseFields != nullptr) {
    m_enterpriseFields->setVisible(m_hasPendingSecret && m_pendingEnterprise);
  }
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    if (m_pendingSsid.empty()) {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt"));
    } else if (m_pendingEnterprise) {
      m_passwordTitle->setText(i18n::tr("control-center.network.enterprise-prompt-for", "ssid", m_pendingSsid));
    } else {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid));
    }
  }
}

void NetworkTab::setCredentialError(const std::string& message) {
  if (m_credentialError == nullptr) {
    return;
  }
  m_credentialError->setText(message);
  m_credentialError->setVisible(!message.empty());
  // Showing or hiding the message changes the card's height, and the card is
  // above the network list.
  PanelManager::instance().requestLayout();
}

network_enterprise::EnterpriseCredentials NetworkTab::collectEnterpriseCredentials(const std::string& password) const {
  const auto selectedIndexOf = [](const Select* select) -> std::size_t {
    return select != nullptr ? select->selectedIndex() : kDefaultOptionIndex;
  };
  const auto valueOf = [](const Input* input) -> std::string {
    return input != nullptr ? input->value() : std::string{};
  };

  network_enterprise::EnterpriseCredentials credentials;
  credentials.eap = optionAt(kEapOptions, selectedIndexOf(m_eapSelect)).method;
  credentials.phase2 = optionAt(kPhase2Options, selectedIndexOf(m_phase2Select)).auth;
  credentials.identity = valueOf(m_identityInput);
  credentials.anonymousIdentity = valueOf(m_anonymousIdentityInput);
  credentials.password = password;
  credentials.caCertPath = valueOf(m_caCertInput);
  credentials.domainSuffixMatch = valueOf(m_domainMatchInput);
  return credentials;
}

std::string NetworkTab::enterpriseBlockReason(const AccessPointInfo& ap) const {
  if (m_network == nullptr || !m_network->supportsEnterprise()) {
    return i18n::tr("control-center.network.error-enterprise-unsupported");
  }
  if (!network_enterprise::passwordAuthUsable(ap.keyManagement)) {
    return i18n::tr("control-center.network.error-certificate-required");
  }
  return {};
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  // A form the user is filling outranks NM's own secret request: letting it
  // through would replace the card mid-typing and discard what has been entered.
  // Decline instead, so NM stops waiting rather than blocking on a prompt that
  // will never be answered.
  if (m_hasPendingSecret && m_pendingAccessPoint.has_value()) {
    if (m_secrets != nullptr) {
      m_secrets->cancelSecret();
    }
    return;
  }

  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  // NM is asking for one secret against a profile it already holds, so only the
  // password is missing; the rest of the 802.1X form would have nothing to fill.
  m_pendingEnterprise = false;
  PanelManager::instance().requestLayout();
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_pendingEnterprise = ap.isEnterprise();
  if (m_pendingEnterprise) {
    // Say up front when this network cannot be joined with a password, rather
    // than after the user has filled in the whole form.
    setCredentialError(enterpriseBlockReason(ap));
  }
  if (m_listScroll != nullptr) {
    m_listScroll->setScrollOffset(0.0F);
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value() && m_pendingEnterprise) {
    if (m_network == nullptr) {
      return;
    }
    // Every rejection below leaves the card open with a reason showing, so the
    // user can fix the field rather than watch the prompt close and nothing happen.
    if (const auto blocked = enterpriseBlockReason(*m_pendingAccessPoint); !blocked.empty()) {
      setCredentialError(blocked);
      PanelManager::instance().refresh();
      return;
    }
    const auto credentials = collectEnterpriseCredentials(value);
    const auto problem = network_enterprise::validate(credentials);
    if (problem != network_enterprise::Validation::Ok) {
      setCredentialError(validationMessage(problem));
      PanelManager::instance().refresh();
      return;
    }
    m_network->activateEnterpriseAccessPoint(*m_pendingAccessPoint, credentials);
  } else if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingEnterprise = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  for (Input* input : {m_identityInput, m_anonymousIdentityInput, m_caCertInput, m_domainMatchInput}) {
    if (input != nullptr) {
      input->setValue("");
    }
  }
  if (m_eapSelect != nullptr) {
    m_eapSelect->setSelectedIndexSilently(kDefaultOptionIndex);
  }
  if (m_phase2Select != nullptr) {
    m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
    m_phase2Select->setEnabled(false); // PEAP is the default, and it pins MSCHAPv2.
  }
  setCredentialError({});
}

bool NetworkTab::networkAvailable() const noexcept { return m_network != nullptr && m_network->available(); }

// Physical links that are up right now. The promote affordance and its badge only
// make sense once there is more than one, since a lone link is always primary.
int NetworkTab::activeLinkCount() const {
  if (m_network == nullptr) {
    return 0;
  }
  int count = 0;
  for (const auto& ap : m_network->accessPoints()) {
    if (ap.active) {
      ++count;
      break;
    }
  }
  for (const auto& wired : m_network->wiredConnections()) {
    if (wired.active) {
      ++count;
    }
  }
  return count;
}

bool NetworkTab::isPrimaryDevice(const std::string& devicePath) const {
  if (m_network == nullptr || devicePath.empty()) {
    return false;
  }
  const std::string& primary = m_network->state().primaryDevicePath;
  return !primary.empty() && primary == devicePath;
}

void NetworkTab::syncLiveState(Renderer& renderer) {
  if (!networkAvailable()) {
    return;
  }
  const NetworkState& s = m_network->state();
  if (m_wifiTogglePending) {
    if (s.wirelessEnabled == m_wifiToggleTarget) {
      m_wifiToggleTargetObserved = true;
    }
    if (m_wifiToggleWriteComplete && m_wifiToggleTargetObserved) {
      m_wifiTogglePending = false;
      m_wifiToggleWriteComplete = false;
      m_wifiToggleTargetObserved = false;
    }
  }
  static const std::string kNoExternalIp;
  const std::string& externalIp = m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
  // The addresses arrive after the rows are built (DHCP, then the WAN lookup), so
  // showing them adds a line and the list has to be laid out again.
  // Addresses are pushed per row rather than into one remembered "active" row:
  // two NICs (or two radios) can be up at once, and each carries its own. A row
  // that is not connected gets an empty detail, which hides the line.
  bool detailChanged = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it == m_apRows.end()) {
      continue;
    }
    // The address comes from the link's own device, so it stays right when some
    // other link is the primary one. The WAN address belongs to whichever link
    // holds the default route, so it is asked per device too.
    // The band comes from the state's associated BSS, so it only describes the
    // link that is actually up.
    const std::uint32_t frequencyMhz = s.kind == NetworkConnectivity::Wireless && s.connected ? s.frequencyMhz : 0;
    const std::string detail =
        ap.active ? connectedDetail(ap.ipv4, frequencyMhz, isPrimaryDevice(ap.devicePath), externalIp) : std::string{};
    detailChanged = it->second->setDetail(detail) || detailChanged;
  }
  for (const auto& wired : m_network->wiredConnections()) {
    const auto it = m_wiredRows.find(wired.path);
    if (it == m_wiredRows.end()) {
      continue;
    }
    const std::string detail =
        wired.active ? connectedDetail(wired.ipv4, 0, isPrimaryDevice(wired.devicePath), externalIp) : std::string{};
    detailChanged = it->second->setDetail(detail) || detailChanged;
  }
  if (detailChanged && m_list != nullptr) {
    m_list->layout(renderer);
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(m_wifiTogglePending ? m_wifiToggleTarget : s.wirelessEnabled);
    m_wifiToggle->setEnabled(!m_wifiTogglePending);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
}

void NetworkTab::requestWirelessEnabled(bool enabled) {
  m_wifiTogglePending = true;
  m_wifiToggleTarget = enabled;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  const std::uint64_t generation = ++m_wifiToggleRequestGeneration;
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setEnabled(false);
  }
  if (m_network == nullptr) {
    handleWirelessEnabledCompletion(generation, false);
    return;
  }
  m_network->setWirelessEnabled(enabled, [this, generation](bool success) {
    handleWirelessEnabledCompletion(generation, success);
  });
}

void NetworkTab::handleWirelessEnabledCompletion(std::uint64_t generation, bool success) {
  if (!m_wifiTogglePending || generation != m_wifiToggleRequestGeneration) {
    return;
  }
  m_wifiToggleWriteComplete = success;
  if (!success) {
    m_wifiTogglePending = false;
    m_wifiToggleTargetObserved = false;
  }
  PanelManager::instance().requestUpdateOnly();
  PanelManager::instance().requestRedraw();
}

// Identity of the built list: which rows exist, in which order, which controls
// each carries, and how each activates. The signal strength is absent by design —
// it refreshes in place through syncApRows(), so a scan update no longer tears the
// list down. Access points arrive sorted, so a change in row order changes the key.
std::string
NetworkTab::structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<WiredConnectionInfo>& wired) const {
  std::string key;
  // Which link is primary and how many are up decide the badge and whether a row
  // is clickable at all, so both belong to the structure.
  key += "primary:";
  key += m_network != nullptr ? m_network->state().primaryDevicePath : std::string{};
  key += "\nactive:";
  key += std::to_string(activeLinkCount());
  key.push_back('\n');
  for (const auto& ap : aps) {
    key += ap.ssid;
    key.push_back(':');
    key += ap.secured ? '1' : '0';
    key.push_back(':');
    key += static_cast<char>('0' + static_cast<int>(ap.keyManagement));
    key.push_back(':');
    key += ap.active ? '1' : '0';
    key.push_back(':');
    key += (m_network != nullptr && m_network->hasSavedConnection(ap.ssid)) ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& conn : wired) {
    key += conn.path;
    key.push_back(':');
    key += conn.name;
    key.push_back(':');
    key += conn.active ? '1' : '0';
    key.push_back('\n');
  }
  const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
  const bool scanning = m_network != nullptr && m_network->state().scanning;
  key += "avail:";
  key += networkAvailable() ? '1' : '0';
  key += "\nwifi:";
  key += wirelessEnabled ? '1' : '0';
  key += "\nscan:";
  key += scanning ? '1' : '0';
  // Cellular rows live-sync their state/signal, so only the structural
  // identity (which modems exist) belongs to the key.
  key += "\ncellular:\n";
  if (m_modem != nullptr) {
    for (const auto& modem : m_modem->modems()) {
      key += modem.path;
      key.push_back(':');
      key += modem.name;
      key.push_back('\n');
    }
  }
  return key;
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }

  std::vector<AccessPointInfo> aps;
  if (m_network != nullptr) {
    aps = sortedAccessPoints(m_network->accessPoints());
  }
  static const std::vector<WiredConnectionInfo> kNoWired;
  const auto& wired = m_network != nullptr ? m_network->wiredConnections() : kNoWired;
  const std::string nextStructure = structureKey(aps, wired);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;
  const float scale = contentScale();

  auto buildApRows = [&]() {
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    if (aps.empty()) {
      container->addChild(
          ui::label({
              .text = i18n::tr("control-center.network.no-networks"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      std::vector<AccessPointInfo> activeAps;
      std::vector<AccessPointInfo> savedAps;
      std::vector<AccessPointInfo> availableAps;
      activeAps.reserve(aps.size());
      savedAps.reserve(aps.size());
      availableAps.reserve(aps.size());

      for (const auto& ap : aps) {
        const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
        if (ap.active) {
          activeAps.push_back(ap);
        } else if (saved) {
          savedAps.push_back(ap);
        } else {
          availableAps.push_back(ap);
        }
      }

      auto addRows = [&](Flex& parent, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        for (const auto& ap : bucket) {
          const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
          // Every access point on a radio shares its device path, so the active
          // one has to be singled out or the whole list would go inert.
          const bool primary = ap.active && isPrimaryDevice(ap.devicePath);
          auto row = std::make_unique<AccessPointRow>(
              scale, ap, saved, primary, primary && activeLinkCount() > 1,
              [this](const AccessPointInfo& clicked) {
                if (clicked.active || m_network == nullptr) {
                  return;
                }
                if (clicked.requiresCredentials() && !m_network->hasSavedConnection(clicked.ssid)) {
                  showPasswordPrompt(clicked);
                  PanelManager::instance().refresh();
                  return;
                }
                m_network->activateAccessPoint(clicked);
              },
              [this](const AccessPointInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->forgetSsid(clicked.ssid);
                }
                PanelManager::instance().refresh();
              },
              [this, ap]() {
                if (m_network != nullptr) {
                  m_network->disconnectAccessPoint(ap);
                }
                PanelManager::instance().refresh();
              },
              [this, ap]() {
                if (m_network != nullptr) {
                  m_network->makePrimary(ap);
                }
                PanelManager::instance().refresh();
              }
          );
          auto* rowPtr = row.get();
          parent.addChild(std::move(row));
          m_apRows.emplace(rowPtr->ssid(), rowPtr);
        }
      };

      addRows(*container, activeAps);

      auto addBucket = [&](const std::string& title, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        auto group = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * scale,
        });
        group->addChild(makeBucketHeaderRow(title, scale));
        addRows(*group, bucket);
        container->addChild(std::move(group));
      };

      addBucket(i18n::tr("control-center.network.saved"), savedAps);
      addBucket(i18n::tr("control-center.network.available"), availableAps);
    }
    return container;
  };

  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_apRows.clear();
  m_wiredRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  const float opacity = panelCardOpacity();
  if (!networkAvailable()) {
    auto unavailableCard = ui::column({
        .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
    });
    addTitle(*unavailableCard, i18n::tr("control-center.network.unavailable-title"), scale);
    addBody(*unavailableCard, i18n::tr("control-center.network.unavailable-detail"), scale);
    m_list->addChild(std::move(unavailableCard));
  } else {
    // Wired profiles exist only on backends that report them, so the card is
    // absent entirely on a machine with no ethernet connection saved.
    if (!wired.empty()) {
      auto wiredCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });
      wiredCard->addChild(makeCardHeaderRow(i18n::tr("control-center.network.wired"), scale));

      for (const auto& conn : wired) {
        const bool primary = conn.active && isPrimaryDevice(conn.devicePath);
        auto row = std::make_unique<ConnectionRow>(
            renderer, scale,
            ConnectionRowSpec{
                .name = conn.name,
                .glyph = conn.virtualLink ? "hierarchy-2" : "ethernet",
                .active = conn.active,
                .onActivate =
                    [this, conn]() {
                      if (m_network != nullptr) {
                        m_network->activateWiredConnection(conn);
                      }
                      PanelManager::instance().refresh();
                    },
                .onDeactivate =
                    [this, conn]() {
                      if (m_network != nullptr) {
                        m_network->deactivateWiredConnection(conn);
                      }
                      PanelManager::instance().refresh();
                    },
                .showPrimaryBadge = primary && activeLinkCount() > 1,
                .primary = primary,
                .onMakePrimary =
                    [this, conn]() {
                      if (m_network != nullptr) {
                        m_network->makePrimary(conn);
                      }
                      PanelManager::instance().refresh();
                    },
            }
        );
        auto* rowPtr = row.get();
        wiredCard->addChild(std::move(row));
        m_wiredRows.emplace(conn.path, rowPtr);
      }

      m_list->addChild(std::move(wiredCard));
    }

    if (m_modem != nullptr && !m_modem->modems().empty()) {
      auto cellularCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });

      auto cellularHeader = makeCardHeaderRow(i18n::tr("control-center.network.cellular"), scale);
      cellularHeader->addChild(
          ui::toggle({
              .out = &m_cellularToggle,
              .checkedImmediate = cellularToggleDisplayChecked(),
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestCellularEnabled(checked); },
          })
      );
      cellularCard->addChild(std::move(cellularHeader));

      for (const auto& modem : m_modem->modems()) {
        auto row = std::make_unique<CellularRow>(scale, modem);
        m_cellularRows.push_back(row.get());
        cellularCard->addChild(std::move(row));
      }

      m_list->addChild(std::move(cellularCard));
    }

    {
      auto wifiCard = ui::column({
          .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
      });

      auto wifiHeader = makeCardHeaderRow(i18n::tr("control-center.network.wifi"), scale);
      wifiHeader->addChild(
          ui::spinner({
              .out = &m_scanSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
              .visible = false,
          })
      );

      wifiHeader->addChild(
          ui::button({
              .out = &m_rescanButton,
              .glyph = "refresh",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_network != nullptr) {
                  m_network->requestScan();
                }
              },
          })
      );

      wifiHeader->addChild(
          ui::toggle({
              .out = &m_wifiToggle,
              .checkedImmediate = m_network->state().wirelessEnabled,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestWirelessEnabled(checked); },
          })
      );
      wifiCard->addChild(std::move(wifiHeader));

      wifiCard->addChild(buildApRows());

      m_list->addChild(std::move(wifiCard));

      // Live state (spinner visibility/animation, toggle checked) is owned by
      // syncLiveState(), which runs every frame after the card is attached.
      // rebuildApList() builds structure only and must not drive animations.
    }
  }
  m_list->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it != m_apRows.end() && it->second->syncLiveMetrics(ap)) {
      changed = true;
    }
  }
  return changed;
}

bool NetworkTab::syncCellularCard() {
  if (m_modem == nullptr) {
    return false;
  }
  if (m_cellularTogglePending) {
    // Bringing mobile data up walks Enabling -> Searching -> Registered ->
    // Connecting, so the requested position is held instead of snapping back
    // while the modem works. Give up once it lands or the request times out.
    const bool reached = cellularToggleChecked() == m_cellularToggleTarget;
    const bool timedOut = std::chrono::steady_clock::now() - m_cellularTogglePendingSince > kCellularPendingTimeout;
    if (reached || timedOut) {
      m_cellularTogglePending = false;
      m_cellularTogglePendingTimer.stop();
    }
  }
  const auto& modems = m_modem->modems();
  bool changed = false;
  const std::size_t count = std::min(m_cellularRows.size(), modems.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (m_cellularRows[i] != nullptr && m_cellularRows[i]->syncLive(modems[i])) {
      changed = true;
    }
  }
  if (m_cellularToggle != nullptr) {
    m_cellularToggle->setChecked(cellularToggleDisplayChecked());
  }
  return changed;
}

bool NetworkTab::cellularToggleChecked() const {
  if (m_network != nullptr && m_network->canActivateCellularConnection()) {
    return m_network->state().cellularActive;
  }
  if (m_modem != nullptr) {
    return std::ranges::any_of(m_modem->modems(), [](const CellularModemInfo& modem) { return modem.enabled(); });
  }
  return false;
}

// The requested position while a toggle request is in flight, the observed one
// otherwise.
bool NetworkTab::cellularToggleDisplayChecked() const {
  return m_cellularTogglePending ? m_cellularToggleTarget : cellularToggleChecked();
}

void NetworkTab::requestCellularEnabled(bool enabled) {
  const bool nmCellular = m_network != nullptr && m_network->canActivateCellularConnection();
  if (enabled) {
    // GNOME parity: powering the modem is a prerequisite; the gsm connection is
    // what actually brings up mobile data.
    if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(true);
    }
    if (nmCellular) {
      m_network->activateCellularConnection();
    }
  } else {
    // Data off while the modem stays registered, like GNOME's mobile-data
    // switch. Without an NM gsm profile there is only modem power to cut.
    if (nmCellular) {
      m_network->deactivateCellularConnection();
    } else if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(false);
    }
  }
  m_cellularTogglePending = true;
  m_cellularToggleTarget = enabled;
  m_cellularTogglePendingSince = std::chrono::steady_clock::now();
  // Nothing else wakes the panel if the request never changes any state.
  m_cellularTogglePendingTimer.start(kCellularPendingTimeout + std::chrono::milliseconds(50), []() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
}
