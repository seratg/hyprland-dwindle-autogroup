#include "plugin.hpp"
#include "globals.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

using Desktop::View::SFullscreenState;

/*! Recursively collect all dwindle child nodes for given root node */
void collectDwindleChildNodes(std::deque<SP<Layout::Tiled::SDwindleNodeData>>* pDeque, SP<Layout::Tiled::SDwindleNodeData> pNode)
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

/*! After original toggleGroup creates a group (with just pSelfWindow), add dwindle siblings. */
static void newAutoCreateGroup(PHLWINDOW pSelfWindow)
{
    if (!pSelfWindow->m_group) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup - group was not created");
        return;
    }

    if (pSelfWindow->m_isFloating) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for floating window");
        return;
    }

    auto* pDwindleAlgo = getDwindleAlgo(pSelfWindow);
    if (!pDwindleAlgo) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring non-dwindle layout");
        return;
    }

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Collecting dwindle child nodes");

    const auto P_DWINDLE_NODE = g_pNodeFromWindow(pDwindleAlgo, pSelfWindow);
    if (!P_DWINDLE_NODE) {
        Log::logger->log(Log::ERR, "[dwindle-autogroup] Failed to get dwindle node for window");
        return;
    }

    const auto P_PARENT_NODE = P_DWINDLE_NODE->pParent.lock();
    if (!P_PARENT_NODE) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for a single window");
        return;
    }

    const auto P_SIBLING_NODE = P_PARENT_NODE->children[0].lock() == P_DWINDLE_NODE ? P_PARENT_NODE->children[1].lock() : P_PARENT_NODE->children[0].lock();
    if (!P_SIBLING_NODE) {
        Log::logger->log(Log::ERR, "[dwindle-autogroup] Sibling node is null");
        return;
    }

    std::deque<SP<Layout::Tiled::SDwindleNodeData>> p_dDwindleNodes;
    collectDwindleChildNodes(&p_dDwindleNodes, P_SIBLING_NODE);

    std::vector<PHLWINDOW> vWindows;
    for (auto& node : p_dDwindleNodes) {
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
        // Abort if any sibling is already in a group
        if (curWindow->m_group) {
            Log::logger->log(Log::INFO, "[dwindle-autogroup] Ignoring autogroup for nested groups: window {:x} is in a group", curWindow);
            return;
        }
        vWindows.push_back(curWindow);
    }

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Found {} windows to autogroup", vWindows.size());

    // CGroup::add() handles removing windows from the layout and adding group decorations
    for (auto& curWindow : vWindows) {
        if (!curWindow)
            continue;
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Moving window {:x} into group", curWindow);
        pSelfWindow->m_group->add(curWindow);
    }

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Autogroup done, {} windows moved", vWindows.size());
}

/*! Custom ungroup logic that places windows back in the dwindle tree extending from the group head. */
static void newAutoDestroyGroup(PHLWINDOW pSelfWindow, std::vector<PHLWINDOW> vGroupWindows)
{
    if (vGroupWindows.empty() || !pSelfWindow->m_group) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] No group/windows, aborting ungroup");
        return;
    }

    // Keep the group alive throughout (individual window m_group fields get reset during removal)
    auto pGroup = pSelfWindow->m_group;

    // Determine current layout
    auto* pDwindleAlgo = getDwindleAlgo(pSelfWindow);
    if (!pDwindleAlgo) {
        Log::logger->log(Log::INFO, "[dwindle-autogroup] Non-dwindle layout, falling back to original destroy");
        pGroup->destroy();
        return;
    }

    // Exit fullscreen for the group head if needed
    if (pSelfWindow->isFullscreen())
        g_pCompositor->setWindowFullscreenState(pSelfWindow, SFullscreenState{.internal = FSMODE_NONE, .client = FSMODE_NONE});

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping {} windows", vGroupWindows.size());

    const bool GROUPS_LOCKED_PREV = g_pKeybindManager->m_groupsLocked;
    g_pKeybindManager->m_groupsLocked = true;

    // The visible (current) window should be removed last so switchTargets gives it the group's position
    PHLWINDOW pVisible = pGroup->current();
    if (!pVisible)
        pVisible = pSelfWindow;

    // Focus the visible window first — it's the focal point for placing the first hidden window
    Desktop::focusState()->fullWindowFocus(pVisible, Desktop::FOCUS_REASON_OTHER);

    // Remove all non-visible windows first.
    // CGroup::remove() calls assignToSpace() for non-last windows, which adds them to the
    // dwindle layout next to the focused window.
    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow || pWindow == pVisible)
            continue;

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping window {:x}", pWindow);
        pGroup->remove(pWindow);

        // Focus the just-placed window so the next one is placed as its dwindle child
        Desktop::focusState()->fullWindowFocus(pWindow, Desktop::FOCUS_REASON_OTHER);
    }

    // Remove the visible window last — it's the only remaining window, so CGroup::remove()
    // calls switchTargets() to put it at the group's original layout position.
    if (pGroup->has(pVisible))
        pGroup->remove(pVisible);

    g_pKeybindManager->m_groupsLocked = GROUPS_LOCKED_PREV;

    // Update decorations for all ungrouped windows
    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow)
            continue;
        pWindow->updateWindowDecos();
    }

    g_pCompositor->updateAllWindowsAnimatedDecorationValues();

    // Return focus to the original window
    if (pSelfWindow)
        Desktop::focusState()->fullWindowFocus(pSelfWindow, Desktop::FOCUS_REASON_OTHER);

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Ungrouping done");
}

SDispatchResult newToggleGroup(std::string args)
{
    const auto PWINDOW = Desktop::focusState()->window();

    if (!PWINDOW) {
        // No focused window, let original handle it
        return ((toggleGroupFuncT)g_pToggleGroupHook->m_original)(args);
    }

    bool isCreating = !PWINDOW->m_group;

    if (isCreating) {
        // Call original to create the group with just the focused window
        SDispatchResult result = ((toggleGroupFuncT)g_pToggleGroupHook->m_original)(args);
        if (!result.success)
            return result;

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Triggered createGroup for {:x}", PWINDOW);

        // After creation, add dwindle siblings into the new group
        newAutoCreateGroup(PWINDOW);
        return result;
    }
    else {
        // Capture group members before we destroy the group
        std::vector<PHLWINDOW> vGroupMembers;
        for (auto& wr : PWINDOW->m_group->windows()) {
            if (auto w = wr.lock())
                vGroupMembers.push_back(w);
        }

        Log::logger->log(Log::INFO, "[dwindle-autogroup] Triggered destroyGroup for {:x}", PWINDOW);

        // Handle destroy with custom dwindle-aware logic (does NOT call original)
        newAutoDestroyGroup(PWINDOW, vGroupMembers);
        return {};
    }
}
