#include "plugin.hpp"
#include "globals.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>

/*! Recursively collect all dwindle child nodes for given root node
 *
 * This function is similar to Hyprland's private addToDequeRecursive function.
 *
 * \param[out] pDeque   deque to store the found child nodes into
 * \param[in] pNode     Dwindle node which children should be collected
 */
void collectDwindleChildNodes(std::deque<SDwindleNodeData*>* pDeque, SDwindleNodeData* pNode)
{
    if (pNode->isNode) {
        collectDwindleChildNodes(pDeque, pNode->children[0]);
        collectDwindleChildNodes(pDeque, pNode->children[1]);
    }
    else {
        pDeque->emplace_back(pNode);
    }
}

/*! Collect all windows that belong to the same group
 *
 * This function is similar to a part of the logic present in
 * Hyprland's CWindow::destroyGroup function.
 *
 * \param[out] pDeque   deque to store the found group windows into
 * \param[in] pWindow   any window that belongs to a group (doesn't have to be the group head window)
 */
void collectGroupWindows(std::vector<PHLWINDOW>* pMembersVec, PHLWINDOW pWindow)
{
    PHLWINDOW curr = pWindow;
    do {
        curr = curr->m_groupData.pNextWindow.lock();
        pMembersVec->push_back(curr);
    } while (curr != pWindow);
}

/*! Move given window into a group
 *
 * This is almost the same as CKeybindManager::moveWindowIntoGroup (dispatcher) function,
 * but without making the new window a group head and focused.
 *
 * \param[in] pWindow       Window to be inserted into a group
 * \param[in] pGroupWindow  Window that's a part of a group to insert the pWindow into
 */
void moveIntoGroup(PHLWINDOW pWindow, PHLWINDOW pGroupHeadWindow)
{
    Debug::log(LOG, "[dwindle-autogroup] Moving window {:x} into group {:x}", pWindow, pGroupHeadWindow);

    if (!pWindow || !pGroupHeadWindow) {
        Debug::log(ERR, "[dwindle-autogroup] Null window pointer in moveIntoGroup");
        return;
    }

    if (pWindow->m_groupData.deny) {
        Debug::log(LOG, "[dwindle-autogroup] Window denied grouping");
        return;
    }

    const auto P_LAYOUT = g_pLayoutManager->getCurrentLayout();
    if (!P_LAYOUT) {
        Debug::log(ERR, "[dwindle-autogroup] No layout available");
        return;
    }

    // remove the window from layout (will become a part of a group)
    P_LAYOUT->onWindowRemoved(pWindow);

    // Insert the new window into the group.
    // Depending on the config, the new window will either be inserted after
    // the current/head window in the group, or at the end of the group.
    const auto* USE_CURR_POS = (Hyprlang::INT* const*)g_pConfigManager->getConfigValuePtr("group:insert_after_current");
    PHLWINDOW pGroupInsertionWindow = (USE_CURR_POS && *USE_CURR_POS) ? pGroupHeadWindow : pGroupHeadWindow->getGroupTail();

    if (!pGroupInsertionWindow) {
        Debug::log(ERR, "[dwindle-autogroup] Failed to get group insertion window");
        return;
    }

    pGroupInsertionWindow->insertWindowToGroup(pWindow);

    // Since the inserted window is not becoming the group head, we need to hide it
    // (this is the difference from the original function, which would focus the window,
    // making it the new group head)
    pWindow->setHidden(true);

    // Add group bar decoration if needed
    if (!pWindow->getDecorationByType(DECORATION_GROUPBAR))
        pWindow->addWindowDeco(makeUnique<CHyprGroupBarDecoration>(pWindow));
}

/*! Check common pre-conditions for group creation/deletion and perform needed initializations
 *
 * \param[out] pDwindleLayout  Pointer to dwindle layout instance
 * \return  Necessary pre-conditions succeded?
 */
bool handleGroupOperation(CHyprDwindleLayout** pDwindleLayout)
{
    const auto P_LAYOUT = g_pLayoutManager->getCurrentLayout();
    if (P_LAYOUT->getLayoutName() != "dwindle") {
        Debug::log(LOG, "[dwindle-autogroup] Ignoring non-dwindle layout");
        return false;
    }

    *pDwindleLayout = dynamic_cast<CHyprDwindleLayout*>(P_LAYOUT);
    return true;
}

void newCreateGroup(CWindow* self)
{
    // Run the original function first, creating a "classical" group
    // with just the currently selected window in it
    ((createGroupFuncT)g_pCreateGroupHook->m_original)(self);

    // Only continue if the group really was created, as there are some pre-conditions to that
    // checked in the original function
    if (self->m_groupData.pNextWindow.expired()) {
        Debug::log(LOG, "[dwindle-autogroup] Ignoring autogroup - invalid / non-group widnow");
        return;
    }

    // Skip floating windows
    if (self->m_isFloating) {
        Debug::log(LOG, "[dwindle-autogroup] Ignoring autogroup for floating window");
        return;
    }

    Debug::log(LOG, "[dwindle-autogroup] Triggered createGroup for {:x}", self->m_self.lock());

    // Obtain an instance of the dwindle layout, also run some general pre-conditions
    // for the plugin, quit now if they're not met.
    CHyprDwindleLayout* pDwindleLayout = nullptr;
    if (!handleGroupOperation(&pDwindleLayout))
        return;

    Debug::log(LOG, "[dwindle-autogroup] Collecting dwindle child nodes");

    // Collect all child dwindle nodes, we'll want to add all of those into a group
    PHLWINDOW pSelfWindow = self->m_self.lock();
    if (!pSelfWindow) {
        Debug::log(ERR, "[dwindle-autogroup] Failed to lock self window");
        return;
    }

    const auto P_DWINDLE_NODE = g_pNodeFromWindow(pDwindleLayout, pSelfWindow);
    if (!P_DWINDLE_NODE) {
        Debug::log(ERR, "[dwindle-autogroup] Failed to get dwindle node for window");
        return;
    }

    const auto P_PARENT_NODE = P_DWINDLE_NODE->pParent;
    if (!P_PARENT_NODE) {
        Debug::log(LOG, "[dwindle-autogroup] Ignoring autogroup for a single window");
        return;
    }

    const auto P_SIBLING_NODE = P_PARENT_NODE->children[0] == P_DWINDLE_NODE ? P_PARENT_NODE->children[1] : P_PARENT_NODE->children[0];
    if (!P_SIBLING_NODE) {
        Debug::log(ERR, "[dwindle-autogroup] Sibling node is null");
        return;
    }

    std::deque<SDwindleNodeData*> p_dDwindleNodes;
    collectDwindleChildNodes(&p_dDwindleNodes, P_SIBLING_NODE);

    // Collect window pointers before modifying the layout
    // (node pointers become invalid after onWindowRemoved is called)
    std::vector<PHLWINDOW> vWindows;
    for (auto& node : p_dDwindleNodes) {
        auto curWindow = node->pWindow.lock();
        if (!curWindow) {
            Debug::log(LOG, "[dwindle-autogroup] Skipping null window");
            continue;
        }
        // Stop if one of the dwindle child nodes is already in a group
        if (!curWindow->m_groupData.pNextWindow.expired()) {
            Debug::log(LOG, "[dwindle-autogroup] Ignoring autogroup for nested groups: window {:x} is group", curWindow);
            return;
        }
        vWindows.push_back(curWindow);
    }

    Debug::log(LOG, "[dwindle-autogroup] Found {} windows to autogroup", vWindows.size());

    // Get the group head window once (already locked earlier as pSelfWindow)
    if (!pSelfWindow) {
        Debug::log(ERR, "[dwindle-autogroup] Group head window is null");
        return;
    }

    // Add all of the dwindle child node windows into the group
    for (auto& curWindow : vWindows) {
        if (!curWindow) {
            Debug::log(LOG, "[dwindle-autogroup] Skipping null window in grouping loop");
            continue;
        }
        Debug::log(LOG, "[dwindle-autogroup] Moving window {:x} into group", curWindow);
        moveIntoGroup(curWindow, pSelfWindow);
    }

    // Update decorations and recalculate layout only once after all windows are added
    pSelfWindow->updateWindowDecos();

    const auto P_LAYOUT_FINAL = g_pLayoutManager->getCurrentLayout();
    if (P_LAYOUT_FINAL) {
        P_LAYOUT_FINAL->recalculateWindow(pSelfWindow);
    }

    Debug::log(LOG, "[dwindle-autogroup] Autogroup done, {} windows moved", vWindows.size());
}

void newDestroyGroup(CWindow* self)
{
    // We can't use the original function here (other than for falling back)
    // as it removes the group head and then creates the new windows on the
    // layout. This often messes up the user layout of the windows.
    //
    // The goal of this function is to ungroup the windows such that they
    // only continue as children from the dwindle binary tree node the group
    // head was on.

    // Only continue if the window is in a group
    if (self->m_groupData.pNextWindow.expired()) {
        Debug::log(LOG, "[dwindle-autogroup] Ignoring ungroup - invalid / non-group widnow");
        return;
    }

    Debug::log(LOG, "[dwindle-autogroup] Triggered destroyGroup for {:x}", self->m_self.lock());

    // Obtain an instance of the dwindle layout, also run some general pre-conditions
    // for the plugin, fall back now if they're not met.
    CHyprDwindleLayout* pDwindleLayout = nullptr;
    if (!handleGroupOperation(&pDwindleLayout)) {
        ((destroyGroupFuncT)g_pDestroyGroupHook->m_original)(self);
        return;
    }

    std::vector<PHLWINDOW> vGroupWindows;
    collectGroupWindows(&vGroupWindows, self->m_self.lock());

    if (vGroupWindows.empty()) {
        Debug::log(LOG, "[dwindle-autogroup] No windows in group, aborting");
        return;
    }

    PHLWINDOW pSelfWindow = self->m_self.lock();

    // If the group head window is in fullscreen, unfullscreen it.
    // We need to have the window placed in the layout, to figure out where
    // to ungroup the rest of the windows.
    g_pCompositor->setWindowFullscreenState(pSelfWindow, SFullscreenState{.internal = FSMODE_NONE, .client = FSMODE_NONE});

    Debug::log(LOG, "[dwindle-autogroup] Ungroupping {} windows", vGroupWindows.size());

    // Set a groups lock flag
    const bool GROUPS_LOCKED_PREV = g_pKeybindManager->m_groupsLocked;
    g_pKeybindManager->m_groupsLocked = true;

    // First pass: break all group links
    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow) continue;
        Debug::log(LOG, "[dwindle-autogroup] Breaking group links for window {:x}", pWindow);
        pWindow->m_groupData.pNextWindow.reset();
        pWindow->m_groupData.head = false;
    }

    // Second pass: process windows and add them to layout
    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow) continue;

        Debug::log(LOG, "[dwindle-autogroup] Ungroupping window {:x}", pWindow);

        // Current / Visible window (this isn't always the head)
        if (!pWindow->isHidden()) {
            Debug::log(LOG, "[dwindle-autogroup] -> Visible window ungroup");

            // This window is already visible in the layout, we don't need to create
            // a new layout window for it.
            //
            // The original destroyGroup removes the window from the layout here,
            // which is what causes the weird ungroupping behavior as this window
            // is then recreated, which spawns it in a potentially unexpected place
            // (often determined by the cursor position).
        }
        else {
            Debug::log(LOG, "[dwindle-autogroup] -> Hidden window ungroup");
            pWindow->setHidden(false);

            g_pLayoutManager->getCurrentLayout()->onWindowCreatedTiling(pWindow);

            // Focus the window that we just spawned, so that on the next iteration
            // the window created will be it's dwindle child node.
            // This allows the original group head to remain a parent window to all
            // of the other (groupped) nodes.
            //
            // Note that this won't preserve the exact original layout of the group
            // but it will make sure all of the groupped windows will extend from
            // the dwindle node of the group head window. Preserving the original
            // layout isn't really possible, since new windows can be added into
            // groups after they were created.
            g_pCompositor->focusWindow(pWindow);
        }
    }

    g_pKeybindManager->m_groupsLocked = GROUPS_LOCKED_PREV;

    Debug::log(LOG, "[dwindle-autogroup] All windows ungroupped, updating decorations");

    // Update decorations for all windows at the end
    for (PHLWINDOW pWindow : vGroupWindows) {
        if (!pWindow) continue;
        pWindow->updateWindowDecos();
    }

    g_pCompositor->updateAllWindowsAnimatedDecorationValues();

    // Leave with the focus the original (main) window
    if (pSelfWindow)
        g_pCompositor->focusWindow(pSelfWindow);

    Debug::log(LOG, "[dwindle-autogroup] Ungroupping done");
}
