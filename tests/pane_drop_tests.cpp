#include "ui_widgets.h"
#include <array>
#include <iostream>
#include <stdexcept>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct PaneDropFixture {
    int sourcePane;
    bool enabled;
    const char* payloadType;
    int deliveries=0;
    int deliveredSource=-1;
    std::array<ImVec2,2> rows{}, backgrounds{}, headers{}, footers{};

    PaneDropFixture(int source, bool accept=true, const char* type="FILE_DRAG")
        : sourcePane(source), enabled(accept), payloadType(type) {
        ImGui::CreateContext();
        auto& io=ImGui::GetIO();
        io.IniFilename=nullptr;
        io.DisplaySize=ImVec2(720,460);
        io.DeltaTime=1.0f/60;
        unsigned char* pixels;
        int width,height;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        frame();
        frame();
    }
    ~PaneDropFixture() { ImGui::DestroyContext(); }

    void frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(720,460));
        ImGui::Begin("Workspace",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove);
        for (int pane=0;pane<2;++pane) {
            ImGui::SetCursorPos(ImVec2(10+350.0f*pane,10));
            ImGui::PushID(pane);
            ImGui::BeginChild("Pane",ImVec2(340,420),ImGuiChildFlags_Borders);
            ImGui::Button("Location",ImVec2(200,30));
            headers[pane]=center();
            if (ImGui::BeginTable("Files",1,ImGuiTableFlags_ScrollY,ImVec2(0,300))) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Selectable("File.txt",false,ImGuiSelectableFlags_SpanAllColumns);
                rows[pane]=center();
                if (pane==sourcePane && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(payloadType,&pane,sizeof(pane));
                    ImGui::TextUnformatted("Copy File.txt");
                    ImGui::EndDragDropSource();
                }
                auto position=ImGui::GetWindowPos();
                backgrounds[pane]=ImVec2(position.x+100,position.y+200);
                ImGui::EndTable();
            }
            ImGui::TextUnformatted("1 item");
            footers[pane]=center();
            if (auto payload=ui::endDropTargetChild<int>("FILE_DRAG",enabled && pane!=sourcePane)) {
                ++deliveries;
                deliveredSource=*payload;
            }
            ImGui::PopID();
        }
        ImGui::End();
        ImGui::Render();
    }
    static ImVec2 center() {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        return ImVec2((a.x+b.x)*0.5f,(a.y+b.y)*0.5f);
    }
    void dragTo(ImVec2 destination) {
        auto& io=ImGui::GetIO();
        io.AddMousePosEvent(rows[sourcePane].x,rows[sourcePane].y);
        frame();
        io.AddMouseButtonEvent(0,true);
        frame();
        io.AddMousePosEvent(rows[sourcePane].x+25,rows[sourcePane].y+8);
        frame();
        require(ImGui::GetDragDropPayload()!=nullptr,"Drag did not start from a file row");
        io.AddMousePosEvent(destination.x,destination.y);
        frame();
        frame();
        require(deliveries==0,"Copy started before the mouse was released");
        io.AddMouseButtonEvent(0,false);
        frame();
        frame();
    }
};

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    try {
        for (int source:{0,1}) for (int area=0;area<4;++area) {
            PaneDropFixture fixture(source);
            int target=1-source;
            ImVec2 destination=area==0?fixture.backgrounds[target]:area==1?fixture.rows[target]:
                area==2?fixture.headers[target]:fixture.footers[target];
            fixture.dragTo(destination);
            require(fixture.deliveries==1,"The destination pane did not accept exactly one drop");
            require(fixture.deliveredSource==source,"Drop lost its source pane");
        }
        {
            PaneDropFixture fixture(0,false);
            fixture.dragTo(fixture.backgrounds[1]);
            require(fixture.deliveries==0,"Unavailable pane accepted a drop");
        }
        {
            PaneDropFixture fixture(0,true,"OTHER_PAYLOAD");
            fixture.dragTo(fixture.backgrounds[1]);
            require(fixture.deliveries==0,"Unrelated payload was treated as a file");
        }
        {
            PaneDropFixture fixture(0);
            fixture.dragTo(fixture.backgrounds[0]);
            require(fixture.deliveries==0,"Source pane accepted its own drop");
        }
        std::cout<<"Pane drag-and-drop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
