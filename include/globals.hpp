#pragma once

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprlang.hpp>

using Desktop::View::CWindow;

const CHyprColor              s_notifyColor      = {0x61 / 255.0f, 0xAF / 255.0f, 0xEF / 255.0f, 1.0f}; // RGBA
const PLUGIN_DESCRIPTION_INFO s_pluginDescription = {"dwindle-autogroup", "Dwindle Autogroup", "ItsDrike", "1.0"};

inline HANDLE PHANDLE = nullptr;

typedef Config::Actions::ActionResult (*toggleGroupFuncT)(std::optional<PHLWINDOW>);
inline CFunctionHook* g_pToggleGroupHook = nullptr;

typedef SP<Layout::Tiled::SDwindleNodeData> (*nodeFromWindowFuncT)(void*, PHLWINDOW);
inline nodeFromWindowFuncT g_pNodeFromWindow = nullptr;
