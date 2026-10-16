#pragma once

#include <imgui.h>

#include <algorithm>
#include <string>

namespace rtc_console {
namespace ui {

inline const char* SettingLabel(const std::string& key) {
  static const struct { const char* key; const char* label; } labels[] = {
      {"LAN", "局域网模式"}, {"api_server", "API 服务器"},
      {"signaling_server", "信令服务器"}, {"ice_servers", "ICE 服务器"},
      {"urls", "服务器地址"}, {"username", "用户名"}, {"credential", "凭据"},
      {"log_path", "日志目录"}, {"resolution_limit", "分辨率限制"},
      {"stereo", "双目画面"}, {"surround_grid", "环视拼接画面"},
      {"surround_front", "前视画面"}, {"surround_rear", "后视画面"},
      {"surround_left", "左视画面"}, {"surround_right", "右视画面"},
      {"vision_processing", "视觉处理"}, {"downscale", "降采样比例"},
      {"encode_params", "编码参数"}, {"use_codec_pool", "启用编解码器池"},
      {"codecs", "编码档位"}, {"qp_range", "QP 范围"},
      {"qp_threshold", "QP 阈值"}, {"I_frame_interval", "关键帧间隔"},
      {"bitrate_mode", "码率模式"}, {"bitrate_minmum", "最低码率（bps）"},
      {"bitrate_start", "起始码率（bps）"}, {"bitrate_maxmum", "最高码率（bps）"},
      {"playout_delay_min_ms", "最小播放延迟（ms）"},
      {"playout_delay_max_ms", "最大播放延迟（ms）"},
      {"netstats_report", "启用网络统计"}, {"use_NVENC", "启用 NVENC 编码"},
      {"use_NVDEC", "启用 NVDEC 解码"}, {"jetson_h264_encoder", "Jetson H.264 编码器"},
      {"ping_timeout", "Ping 超时（ms）"}, {"pong_timeout", "Pong 超时（ms）"},
      {"reconnect_interval", "重连间隔（ms）"}};
  for (const auto& entry : labels) {
    if (key == entry.key) return entry.label;
  }
  return key.c_str();
}

inline const char* ActionLabel(const std::string& key) {
  static const struct { const char* key; const char* label; } labels[] = {
      {"toggle_interleave", "切换像素交错显示"}, {"toggle_view", "切换双目与环视"},
      {"select_1", "选择设备／画面 1"}, {"select_2", "选择设备／画面 2"},
      {"select_3", "选择设备／画面 3"}, {"select_4", "选择设备／画面 4"},
      {"takeover", "接管设备"}, {"release_takeover", "停止并退出接管"},
      {"brake_takeover", "制动接管／按住制动"}, {"stop_and_release", "急停并退出接管"},
      {"gear_forward", "请求前进档"}, {"gear_neutral", "请求空档"},
      {"gear_reverse", "请求倒档"}, {"throttle_up", "增加油门设定"},
      {"throttle_down", "减小油门设定"}, {"forward", "前进"}, {"reverse", "后退"},
      {"left", "左转"}, {"right", "右转"}, {"emergency_stop", "按住紧急停止"},
      {"escape", "返回双目／退出全屏"}};
  for (const auto& entry : labels) {
    if (key == entry.key) return entry.label;
  }
  return key.c_str();
}

inline ImVec4 Accent() { return ImVec4(0.24f, 0.78f, 0.68f, 1.0f); }
inline ImVec4 Muted() { return ImVec4(0.53f, 0.60f, 0.67f, 1.0f); }
inline ImVec4 Surface() { return ImVec4(0.075f, 0.098f, 0.125f, 1.0f); }
inline ImVec4 Background() { return ImVec4(0.045f, 0.063f, 0.086f, 1.0f); }
inline ImVec4 Warning() { return ImVec4(0.96f, 0.72f, 0.38f, 1.0f); }

inline void ApplyStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(20, 18);
  style.FramePadding = ImVec2(12, 7);
  style.ItemSpacing = ImVec2(12, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.CellPadding = ImVec2(10, 8);
  style.WindowRounding = 0;
  style.ChildRounding = 0;
  style.FrameRounding = 0;
  style.PopupRounding = 0;
  style.TabRounding = 0;
  style.ScrollbarRounding = 0;
  style.GrabRounding = 0;
  style.ImageRounding = 0;
  style.TreeLinesRounding = 0;
  style.DragDropTargetRounding = 0;
  style.WindowBorderSize = 0;
  style.ChildBorderSize = 1;
  style.FrameBorderSize = 1;
  style.ScrollbarSize = 10;
  style.GrabMinSize = 12;
  style.Colors[ImGuiCol_WindowBg] = Background();
  style.Colors[ImGuiCol_ChildBg] = Surface();
  style.Colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.12f, 0.15f, 1);
  style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.93f, 0.96f, 1);
  style.Colors[ImGuiCol_TextDisabled] = Muted();
  style.Colors[ImGuiCol_Border] = ImVec4(0.19f, 0.25f, 0.31f, 0.65f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.135f, 0.17f, 1);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.19f, 0.23f, 1);
  style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.23f, 0.27f, 1);
  style.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.18f, 0.22f, 1);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.26f, 0.30f, 1);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.35f, 0.32f, 1);
  style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.25f, 0.24f, 1);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.13f, 0.30f, 0.29f, 1);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.36f, 0.32f, 1);
  style.Colors[ImGuiCol_CheckMark] = Accent();
  style.Colors[ImGuiCol_SliderGrab] = Accent();
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.87f, 0.75f, 1);
  style.Colors[ImGuiCol_Separator] = style.Colors[ImGuiCol_Border];
  style.Colors[ImGuiCol_Tab] = Surface();
  style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_HeaderHovered];
  style.Colors[ImGuiCol_TabSelected] = style.Colors[ImGuiCol_Header];
  style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.11f, 0.15f, 0.19f, 1);
  style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
  style.Colors[ImGuiCol_NavCursor] = Accent();
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.24f, 0.78f, 0.68f, 0.25f);
}

inline void Heading(const char* title, const char* description = nullptr) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.35f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (description && *description) ImGui::TextDisabled("%s", description);
  ImGui::Spacing();
}

inline void Section(const char* title) {
  ImGui::TextUnformatted(title);
  ImGui::Separator();
  ImGui::Spacing();
}

inline void Badge(const char* label, const ImVec4& color) {
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 text = ImGui::CalcTextSize(label);
  const ImVec2 size(text.x + 30, text.y + 12);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
      ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.10f)));
  draw->AddRectFilled(ImVec2(start.x + 8, start.y + size.y * 0.5f - 3),
      ImVec2(start.x + 14, start.y + size.y * 0.5f + 3),
      ImGui::ColorConvertFloat4ToU32(color));
  draw->AddText(ImVec2(start.x + 21, start.y + 6),
      ImGui::ColorConvertFloat4ToU32(color), label);
  ImGui::Dummy(size);
}

inline void Icon(ImDrawList* draw, ImVec2 origin, int page, ImU32 color) {
  const auto point = [origin](float x, float y) {
    return ImVec2(origin.x + x, origin.y + y);
  };
  if (page == 0) {
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 2; ++column) {
        draw->AddRect(point(column * 10.0f, row * 10.0f),
            point(column * 10.0f + 6, row * 10.0f + 6), color, 0, 0, 1.5f);
      }
    }
  } else if (page == 1) {
    draw->AddRect(point(0, 0), point(16, 11), color, 0, 0, 1.5f);
    draw->AddLine(point(8, 11), point(8, 16), color, 1.5f);
    draw->AddLine(point(3, 16), point(13, 16), color, 1.5f);
  } else if (page == 2) {
    draw->AddLine(point(0, 16), point(0, 0), color, 1.5f);
    draw->AddLine(point(0, 16), point(17, 16), color, 1.5f);
    draw->AddLine(point(4, 12), point(8, 6), color, 1.5f);
    draw->AddLine(point(8, 6), point(12, 9), color, 1.5f);
    draw->AddLine(point(12, 9), point(17, 2), color, 1.5f);
  } else {
    for (int row = 0; row < 3; ++row) {
      const float y = 2.0f + row * 6.0f;
      const float x = row == 1 ? 12.0f : 5.0f;
      draw->AddLine(point(0, y), point(17, y), color, 1.5f);
      draw->AddRectFilled(point(x - 2.5f, y - 2.5f), point(x + 2.5f, y + 2.5f), color);
    }
  }
}

inline bool PageTab(const char* label, bool selected) {
  ImGui::PushStyleColor(ImGuiCol_Button, selected
      ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, selected
      ? Accent() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
  const bool clicked = ImGui::Button(label);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  return clicked;
}

inline bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.39f, 0.34f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.13f, 0.49f, 0.42f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.32f, 0.28f, 1));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return clicked;
}

inline void EmptyState(const char* title, const char* detail) {
  const ImVec2 available = ImGui::GetContentRegionAvail();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
      std::max(0.0f, (available.y - ImGui::GetTextLineHeightWithSpacing() * 2) * 0.5f));
  const char* lines[] = {title, detail};
  const int count = available.y >= ImGui::GetTextLineHeight() * 2 +
      ImGui::GetStyle().ItemSpacing.y ? 2 : 1;
  for (int line = 0; line < count; ++line) {
    const char* text = lines[line];
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
        std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x) * 0.5f));
    ImGui::TextDisabled("%s", text);
  }
}

}
}
