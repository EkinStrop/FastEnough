#include "app.h"
#include "ui_widgets.h"
#include "device_dokan.h"
#include "mcraw_projfs.h"
#include <algorithm>
#include <cctype>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace {
bool finished(BatchState state) {
    return state == BatchState::Completed || state == BatchState::Failed || state == BatchState::Stopped;
}

const char* batchStateLabel(const TransferBatch& batch) {
    auto state = batch.state.load();
    if (!finished(state) && batch.disconnected) return "Waiting for phone";
    if (batch.waitingConflict) return "Needs a decision";
    switch (state) {
    case BatchState::Queued: return "Queued";
    case BatchState::Running: return "Copying";
    case BatchState::Paused: return "Paused";
    case BatchState::Verifying: return "Checking files";
    case BatchState::Completed:
        return batch.errorSkippedFiles > 0 || batch.crcFailCount() > 0 ? "Completed with errors" : "Completed";
    case BatchState::Failed: return "Failed";
    case BatchState::Stopped: return "Cancelled";
    default: return "Needs a decision";
    }
}

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(0, pos);
}

std::wstring workspaceWide(const std::string& value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count);
    return result;
}
}

std::string App::deviceDisplayName(int slot) const {
    slot &= 1;
    auto found = m_deviceDisplayNames.find(m_slotSerial[slot]);
    if (found != m_deviceDisplayNames.end() && !found->second.empty()) return found->second;
    if (!m_slotSerial[slot].empty()) return m_slotSerial[slot];
    return "Android device";
}

std::string App::panelDisplayName(const FilePanel& panel) const {
    if (panel.isConnections) return "Connections";
    return panel.isAndroid || panel.isApps ? deviceDisplayName(panel.deviceSlot) : "This PC";
}

bool App::panelAvailable(const FilePanel& panel) const {
    if (panel.isConnections) return false;
    return !panel.isAndroid && !panel.isApps ||
        (m_slotConnected[panel.deviceSlot & 1] && m_deviceSlots[panel.deviceSlot & 1].isServerRunning());
}

void App::renderWorkspaceHeader() {
    const float s = ui::scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16*s,8*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("##WorkspaceHeader", ImVec2(0,48*s), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysUseWindowPadding);
    ui::button("F##Brand", ui::Icon::None, ui::Appearance::Selected, ImVec2(28*s,32*s));
    if (ImGui::GetWindowWidth() > 760*s) {
        ImGui::SameLine(0,10*s); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Fast Enough");
    }
    ImGui::SameLine(0,24*s);
    auto tab = [&](const char* name, bool selected) {
        if (selected) ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if(ui::semiboldFont)ImGui::PushFont(ui::semiboldFont);
        bool pressed = ui::button(name,ui::Icon::None,ui::Appearance::Quiet);
        if(ui::semiboldFont)ImGui::PopFont();
        if (selected) ImGui::PopStyleColor();
        if (selected) {
            ImVec2 a=ImGui::GetItemRectMin(), b=ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x+8*s,b.y+6*s),ImVec2(b.x-8*s,b.y+6*s),ImGui::GetColorU32(ImGuiCol_CheckMark),2*s);
        }
        return pressed;
    };
    if (tab("Files",!m_showAppsWorkspace&&!m_showBackupManager&&!m_showConnectionsWorkspace)) {
        m_showAppsWorkspace=m_showBackupManager=m_showConnectionsWorkspace=false;
        m_lastFocusedPanel=&m_rightPanel;
    }
    ImGui::SameLine(0,4*s);
    if (tab("Apps",m_showAppsWorkspace)) {
        if (!m_showAppsWorkspace) {
            int slot=m_lastFocusedPanel?m_lastFocusedPanel->deviceSlot:0;
            m_appsWorkspacePanel.isAndroid=true;
            m_appsWorkspacePanel.isApps=true;
            m_appsWorkspacePanel.deviceSlot=slot;
            m_appsWorkspacePanel.needsRefresh=true;
        }
        m_showAppsWorkspace=true; m_showBackupManager=m_showConnectionsWorkspace=false;
    }
    ImGui::SameLine(0,4*s);
    if (tab("Backups",m_showBackupManager)) {
        if (!m_showBackupManager) {
            m_backupManagerNeedsAppRefresh=m_backupManagerNeedsBackupRefresh=true;
            m_backupRootAccess=BackupRootAccess::Unknown;
            m_backupRootAccessSerial.clear();
        }
        m_showBackupManager=true; m_showAppsWorkspace=m_showConnectionsWorkspace=false;
    }
    int ready=0;
    for (int i=0;i<2;++i) if (m_slotConnected[i]&&m_deviceSlots[i].isServerRunning()) ++ready;
    std::string deviceLabel=std::to_string(ready)+(ready==1?" phone ready":" phones ready");
    float rightWidth=ImGui::CalcTextSize(deviceLabel.c_str()).x+132*s;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-rightWidth));
    if (ui::button((deviceLabel+"##Devices").c_str(),ui::Icon::Phone)) ImGui::OpenPopup("##WorkspaceDevices");
    if (ImGui::BeginPopup("##WorkspaceDevices")) {
        for (int slot=0;slot<2;++slot) {
            if (m_slotSerial[slot].empty()) continue;
            std::string name=deviceDisplayName(slot)+(m_slotConnected[slot]?"":" (disconnected)")+"##device"+std::to_string(slot);
            if (ImGui::MenuItem(name.c_str())) { m_detailsDeviceSlot=slot; m_workspaceDrawer=2; }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Connect a device...")) {
            m_showConnectionsWorkspace=true; m_showAppsWorkspace=m_showBackupManager=false;
            m_connectionsWorkspacePanel.isConnections=true;
        }
        if (ImGui::MenuItem("Pair over WiFi...")) m_showWifiPairing=true;
        ImGui::EndPopup();
    }
    ImGui::SameLine(0,2*s);
    if (ui::button("##WorkspaceSettings",ui::Icon::Settings,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Workspace settings")) m_workspaceDrawer=3;
    ImGui::SameLine(0,2*s);
    if (ui::button("##WorkspaceMore",ui::Icon::More,ui::Appearance::Quiet,ImVec2(32*s,32*s),"All commands")) ImGui::OpenPopup("##WorkspaceMenu");
    renderMenuBar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::renderWorkspaceToolbar() {
    const float s=ui::scale();
    FilePanel& source=m_lastFocusedPanel==&m_leftPanel?m_leftPanel:m_rightPanel;
    FilePanel& destination=&source==&m_leftPanel?m_rightPanel:m_leftPanel;
    bool fileView=!source.isApps&&!source.isConnections;
    bool hasSelection=fileView&&!source.selectedIndices.empty();
    bool canTransfer=hasSelection&&panelAvailable(source)&&panelAvailable(destination)&&!destination.isApps;
    if (source.isAndroid&&destination.isAndroid&&source.deviceSlot==destination.deviceSlot) canTransfer=false;
    bool canMove=canTransfer&&!source.isAndroid&&!destination.isAndroid;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*s,6*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("##WorkspaceToolbar",ImVec2(0,44*s),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    std::string selection=hasSelection?std::to_string(source.selectedIndices.size())+" selected, "+formatSize(source.selectedFileSize()):"Select files to copy";
    bool wide=ImGui::GetWindowWidth()>980*s;
    ui::textClipped(selection,(wide?150.0f:110.0f)*s,ImGui::GetColorU32(ImGuiCol_TextDisabled),32*s);
    ImGui::SameLine(0,10*s);
    ImGui::BeginDisabled(!canTransfer);
    std::string name=panelDisplayName(destination);
    std::string copyLabel="Copy to "+name;
    float copyWidth=std::min(230*s,ImGui::CalcTextSize(copyLabel.c_str()).x+45*s);
    if(ui::button(copyLabel.c_str(),ui::Icon::Copy,ui::Appearance::Primary,ImVec2(copyWidth,32*s),copyLabel.c_str())) startWorkspaceTransfer(false);
    ImGui::EndDisabled();
    if(wide) {
        ImGui::SameLine(0,4*s); ImGui::BeginDisabled(!canMove);
        if(ui::button("Move",ui::Icon::Move,ui::Appearance::Secondary)) startWorkspaceTransfer(true);
        ImGui::EndDisabled();
        ui::tooltip("Move files between local folders. Use Copy for transfers involving a phone.");
        ImGui::SameLine(0,4*s); ImGui::BeginDisabled(!hasSelection||!panelAvailable(source));
        if(ui::button("##ToolbarDelete",ui::Icon::Trash,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Delete selected files")) {
            m_contextPanel=&source; m_contextIndex=*source.selectedIndices.begin(); m_deletePermanent=false; m_showDeleteConfirm=true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0,12*s); ImGui::BeginDisabled(!fileView||!panelAvailable(source));
        if(ui::button("New folder",ui::Icon::Plus,ui::Appearance::Secondary)) { m_contextPanel=&source; m_newFolderName[0]=0; m_showNewFolderPopup=true; }
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0,4*s);
    if(ui::button("##FileActions",ui::Icon::More,ui::Appearance::Quiet,ImVec2(32*s,32*s),"More file actions")) ImGui::OpenPopup("##FileActionsMenu");
    if(ImGui::BeginPopup("##FileActionsMenu")) {
        if(ImGui::MenuItem("Move to other pane",nullptr,false,canMove)) startWorkspaceTransfer(true);
        if(ImGui::MenuItem("Rename...","F2",false,hasSelection&&source.selectedIndices.size()==1&&panelAvailable(source))) {
            m_contextPanel=&source; m_contextIndex=*source.selectedIndices.begin(); strcpy_s(m_renameBuf,source.entryName(m_contextIndex).c_str());m_showRenamePopup=true;
        }
        if(ImGui::MenuItem("Delete...","Del",false,hasSelection&&panelAvailable(source))) {
            m_contextPanel=&source;m_contextIndex=*source.selectedIndices.begin();m_deletePermanent=false;m_showDeleteConfirm=true;
        }
        if(ImGui::MenuItem("New folder...",nullptr,false,fileView&&panelAvailable(source))) {
            m_contextPanel=&source;m_newFolderName[0]=0;m_showNewFolderPopup=true;
        }
        ImGui::Separator();
        if(ImGui::MenuItem("Show hidden files",nullptr,source.showHidden)) { source.showHidden=!source.showHidden;source.needsRefresh=true; }
        ImGui::EndPopup();
    }
    float trailingWidth=ImGui::CalcTextSize("Compare").x+97*s;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-trailingWidth));
    ImGui::BeginDisabled(!fileView||destination.isApps||destination.isConnections);
    if(ui::button("Compare",ui::Icon::Compare,m_compareEnabled?ui::Appearance::Selected:ui::Appearance::Quiet)) {
        m_compareEnabled=!m_compareEnabled;m_compareDirty=true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0,4*s);ImGui::BeginDisabled(!hasSelection);
    if(ui::button("##SelectionDetails",ui::Icon::Info,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Selection details")) m_workspaceDrawer=1;
    ImGui::EndDisabled();
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
}

void App::renderPaneHeader(FilePanel& panel, PanelSide side) {
    const float s=ui::scale();
    ImGui::PushID(&panel);
    std::string title=panelDisplayName(panel);
    std::string caption=panel.isConnections?"USB and wireless connections":"Local storage";
    if(panel.isAndroid||panel.isApps) {
        int slot=panel.deviceSlot&1;
        caption=panelAvailable(panel)?"Connected":"Disconnected";
        if(panelAvailable(panel)) {
            caption+=m_deviceSlots[slot].isDirectConnection()?"  /  Direct connection":"  /  ADB";
            if(slot==0&&m_dualChannelAvailable)caption+=" + "+m_secondaryChannelType;
        }
    }
    ImVec2 origin=ImGui::GetCursorScreenPos();
    float width=ImGui::GetContentRegionAvail().x;
    if(ui::button("##PaneLocation",ui::Icon::None,ui::Appearance::Quiet,ImVec2(width-40*s,44*s),"Choose a location"))
        ImGui::OpenPopup("##PaneLocationMenu");
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x,origin.y+5*s),ImVec2(origin.x+32*s,origin.y+39*s),ImGui::GetColorU32(ImGuiCol_Button),6*s);
    ui::icon(panel.isAndroid||panel.isApps||panel.isConnections?ui::Icon::Phone:ui::Icon::Computer,
        ImVec2(origin.x+6*s,origin.y+11*s),20*s,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    float textWidth=std::max(20*s,width-102*s);
    if(ui::semiboldFont)ImGui::PushFont(ui::semiboldFont);
    ui::drawTextClipped(ImVec2(origin.x+44*s,origin.y+4*s),title,textWidth,ImGui::GetColorU32(ImGuiCol_Text));
    float arrowX=origin.x+44*s+std::min(textWidth,ImGui::CalcTextSize(title.c_str()).x)+6*s;
    if(ui::semiboldFont)ImGui::PopFont();
    ui::icon(ui::Icon::Down,ImVec2(arrowX,origin.y+4*s),14*s,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ui::drawTextClipped(ImVec2(origin.x+44*s,origin.y+25*s),caption,width-88*s,ImGui::GetColorU32(ImGuiCol_TextDisabled),12*s);
    ImGui::SameLine(0,8*s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+6*s);
    if(ui::button("##PaneInfo",ui::Icon::Info,ui::Appearance::Quiet,ImVec2(32*s,32*s),panel.isAndroid||panel.isApps?"Device details":"Local file details")) {
        if(panel.isAndroid||panel.isApps){m_detailsDeviceSlot=panel.deviceSlot;m_workspaceDrawer=2;}
        else{m_lastFocusedPanel=&panel;m_workspaceDrawer=1;}
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(280*s,0),ImVec2(460*s,500*s));
    if(ImGui::BeginPopup("##PaneLocationMenu")) {
        if(!panel.isApps&&ImGui::Selectable("This PC",!panel.isAndroid&&!panel.isConnections)) switchPanelMode(panel,false);
        for(int slot=0;slot<2;++slot) {
            if(m_slotSerial[slot].empty()&&!m_slotConnected[slot]) continue;
            std::string serial=m_slotSerial[slot];
            std::string suffix=serial.size()>6?serial.substr(serial.size()-6):serial;
            std::string label=deviceDisplayName(slot)+" ("+suffix+")"+(!m_slotConnected[slot]?" [offline]":"")+"##"+std::to_string(slot);
            if(ImGui::Selectable(label.c_str(),(panel.isAndroid||panel.isApps)&&panel.deviceSlot==slot)) {
                if(panel.isApps) {
                    panel.deviceSlot=slot;panel.appEntries.clear();panel.selectedIndices.clear();panel.needsRefresh=true;
                } else if(!panel.isAndroid||panel.isConnections||panel.deviceSlot!=slot) {
                    if(!panel.isAndroid||panel.isConnections) switchPanelMode(panel,true);
                    panel.deviceSlot=slot;panel.isAndroid=true;panel.isConnections=false;
                    panel.insideMcraw=false;panel.mcrawFilePath.clear();panel.androidEntries.clear();panel.selectedIndices.clear();
                    panel.navHistory.clear();panel.navHistoryPos=-1;
                    panel.currentPath=m_slotStorageRoot[slot].empty()?"/sdcard":m_slotStorageRoot[slot];
                    strcpy_s(panel.pathInput,panel.currentPath.c_str());panel.needsRefresh=true;
                }
            }
        }
        ImGui::Separator();
        if(ImGui::Selectable("Connect a device...")) {
            m_showConnectionsWorkspace=true;m_showAppsWorkspace=m_showBackupManager=false;m_connectionsWorkspacePanel.isConnections=true;
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::PopID();
}

void App::renderPaneNavigation(FilePanel& panel) {
    const float s=ui::scale();
    bool requestEdit=m_lastFocusedPanel==&panel && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L);
    if(requestEdit){panel.editingPath=true;panel.pathFocusRequested=true;strcpy_s(panel.pathInput,panel.currentPath.c_str());}
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::BeginChild("##PaneNavigation",ImVec2(0,28*s),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    auto history=[&](int step) {
        panel.navHistoryPos+=step;panel.currentPath=panel.navHistory[panel.navHistoryPos];
        panel.selectedIndices.clear();panel.focusedIndex=-1;panel.searchFilter[0]=0;
        panel.needsRefresh=true;panel.navigationTransitionPending=true;
        strcpy_s(panel.pathInput,panel.currentPath.c_str());
    };
    ImGui::BeginDisabled(panel.navHistoryPos<=0);
    if(ui::button("##Back",ui::Icon::Back,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Back"))history(-1);
    ImGui::EndDisabled();ImGui::SameLine(0,0);
    ImGui::BeginDisabled(panel.navHistoryPos<0||panel.navHistoryPos+1>=(int)panel.navHistory.size());
    if(ui::button("##Forward",ui::Icon::Forward,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Forward"))history(1);
    ImGui::EndDisabled();ImGui::SameLine(0,0);
    if(ui::button("##Up",ui::Icon::Up,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Up one folder"))navigateUp(panel);
    ImGui::SameLine(0,5*s);
    float available=ImGui::GetContentRegionAvail().x;
    if(panel.editingPath) {
        ImGui::SetNextItemWidth(available);
        if(requestEdit || panel.pathFocusRequested){ImGui::SetKeyboardFocusHere();panel.pathFocusRequested=false;}
        if(ImGui::InputText("##FolderPath",panel.pathInput,sizeof(panel.pathInput),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll)) {
            std::string path=panel.pathInput;
            if(!path.empty()){navigateToDirectory(panel,path);panel.editingPath=false;}
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))panel.editingPath=false;
    } else {
        std::vector<std::pair<std::string,std::string>> crumbs;
        std::string path=panel.currentPath;
        char separator=panel.isAndroid?'/':'\\';
        size_t offset=0;
        if(panel.isAndroid&&path.starts_with('/')){crumbs.push_back({"/","/"});offset=1;}
        else if(path.size()>=3&&path[1]==':'){crumbs.push_back({path.substr(0,2),path.substr(0,3)});offset=3;}
        while(offset<path.size()){
            size_t next=path.find(separator,offset);if(next==std::string::npos)next=path.size();
            if(next>offset)crumbs.push_back({path.substr(offset,next-offset),path.substr(0,next)});
            offset=next+1;
        }
        float total=0;for(auto& crumb:crumbs)total+=ImGui::CalcTextSize(crumb.first.c_str()).x+34*s;
        bool condensed=total>available-40*s;
        if(condensed) {
            if(ui::button("##ParentFolders",ui::Icon::More,ui::Appearance::Quiet,ImVec2(28*s,28*s),"Parent folders"))ImGui::OpenPopup("##ParentFolderMenu");
            if(ImGui::BeginPopup("##ParentFolderMenu")) {
                for(auto& crumb:crumbs)if(ImGui::MenuItem(crumb.second.c_str()))navigateToDirectory(panel,crumb.second);
                ImGui::EndPopup();
            }
            ImGui::SameLine(0,2*s);
        }
        size_t first=condensed&&!crumbs.empty()?crumbs.size()-1:0;
        for(size_t i=first;i<crumbs.size();++i){
            if(i>first){ImGui::SameLine(0,0);ImGui::AlignTextToFramePadding();ImGui::TextDisabled(">");ImGui::SameLine(0,0);}
            ImGui::PushID((int)i);
            float remaining=std::max(24*s,ImGui::GetContentRegionAvail().x-36*s);
            float width=std::min(remaining,ImGui::CalcTextSize(crumbs[i].first.c_str()).x+20*s);
            if(ui::button(crumbs[i].first.c_str(),ui::Icon::None,ui::Appearance::Quiet,ImVec2(width,28*s),crumbs[i].second.c_str()))navigateToDirectory(panel,crumbs[i].second);
            ImGui::PopID();
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-32*s));
        if(ui::button("##EditPath",ui::Icon::File,ui::Appearance::Quiet,ImVec2(28*s,28*s),"Edit folder path (Ctrl+L)")){panel.editingPath=true;panel.pathFocusRequested=true;strcpy_s(panel.pathInput,panel.currentPath.c_str());}
    }
    ImGui::EndChild();
}

float App::transferDockHeight() {
    const float s=ui::scale();
    std::lock_guard<std::mutex> lock(m_batchMutex);
    if(m_batchQueue.empty()||!m_prefs.transferDockExpanded) return 38*s;
    bool conflict=std::any_of(m_batchQueue.begin(),m_batchQueue.end(),[](const auto& batch){return batch->waitingConflict.load();});
    return ((m_showTransferChannels?216.0f:132.0f)+(conflict?20.0f:0.0f))*s;
}

void App::renderTransferDock(float height) {
    const float s=ui::scale();
    std::vector<std::shared_ptr<TransferBatch>> batches;
    { std::lock_guard<std::mutex> lock(m_batchMutex);batches.assign(m_batchQueue.begin(),m_batchQueue.end()); }
    std::shared_ptr<TransferBatch> batch;
    int queued=0;
    for(auto& b:batches) {
        if(b->state==BatchState::Queued)++queued;
        if(!batch&&!finished(b->state.load()))batch=b;
    }
    if(!batch&&!batches.empty())batch=batches.back();
    if(batch&&batch!=m_lastDockBatch.lock()) {
        m_lastDockBatch=batch;
        if(!finished(batch->state.load()))m_prefs.transferDockExpanded=true;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(18*s,5*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::BeginChild("##TransferDock",ImVec2(0,height),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar);
    if(ui::button("Transfers",ui::Icon::Transfer)) {m_prefs.transferDockExpanded=!m_prefs.transferDockExpanded;m_prefs.save();}
    ImGui::SameLine();ImGui::AlignTextToFramePadding();
    if(batch)ImGui::TextDisabled("%s%s",batchStateLabel(*batch),queued>0?("  |  "+std::to_string(queued)+" queued").c_str():"");
    else ImGui::TextDisabled("No transfers. Select files and choose Copy.");
    float controlsWidth=batch?ImGui::CalcTextSize("Details").x+135*s:50*s;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-controlsWidth));
    if(batch) {
        if(ui::button("Details",ui::Icon::Info)) {m_transferDetailsBatch=batch;m_overlayVisible=true;}
        ImGui::SameLine(0,4*s);
        if(ui::button("##TransferChannels",ui::Icon::Settings,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Connection speeds and queued transfers")) {
            m_showTransferChannels=!m_showTransferChannels;m_prefs.transferDockExpanded=true;
        }
        ImGui::SameLine(0,4*s);
    }
    if(ui::button("##CollapseTransfers",m_prefs.transferDockExpanded?ui::Icon::Down:ui::Icon::Up,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Expand or collapse transfer queue")) {m_prefs.transferDockExpanded=!m_prefs.transferDockExpanded;m_prefs.save();}
    if(batch&&m_prefs.transferDockExpanded) {
        ImGui::Separator();
        bool done=finished(batch->state.load());
        if(ImGui::BeginTable("##DockJob",3,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Route",ImGuiTableColumnFlags_WidthStretch,0.50f);
            ImGui::TableSetupColumn("Progress",ImGuiTableColumnFlags_WidthStretch,0.34f);
            ImGui::TableSetupColumn("Actions",ImGuiTableColumnFlags_WidthFixed,142*s);
            ImGui::TableNextRow();ImGui::TableNextColumn();
            std::string source=batch->isLocalCopy||(!batch->isPull&&!batch->isCrossDevice)?"This PC":deviceDisplayName(batch->srcDeviceSlot);
            std::string destination=batch->isLocalCopy||batch->isPull?"This PC":deviceDisplayName(batch->dstDeviceSlot);
            ui::textClipped(source+"  >  "+destination,ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));
            if(!batch->files.empty())ui::textClipped(parentPath(batch->files.front().sourcePath)+"  >  "+parentPath(batch->files.front().destPath),ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TableNextColumn();
            bool running=batch->state==BatchState::Running&&!batch->disconnected;
            ImGui::TextUnformatted(running?formatSpeed(batch->speedBytesPerSec.load()).c_str():batchStateLabel(*batch));
            if(running&&batch->etaSeconds>=0) {ImGui::SameLine();ImGui::TextDisabled("%s left",formatETA(batch->etaSeconds.load()).c_str());}
            float fraction=batch->state==BatchState::Verifying?batch->crcProgress.load():batch->totalProgress.load();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::ProgressBar(std::clamp(fraction,0.0f,1.0f),ImVec2(-1,5*s),"");ImGui::PopStyleColor();
            ImGui::TextDisabled("%s of %s",formatSize(batch->totalTransferred.load()).c_str(),formatSize(batch->totalBytes.load()).c_str());
            ImGui::TableNextColumn();
            if(done) {
                if(ui::button("View log",ui::Icon::File,ui::Appearance::Secondary)) {
                    strcpy_s(m_debugTagFilter,("#"+std::to_string(batch->logId)).c_str());
                    m_debugLevelFilter=0;m_showDebugWindow=true;
                }
                ImGui::SameLine(0,3*s);
                if(ui::button("##DismissDockJob",ui::Icon::Close,ui::Appearance::Quiet,ImVec2(30*s,32*s),"Dismiss completed transfer")) {
                    std::lock_guard<std::mutex> lock(m_batchMutex);
                    std::erase(m_batchQueue,batch);
                }
            } else if(batch->disconnected) {
                if(ui::button("Retry now",ui::Icon::Refresh,ui::Appearance::Secondary)) {batch->logEvent(LogLevel::Info,"Retry requested");++batch->retryGeneration;batch->userRetryRequested=true;m_batchCV.notify_all();}
            } else {
                bool paused=batch->state==BatchState::Paused||batch->pauseRequested;
                ImGui::BeginDisabled(done||batch->state==BatchState::Verifying||batch->waitingConflict);
                if(ui::button(paused?"Resume":"Pause",paused?ui::Icon::Play:ui::Icon::Pause,ui::Appearance::Secondary)) {batch->logEvent(LogLevel::Info,paused?"Resume requested":"Pause requested");batch->pauseRequested=!paused;m_batchCV.notify_all();}
                ImGui::EndDisabled();
            }
            if(!done) {
                ImGui::SameLine(0,3*s);
                if(ui::button("##CancelDockJob",ui::Icon::Close,ui::Appearance::Quiet,ImVec2(30*s,32*s),"Cancel this transfer")) ImGui::OpenPopup("##CancelDockTransfer");
            }
            if(ImGui::BeginPopup("##CancelDockTransfer")) {
                ImGui::TextUnformatted("Cancel this transfer?");
                if(ui::button("Cancel transfer",ui::Icon::Close,ui::Appearance::Secondary)) {batch->logEvent(LogLevel::Info,"Cancel requested");batch->stopRequested=true;batch->userRetryRequested=false;m_batchCV.notify_all();ImGui::CloseCurrentPopup();}
                if(ui::button("Keep copying"))ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }
        if(batch->waitingConflict) {
            if(ui::button("A destination file already exists. Review options",ui::Icon::Info,ui::Appearance::Selected)) {m_transferDetailsBatch=batch;m_overlayVisible=true;}
        } else if(done&&batch->state==BatchState::Failed) ImGui::TextDisabled("Open Details to review transfer errors.");
        else ImGui::TextDisabled("%d items  |  %s",batch->totalFiles(),batch->isLocalCopy?"Local file transfer":m_prefs.enableCrcVerification?"File checking enabled":"File checking disabled");
        if(m_showTransferChannels) {
            ImGui::Separator();
            ImGui::BeginChild("##DockDetails",ImVec2(0,0),ImGuiChildFlags_None);
            if(batch->useParallelChannels||batch->useMultiNic) {
                for(int channel=0;channel<std::min(batch->numChannels,TransferBatch::MAX_CHANNELS);++channel) {
                    auto& c=batch->channels[channel];
                    ImGui::TextDisabled("%s: %s",c.channelName.c_str(),formatSpeed(done||batch->disconnected||batch->state==BatchState::Paused?0:c.speed.load()).c_str());
                }
            }
            for(auto& queuedBatch:batches) {
                if(queuedBatch==batch)continue;
                ImGui::PushID(queuedBatch.get());
                std::string label=std::string(batchStateLabel(*queuedBatch))+": "+std::to_string(queuedBatch->totalFiles())+" files";
                if(ImGui::Selectable(label.c_str())) {m_transferDetailsBatch=queuedBatch;m_overlayVisible=true;}
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();ImGui::PopStyleVar(2);
}

void App::renderWorkspaceDrawer() {
    if(!m_workspaceDrawer)return;
    const float s=ui::scale();
    auto* viewport=ImGui::GetMainViewport();
    float width=std::min(400*s,viewport->WorkSize.x-24*s);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x-width-8*s,viewport->WorkPos.y+58*s));
    ImGui::SetNextWindowSize(ImVec2(width,std::max(200*s,viewport->WorkSize.y-96*s)));
    const char* title=m_workspaceDrawer==1?"File details":m_workspaceDrawer==2?"Device details":"Workspace settings";
    bool open=true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*s,18*s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    if(ImGui::Begin("##WorkspaceDrawer",&open,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoTitleBar)) {
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(title);
        ImGui::SameLine(ImGui::GetWindowWidth()-54*s);
        if(ui::button("##CloseDrawer",ui::Icon::Close,ui::Appearance::Quiet,ImVec2(30*s,30*s),"Close details"))m_workspaceDrawer=0;
        ImGui::Separator();ImGui::Spacing();
        if(m_workspaceDrawer==3) {
            const char* modes[]={"Follow Windows","Dark","Light"};
            ImGui::TextUnformatted("Appearance");ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##WorkspaceTheme",&m_theme.mode,modes,3)){m_theme.customColors=false;m_pendingScale=m_theme.userScale;m_theme.save();}
            ImGui::Spacing();ImGui::TextUnformatted("File rows");
            int density=m_prefs.compactRows?1:0;const char* densities[]={"Comfortable","Compact"};ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##WorkspaceDensity",&density,densities,2)){m_prefs.compactRows=density==1;m_prefs.save();}
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            if(ui::button("Customize appearance",ui::Icon::Settings,ui::Appearance::Secondary))m_showThemeWindow=true;
            if(ui::button("Preferences",ui::Icon::Settings,ui::Appearance::Secondary))m_showPreferences=true;
            if(ui::button("Activity log",ui::Icon::File,ui::Appearance::Secondary)){m_showDebugWindow=true;m_workspaceDrawer=0;}
            ImGui::Spacing();ImGui::TextDisabled("Keyboard shortcuts");
            ImGui::TextUnformatted("Ctrl+A    Select all\nF2           Rename\nF5           Refresh folders\nDelete    Delete selection\nCtrl+L    Edit folder path\nEscape  Close details or clear selection");
        } else if(m_workspaceDrawer==2) {
            int slot=m_detailsDeviceSlot&1;
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##DetailsDevice",deviceDisplayName(slot).c_str())) {
                for(int i=0;i<2;++i)if(!m_slotSerial[i].empty()) {
                    if(ImGui::Selectable((deviceDisplayName(i)+"##"+std::to_string(i)).c_str(),i==slot))m_detailsDeviceSlot=i;
                }
                ImGui::EndCombo();
            }
            bool connected=m_slotConnected[slot]&&m_deviceSlots[slot].isServerRunning();
            ImGui::TextDisabled("%s",connected?"Connected":"Disconnected");
            ImGui::Spacing();ImGui::TextDisabled("Connection");
            ImGui::TextWrapped("%s",m_deviceSlots[slot].isDirectConnection()?"Direct connection":"ADB connection");
            if(slot==0&&m_dualChannelAvailable)ImGui::TextDisabled("Additional connection: %s",m_secondaryChannelType.c_str());
            auto& prefs=m_prefs.pipePreferencesFor(m_slotSerial[slot]);
            ImGui::Spacing();
            if(ImGui::CollapsingHeader("Advanced connection settings")) {
                ImGui::TextWrapped("Requested streams apply when transfer channels are opened.");
                ImGui::TextUnformatted("USB streams");
                ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##USBStreams",&prefs.usbPipeCount,1,4)){m_prefs.save();}
                ImGui::TextUnformatted("WiFi streams");
                ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##WiFiStreams",&prefs.wifiPipeCount,1,4)){m_prefs.save();}
                if(ui::button("Network adapters",ui::Icon::Wifi))m_showNicConfig=true;
                if(ui::button("Root, keep awake and ADB",ui::Icon::Settings))m_showDeviceSettings=true;
            }
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();ImGui::TextDisabled("Windows integration");
            auto& mount=DeviceMountManager::instance(slot);
            ImGui::BeginDisabled(!connected||m_asyncBusy);
            if(mount.isMounted()) {
                if(ui::button("Open mounted drive",ui::Icon::Computer,ui::Appearance::Secondary)) {
                    auto path=workspaceWide(mount.mountPoint());ShellExecuteW(nullptr,L"explore",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                }
                if(ui::button("Unmount drive"))postAsync("Unmounting drive...",[slot](){DeviceMountManager::instance(slot).unmount();});
            } else if(ui::button("Mount as drive",ui::Icon::Computer,ui::Appearance::Secondary)) {
                std::string root=m_slotStorageRoot[slot],drive=slot==0?"P:\\":"Q:\\";
                postAsync("Mounting drive...",[this,slot,root,drive](){
                    bool ok=DeviceMountManager::instance(slot).mount(&m_deviceSlots[slot],root,drive);
                    m_statusMessage=ok?"Mounted as "+drive:"Unable to mount the drive. Check the Dokan installation.";
                    m_statusTime=std::chrono::steady_clock::now();
                });
            }
            ImGui::EndDisabled();
            ImGui::Spacing();if(ui::button("Pair over WiFi",ui::Icon::Wifi))m_showWifiPairing=true;
            ImGui::BeginDisabled(!connected);
            if(ui::button("WiFi setup wizard",ui::Icon::Wifi)){m_wizardSerial=m_slotSerial[slot];m_wizardStep=0;m_showWifiWizard=true;}
            ImGui::EndDisabled();
        } else if(m_workspaceDrawer==1) {
            FilePanel* panel=m_lastFocusedPanel?m_lastFocusedPanel:&m_rightPanel;
            ImGui::TextWrapped("%s",panelDisplayName(*panel).c_str());
            ImGui::TextDisabled("Location");ImGui::TextWrapped("%s",panel->currentPath.c_str());ImGui::Spacing();
            if(panel->selectedIndices.empty())ImGui::TextDisabled("Select a file to see its details.");
            else {
                ImGui::Text("%d selected",(int)panel->selectedIndices.size());
                ImGui::TextDisabled("Total file size: %s",formatSize(panel->selectedFileSize()).c_str());
                if(panel->selectedIndices.size()==1) {
                    int index=*panel->selectedIndices.begin();
                    if(panel->validIndex(index)) {
                        std::string name=panel->entryName(index);
                        ImGui::Spacing();ImGui::TextWrapped("%s",name.c_str());
                        ImGui::TextDisabled("%s",panel->entryDate(index).c_str());
                        bool mcraw=name.size()>6&&_stricmp(name.c_str()+name.size()-6,".mcraw")==0;
                        ImGui::BeginDisabled(!panelAvailable(*panel));
                        if(mcraw&&ui::button("Browse frames",ui::Icon::Folder,ui::Appearance::Secondary)) {
                            std::string path=panel->currentPath;char sep=panel->isAndroid?'/':'\\';if(!path.empty()&&path.back()!=sep)path+=sep;path+=name;
                            if(panel->isAndroid)navigateToDirectory(*panel,path);
                            else postAsync("Opening MotionCam frames...",[this,panel,path](){
                                std::string mounted=McrawMountManager::instance().mountMcraw(path);
                                if(!mounted.empty()){panel->insideMcraw=true;panel->mcrawFilePath=path;navigateToDirectory(*panel,mounted);}
                            });
                            m_workspaceDrawer=0;
                        }
                        if(ui::button("Rename",ui::Icon::File)){m_contextPanel=panel;m_contextIndex=index;strcpy_s(m_renameBuf,name.c_str());m_showRenamePopup=true;}
                        ImGui::EndDisabled();
                    }
                }
            }
        }
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::IsKeyPressed(ImGuiKey_Escape))m_workspaceDrawer=0;
    }
    ImGui::End();ImGui::PopStyleColor();ImGui::PopStyleVar();
    if(!open)m_workspaceDrawer=0;
}
