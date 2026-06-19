#include "plugin.hpp"
#include "globals.hpp"
#include <deque>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

using Desktop::View::SFullscreenState;

/*! Recursively collect all dwindle leaf nodes for given root node */
static void collectDwindleChildNodes(std::deque<SP<Layout::Tiled::SDwindleNodeData>>* pDeque, SP<Layout::Tiled::SDwindleNodeData> pNode)
{
    if (!pNode)
        return;

    if (pNode->isNode) {
        collectDwindleChildNodes(pDeque, pNode->children[0].lock());
        collectDwindleChildNodes(pDeque, pNode->children[1].lock());
    }
    else {
        pDeque->emplace_back(pNode);
    }
}

/*! Get the CDwindleAlgorithm for the given window's workspace, or nullptr if not dwindle */
static Layout::Tiled::CDwindleAlgorithm* getDwindleAlgo(PHLWINDOW pWindow)
{
    if (!pWindow || !pWindow->m_workspace)
        return nullptr;
    auto pSpace = pWindow->m_workspace->m_space;
    if (!pSpace)
        return nullptr;
    auto pAlgo = pSpace->algorithm();
    if (!pAlgo)
        return nullptr;
    return dynamic_cast<Layout::Tiled::CDwindleAlgorithm*>(pAlgo->tiledAlgo().get());
}

/*! Collect the dwindle siblings of pSelfWindow that should join the group.
 *  Must be called BEFORE toggleGroup so the window is still a tiled leaf.
 *  Returns empty vector if not applicable (floating, not dwindle, already grouped sibling). */
static std::vector<PHLWINDOW> collectDwindleSiblings(PHLWINDOW pSelfWindow)
{
    if (pSelfWindow->m_isFloating) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for floating window");
        return {};
    }

    auto* pDwindleAlgo = getDwindleAlgo(pSelfWindow);
    if (!pDwindleAlgo) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring non-dwindle layout");
        return {};
    }

    const auto P_DWINDLE_NODE = g_pNodeFromWindow(pDwindleAlgo, pSelfWindow);
    if (!P_DWINDLE_NODE) {
        Log::logger->log(Log::ERR, "[dwindle-autogroup] Failed to get dwindle node for window");
        return {};
    }

    const auto P_PARENT_NODE = P_DWINDLE_NODE->pParent.lock();
    if (!P_PARENT_NODE) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for a single window");
        return {};
    }

    const auto P_SIBLING_NODE = P_PARENT_NODE->children[0].lock() == P_DWINDLE_NODE
        ? P_PARENT_NODE->children[1].lock()
        : P_PARENT_NODE->children[0].lock();
    if (!P_SIBLING_NODE) {
        Log::logger->log(Log::ERR, "[dwindle-autogroup] Sibling node is null");
        return {};
    }

    std::deque<SP<Layout::Tiled::SDwindleNodeData>> dwindleNodes;
    collectDwindleChildNodes(&dwindleNodes, P_SIBLING_NODE);

    std::vector<PHLWINDOW> result;
    for (auto& node : dwindleNodes) {
        auto pTarget = node->pTarget.lock();
        if (!pTarget) {
            Log::logger->log(Log::INFO, "[dwindle-autogroup] Skipping null target");
            continue;
        }
        auto curWindow = pTarget->window();
        if (!curWindow) {
            Log::logger->log(Log::INFO, "[dwindle-autogroup] Skipping null window");
            continue;
        }
        if (curWindow->m_group) {
            Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for nested groups: window {:x} is in a group", curWindow);
            return {};
        }
        result.push_back(curWindow);
    }

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Collected {} siblings to autogroup", result.size());
    return result;
}

/*! Custom ungroup logic that places windows back in the dwindle tree extending from the group head. */
static void newAutoDestroyGroup(PHLWINDOW pSelfWindow, std::vector<PHLWINDOW> vGroupWindows)
{
    if (vGroupWindows.empty() || !pSelfWindow->m_group) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] No group/windows, aborting ungroup");
        return;
    }

    auto pGroup = pSelfWindow->m_group;

    auto* pDwindleAlgo = getDwindleAlgo(pSelfWindow);
    if (!pDwindleAlgo) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Non-dwindle layout, falling back to original destroy");
        pGroup->destroy();
        return;
    }

    if (pSelfWindow->isFullscreen())
        g_pCompositor->setWindowFullscreenState(pSelfWindow, SFullscreenState{.internal = FSMODE_NONE, .client = FSMODE_NONE});

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping {} windows", vGroupWindows.size());

    const bool GROUPS_LOCKED_PREV = g_pKeybindManager->m_groupsLocked;
    g_pKeybindManager->m_groupsLocked = true;

    PHLWINDOW pVisible = pGroup->current();
    if (!pVisible)
        pVisible = pSelfWindow;

    Desktop::focusState()->fullWindowFocus(pVisible, Desktop::FOCUS_REASON_OTHER);

    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow || pWindow == pVisible)
            continue;

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping window {:x}", pWindow);
        pGroup->remove(pWindow);
        Desktop::focusState()->fullWindowFocus(pWindow, Desktop::FOCUS_REASON_OTHER);
    }

    if (pGroup->has(pVisible))
        pGroup->remove(pVisible);

    g_pKeybindManager->m_groupsLocked = GROUPS_LOCKED_PREV;

    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow)
            continue;
        pWindow->updateWindowDecos();
    }

    g_pCompositor->updateAllWindowsAnimatedDecorationValues();

    if (pSelfWindow)
        Desktop::focusState()->fullWindowFocus(pSelfWindow, Desktop::FOCUS_REASON_OTHER);

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping done");
}

Config::Actions::ActionResult newToggleGroup(std::optional<PHLWINDOW> window)
{
    const auto PWINDOW = window.value_or(Desktop::focusState()->window());

    if (!PWINDOW)
        return ((toggleGroupFuncT)g_pToggleGroupHook->m_original)(window);

    const bool isCreating = !PWINDOW->m_group;

    if (isCreating) {
        // Collect siblings BEFORE calling original — the window is still a tiled
        // leaf in the dwindle tree here. After toggleGroup runs, it becomes part
        // of a CWindowGroupTarget and getNodeFromWindow returns null.
        auto siblings = collectDwindleSiblings(PWINDOW);

        auto result = ((toggleGroupFuncT)g_pToggleGroupHook->m_original)(window);
        if (!result.has_value())
            return result;

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Triggered createGroup for {:x}", PWINDOW);

        if (!siblings.empty() && PWINDOW->m_group) {
            for (auto& w : siblings) {
                Log::logger->log(Log::INFO, "[dwindle-autogroup] Moving window {:x} into group", w);
                PWINDOW->m_group->add(w);
            }
            Log::logger->log(Log::INFO, "[dwindle-autogroup] Autogroup done, {} windows moved", siblings.size());
        }

        return result;
    }
    else {
        std::vector<PHLWINDOW> vGroupMembers;
        for (auto& wr : PWINDOW->m_group->windows()) {
            if (auto w = wr.lock())
                vGroupMembers.push_back(w);
        }

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Triggered destroyGroup for {:x}", PWINDOW);
        newAutoDestroyGroup(PWINDOW, vGroupMembers);
        return Config::Actions::SActionResult{};
    }
}
