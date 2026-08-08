#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/vpn_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<VpnWidget::Options>& vpnWidgetDefinition();
