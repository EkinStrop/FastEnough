#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>

namespace ui {
enum class Icon { None, Folder, File, Image, Video, Music, Archive, Apps, Phone, Computer,
    Back, Forward, Up, Down, Refresh, Search, Star, Copy, Move, Trash, Plus, Compare,
    Info, Settings, Transfer, Pause, Play, Close, More, Check, Usb, Wifi };
enum class Appearance { Quiet, Secondary, Primary, Selected };

inline ImFont* semiboldFont=nullptr;

inline float scale() { return ImGui::GetFontSize() / 14.0f; }

template<typename T>
inline std::optional<T> endDropTargetChild(const char* payloadType, bool enabled) {
    static_assert(std::is_trivially_copyable_v<T>);
    // EndChild registers the whole pane, including its nested table, as the last item.
    ImGui::EndChild();
    std::optional<T> value;
    if (enabled && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType)) {
            if (payload->DataSize == sizeof(T)) {
                // Delivery clears the payload when the target ends.
                value.emplace();
                std::memcpy(&*value, payload->Data, sizeof(T));
            }
        }
        ImGui::EndDragDropTarget();
    }
    return value;
}

inline void icon(Icon type, ImVec2 position, float size, ImU32 color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float unit = size / 24.0f;
    const float stroke = std::max(1.0f, size / 13.0f);
    auto point = [&](float x, float y) { return ImVec2(position.x + x * unit, position.y + y * unit); };
    auto path = [&](std::initializer_list<ImVec2> points, bool closed = false) {
        for (auto p : points) draw->PathLineTo(point(p.x, p.y));
        draw->PathStroke(color, closed ? ImDrawFlags_Closed : 0, stroke);
    };
    auto rect = [&](float x, float y, float w, float h, float radius = 2.0f) {
        draw->AddRect(point(x, y), point(x + w, y + h), color, radius * unit, 0, stroke);
    };
    auto circle = [&](float x, float y, float r) { draw->AddCircle(point(x, y), r * unit, color, 16, stroke); };
    switch (type) {
    case Icon::Folder: path({{3,7},{3,4},{9,4},{12,7},{21,7},{21,20},{3,20}}, true); path({{3,10},{21,10}}); break;
    case Icon::File: case Icon::Archive:
        path({{13,3},{5,3},{5,21},{19,21},{19,9},{13,3},{13,9},{19,9}});
        if (type == Icon::Archive) path({{11,11},{13,11},{11,14},{13,14},{11,17},{13,17}});
        else { path({{8,13},{16,13}}); path({{8,17},{13,17}}); } break;
    case Icon::Image: rect(3,3,18,18); circle(8,8,1.5f); path({{3,17},{9,11},{13,15},{16,12},{21,17}}); break;
    case Icon::Video: rect(3,3,18,18); path({{10,8},{16,12},{10,16}},true); break;
    case Icon::Music: path({{9,17},{9,5},{20,3},{20,15}}); circle(6,18,3); circle(17,16,3); break;
    case Icon::Apps: rect(3,3,7,7); rect(14,3,7,7); rect(3,14,7,7); rect(14,14,7,7); break;
    case Icon::Phone: rect(6,2,12,20,3); path({{10,5},{14,5}}); path({{11,18},{13,18}}); break;
    case Icon::Computer: rect(3,4,18,13); path({{12,17},{12,21},{8,21},{16,21}}); break;
    case Icon::Back: path({{14,6},{8,12},{14,18}}); break;
    case Icon::Forward: path({{10,6},{16,12},{10,18}}); break;
    case Icon::Up: path({{6,14},{12,8},{18,14}}); break;
    case Icon::Down: path({{6,10},{12,16},{18,10}}); break;
    case Icon::Refresh: draw->PathArcTo(point(12,12),8*unit,0.65f,5.6f,20); draw->PathStroke(color,0,stroke); path({{20,3},{20,8},{15,8}}); break;
    case Icon::Search: circle(10.5f,10.5f,6.5f); path({{16,16},{21,21}}); break;
    case Icon::Star: path({{12,3},{15,9},{21,10},{17,14},{18,21},{12,18},{6,21},{7,14},{3,10},{9,9}},true); break;
    case Icon::Copy: rect(8,8,12,13); path({{16,8},{16,3},{3,3},{3,16},{8,16}}); break;
    case Icon::Move: path({{4,12},{20,12},{15,7},{20,12},{15,17}}); path({{4,5},{4,7}}); path({{4,17},{4,19}}); break;
    case Icon::Trash: path({{3,6},{21,6}}); path({{9,6},{9,3},{15,3},{15,6}}); path({{5,6},{6,21},{18,21},{19,6}}); path({{10,10},{10,17}}); path({{14,10},{14,17}}); break;
    case Icon::Plus: path({{12,5},{12,19}}); path({{5,12},{19,12}}); break;
    case Icon::Compare: path({{12,3},{12,21}}); path({{3,7},{8,7},{5,4},{8,7},{5,10}}); path({{21,17},{16,17},{19,14},{16,17},{19,20}}); break;
    case Icon::Info: circle(12,12,9); path({{12,11},{12,17}}); circle(12,7,0.5f); break;
    case Icon::Settings: path({{5,3},{5,7}}); path({{5,11},{5,21}}); rect(3,7,4,4,1); path({{12,3},{12,14}}); path({{12,18},{12,21}}); rect(10,14,4,4,1); path({{19,3},{19,5}}); path({{19,9},{19,21}}); rect(17,5,4,4,1); break;
    case Icon::Transfer: path({{4,7},{19,7},{15,3},{19,7},{15,11}}); path({{20,17},{5,17},{9,13},{5,17},{9,21}}); break;
    case Icon::Pause: path({{8,5},{8,19}}); path({{16,5},{16,19}}); break;
    case Icon::Play: path({{8,4},{20,12},{8,20}},true); break;
    case Icon::Close: path({{6,6},{18,18}}); path({{6,18},{18,6}}); break;
    case Icon::More: for (float x : {5.0f,12.0f,19.0f}) draw->AddCircleFilled(point(x,12),1.3f*unit,color); break;
    case Icon::Check: path({{5,12},{10,17},{20,7}}); break;
    case Icon::Usb: path({{12,20},{12,3},{9,6},{12,3},{15,6}}); path({{12,16},{6,12},{6,7}}); path({{12,13},{18,9},{18,5}}); circle(12,20,1.5f); circle(6,7,1.5f); rect(16.5f,3.5f,3,3,0); break;
    case Icon::Wifi: for (float r : {5.0f,10.0f,15.0f}) { draw->PathArcTo(point(12,21),r*unit,3.95f,5.47f,16); draw->PathStroke(color,0,stroke); } circle(12,21,0.7f); break;
    default: break;
    }
}

inline void tooltip(const char* text) {
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

inline void drawTextClipped(ImVec2 position, const std::string& text, float width, ImU32 color, float fontSize=0) {
    if (width <= 0) return;
    if (fontSize<=0) fontSize=ImGui::GetFontSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(position, ImVec2(position.x + width, position.y + ImGui::GetTextLineHeight()), true);
    if (ImGui::GetFont()->CalcTextSizeA(fontSize,FLT_MAX,0,text.c_str()).x <= width+1.0f) {
        draw->AddText(ImGui::GetFont(),fontSize,position,color,text.c_str());
    } else {
        const char* end = text.c_str();
        float ellipsisWidth = ImGui::GetFont()->CalcTextSizeA(fontSize,FLT_MAX,0,"...").x;
        ImGui::GetFont()->CalcTextSizeA(fontSize, std::max(1.0f, width - ellipsisWidth), 0,
            text.c_str(), nullptr, &end);
        std::string shortened(text.c_str(), end);
        shortened += "...";
        draw->AddText(ImGui::GetFont(),fontSize,position,color,shortened.c_str());
    }
    draw->PopClipRect();
}

inline bool button(const char* label, Icon glyph = Icon::None,
    Appearance appearance = Appearance::Quiet, ImVec2 size = ImVec2(0,0), const char* hint = nullptr) {
    const float s = scale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* end = strstr(label,"##");
    std::string visible = end ? std::string(label,end) : std::string(label);
    float iconWidth = glyph == Icon::None ? 0.0f : 18.0f*s;
    float gap = iconWidth > 0 && !visible.empty() ? 7.0f*s : 0.0f;
    ImVec2 textSize = ImGui::CalcTextSize(visible.c_str());
    if (size.x == 0) size.x = textSize.x + iconWidth + gap + 20.0f*s;
    if (size.y == 0) size.y = 32.0f*s;
    ImVec4 fill = style.Colors[ImGuiCol_Button];
    ImVec4 text = style.Colors[ImGuiCol_Text];
    if (appearance == Appearance::Quiet) { fill.w=0; text=style.Colors[ImGuiCol_TextDisabled]; }
    if (appearance == Appearance::Selected) fill=style.Colors[ImGuiCol_Header];
    if (appearance == Appearance::Primary) {
        fill=style.Colors[ImGuiCol_CheckMark];
        float brightness=fill.x*0.299f+fill.y*0.587f+fill.z*0.114f;
        text=brightness>0.45f?ImVec4(0.06f,0.10f,0.18f,1):ImVec4(1,1,1,1);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, appearance==Appearance::Secondary?1.0f:0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,fill);
    if (appearance==Appearance::Primary) {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(std::min(fill.x+0.09f,1.0f),std::min(fill.y+0.09f,1.0f),std::min(fill.z+0.09f,1.0f),1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,fill);
    }
    ImGui::PushID(label);
    bool pressed=ImGui::Button("##action",size);
    ImVec2 min=ImGui::GetItemRectMin(), max=ImGui::GetItemRectMax();
    ImDrawList* draw=ImGui::GetWindowDrawList();
    draw->PushClipRect(min,max,true);
    ImU32 ink=ImGui::GetColorU32(text);
    float contentWidth=textSize.x+iconWidth+gap;
    ImVec2 start(min.x+std::max(6.0f*s,(max.x-min.x-contentWidth)*0.5f),min.y);
    if (glyph!=Icon::None) icon(glyph,ImVec2(start.x,min.y+(size.y-iconWidth)*0.5f),iconWidth,ink);
    ImVec2 textPosition(start.x+iconWidth+gap,min.y+(size.y-textSize.y)*0.5f);
    drawTextClipped(textPosition,visible,std::max(0.0f,max.x-6*s-textPosition.x),ink);
    draw->PopClipRect();
    ImGui::PopID();
    ImGui::PopStyleColor(appearance==Appearance::Primary?3:1);
    ImGui::PopStyleVar();
    tooltip(hint);
    return pressed;
}

inline void textClipped(const std::string& text, float width, ImU32 color, float height = 0) {
    ImVec2 pos=ImGui::GetCursorScreenPos();
    if(height<=0)height=ImGui::GetTextLineHeight();
    drawTextClipped(ImVec2(pos.x,pos.y+(height-ImGui::GetTextLineHeight())*0.5f),text,width,color);
    ImGui::Dummy(ImVec2(std::max(1.0f,width),height));
    tooltip(text.c_str());
}
}
