// EventStudio: camera control, recording, preview and offline processing for Prophesee event cameras (OpenEB).

#include <cstdio>
#include <filesystem>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app/app.h"

namespace {

std::vector<std::filesystem::path> g_dropped_files;

void glfw_error_callback(int error, const char *description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void drop_callback(GLFWwindow *, int count, const char **paths) {
    for (int i = 0; i < count; ++i)
        g_dropped_files.push_back(std::filesystem::u8path(paths[i]));
}

void setup_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &style      = ImGui::GetStyle();
    style.WindowRounding    = 4.f;
    style.FrameRounding     = 3.f;
    style.GrabRounding      = 3.f;
    style.TabRounding       = 3.f;
    style.ScrollbarRounding = 6.f;
    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    ImVec4 *c               = style.Colors;
    c[ImGuiCol_Header]        = ImVec4(0.20f, 0.30f, 0.45f, 0.80f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.40f, 0.60f, 0.90f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.25f, 0.45f, 0.70f, 1.00f);
    c[ImGuiCol_WindowBg]      = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    style.ScaleAllSizes(scale);

    ImGuiIO &io = ImGui::GetIO();
    const char *fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
    for (const char *f : fonts) {
        if (std::filesystem::exists(f) && io.Fonts->AddFontFromFileTTF(f, 16.f * scale))
            return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.f * scale;
    io.Fonts->AddFontDefault(&cfg);
}

} // namespace

int main(int argc, char **argv) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(1600, 950, "EventStudio - OpenEB", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "EventStudio_layout.ini";
    float xscale = 1.f, yscale = 1.f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    setup_style(std::max(1.f, xscale));
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    {
        evs::app::App app;
        // EventStudio [recording] [--masks file.masks]
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--masks" && i + 1 < argc)
                app.load_masks(std::filesystem::u8path(argv[++i]));
            else
                app.open_file(std::filesystem::u8path(arg));
        }

        while (!glfwWindowShouldClose(window) && !app.quit_requested()) {
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                ImGui_ImplGlfw_Sleep(20);
                continue;
            }
            if (!g_dropped_files.empty()) {
                app.open_file(g_dropped_files.back());
                g_dropped_files.clear();
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.08f, 0.08f, 0.10f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
        app.shutdown();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
