#pragma once
#include "ui_widgets.h"

namespace ui {
inline ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

struct PanelStyle {
    float s = ui::scale();
    ImVec4 background, surface, raised, border, text, muted, blue, green, red, gold;

    explicit PanelStyle(bool light) {
        background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        surface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
        raised = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        border = light ? rgb(174, 197, 221) : rgb(45, 61, 82);
        text = light ? rgb(26, 45, 68) : rgb(244, 247, 252);
        muted = light ? rgb(74, 98, 124) : rgb(180, 195, 215);
        blue = light ? rgb(0, 111, 188) : rgb(71, 208, 255);
        green = light ? rgb(0, 127, 87) : rgb(39, 234, 161);
        red = light ? rgb(184, 52, 64) : rgb(255, 133, 143);
        gold = light ? rgb(145, 106, 10) : rgb(233, 194, 48);
    }

    ImU32 color(ImVec4 value) const { return ImGui::GetColorU32(value); }

    void box(ImVec2 p, ImVec2 size, ImVec4 top, ImVec4 bottom, ImVec4 edge, float radius = 9) const {
        auto* draw = ImGui::GetWindowDrawList();
        int first = draw->VtxBuffer.Size;
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), color(top), radius * s);
        for (int i = first; i < draw->VtxBuffer.Size; ++i) {
            auto& vertex = draw->VtxBuffer[i];
            float t = std::clamp((vertex.pos.y - p.y) / std::max(1.0f, size.y), 0.0f, 1.0f);
            ImVec4 shade(top.x + (bottom.x - top.x) * t, top.y + (bottom.y - top.y) * t,
                top.z + (bottom.z - top.z) * t, top.w + (bottom.w - top.w) * t);
            ImU32 alpha = vertex.col & IM_COL32_A_MASK;
            vertex.col = (color(shade) & ~IM_COL32_A_MASK) | alpha;
        }
        draw->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), color(edge), radius * s, 0, s);
    }

    void label(ImVec2 p, const std::string& value, float width, ImVec4 ink,
        float size = 14, bool bold = false) const {
        if (width <= 0) return;
        ImFont* font = bold && ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
        if (size >= 24 && ui::connectionHeadingFont) font = ui::connectionHeadingFont;
        float fontSize = size * s;
        std::string visible = value;
        if (font->CalcTextSizeA(fontSize, FLT_MAX, 0, value.c_str()).x > width) {
            const char* end = value.c_str();
            float dots = font->CalcTextSizeA(fontSize, FLT_MAX, 0, "...").x;
            font->CalcTextSizeA(fontSize, std::max(1.0f, width - dots), 0, value.c_str(), nullptr, &end);
            visible.assign(value.c_str(), end);
            visible += "...";
        }
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(p, ImVec2(p.x + width, p.y + fontSize + 3 * s), true);
        draw->AddText(font, fontSize, p, color(ink), visible.c_str());
        draw->PopClipRect();
    }

    void tile(ui::Icon glyph, ImVec2 p, float size = 40) const {
        box(p, ImVec2(size * s, size * s), rgb(27, 57, 90), rgb(16, 36, 59), size >= 56 ? rgb(41, 115, 176) : border, 9);
        ui::icon(glyph, ImVec2(p.x + size * .22f * s, p.y + size * .22f * s), size * .56f * s,
            ImGui::GetColorU32(glyph == ui::Icon::Phone ? rgb(225, 245, 255) : rgb(103, 218, 255)));
    }

    void heading(ImVec2 p, float width, ui::Icon glyph, const char* title, const char* subtitle) const {
        tile(glyph, p);
        label(ImVec2(p.x + 58 * s, p.y + 1 * s), title, width - 58 * s, text, 17, true);
        label(ImVec2(p.x + 58 * s, p.y + 26 * s), subtitle, width - 58 * s, muted, 13);
    }

    bool button(const char* id, const char* title, ui::Icon glyph, ImVec2 p, ImVec2 size,
        ImVec4 ink, int treatment = 0) const {
        ImGui::SetCursorScreenPos(p);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        bool pressed = ImGui::Button(id, size);
        ImGui::PopStyleColor(3);
        bool hovered = ImGui::IsItemHovered();
        ImVec4 top = raised, bottom = surface, edge = border;
        if (treatment == 1) { top = rgb(29, 163, 250); bottom = rgb(18, 94, 215); edge = rgb(71, 190, 255); }
        if (treatment == 2) { top = rgb(148, 122, 24); bottom = rgb(104, 85, 20); edge = gold; }
        if (treatment == 3) { top = rgb(75, 53, 67); bottom = rgb(54, 46, 61); edge = rgb(140, 80, 95); }
        if (treatment == 4) { top = rgb(31, 82, 133); bottom = rgb(26, 60, 100); edge = rgb(49, 132, 211); }
        if (hovered || ImGui::IsItemFocused()) {
            top.x = std::min(1.0f, top.x + .06f);
            top.y = std::min(1.0f, top.y + .06f);
            top.z = std::min(1.0f, top.z + .06f);
            edge = ink;
        }
        if (ImGui::IsItemActive()) std::swap(top, bottom);
        box(p, size, top, bottom, edge, 7);
        ImFont* font = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
        float textWidth = font->CalcTextSizeA(14 * s, FLT_MAX, 0, title).x;
        float iconWidth = glyph == ui::Icon::None ? 0 : 19 * s;
        float gap = iconWidth && *title ? 8 * s : 0;
        float x = p.x + (*title ? std::max(8 * s, (size.x - textWidth - iconWidth - gap) * .5f) : (size.x - iconWidth) * .5f);
        if (iconWidth) ui::icon(glyph, ImVec2(x, p.y + (size.y - iconWidth) * .5f), iconWidth, color(ink));
        label(ImVec2(x + iconWidth + gap, p.y + (size.y - 14 * s) * .5f - s), title,
            size.x - (x - p.x) - iconWidth - gap - 6 * s, ink, 14, true);
        return pressed;
    }

    bool search(const char* id, const char* hint, char* value, size_t capacity, ImVec2 p, float width) const {
        box(p, ImVec2(width, 36 * s), raised, surface, border, 7);
        ui::icon(ui::Icon::Search, ImVec2(p.x + 11 * s, p.y + 9.5f * s), 17 * s, color(muted));
        ImGui::SetCursorScreenPos(ImVec2(p.x + 32 * s, p.y + (36 * s - ImGui::GetFrameHeight()) * .5f));
        ImGui::SetNextItemWidth(std::max(1.0f, width - 36 * s));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        bool changed = ImGui::InputTextWithHint(id, hint, value, capacity);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return changed;
    }

    void status(ImVec2 p, const std::string& value, bool available, float width) const {
        auto ink = available ? green : muted;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5 * s, p.y + 9 * s), 4.5f * s, color(ink));
        label(ImVec2(p.x + 20 * s, p.y), value, width - 20 * s, ink, 13, true);
    }
};
}
