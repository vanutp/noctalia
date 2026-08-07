#include "shell/control_center/tabs/network_row.h"

#include "core/files/resource_paths.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

} // namespace

NetworkRowBase::NetworkRowBase(float scale, bool primary) : m_primary(primary) {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Style::spaceSm * scale);
  setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  setMinHeight(kRowMinHeight * scale);
  setRadius(Style::scaledRadiusMd(scale));
  setFill(colorSpecFromRole(ColorRole::Surface));
  clearBorder();
}

bool NetworkRowBase::setDetail(const std::string& text) {
  if (m_detail == nullptr) {
    return false;
  }
  const bool show = !text.empty();
  bool changed = m_detail->setText(text);
  if (m_detail->visible() != show) {
    m_detail->setVisible(show);
    m_detail->setParticipatesInLayout(show);
    changed = true;
  }
  return changed;
}

void NetworkRowBase::doLayout(Renderer& renderer) {
  if (m_inputArea == nullptr) {
    return;
  }
  m_inputArea->setVisible(false);
  Flex::doLayout(renderer);
  m_inputArea->setVisible(true);
  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setSize(width(), height());
  // The click target stops short of the trailing button, but only while that
  // button takes part in the layout: a row whose button is collapsed is
  // clickable end to end, while one that merely reserves the slot is not.
  if (m_actionButton != nullptr && m_actionButton->participatesInLayout()) {
    m_inputArea->setSize(std::max(0.0f, m_actionButton->x() - gap()), height());
  }
  applyState();
}

LayoutSize NetworkRowBase::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void NetworkRowBase::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void NetworkRowBase::addTitleColumn(float scale, const std::string& name, bool active) {
  addChild(
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0f},
          ui::label({
              .out = &m_title,
              .text = name,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = active ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .out = &m_detail,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .participatesInLayout = false,
          })
      )
  );
}

void NetworkRowBase::addPrimaryBadge(float scale) {
  addChild(
      ui::glyph({
          .glyph = "route",
          .glyphSize = Style::baseGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
}

void NetworkRowBase::finishRow(Button* actionButton) {
  m_actionButton = actionButton;
  auto area = ui::inputArea({});
  area->setPropagateEvents(true);
  if (!m_primary) {
    area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnLeave([this]() { applyState(); });
    area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnClick([this](const InputArea::PointerData& /*data*/) { onRowClicked(); });
  }
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  applyState();
  m_paletteConn = paletteChanged().connect([this] { applyState(); });
}

void NetworkRowBase::applyState() {
  const bool hov = !m_primary && m_inputArea != nullptr && m_inputArea->hovered();
  const bool pressed = !m_primary && m_inputArea != nullptr && m_inputArea->pressed();
  if (pressed) {
    setFill(colorSpecFromRole(ColorRole::Primary));
    setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    if (m_title != nullptr) {
      m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    }
    if (m_detail != nullptr) {
      m_detail->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    }
    return;
  }
  setFill(colorSpecFromRole(ColorRole::Surface));
  if (hov) {
    setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
  } else {
    clearBorder();
  }
  if (m_title != nullptr) {
    m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
  }
  if (m_detail != nullptr) {
    m_detail->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }
}

ConnectionRow::ConnectionRow(Renderer& renderer, float scale, ConnectionRowSpec spec)
    : NetworkRowBase(scale, spec.primary), m_active(spec.active), m_enabled(spec.enabled),
      m_onActivate(std::move(spec.onActivate)), m_onDeactivate(std::move(spec.onDeactivate)),
      m_onMakePrimary(std::move(spec.onMakePrimary)) {
  if (!spec.iconAsset.empty()) {
    const float iconSize = Style::baseGlyphSize * scale;
    auto icon = ui::image({.width = iconSize, .height = iconSize});
    icon->setForegroundTint(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    icon->setSourceFile(
        renderer, paths::assetPath(spec.iconAsset).string(), static_cast<int>(std::round(iconSize)), true
    );
    addChild(std::move(icon));
  } else if (!spec.glyph.empty()) {
    addChild(
        ui::glyph({
            .glyph = spec.glyph,
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  addTitleColumn(scale, spec.name, m_active);

  if (spec.showPrimaryBadge) {
    addPrimaryBadge(scale);
  }

  // Only the active row carries a button, to disconnect, and the slot collapses
  // otherwise — there is no trailing column here that needs to stay aligned.
  // Connecting is the row click itself.
  Button* actionButton = nullptr;
  addChild(
      ui::button({
          .out = &actionButton,
          .glyph = "plug-off",
          .glyphSize = Style::baseGlyphSize * scale,
          .enabled = m_enabled,
          .variant = ButtonVariant::Destructive,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .visible = m_active,
          .participatesInLayout = m_active,
          .onClick =
              [this]() {
                if (m_enabled && m_onDeactivate) {
                  m_onDeactivate();
                }
              },
      })
  );

  finishRow(actionButton);
}

void ConnectionRow::onRowClicked() {
  if (!m_enabled) {
    return;
  }
  if (!m_active) {
    if (m_onActivate) {
      m_onActivate();
    }
    return;
  }
  // A link that can carry the default route is promoted rather than torn down.
  // VPNs and exit nodes pass no promote callback and still disconnect here.
  if (m_onMakePrimary) {
    m_onMakePrimary();
    return;
  }
  if (m_onDeactivate) {
    m_onDeactivate();
  }
}
