
#include "globals.hpp"
#include "OvGridLayout.hpp"
#include "dispatchers.hpp"

// Grid layout node management functions
// Responsible for storing window state during overview mode

// Retrieve window node data from window pointer
SOvGridNodeData *OvGridLayout::getNodeFromWindow(PHLWINDOW pWindow)
{
    for (auto &nd : m_lOvGridNodesData)
    {
        if (nd.pWindow == pWindow)
            return &nd;
    }

    return nullptr;
}


// Retrieve old layout record node from window pointer
SOldLayoutRecordNodeData *OvGridLayout::getOldLayoutRecordNodeFromWindow(PHLWINDOW pWindow)
{
    for (auto &nd : m_lSOldLayoutRecordNodeData)
    {
        if (nd.pWindow == pWindow)
            return &nd;
    }

    return nullptr;
}

// Count windows on a specific workspace
int OvGridLayout::getNodesNumOnWorkspace(const WORKSPACEID &ws)
{
    int no = 0;
    for (auto &n : m_lOvGridNodesData)
    {
        if (n.workspaceID == ws)
            no++;
    }

    return no;
}

// Resize and reposition node, accounting for titlebar and group bar
void OvGridLayout::resizeNodeSizePos(SOvGridNodeData *node, int x, int y, int width, int height)
{
    int groupbar_height_fix;
    // Adjust for group bar if window belongs to a group
    if(node->pWindow->m_group) {
        groupbar_height_fix = g_hycov_groupBarHeight;
    } else {
        groupbar_height_fix = 0;
    }
    node->size = Vector2D(width, height - g_hycov_height_of_titlebar - groupbar_height_fix);
    node->position = Vector2D(x, y + g_hycov_height_of_titlebar + groupbar_height_fix);
    applyNodeDataToWindow(node);
}

// Initialize overview mode and record current window states
void OvGridLayout::beginOverview()
{
    PHLMONITOR pTargetMonitor;

    // Record all visible windows before entering overview
    for (auto &w : g_pCompositor->m_windows)
    {
        PHLWINDOW pWindow = w;

        if (pWindow->isHidden() || !pWindow->m_isMapped || pWindow->m_fadingOut)
            continue;

        if(pWindow->monitorID() != Desktop::focusState()->monitor()->m_id && g_hycov_only_active_monitor && !g_hycov_forece_display_all && !g_hycov_forece_display_all_in_one_monitor)
            continue;

        const auto pNode = &m_lSOldLayoutRecordNodeData.emplace_back();
        pNode->pWindow = pWindow;
        onWindowCreatedTilingInternal(pWindow);
    }
}

// Add window to overview and record its original state
void OvGridLayout::onWindowCreatedTilingInternal(PHLWINDOW pWindow)
{
    PHLMONITOR pTargetMonitor;
    if(g_hycov_forece_display_all_in_one_monitor) {
        pTargetMonitor = Desktop::focusState()->monitor();
    } else {
      pTargetMonitor = pWindow->m_monitor.lock();
    }

    const auto pNode = &m_lOvGridNodesData.emplace_back();

    auto pActiveWorkspace = pTargetMonitor->m_activeWorkspace;

    auto pWindowOriWorkspace = pWindow->m_workspace;

    auto oldLayoutRecordNode = getOldLayoutRecordNodeFromWindow(pWindow);
    if(oldLayoutRecordNode) {
        pNode->isInOldLayout = true;
        m_lSOldLayoutRecordNodeData.remove(*oldLayoutRecordNode);
    }

    // record if window is the active window in its group
    if(pWindow->m_group && pWindow->m_group->current() == pWindow) {
        pNode->isGroupActive = true;
    }

    pNode->workspaceID = pWindow->m_workspace->m_id;
    pNode->pWindow = pWindow;
    pNode->workspaceName = pWindowOriWorkspace->m_name;

    pNode->ovbk_windowMonitorId = pWindow->monitorID();
    pNode->ovbk_windowWorkspaceId = pWindow->m_workspace->m_id;
    pNode->ovbk_windowFullscreenMode  = pWindowOriWorkspace->m_fullscreenMode;
    pNode->ovbk_position = pWindow->m_realPosition->goal();
    pNode->ovbk_size = pWindow->m_realSize->goal();
    pNode->ovbk_windowIsFloating = pWindow->m_isFloating;
    pNode->ovbk_windowIsFullscreen = pWindow->isFullscreen();
    pNode->ovbk_windowWorkspaceName = pWindowOriWorkspace->m_name;

    pNode->ovbk_windowIsWithBorder = true;
    pNode->ovbk_windowIsWithDecorate = true;
    pNode->ovbk_windowIsWithRounding = true;
    pNode->ovbk_windowIsWithShadow = true;

    // move window to active workspace if from a different one
    if ((!g_pCompositor->isWorkspaceSpecial(pNode->workspaceID) || g_hycov_show_special) && pNode->isInOldLayout && (pWindowOriWorkspace->m_id != pActiveWorkspace->m_id || pWindowOriWorkspace->m_name != pActiveWorkspace->m_name) && (!(g_hycov_only_active_workspace || g_hycov_force_display_only_current_workspace) || g_hycov_forece_display_all || g_hycov_forece_display_all_in_one_monitor))    {
        pWindow->m_workspace = pActiveWorkspace;
        pNode->workspaceID = pWindow->m_workspace->m_id;
        pNode->workspaceName = pActiveWorkspace->m_name;
        pWindow->m_monitor = pTargetMonitor;
        pNode->ovbk_movedForOverview = true;
    }

    // clean fullscreen status
    if (pWindow->isFullscreen()) {
        g_pCompositor->setWindowFullscreenInternal(pWindow, FSMODE_NONE);
    }

    // clean floating status (only apply to old layout window)
    if (pWindow->m_isFloating && pNode->isInOldLayout) {
        pWindow->m_isFloating = false;
        pWindow->updateWindowData();
    }

    recalculateMonitorById(pWindow->monitorID());
}


// Remove window from source layout when exiting overview
void OvGridLayout::removeOldLayoutData(PHLWINDOW pWindow) {
    auto pNode = getNodeFromWindow(pWindow);

    // Get the source layout name
    std::string configLayoutName = g_hycov_overview_source_layout.empty() ? g_hycov_configLayoutName : g_hycov_overview_source_layout;
    hycov_log(LOG,"remove data of old layout:{}",configLayoutName);

    // Find the original workspace to access the source layout's algo
    WORKSPACEID srcWsId = pNode ? pNode->ovbk_windowWorkspaceId : pWindow->m_workspace->m_id;
    auto sourceWs = g_pCompositor->getWorkspaceByID(srcWsId);

    if (!sourceWs || !sourceWs->m_space || !sourceWs->m_space->algorithm()) {
        hycov_log(Log::ERR,"removeOldLayoutData: could not find source workspace or space for window");
        return;
    }

    // Find the target for this window in the source workspace's space
    SP<Layout::ITarget> pTarget;
    for (auto& wt : sourceWs->m_space->targets()) {
        if (auto t = wt.lock(); t && t->window() == pWindow) {
            pTarget = t;
            break;
        }
    }

    if (!pTarget) {
        // Try active workspace space as fallback
        auto activeWs = Desktop::focusState()->monitor()->m_activeWorkspace;
        if (activeWs && activeWs->m_space) {
            for (auto& wt : activeWs->m_space->targets()) {
                if (auto t = wt.lock(); t && t->window() == pWindow) {
                    pTarget = t;
                    break;
                }
            }
        }
    }

    if (!pTarget) {
        hycov_log(LOG,"removeOldLayoutData: no target found for window, skipping source layout removal");
        return;
    }

    auto& tiledAlgo = sourceWs->m_space->m_algorithm->m_tiled;

    if(configLayoutName == "dwindle") {
        // dwindle recalculate hook is already active during overview, so just call removeTarget directly
        tiledAlgo->removeTarget(pTarget);
    } else if(configLayoutName == "master") {
        // master recalculate hook is already active during overview
        tiledAlgo->removeTarget(pTarget);
    } else if (configLayoutName == "scrolling") {
        tiledAlgo->removeTarget(pTarget);
    } else {
        hycov_log(Log::ERR,"unknown old layout:{}, trying generic target removal",configLayoutName);
        tiledAlgo->removeTarget(pTarget);
    }
}

// Remove window from overview and recalculate layout
void OvGridLayout::removeWindowFromOverview(PHLWINDOW pWindow) {
    const auto pNode = getNodeFromWindow(pWindow);

    if (!pNode)
        return;

    if(pNode->isInOldLayout) {
        removeOldLayoutData(pWindow);
    }

    m_lOvGridNodesData.remove(*pNode);

    if(m_lOvGridNodesData.empty()){
        return;
    }

    recalculateMonitorById(pWindow->monitorID());
}

// Calculate grid layout positions for all windows in a workspace
void OvGridLayout::calculateWorkspace(const WORKSPACEID &ws)
{
    const auto pWorksapce = g_pCompositor->getWorkspaceByID(ws);
    auto dataSize = m_lOvGridNodesData.size();
    auto pTempNodes = new SOvGridNodeData*[dataSize + 1];
    SOvGridNodeData *pNode;
    int i, n = 0;
    int cx, cy;
    int dx, cw, ch;;
    int cols, rows, overcols,NODECOUNT;

    if (!pWorksapce) {
        delete[] pTempNodes;
        return;
    }

    // Count windows on this workspace
    NODECOUNT = getNodesNumOnWorkspace(pWorksapce->m_id);
    const auto pMonitor = pWorksapce->m_monitor.lock();

    if (NODECOUNT == 0) {
        delete[] pTempNodes;
        return;
    }

    // Get layout configuration
    static const auto *PBORDERSIZE = &g_hycov_bordersize;
    static const auto *GAPPO = &g_hycov_overview_gappo;  // outer gap
    static const auto *GAPPI = &g_hycov_overview_gappi;  // inner gap

    // Get monitor's available area
    const auto RESERVED = pMonitor->logicalBoxMinusReserved();
    int m_x = pMonitor->m_position.x;
    int m_y = pMonitor->m_position.y;
    int w_x = RESERVED.x;
    int w_y = RESERVED.y;
    int m_width = pMonitor->m_size.x;
    int m_height = pMonitor->m_size.y;
    int w_width = RESERVED.w;
    int w_height = RESERVED.h;

    // Collect windows for this workspace
    for (auto &node : m_lOvGridNodesData)
    {
        if (node.workspaceID == ws)
        {
            pTempNodes[n] = &node;
            n++;
        }
    }

    pTempNodes[n] = NULL;  // Null-terminate array

    if (NODECOUNT == 0) {
        delete[] pTempNodes;
        return;
    }

    // Special case: single window - center it
    if (NODECOUNT == 1)
    {
        pNode = pTempNodes[0];
        cw = (w_width - 2 * (*GAPPO)) * 0.7;
        ch = (w_height - 2 * (*GAPPO)) * 0.8;
        resizeNodeSizePos(pNode, w_x + (int)((m_width - cw) / 2), w_y + (int)((w_height - ch) / 2),
                          cw - 2 * (*PBORDERSIZE), ch - 2 * (*PBORDERSIZE));
        delete[] pTempNodes;
        return;
    }

    // Special case: two windows - side by side layout
    if (NODECOUNT == 2)
    {
        pNode = pTempNodes[0];
        cw = (w_width - 2 * (*GAPPO) - (*GAPPI)) / 2;
        ch = (w_height - 2 * (*GAPPO)) * 0.65;
        resizeNodeSizePos(pNode, m_x + cw + (*GAPPO) + (*GAPPI), m_y + (m_height - ch) / 2 + (*GAPPO),
                          cw - 2 * (*PBORDERSIZE), ch - 2 * (*PBORDERSIZE));
        resizeNodeSizePos(pTempNodes[1], m_x + (*GAPPO), m_y + (m_height - ch) / 2 + (*GAPPO),
                          cw - 2 * (*PBORDERSIZE), ch - 2 * (*PBORDERSIZE));
        delete[] pTempNodes;
        return;
    }

    // General case: grid layout for 3+ windows
    // Calculate grid dimensions for roughly square aspect ratio
    for (cols = 0; cols <= NODECOUNT / 2; cols++)
        if (cols * cols >= NODECOUNT)
            break;

    // Prefer rows over cols for better spacing
    rows = (cols && (cols - 1) * cols >= NODECOUNT) ? cols - 1 : cols;

    // Calculate cell height and width based on gap configuration
    ch = (int)((w_height - 2 * (*GAPPO) - (rows - 1) * (*GAPPI)) / rows);
    cw = (int)((w_width - 2 * (*GAPPO) - (cols - 1) * (*GAPPI)) / cols);

    // Handle last row which may have fewer windows
    overcols = NODECOUNT % cols;

    if (overcols)
        dx = (int)((w_width - overcols * cw - (overcols - 1) * (*GAPPI)) / 2) - (*GAPPO);

    // Position each window in the grid
    for (i = 0, pNode = pTempNodes[0]; pNode; pNode = pTempNodes[i + 1], i++)
    {
        cx = w_x + (i % cols) * (cw + (*GAPPI));
        cy = w_y + (int)(i / cols) * (ch + (*GAPPI));
        if (overcols && i >= (NODECOUNT-overcols))
        {
            cx += dx;
        }
        resizeNodeSizePos(pNode, cx + (*GAPPO), cy + (*GAPPO), cw - 2 * (*PBORDERSIZE), ch - 2 * (*PBORDERSIZE));
    }
    delete[] pTempNodes;
}

// Recalculate overview layout for a specific monitor
void OvGridLayout::recalculateMonitorById(const MONITORID &monid)
{
    const auto pMonitor = g_pCompositor->getMonitorFromID(monid);

    if(!pMonitor || !pMonitor->m_activeWorkspace)
        return;

    g_pHyprRenderer->damageMonitor(pMonitor);

    if (pMonitor->activeSpecialWorkspaceID()) {
        calculateWorkspace(pMonitor->activeSpecialWorkspaceID());
        return;
    }

    const auto pWorksapce = g_pCompositor->getWorkspaceByID(pMonitor->activeWorkspaceID());
    if (!pWorksapce)
        return;

    calculateWorkspace(pWorksapce->m_id);
}

// Apply calculated position and size to window
void OvGridLayout::applyNodeDataToWindow(SOvGridNodeData *pNode)
{
    const auto pWindow = pNode->pWindow;

    // Set logical position and size
    pWindow->m_size = pNode->size;
    pWindow->m_position = pNode->position;

    // Set rendering position and size
    auto calcPos = pWindow->m_position;
    auto calcSize = pWindow->m_size;

    *pWindow->m_realSize = calcSize;
    *pWindow->m_realPosition = calcPos;
    pWindow->sendWindowSize();

    // Update window decorations
    pWindow->updateWindowDecos();
}

// Switch to the original workspace of the active window
void OvGridLayout::changeToActivceSourceWorkspace()
{
    PHLWINDOW pWindow = nullptr;
    SOvGridNodeData *pNode;
    PHLWORKSPACE pWorksapce;
    hycov_log(LOG,"changeToActivceSourceWorkspace");
    pWindow = Desktop::focusState()->window();
    pNode = getNodeFromWindow(pWindow);

    // Get the original workspace from the node or current window
    if(pNode) {
        pWorksapce = g_pCompositor->getWorkspaceByID(pNode->ovbk_windowWorkspaceId);
    } else if(pWindow) {
        pWorksapce = pWindow->m_workspace;
    } else {
        pWorksapce = Desktop::focusState()->monitor()->m_activeWorkspace;
    }
    hycov_log(LOG,"changeToWorkspace:{}",pWorksapce->m_id);
    g_pEventManager->postEvent(SHyprIPCEvent{"workspace", pWorksapce->m_name});
}

// Restore all windows to their original workspace and state after exiting overview
std::pair<int, int> OvGridLayout::moveWindowToSourceWorkspace()
{
    PHLWORKSPACE pWorkspace;
    int restored = 0;
    int failed = 0;

    hycov_log(LOG,"moveWindowToSourceWorkspace");

    // Restore each window's original location
    for (auto &nd : m_lOvGridNodesData)
    {
        if (!nd.pWindow) {
            continue;
        }

        // Check if window needs restoration
        const bool needsRestore = nd.ovbk_movedForOverview || nd.pWindow->m_workspace->m_id != nd.ovbk_windowWorkspaceId || nd.workspaceName != nd.ovbk_windowWorkspaceName;
        if (needsRestore)
        {
            // Get or create the source workspace
            pWorkspace = g_pCompositor->getWorkspaceByID(nd.ovbk_windowWorkspaceId);
            if (!pWorkspace){
                hycov_log(LOG,"source workspace no exist");
                g_hycov_pSpawnHook->hook();
                pWorkspace = g_pCompositor->createNewWorkspace(nd.ovbk_windowWorkspaceId,nd.ovbk_windowMonitorId,nd.ovbk_windowWorkspaceName);
                g_hycov_pSpawnHook->unhook();
                hycov_log(LOG,"create workspace: id:{} monitor:{} name:{}",nd.ovbk_windowWorkspaceId,nd.pWindow->monitorID(),nd.ovbk_windowWorkspaceName);
            }

            // Restore window to monitor and workspace
            auto pMonitor = g_pCompositor->getMonitorFromID(nd.ovbk_windowMonitorId);
            if (!pWorkspace || !pMonitor) {
                failed++;
                hycov_log(Log::ERR,"restore source workspace failed,window:{} workspace:{} monitor:{}",nd.pWindow,nd.ovbk_windowWorkspaceId,nd.ovbk_windowMonitorId);
                continue;
            }

            nd.pWindow->m_monitor = pMonitor;
            nd.pWindow->m_workspace = pWorkspace;
            nd.workspaceID = nd.ovbk_windowWorkspaceId;
            nd.workspaceName = nd.ovbk_windowWorkspaceName;
            nd.pWindow->m_position = nd.ovbk_position;
            nd.pWindow->m_size = nd.ovbk_size;
            g_pHyprRenderer->damageWindow(nd.pWindow);
            nd.ovbk_movedForOverview = false;
            restored++;
        }
    }

    hycov_log(LOG,"moveWindowToSourceWorkspace done,restored:{} failed:{}",restored,failed);
    return {restored, failed};
}
