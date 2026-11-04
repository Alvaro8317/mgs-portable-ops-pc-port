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
//   4. import and export saves (`setup.py import-save/export-save`, F18c);
//   5. the game settings (F24a): the renderer, the internal resolution, the
//      window size and fullscreen, saved in settings.toml (the path and the
//      values come from `setup.py status --json`), which `setup.py play`
//      turns into mpo_boot options.
//   6. in English or Spanish (F25d): the system's language, or the one chosen
//      in Settings (`language` in settings.toml). Every text is in
//      launcher_strings.inc; what setup.py prints stays as it is (English).
//
// When the game stops with an error (for example an HLE import that is not
// implemented, CLAUDE.md §5) its last lines are shown, never hidden.
//
//   mpo_launcher [--data <dir>] [--source <repo>] [--python <exe>]
//                [--settings <file>] [--screenshot <file.bmp>] [--browse-saves]
//
// --settings uses that settings.toml instead of the one in the configuration
// folder (setup.py status/play --settings): for tests and screenshots that
// must not touch the player's.
//
// --screenshot saves the window once the install state is read and quits
// (with SDL_VIDEO_DRIVER=offscreen, for the evidence of a session).
// --browse-saves opens the browser of "Import a save..." at start (with
// --screenshot, the evidence of the browser, F22c). For a screenshot in a
// language, LANG=es_ES.UTF-8 (or a settings.toml with `language`).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
#include "launcher_font.hpp"
#include "launcher_text.hpp"
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

using mpo::launcher::Language;
using mpo::launcher::Text;

// The language every text is shown in (F25d): the setting's, or the
// system's. Set before the first frame and when the setting changes.
Language g_language = Language::kEnglish;

const char *tr(Text id) {
    return mpo::launcher::text(g_language, id);
}

std::string trf(Text id, std::initializer_list<std::string_view> args) {
    return mpo::launcher::format_text(tr(id), args);
}

// A label whose ImGui id does not change with the language ("Jugar###play"),
// so the gamepad's focus stays on it when the language changes.
std::string label(Text id, const char *imgui_id) {
    return std::string(tr(id)) + "###" + imgui_id;
}

// The system's language: Windows' (and macOS') preferred UI languages through
// SDL; on Linux, LC_ALL, LC_MESSAGES, LANG and LANGUAGE as gettext reads them
// (SDL only reads LANG and LANGUAGE), then SDL's. English unless one is "es".
Language system_language() {
    std::vector<std::string> locales;
#if !defined(_WIN32) && !defined(__APPLE__)
    locales = mpo::launcher::posix_locales([](const char *name) { return SDL_getenv(name); });
#endif
    int count = 0;
    if (SDL_Locale **list = SDL_GetPreferredLocales(&count); list != nullptr) {
        for (int i = 0; i < count; ++i) {
            if (list[i] == nullptr || list[i]->language == nullptr) continue;
            std::string locale = list[i]->language;
            if (list[i]->country != nullptr) locale += std::string("_") + list[i]->country;
            locales.push_back(std::move(locale));
        }
        SDL_free(list);
    }
    return mpo::launcher::language_from_locales(locales);
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

    void open(std::string title, Mode mode, const fs::path &start,
              std::vector<Shortcut> shortcuts = {}) {
        title_ = std::move(title);
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
        if (ImGui::Button(label(Text::kBrowserGo, "go").c_str()) || typed)
            go_to(text_path(path_input_));
        // The next item on this row, or on a new one when it does not fit:
        // Windows adds a button per drive (F22c: Cancel ended up off the window).
        const auto next_in_row = [](const std::string &text, float extra) {
            const ImGuiStyle &style = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(text.c_str(), nullptr, true).x +
                                style.FramePadding.x * 2.0f + extra;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
        };
        if (ImGui::Button(label(Text::kBrowserUp, "up").c_str()) && folder_.has_parent_path() &&
            folder_.parent_path() != folder_) {
            folder_ = folder_.parent_path();
            refresh();
        }
        const std::string home = label(Text::kBrowserHome, "home");
        next_in_row(home, 0.0f);
        if (ImGui::Button(home.c_str())) {
            folder_ = home_folder();
            refresh();
        }
        if constexpr (kWindows) {
            for (char drive = 'C'; drive <= 'Z'; ++drive) {
                const std::string root = std::string(1, drive) + ":\\";
                std::error_code ec;
                if (!fs::exists(root, ec)) continue;
                next_in_row(root, 0.0f);
                if (ImGui::Button(root.c_str())) {
                    folder_ = root;
                    refresh();
                }
            }
        }
        for (const Shortcut &shortcut : shortcuts_) {
            next_in_row(shortcut.label, 0.0f);
            if (ImGui::Button(shortcut.label.c_str())) {
                folder_ = shortcut.folder;
                refresh();
            }
        }
        const std::string hidden = label(Text::kBrowserShowHidden, "hidden");
        next_in_row(hidden, ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Checkbox(hidden.c_str(), &show_hidden_)) refresh();
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", error_.c_str());
        ImGui::Separator();
        // The buttons go below the list, always in view.
        if (ImGui::BeginChild("entries", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_None)) {
            std::optional<fs::path> next;
            for (const mpo::launcher::DirEntry &entry : entries_) {
                const std::string name =
                    (entry.directory ? std::string(tr(Text::kBrowserFolderTag)) + "  " : "") +
                    entry.name;
                if (ImGui::Selectable(name.c_str())) {
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
            if (entries_.empty()) ImGui::TextDisabled("%s", tr(Text::kBrowserEmpty));
            if (next) {
                folder_ = *next;
                refresh();
            }
        }
        ImGui::EndChild();
        if (mode_ == Mode::kFolder) {
            if (ImGui::Button(label(Text::kBrowserChooseFolder, "choose").c_str())) {
                chosen_ = folder_;
                done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(label(Text::kCancel, "cancel").c_str())) ImGui::CloseCurrentPopup();
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
            error_ = trf(Text::kBrowserNoSuchFolder, {path_text(path)});
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
        error_ = ec ? trf(Text::kBrowserCannotOpen, {ec.message()}) : "";
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
        shortcuts.push_back(
            {tr(flatpak ? Text::kShortcutPpssppFlatpak : Text::kShortcutPpsspp), folder});
    }
    return shortcuts;
}

enum class Pick { kNone, kIso, kFonts, kAddFonts, kImportSave, kExportSave };

struct Launcher {
    mpo::launcher::LauncherPaths paths;
    fs::path data;
    fs::path settings_file; // --settings, empty for setup.py's default
    std::optional<mpo::launcher::InstallStatus> status;
    Task status_task;
    Task work; // install, import-save, export-save (one at a time)
    Task stages;
    Task game;
    // What `work` runs, its title above the progress bar.
    Text work_title{Text::kWorkInstall};
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
    // Why settings.toml could not be written, until the next change.
    std::string settings_error;
    // F25d: the system's language, for the "auto" setting; the window, for
    // its title.
    Language system{Language::kEnglish};
    SDL_Window *window{};

    // The language of the setting (once status is read), else the system's.
    void apply_language() {
        const Language language =
            mpo::launcher::resolve_language(status ? status->settings.language : "auto", system);
        if (language == g_language) return;
        g_language = language;
        if (window != nullptr) SDL_SetWindowTitle(window, tr(Text::kWindowTitle));
    }

    std::vector<std::string> command(std::string_view subcommand,
                                     const std::vector<std::string> &args) const {
        return mpo::launcher::setup_command(paths, subcommand, data, args);
    }

    // status and play read settings.toml: --settings goes to both.
    std::vector<std::string> settings_command(std::string_view subcommand,
                                              std::vector<std::string> args) const {
        if (!settings_file.empty()) {
            args.emplace_back("--settings");
            args.push_back(path_text(settings_file));
        }
        return command(subcommand, args);
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
        if (!status_task.start(settings_command("status", {"--json"}), error))
            fail_to_start(tr(Text::kFailCannotRunSetup), error);
    }

    void start_work(Text title, const std::vector<std::string> &argv) {
        std::string error;
        work_title = title;
        work_progress.reset();
        if (!work.start(argv, error))
            fail_to_start(trf(Text::kFailCannotStart, {tr(work_title)}), error);
    }

    void start_stages() {
        if (stages.running()) return;
        std::error_code ec;
        fs::remove(stop_file(), ec);
        std::string error;
        stages_progress.reset();
        if (!stages.start(command("stages", {"--low-priority"}), error))
            fail_to_start(tr(Text::kFailStartStages), error);
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
            apply_language();
        }
        for (const std::string &line : work.poll())
            if (auto p = mpo::launcher::parse_progress_line(line)) work_progress = *p;
        if (work.exit_code()) {
            const bool ok = *work.exit_code() == 0;
            if (!ok) fail(trf(Text::kFailWork, {tr(work_title)}), work);
            if (ok && work_title == Text::kWorkInstall && compile_stages_after_install)
                start_stages_when_idle = true;
            work.reset();
            refresh_status();
        }
        for (const std::string &line : stages.poll())
            if (auto p = mpo::launcher::parse_progress_line(line); p && p->step == "stages")
                stages_progress = *p;
        if (stages.exit_code()) {
            if (*stages.exit_code() != 0) fail(tr(Text::kFailStages), stages);
            stages.reset();
            refresh_status();
        }
        for (const std::string &line : game.poll()) (void)line;
        if (game.exit_code()) {
            if (*game.exit_code() != 0) fail(tr(Text::kFailGame), game);
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
        ImGui::SeparatorText(tr(Text::kStepGame));
        ImGui::TextWrapped("%s", tr(Text::kStepGameHelp));
        ImGui::SetNextItemWidth(-160.0f);
        ImGui::InputText("##iso", &iso);
        ImGui::SameLine();
        if (ImGui::Button(label(Text::kBrowse, "browse_iso").c_str())) {
            pick = Pick::kIso;
            browser.open(label(Text::kChooseIso, "pick"), Browser::Mode::kIsoFile, text_path(iso));
        }
        ImGui::SeparatorText(tr(Text::kStepFonts));
        ImGui::TextWrapped("%s", tr(Text::kStepFontsHelp));
        ImGui::SetNextItemWidth(-160.0f);
        ImGui::InputText("##fonts", &fonts);
        ImGui::SameLine();
        if (ImGui::Button(label(Text::kBrowse, "browse_fonts").c_str())) {
            pick = Pick::kFonts;
            browser.open(label(Text::kChooseFontsFolder, "pick"), Browser::Mode::kFolder,
                         text_path(fonts));
        }
        ImGui::SeparatorText(tr(Text::kStepInstall));
        ImGui::Checkbox(label(Text::kCompileAfterInstall, "compile_after").c_str(),
                        &compile_stages_after_install);
        ImGui::TextWrapped("%s", tr(Text::kInstallTime));
        if constexpr (kWindows) {
            // Avast quarantined llvm-mingw's clang-23.exe on a player's PC (F22c).
            ImGui::TextWrapped(
                "%s", trf(Text::kAntivirus, {data_folder().empty() ? "%LOCALAPPDATA%\\mpo-pc-port"
                                                                   : path_text(data_folder())})
                          .c_str());
        }
        ImGui::BeginDisabled(iso.empty() || work.running() || stages.running());
        if (ImGui::Button(label(Text::kInstallButton, "install").c_str(), ImVec2(220.0f, 0.0f))) {
            std::vector<std::string> args{"--iso", iso};
            if (!fonts.empty()) {
                args.emplace_back("--fonts");
                args.push_back(fonts);
            }
            start_work(Text::kWorkInstall, command("install", args));
        }
        ImGui::EndDisabled();
    }

    void draw_play() {
        ImGui::SeparatorText(tr(Text::kPlaySection));
        const bool all =
            status->stages_total > 0 && status->stages_optimized >= status->stages_total;
        ImGui::BeginDisabled(game.running());
        if (ImGui::Button(
                label(game.running() ? Text::kPlaying : Text::kPlayButton, "play").c_str(),
                ImVec2(260.0f, 64.0f))) {
            std::string error;
            if (!game.start(settings_command("play", {}), error))
                fail_to_start(tr(Text::kFailCannotStartGame), error);
        }
        ImGui::EndDisabled();
        if (status->stages_total > 0) {
            ImGui::TextUnformatted(
                trf(Text::kStagesCompiled,
                    {std::to_string(status->stages_installed), std::to_string(status->stages_total),
                     std::to_string(status->stages_optimized)})
                    .c_str());
            if (!all) ImGui::TextWrapped("%s", tr(Text::kStagesInterpreter));
        }
        if (stages.running()) {
            float fraction = 0.0f;
            std::string progress = tr(Text::kStagesStarting);
            if (stages_progress) {
                progress = stages_progress->message;
                if (stages_progress->total > 0)
                    fraction = static_cast<float>(stages_progress->done) /
                               static_cast<float>(stages_progress->total);
            }
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), progress.c_str());
            if (ImGui::Button(label(Text::kStopCompiling, "stop").c_str())) stop_stages();
        } else if (!all && !work.running()) {
            if (ImGui::Button(label(Text::kCompileInBackground, "compile").c_str())) start_stages();
        }
    }

    // A row of radio buttons, 1x to `top`x; true when the player picked one.
    static bool scale_buttons(const char *id, int top, int &value) {
        bool changed = false;
        ImGui::PushID(id);
        for (int n = 1; n <= top; ++n) {
            if (n > 1) ImGui::SameLine();
            const std::string text = std::to_string(n) + "x";
            if (ImGui::RadioButton(text.c_str(), value == n)) {
                changed = value != n;
                value = n;
            }
        }
        ImGui::PopID();
        return changed;
    }

    // F25d: "auto" (the system's language), English or Spanish, each by its
    // own name. True when the player picked another one.
    bool language_buttons(std::string &language) const {
        using mpo::launcher::language_name;
        bool changed = false;
        const auto option = [&](const std::string &text, std::string_view value) {
            if (ImGui::RadioButton(text.c_str(), language == value) && language != value) {
                language = value;
                changed = true;
            }
        };
        option(trf(Text::kLanguageAuto, {language_name(system)}) + "###lang_auto",
               mpo::launcher::kLanguageAuto);
        ImGui::SameLine();
        option(std::string(language_name(Language::kEnglish)) + "###lang_en", "en");
        ImGui::SameLine();
        option(std::string(language_name(Language::kSpanish)) + "###lang_es", "es");
        return changed;
    }

    // Writes settings.toml when something changed, and shows the language.
    void commit_settings(const mpo::launcher::GameSettings &settings) {
        if (settings == status->settings) return;
        // Kept even when the file cannot be written, so it is tried once
        // per change, not once per frame.
        status->settings = settings;
        settings_error.clear();
        // The file is rewritten with valid values only.
        if (mpo::launcher::save_settings(text_path(status->settings_file), settings,
                                         settings_error))
            status->settings_notices.clear();
        apply_language();
    }

    // Before the install, the language alone (the rest of Settings only
    // matters once the game is installed).
    void draw_language_line() {
        if (status->settings_file.empty()) return;
        // What setup.py found wrong in settings.toml (as Settings shows it).
        for (const std::string &notice : status->settings_notices)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", notice.c_str());
        mpo::launcher::GameSettings settings = status->settings;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr(Text::kLanguage));
        ImGui::SameLine();
        language_buttons(settings.language);
        commit_settings(settings);
        if (!settings_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                               trf(Text::kNotSaved, {settings_error}).c_str());
    }

    void draw_settings() {
        ImGui::SeparatorText(tr(Text::kSettingsSection));
        for (const std::string &notice : status->settings_notices)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", notice.c_str());
        if (status->settings_file.empty()) {
            ImGui::TextDisabled("%s", tr(Text::kSettingsUnknown));
            return;
        }
        using mpo::launcher::GameSettings;
        GameSettings settings = status->settings;
        const bool vulkan = settings.renderer == "vulkan";
        if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingFixedFit)) {
            // The launcher's own: it changes at once, also while playing.
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(Text::kLanguage));
            ImGui::TableNextColumn();
            language_buttons(settings.language);

            ImGui::BeginDisabled(game.running());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(Text::kRenderer));
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("Vulkan", vulkan)) settings.renderer = "vulkan";
            ImGui::SameLine();
            if (ImGui::RadioButton(label(Text::kRendererSoft, "soft").c_str(), !vulkan))
                settings.renderer = "soft";

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(Text::kResolution));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!vulkan);
            scale_buttons("render", GameSettings::kMaxRenderScale, settings.render_scale);
            ImGui::SameLine();
            if (vulkan)
                ImGui::TextDisabled(
                    "%s",
                    trf(Text::kPixels, {mpo::launcher::pixel_size(settings.render_scale)}).c_str());
            else
                ImGui::TextDisabled("%s", tr(Text::kVulkanOnly));
            ImGui::EndDisabled();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(Text::kWindow));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(settings.fullscreen);
            scale_buttons("window", GameSettings::kMaxWindowScale, settings.window_scale);
            ImGui::SameLine();
            if (settings.fullscreen)
                ImGui::TextDisabled("%s", tr(Text::kFullscreenNote));
            else
                ImGui::TextDisabled(
                    "%s",
                    trf(Text::kPixels, {mpo::launcher::pixel_size(settings.window_scale)}).c_str());
            ImGui::EndDisabled();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::Checkbox(label(Text::kFullscreen, "fullscreen").c_str(), &settings.fullscreen);

            // F25b: --aspect (the values are in launcher_core.hpp).
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(Text::kAspect));
            ImGui::TableNextColumn();
            bool preset = false;
            for (const mpo::launcher::AspectChoice &choice : mpo::launcher::kAspectChoices) {
                const bool native = &choice == &mpo::launcher::kAspectChoices[0];
                if (!native) ImGui::SameLine();
                const bool chosen = settings.aspect == choice.value;
                preset = preset || chosen;
                const std::string text = native ? label(Text::kAspectNative, "aspect-native")
                                                : std::string(choice.label);
                if (ImGui::RadioButton(text.c_str(), chosen)) settings.aspect = choice.value;
            }
            if (!preset) {
                // Another one, written by hand in settings.toml.
                ImGui::SameLine();
                ImGui::RadioButton(settings.aspect.c_str(), true);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", tr(Text::kAspectNote));
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        commit_settings(settings);
        if (!settings_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                               trf(Text::kNotSaved, {settings_error}).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (game.running())
            ImGui::TextWrapped("%s", tr(Text::kApplyNextPlay));
        else
            ImGui::TextWrapped("%s", trf(Text::kSavedAt, {status->settings_file}).c_str());
        ImGui::PopStyleColor();
    }

    void open_import_browser() {
        pick = Pick::kImportSave;
        browser.open(label(Text::kChooseImportFolder, "pick"), Browser::Mode::kFolder, {},
                     ppsspp_shortcuts());
    }

    void draw_saves() {
        ImGui::SeparatorText(tr(Text::kSavesSection));
        ImGui::TextWrapped("%s", tr(Text::kSavesHelp));
        ImGui::BeginDisabled(work.running() || game.running());
        if (ImGui::Button(label(Text::kImportSave, "import").c_str())) open_import_browser();
        ImGui::SameLine();
        if (ImGui::Button(label(Text::kExportSaves, "export").c_str())) {
            pick = Pick::kExportSave;
            browser.open(label(Text::kChooseExportFolder, "pick"), Browser::Mode::kFolder, {},
                         ppsspp_shortcuts());
        }
        ImGui::EndDisabled();
    }

    // After the install: the fonts can be added (or changed) at any time.
    void draw_fonts() {
        ImGui::SeparatorText(tr(Text::kFontsSection));
        if (status->fonts) {
            ImGui::TextDisabled("%s", tr(Text::kFontsInstalled));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", tr(Text::kFontsMissing));
            ImGui::TextWrapped("%s", tr(Text::kFontsHelp));
        }
        ImGui::BeginDisabled(work.running() || game.running());
        if (ImGui::Button(
                label(status->fonts ? Text::kChangeFonts : Text::kAddFonts, "fonts").c_str())) {
            pick = Pick::kAddFonts;
            browser.open(label(Text::kChooseFontsFolder, "pick"), Browser::Mode::kFolder, {});
        }
        ImGui::EndDisabled();
    }

    void draw_work() {
        ImGui::SeparatorText(tr(work_title));
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
        ImGui::TextWrapped("%s", tr(Text::kFailExplain));
        if (ImGui::BeginChild("lines", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.5f),
                              ImGuiChildFlags_Borders)) {
            for (const std::string &line : failure_lines) ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button(label(Text::kCopyToClipboard, "copy").c_str())) {
            std::string text = failure_title + "\n";
            for (const std::string &line : failure_lines) text += line + "\n";
            SDL_SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(label(Text::kClose, "close").c_str())) ImGui::CloseCurrentPopup();
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
            start_work(Text::kWorkFonts, command("fonts", {"--fonts", path_text(chosen)}));
            break;
        case Pick::kImportSave:
            start_work(Text::kWorkImportSave, command("import-save", {path_text(chosen)}));
            break;
        case Pick::kExportSave:
            start_work(Text::kWorkExportSaves, command("export-save", {path_text(chosen)}));
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
        ImGui::TextUnformatted(tr(Text::kHeader));
        ImGui::TextDisabled("%s", tr(Text::kNotAffiliated));
        if (!status) {
            ImGui::Separator();
            ImGui::TextUnformatted(
                tr(status_task.running() ? Text::kReadingInstall : Text::kCannotReadInstall));
            if (!status_task.running() && ImGui::Button(label(Text::kTryAgain, "retry").c_str()))
                refresh_status();
        } else if (work.running()) {
            draw_work();
        } else if (!status->playable) {
            draw_language_line();
            draw_install();
        } else {
            draw_play();
            draw_settings();
            draw_saves();
            draw_fonts();
            ImGui::SeparatorText(tr(Text::kWorkInstall));
            ImGui::TextDisabled("%s", trf(Text::kInstallFolder, {status->data}).c_str());
            ImGui::TextDisabled("%s", trf(Text::kInstallIso, {status->iso}).c_str());
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
    fs::path settings;
    fs::path screenshot;
    bool browse_saves = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--browse-saves") {
            browse_saves = true;
        } else if ((arg == "--source" || arg == "--python" || arg == "--data" ||
                    arg == "--settings" || arg == "--screenshot") &&
                   i + 1 < argc) {
            const fs::path value = text_path(argv[++i]);
            (arg == "--source"     ? source
             : arg == "--python"   ? python
             : arg == "--data"     ? data
             : arg == "--settings" ? settings
                                   : screenshot) = value;
        } else {
            std::fprintf(stderr, "usage: mpo_launcher [--data <dir>] [--source <repo>] "
                                 "[--python <exe>] [--settings <file>] "
                                 "[--screenshot <file.bmp>] [--browse-saves]\n");
            return 2;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "mpo_launcher: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // F25d: the system's language until settings.toml says otherwise.
    const Language system = system_language();
    g_language = system;
    const char *base_text = SDL_GetBasePath();
    const fs::path base = base_text != nullptr ? text_path(base_text) : fs::current_path();
    const std::optional<mpo::launcher::LauncherPaths> paths = mpo::launcher::find_paths(
        base, source, python, text_path(MPO_LAUNCHER_SOURCE_ROOT), kWindows);
    if (!paths) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, tr(Text::kMessageBoxTitle),
                                 tr(Text::kMissingFiles), nullptr);
        SDL_Quit();
        return 1;
    }

    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const float ui_scale = scale > 0.0f ? scale : 1.0f;
    SDL_Window *window = SDL_CreateWindow(
        tr(Text::kWindowTitle), static_cast<int>(1000 * ui_scale), static_cast<int>(720 * ui_scale),
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
    // F25d: the font with Latin-1, for the Spanish texts.
    mpo::launcher::add_launcher_font(*io.Fonts);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Launcher launcher;
    launcher.paths = *paths;
    launcher.data = data;
    launcher.settings_file = settings;
    launcher.system = system;
    launcher.window = window;
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
