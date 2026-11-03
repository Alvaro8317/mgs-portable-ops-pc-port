// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_launcher (F22b): the window a player uses instead of the command line.
// It drives profiles/mpo/scripts/setup.py, which does the work:
//
//   1. pick the ISO (and, if setup.py cannot find them, the firmware fonts)
//      with a built-in browser that the mouse, the keyboard and a gamepad can
//      all drive (the Steam Deck's game mode has no usable file dialog);
//   2. `setup.py install`, with its step, progress and log;
//   3. Play (`setup.py play`), while `setup.py stages --low-priority`
//      compiles the stages in the background (quick -O0 first, then
//      optimized), with Stop (after the current stage);
//   4. import and export saves (`setup.py import-save/export-save`, F18c).
//
// When the game stops with an error (for example an HLE import that is not
// implemented, CLAUDE.md §5) its last lines are shown, never hidden.
//
//   mpo_launcher [--data <dir>] [--source <repo>] [--python <exe>]
//                [--screenshot <file.bmp>] [--browse-saves]
//
// --screenshot saves the window once the install state is read and quits
// (with SDL_VIDEO_DRIVER=offscreen, for the evidence of a session).
// --browse-saves opens the browser of "Import a save..." at start (with
// --screenshot, the evidence of the browser, F22c).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"
#include "launcher_core.hpp"
#include "task.hpp"

#ifndef MPO_LAUNCHER_SOURCE_ROOT
#define MPO_LAUNCHER_SOURCE_ROOT ""
#endif

namespace {

namespace fs = std::filesystem;
using mpo::launcher::Task;

#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

std::string path_text(const fs::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path text_path(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path home_folder() {
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    return home != nullptr ? text_path(home) : fs::current_path();
}

fs::path documents_folder() {
    const char *documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return documents != nullptr ? text_path(documents) : fs::path();
}

// A folder the browser offers as a button (PPSSPP's saves, F22c).
struct Shortcut {
    std::string label;
    fs::path folder;
};

// The built-in file browser (a modal popup). The path can also be typed, and
// hidden folders shown: PPSSPP keeps its saves under ~/.config on Linux.
class Browser {
public:
    enum class Mode { kIsoFile, kFolder };

    void open(const char *title, Mode mode, const fs::path &start,
              std::vector<Shortcut> shortcuts = {}) {
        title_ = title;
        mode_ = mode;
        shortcuts_ = std::move(shortcuts);
        std::error_code ec;
        folder_ = !start.empty() && fs::is_directory(start, ec) ? start
                  : !start.empty() && fs::is_directory(start.parent_path(), ec)
                      ? start.parent_path()
                      : home_folder();
        refresh();
        open_request_ = true;
    }

    // True when the player chose something; then `chosen()` is it.
    bool draw() {
        bool done = false;
        if (open_request_) {
            ImGui::OpenPopup(title_.c_str());
            open_request_ = false;
        }
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(
            ImVec2(viewport->WorkSize.x * 0.85f, viewport->WorkSize.y * 0.85f));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_NoResize))
            return false;
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.0f);
        const bool typed =
            ImGui::InputText("##path", &path_input_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Go") || typed) go_to(text_path(path_input_));
        // The next item on this row, or on a new one when it does not fit:
        // Windows adds a button per drive (F22c: Cancel ended up off the window).
        const auto next_in_row = [](const char *label, float extra) {
            const ImGuiStyle &style = ImGui::GetStyle();
            const float width =
                ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f + extra;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
        };
        if (ImGui::Button("Up") && folder_.has_parent_path() && folder_.parent_path() != folder_) {
            folder_ = folder_.parent_path();
            refresh();
        }
        next_in_row("Home", 0.0f);
        if (ImGui::Button("Home")) {
            folder_ = home_folder();
            refresh();
        }
        if constexpr (kWindows) {
            for (char drive = 'C'; drive <= 'Z'; ++drive) {
                const std::string root = std::string(1, drive) + ":\\";
                std::error_code ec;
                if (!fs::exists(root, ec)) continue;
                next_in_row(root.c_str(), 0.0f);
                if (ImGui::Button(root.c_str())) {
                    folder_ = root;
                    refresh();
                }
            }
        }
        for (const Shortcut &shortcut : shortcuts_) {
            next_in_row(shortcut.label.c_str(), 0.0f);
            if (ImGui::Button(shortcut.label.c_str())) {
                folder_ = shortcut.folder;
                refresh();
            }
        }
        constexpr const char *kHidden = "Show hidden folders";
        next_in_row(kHidden, ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Checkbox(kHidden, &show_hidden_)) refresh();
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", error_.c_str());
        ImGui::Separator();
        // The buttons go below the list, always in view.
        if (ImGui::BeginChild("entries", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_None)) {
            std::optional<fs::path> next;
            for (const mpo::launcher::DirEntry &entry : entries_) {
                const std::string label = (entry.directory ? "[folder]  " : "") + entry.name;
                if (ImGui::Selectable(label.c_str())) {
                    const fs::path path = folder_ / text_path(entry.name);
                    if (entry.directory) {
                        next = path;
                    } else {
                        chosen_ = path;
                        done = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            if (entries_.empty()) ImGui::TextDisabled("(nothing to choose here)");
            if (next) {
                folder_ = *next;
                refresh();
            }
        }
        ImGui::EndChild();
        if (mode_ == Mode::kFolder) {
            if (ImGui::Button("Choose this folder")) {
                chosen_ = folder_;
                done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return done;
    }

    [[nodiscard]] const fs::path &chosen() const noexcept { return chosen_; }

private:
    // A typed path: a folder opens; in the ISO mode an existing file is
    // chosen, as a click on it would.
    void go_to(const fs::path &path) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            folder_ = path;
            refresh();
        } else if (mode_ == Mode::kIsoFile && fs::is_regular_file(path, ec)) {
            folder_ = path.parent_path();
            refresh();
        } else {
            error_ = "No such folder: " + path_text(path);
        }
    }

    void refresh() {
        static constexpr std::array<std::string_view, 1> kIso{".iso"};
        std::error_code ec;
        entries_ = mpo::launcher::list_directory(folder_,
                                                 mode_ == Mode::kIsoFile
                                                     ? std::span<const std::string_view>(kIso)
                                                     : std::span<const std::string_view>(),
                                                 ec, show_hidden_);
        error_ = ec ? "Cannot open this folder: " + ec.message() : "";
        path_input_ = path_text(folder_);
    }

    std::string title_;
    Mode mode_{Mode::kIsoFile};
    fs::path folder_;
    fs::path chosen_;
    std::vector<mpo::launcher::DirEntry> entries_;
    std::vector<Shortcut> shortcuts_;
    std::string error_;
    std::string path_input_;
    bool show_hidden_{};
    bool open_request_{};
};

// The PPSSPP save folders found on this machine, as browser buttons.
std::vector<Shortcut> ppsspp_shortcuts() {
    std::vector<Shortcut> shortcuts;
    const std::vector<fs::path> folders =
        mpo::launcher::ppsspp_save_folders(home_folder(), documents_folder(), kWindows);
    for (const fs::path &folder : folders) {
        const bool flatpak = path_text(folder).find("org.ppsspp.PPSSPP") != std::string::npos;
        shortcuts.push_back({flatpak ? "PPSSPP saves (Flatpak)" : "PPSSPP saves", folder});
    }
    return shortcuts;
}

enum class Pick { kNone, kIso, kFonts, kAddFonts, kImportSave, kExportSave };

struct Launcher {
    mpo::launcher::LauncherPaths paths;
    fs::path data;
    std::optional<mpo::launcher::InstallStatus> status;
    Task status_task;
    Task work; // install, import-save, export-save (one at a time)
    Task stages;
    Task game;
    std::string work_name;
    std::optional<mpo::launcher::Progress> work_progress;
    std::optional<mpo::launcher::Progress> stages_progress;
    std::string iso;
    std::string fonts;
    bool compile_stages_after_install{true};
    bool start_stages_when_idle{};
    Browser browser;
    Pick pick{Pick::kNone};
    // A failure to show: title and the last lines of its output.
    std::string failure_title;
    std::vector<std::string> failure_lines;
    bool failure_request{};

    std::vector<std::string> command(std::string_view subcommand,
                                     const std::vector<std::string> &args) const {
        return mpo::launcher::setup_command(paths, subcommand, data, args);
    }

    void fail(std::string title, const Task &task) {
        failure_title = std::move(title);
        const std::vector<std::string> &lines = task.output().lines();
        const std::size_t from = lines.size() > 30u ? lines.size() - 30u : 0u;
        failure_lines.assign(lines.begin() + static_cast<std::ptrdiff_t>(from), lines.end());
        failure_request = true;
    }

    void fail_to_start(std::string title, const std::string &error) {
        failure_title = std::move(title);
        failure_lines = {error, "Python: " + path_text(paths.python),
                         "setup.py: " + path_text(paths.setup_script)};
        failure_request = true;
    }

    void refresh_status() {
        if (status_task.running()) return;
        std::string error;
        if (!status_task.start(command("status", {"--json"}), error))
            fail_to_start("Cannot run setup.py", error);
    }

    void start_work(std::string name, std::vector<std::string> argv) {
        std::string error;
        work_name = std::move(name);
        work_progress.reset();
        if (!work.start(argv, error)) fail_to_start("Cannot start: " + work_name, error);
    }

    void start_stages() {
        if (stages.running()) return;
        std::error_code ec;
        fs::remove(stop_file(), ec);
        std::string error;
        stages_progress.reset();
        if (!stages.start(command("stages", {"--low-priority"}), error))
            fail_to_start("Cannot start compiling the stages", error);
    }

    [[nodiscard]] fs::path data_folder() const {
        if (!data.empty()) return data;
        return status ? text_path(status->data) : fs::path();
    }

    [[nodiscard]] fs::path stop_file() const { return data_folder() / "stop-stages"; }

    void stop_stages() {
        if (!stages.running() || data_folder().empty()) return;
        std::ofstream(stop_file()) << "stop\n";
    }

    void poll() {
        for (const std::string &line : status_task.poll()) (void)line;
        if (status_task.exit_code()) {
            std::string json;
            for (const std::string &line : status_task.output().lines()) json += line + "\n";
            if (*status_task.exit_code() == 0) status = mpo::launcher::parse_status(json);
            status_task.reset();
        }
        for (const std::string &line : work.poll())
            if (auto p = mpo::launcher::parse_progress_line(line)) work_progress = *p;
        if (work.exit_code()) {
            const bool ok = *work.exit_code() == 0;
            if (!ok) fail(work_name + " failed", work);
            if (ok && work_name == "Install" && compile_stages_after_install)
                start_stages_when_idle = true;
            work_name.clear();
            work.reset();
            refresh_status();
        }
        for (const std::string &line : stages.poll())
            if (auto p = mpo::launcher::parse_progress_line(line); p && p->step == "stages")
                stages_progress = *p;
        if (stages.exit_code()) {
            if (*stages.exit_code() != 0) fail("Compiling the stages failed", stages);
            stages.reset();
            refresh_status();
        }
        for (const std::string &line : game.poll()) (void)line;
        if (game.exit_code()) {
            if (*game.exit_code() != 0) fail("The game stopped with an error", game);
            game.reset();
            refresh_status();
        }
        if (start_stages_when_idle && !work.running() && !status_task.running()) {
            start_stages_when_idle = false;
            start_stages();
        }
    }

    void draw_log(const Task &task, float height) {
        if (ImGui::BeginChild("log", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
            for (const std::string &line : task.output().lines())
                ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    void draw_install() {
        ImGui::SeparatorText("1. Your copy of the game");
        ImGui::TextWrapped(
            "Choose the ISO of Metal Gear Solid: Portable Ops (USA, ULUS-10202). Nothing of the "
            "game leaves this computer: the port is compiled here, from your ISO.");
        ImGui::SetNextItemWidth(-160.0f);
        ImGui::InputText("##iso", &iso);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##iso")) {
            pick = Pick::kIso;
            browser.open("Choose the ISO", Browser::Mode::kIsoFile, text_path(iso));
        }
        ImGui::SeparatorText("2. PSP firmware fonts");
        ImGui::TextWrapped(
            "The folder flash0/font of a PSP or of PPSSPP. If it is next to the ISO, it is found "
            "by itself. Without it the game runs, and the save screens and the on-screen keyboard "
            "use a substitute font.");
        ImGui::SetNextItemWidth(-160.0f);
        ImGui::InputText("##fonts", &fonts);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##fonts")) {
            pick = Pick::kFonts;
            browser.open("Choose the fonts folder", Browser::Mode::kFolder, text_path(fonts));
        }
        ImGui::SeparatorText("3. Install");
        ImGui::Checkbox("Then compile every stage in the background",
                        &compile_stages_after_install);
        ImGui::TextWrapped(
            "The first install takes 20 to 40 minutes and needs about 15 GB free (10 GB once "
            "done). You can play as soon as it ends; the stages keep compiling while you play.");
        if constexpr (kWindows) {
            // Avast quarantined llvm-mingw's clang-23.exe on a player's PC (F22c).
            ImGui::TextWrapped(
                "Some antivirus programs (Avast in particular) remove part of the compiler by "
                "mistake. Adding an exception for %s first avoids it (see README.md).",
                data_folder().empty() ? "%LOCALAPPDATA%\\mpo-pc-port"
                                      : path_text(data_folder()).c_str());
        }
        ImGui::BeginDisabled(iso.empty() || work.running() || stages.running());
        if (ImGui::Button("Install", ImVec2(220.0f, 0.0f))) {
            std::vector<std::string> args{"--iso", iso};
            if (!fonts.empty()) {
                args.emplace_back("--fonts");
                args.push_back(fonts);
            }
            start_work("Install", command("install", args));
        }
        ImGui::EndDisabled();
    }

    void draw_play() {
        ImGui::SeparatorText("Play");
        const bool all =
            status->stages_total > 0 && status->stages_optimized >= status->stages_total;
        ImGui::BeginDisabled(game.running());
        if (ImGui::Button(game.running() ? "Playing..." : "Play", ImVec2(260.0f, 64.0f))) {
            std::string error;
            if (!game.start(command("play", {}), error))
                fail_to_start("Cannot start the game", error);
        }
        ImGui::EndDisabled();
        if (status->stages_total > 0) {
            ImGui::Text("Stages compiled: %d of %d (%d optimized)", status->stages_installed,
                        status->stages_total, status->stages_optimized);
            if (!all) {
                ImGui::TextWrapped(
                    "A stage that is not compiled yet runs on the interpreter: it plays, but "
                    "slower. A quick version of every stage comes first, then the optimized one.");
            }
        }
        if (stages.running()) {
            float fraction = 0.0f;
            std::string label = "starting";
            if (stages_progress) {
                label = stages_progress->message;
                if (stages_progress->total > 0)
                    fraction = static_cast<float>(stages_progress->done) /
                               static_cast<float>(stages_progress->total);
            }
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label.c_str());
            if (ImGui::Button("Stop compiling (after this stage)")) stop_stages();
        } else if (!all && !work.running()) {
            if (ImGui::Button("Compile the stages in the background")) start_stages();
        }
    }

    void open_import_browser() {
        pick = Pick::kImportSave;
        browser.open("Choose the save folder to import", Browser::Mode::kFolder, {},
                     ppsspp_shortcuts());
    }

    void draw_saves() {
        ImGui::SeparatorText("Saves");
        ImGui::TextWrapped(
            "Bring a save from a PSP or PPSSPP (the PSP/SAVEDATA/ULUS10202... folder), or take "
            "yours to them.");
        ImGui::BeginDisabled(work.running() || game.running());
        if (ImGui::Button("Import a save...")) open_import_browser();
        ImGui::SameLine();
        if (ImGui::Button("Export my saves...")) {
            pick = Pick::kExportSave;
            browser.open("Choose where to export the saves", Browser::Mode::kFolder, {},
                         ppsspp_shortcuts());
        }
        ImGui::EndDisabled();
    }

    // After the install: the fonts can be added (or changed) at any time.
    void draw_fonts() {
        ImGui::SeparatorText("PSP firmware fonts");
        if (status->fonts) {
            ImGui::TextDisabled("Installed.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Missing: the save screens and the on-screen keyboard use a "
                               "substitute font.");
            ImGui::TextWrapped("Choose the flash0/font folder of a PSP, or assets/flash0/font in "
                               "the PPSSPP folder.");
        }
        ImGui::BeginDisabled(work.running() || game.running());
        if (ImGui::Button(status->fonts ? "Change the fonts..." : "Add the fonts...")) {
            pick = Pick::kAddFonts;
            browser.open("Choose the fonts folder", Browser::Mode::kFolder, {});
        }
        ImGui::EndDisabled();
    }

    void draw_work() {
        ImGui::SeparatorText(work_name.c_str());
        float fraction = 0.0f;
        std::string label = mpo::launcher::format_duration(work.seconds());
        if (work_progress) {
            label = work_progress->step + ": " + work_progress->message + "  (" + label + ")";
            if (work_progress->total > 0)
                fraction = static_cast<float>(work_progress->done) /
                           static_cast<float>(work_progress->total);
        }
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label.c_str());
        draw_log(work, ImGui::GetContentRegionAvail().y * 0.6f);
    }

    void draw_failure() {
        if (failure_request) {
            ImGui::OpenPopup("failure");
            failure_request = false;
        }
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.85f, viewport->WorkSize.y * 0.7f));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("failure", nullptr, ImGuiWindowFlags_NoResize)) return;
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", failure_title.c_str());
        ImGui::TextWrapped(
            "The last lines are below; the full logs are in the logs folder of the install "
            "folder. If the game stopped while playing, what happened since your last save is "
            "lost.");
        if (ImGui::BeginChild("lines", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.5f),
                              ImGuiChildFlags_Borders)) {
            for (const std::string &line : failure_lines) ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("Copy to the clipboard")) {
            std::string text = failure_title + "\n";
            for (const std::string &line : failure_lines) text += line + "\n";
            SDL_SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void draw_browser() {
        if (!browser.draw()) return;
        const fs::path chosen = browser.chosen();
        switch (pick) {
        case Pick::kIso:
            iso = path_text(chosen);
            break;
        case Pick::kFonts:
            fonts = path_text(chosen);
            break;
        case Pick::kAddFonts:
            start_work("Fonts", command("fonts", {"--fonts", path_text(chosen)}));
            break;
        case Pick::kImportSave:
            start_work("Import save", command("import-save", {path_text(chosen)}));
            break;
        case Pick::kExportSave:
            start_work("Export saves", command("export-save", {path_text(chosen)}));
            break;
        case Pick::kNone:
            break;
        }
        pick = Pick::kNone;
    }

    void draw() {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("Metal Gear Solid: Portable Ops - unofficial PC port");
        ImGui::TextDisabled("Not affiliated with Konami. You need your own copy of the game.");
        if (!status) {
            ImGui::Separator();
            ImGui::TextUnformatted(status_task.running() ? "Reading the install..."
                                                         : "Could not read the install state.");
            if (!status_task.running() && ImGui::Button("Try again")) refresh_status();
        } else if (work.running()) {
            draw_work();
        } else if (!status->playable) {
            draw_install();
        } else {
            draw_play();
            draw_saves();
            draw_fonts();
            ImGui::SeparatorText("Install");
            ImGui::TextDisabled("Folder: %s", status->data.c_str());
            ImGui::TextDisabled("ISO: %s", status->iso.c_str());
        }
        draw_browser();
        draw_failure();
        ImGui::End();
    }
};

int run(int argc, char **argv) {
    fs::path source;
    fs::path python;
    fs::path data;
    fs::path screenshot;
    bool browse_saves = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--browse-saves") {
            browse_saves = true;
        } else if ((arg == "--source" || arg == "--python" || arg == "--data" ||
                    arg == "--screenshot") &&
                   i + 1 < argc) {
            const fs::path value = text_path(argv[++i]);
            (arg == "--source"   ? source
             : arg == "--python" ? python
             : arg == "--data"   ? data
                                 : screenshot) = value;
        } else {
            std::fprintf(stderr, "usage: mpo_launcher [--data <dir>] [--source <repo>] "
                                 "[--python <exe>] [--screenshot <file.bmp>] "
                                 "[--browse-saves]\n");
            return 2;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "mpo_launcher: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const char *base_text = SDL_GetBasePath();
    const fs::path base = base_text != nullptr ? text_path(base_text) : fs::current_path();
    const std::optional<mpo::launcher::LauncherPaths> paths = mpo::launcher::find_paths(
        base, source, python, text_path(MPO_LAUNCHER_SOURCE_ROOT), kWindows);
    if (!paths) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MPO launcher",
                                 "The port's files (source/profiles/mpo/scripts/setup.py) are not "
                                 "next to the launcher. Unpack the whole release again.",
                                 nullptr);
        SDL_Quit();
        return 1;
    }

    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const float ui_scale = scale > 0.0f ? scale : 1.0f;
    SDL_Window *window =
        SDL_CreateWindow("MGS: Portable Ops - PC port", static_cast<int>(1000 * ui_scale),
                         static_cast<int>(720 * ui_scale),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "mpo_launcher: no window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.ScaleAllSizes(ui_scale * 1.3f);
    style.FontScaleDpi = ui_scale;
    style.FontScaleMain = 1.3f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Launcher launcher;
    launcher.paths = *paths;
    launcher.data = data;
    launcher.refresh_status();
    if (browse_saves) launcher.open_import_browser();

    bool done = false;
    int frames_after_status = 0;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                 event.window.windowID == SDL_GetWindowID(window)))
                done = true;
        }
        launcher.poll();
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(50);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        launcher.draw();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.09f, 0.08f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!screenshot.empty() && !launcher.status_task.running() && ++frames_after_status > 5) {
            SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
            const bool saved =
                surface != nullptr && SDL_SaveBMP(surface, path_text(screenshot).c_str());
            if (surface != nullptr) SDL_DestroySurface(surface);
            std::fprintf(stderr, "mpo_launcher: screenshot %s: %s\n", path_text(screenshot).c_str(),
                         saved ? "saved" : SDL_GetError());
            done = true;
        }
        SDL_RenderPresent(renderer);
    }
    // Compiling goes on until the stage it is on is done, then stops; an
    // install keeps going by itself (setup.py resumes either next time).
    launcher.stop_stages();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    return run(argc, argv);
}
