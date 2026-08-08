#include "shell/bar/widgets/vpn_widget_definition.h"

const noctalia::bar::WidgetDefinition<VpnWidget::Options>& vpnWidgetDefinition() {
  using noctalia::bar::field;
  using Options = VpnWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "vpn",
      .fields = {
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          field<&Options::hideWhenDisconnected>({
              .key = "hide_when_disconnected",
          }),
      },
  };
  return definition;
}
