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

ConnectionRow::ConnectionRow(Renderer& renderer, float scale, ConnectionRowSpec spec)
    : m_active(spec.active), m_enabled(spec.enabled), m_onActivate(std::move(spec.onActivate)),
      m_onDeactivate(std::move(spec.onDeactivate)) {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Style::spaceSm * scale);
  setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  setMinHeight(kRowMinHeight * scale);
  setRadius(Style::scaledRadiusMd(scale));
  setFill(colorSpecFromRole(ColorRole::Surface));
  clearBorder();

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

  addChild(
      ui::label({
          .out = &m_title,
          .text = spec.name,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = m_active ? FontWeight::Bold : FontWeight::Normal,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      })
  );

  addChild(
      ui::button({
          .out = &m_checkButton,
          .glyph = "check",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .opacity = m_active ? 1.0f : 0.0f,
      })
  );

  addChild(
      ui::button({
          .out = &m_actionButton,
          .glyph = m_active ? "plug-off" : "plug",
          .glyphSize = Style::baseGlyphSize * scale,
          .enabled = m_enabled,
          .variant = m_active ? ButtonVariant::Destructive : ButtonVariant::Default,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() { triggerAction(); },
      })
  );

  auto area = ui::inputArea({});
  area->setPropagateEvents(true);
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { triggerAction(); });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  applyState();
  m_paletteConn = paletteChanged().connect([this] { applyState(); });
}

void ConnectionRow::doLayout(Renderer& renderer) {
  if (m_inputArea == nullptr) {
    return;
  }
  m_inputArea->setVisible(false);
  Flex::doLayout(renderer);
  m_inputArea->setVisible(true);
  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setSize(width(), height());
  if (m_actionButton != nullptr) {
    const float areaWidth = std::max(0.0f, m_actionButton->x() - gap());
    m_inputArea->setSize(areaWidth, height());
  }
  applyState();
}

LayoutSize ConnectionRow::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void ConnectionRow::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void ConnectionRow::triggerAction() {
  if (!m_enabled) {
    return;
  }
  if (m_active) {
    if (m_onDeactivate) {
      m_onDeactivate();
    }
  } else {
    if (m_onActivate) {
      m_onActivate();
    }
  }
}

void ConnectionRow::applyState() {
  const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
  const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
  if (pressed) {
    setFill(colorSpecFromRole(ColorRole::Primary));
    setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    if (m_title != nullptr) {
      m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
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
}
