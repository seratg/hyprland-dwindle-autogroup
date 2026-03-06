#pragma once

#include <hyprland/src/config/ConfigManager.hpp>
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

// SDwindleNodeData is only forward-declared in installed headers;
// we define it here based on the Hyprland 0.54 source.
namespace Layout::Tiled {
    struct SDwindleNodeData {
        WP<SDwindleNodeData>                pParent;
        bool                                isNode   = false;
        WP<Layout::ITarget>                 pTarget;
        std::array<WP<SDwindleNodeData>, 2> children = {};
        WP<SDwindleNodeData>                self;
        bool                                splitTop               = false;
        CBox                                box                    = {0};
        float                               splitRatio             = 1.f;
        bool                                valid                  = true;
        bool                                ignoreFullscreenChecks = false;

        bool operator==(const SDwindleNodeData& rhs) const {
            return pTarget.lock() == rhs.pTarget.lock() && box == rhs.box && pParent == rhs.pParent && children[0] == rhs.children[0] &&
                   children[1] == rhs.children[1];
        }
    };
}

const CHyprColor              s_notifyColor      = {0x61 / 255.0f, 0xAF / 255.0f, 0xEF / 255.0f, 1.0f}; // RGBA
const PLUGIN_DESCRIPTION_INFO s_pluginDescription = {"dwindle-autogroup", "Dwindle Autogroup", "ItsDrike", "1.0"};

inline HANDLE PHANDLE = nullptr;

typedef SDispatchResult (*toggleGroupFuncT)(std::string);
inline CFunctionHook* g_pToggleGroupHook = nullptr;

typedef SP<Layout::Tiled::SDwindleNodeData> (*nodeFromWindowFuncT)(void*, PHLWINDOW);
inline nodeFromWindowFuncT g_pNodeFromWindow = nullptr;
