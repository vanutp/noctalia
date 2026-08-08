#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <functional>
#include <string>

class Button;
class InputArea;
class Label;
class Renderer;

// Shared by NetworkManager VPN profiles and tailscale exit nodes: a name, a
// check mark while active, and one button that connects or disconnects.
struct ConnectionRowSpec {
  std::string name;
  std::string iconAsset; // svg under assets/, empty for no leading icon
  std::string glyph;     // font glyph, used when no iconAsset is given
  bool active = false;
  bool enabled = true;
  std::function<void()> onActivate;
  std::function<void()> onDeactivate;
};

class ConnectionRow : public Flex {
public:
  ConnectionRow(Renderer& renderer, float scale, ConnectionRowSpec spec);

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void triggerAction();
  void applyState();

  bool m_active = false;
  bool m_enabled = true;
  std::function<void()> m_onActivate;
  std::function<void()> m_onDeactivate;
  Label* m_title = nullptr;
  Button* m_checkButton = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};
