#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <functional>
#include <string>

class Button;
class InputArea;
class Label;
class Renderer;

// Everything the network rows share: the card chassis, the name and detail
// column, the primary badge, and the hover/press handling. Subclasses supply
// their own leading and trailing content between addTitleColumn() and
// finishRow(), and answer a click through onRowClicked().
class NetworkRowBase : public Flex {
public:
  // Addresses of the connected link, shown under the name. Returns true when the
  // row needs to be laid out again.
  bool setDetail(const std::string& text);

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

protected:
  // An inert row takes no hover, press, or click handlers at all.
  NetworkRowBase(float scale, bool inert);

  // The connected row carries its addresses underneath the name; every other row
  // is a single line and the detail label stays out of the layout.
  void addTitleColumn(float scale, const std::string& name, bool active);
  void addPrimaryBadge(float scale);

  // Call once every child is in place. An inert row (it already holds the default
  // route, or it is mid-transition) gets no handlers at all, and InputArea only
  // claims the pointer cursor once an onClick is, so withholding them is what
  // keeps the cursor normal.
  void finishRow(Button* actionButton);

  // Only reached on a row that is not inert; see finishRow().
  virtual void onRowClicked() = 0;

private:
  void applyState();

  bool m_inert = false;
  Label* m_title = nullptr;
  Label* m_detail = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};

// Shared by NetworkManager VPN profiles, wired profiles, and tailscale exit
// nodes: a name, and a disconnect button while active. Clicking the row
// connects it, or disconnects it when it is already active.
struct ConnectionRowSpec {
  std::string name;
  std::string iconAsset; // svg under assets/, empty for no leading icon
  std::string glyph;     // font glyph, used when no iconAsset is given
  bool active = false;
  bool connecting = false; // shows a spinner instead of the disconnect button
  bool enabled = true;
  std::function<void()> onActivate;
  std::function<void()> onDeactivate;
  bool showPrimaryBadge = false;
  bool primary = false;
  std::function<void()> onMakePrimary;
};

class ConnectionRow : public NetworkRowBase {
public:
  ConnectionRow(Renderer& renderer, float scale, ConnectionRowSpec spec);

private:
  void onRowClicked() override;

  bool m_active = false;
  bool m_enabled = true;
  std::function<void()> m_onActivate;
  std::function<void()> m_onDeactivate;
  std::function<void()> m_onMakePrimary;
};
