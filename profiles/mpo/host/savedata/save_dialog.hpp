// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtilitySavedata without the HLE and without drawing (F18a).
// Ported from PPSSPP Core/Dialog/PSPSaveDialog.cpp @ v1.20.4.
//
// The request is copied in at InitStart (V1, V2 or V3 size), re-read when the
// game changes it while RUNNING, and written back (common.size bytes) once the
// dialog finishes, like PPSSPP. The file work runs synchronously inside
// Update, where PPSSPP starts its "SaveIO" host thread: an invisible action
// (AUTOLOAD, AUTOSAVE...) is done on the first Update and FINISHED on the
// second, which is what PPSSPP gives when its thread is quick.
//
// Modes: the ones MPO uses (AUTOLOAD, AUTOSAVE, SAVE, LISTLOAD, LISTSAVE,
// LISTALLDELETE; docs/analysis/utility.md) plus LOAD, LISTDELETE, DELETE,
// AUTODELETE, MAKEDATA[SECURE], READDATA[SECURE], WRITEDATA[SECURE] and
// DELETEDATA, which reuse the same file operations. SIZES, LIST, FILES,
// GETSIZE and ERASE[SECURE] are not ported: they run as an empty invisible
// action and unsupported_mode() reports them, so the HLE can stop visibly.

#pragma once

#include "kernel/guest_memory.hpp"
#include "savedata/dialog.hpp"
#include "savedata/psp_types.hpp"
#include "savedata/savedata_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpo::savedata {

// PPSSPP's DisplayState: which screen the dialog shows.
enum class SaveScreen : std::uint8_t {
    kNone, // invisible action
    kSaveListChoice,
    kSaveConfirm,
    kSaveConfirmOverwrite,
    kSaveSaving,
    kSaveDone,
    kLoadListChoice,
    kLoadConfirm,
    kLoadLoading,
    kLoadDone,
    kLoadNoData,
    kDeleteListChoice,
    kDeleteConfirm,
    kDeleteDeleting,
    kDeleteDone,
    kDeleteNoData,
    kSaveFailed,
    kLoadFailed,
    kDeleteFailed,
};

enum class SaveBanner : std::uint8_t { kNone, kSave, kLoad, kDelete };

struct SaveDialogView {
    SaveScreen screen = SaveScreen::kNone;
    SaveBanner banner = SaveBanner::kNone;
    // The saves the screen lists (the first listed_count entries), and the
    // highlighted one.
    std::vector<SaveFileInfo> entries;
    int selected = 0;
    // PPSSPP's English message for the screen (empty on list screens).
    std::string message;
    bool has_yes_no = false;
    bool yes_selected = false;
    bool shows_ok_button = false;
    bool shows_back_button = false;
    std::uint32_t fade = 0;
    std::uint32_t ok_button = button::kCircle;
    std::uint32_t cancel_button = button::kCross;
};

class SaveDialog final : public Dialog {
public:
    // PPSSPP's SAVEDATA_INIT_DELAY_US and SAVEDATA_SHUTDOWN_DELAY_US.
    static constexpr std::int64_t kInitDelayUs = 200'000;
    static constexpr std::int64_t kShutdownDelayUs = 2'000;

    SaveDialog(kernel::Clock &clock, SavedataStore &store)
        : Dialog(DialogType::kSavedata, clock), store_(store) {}

    // sceUtilitySavedataInitStart: 0, error::kInvalidStatus or
    // error::kInvalidParamSize.
    int init(kernel::GuestMemory &memory, std::uint32_t address);
    // sceUtilitySavedataUpdate.
    int update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons);
    // sceUtilitySavedataShutdownStart.
    int shutdown(bool force = false);

    [[nodiscard]] SaveDialogView view() const;
    [[nodiscard]] const SavedataParam &request() const noexcept { return request_; }
    [[nodiscard]] SaveScreen screen() const noexcept { return display_; }
    // Set by init() for a mode this port does not implement.
    [[nodiscard]] std::optional<std::uint32_t> unsupported_mode() const noexcept {
        return unsupported_mode_;
    }

private:
    enum class IoStatus : std::uint8_t { kNone, kPending, kDone };

    [[nodiscard]] std::string selected_save_dir_name() const;
    [[nodiscard]] std::uint64_t entry_size(int idx) const;
    [[nodiscard]] bool is_mode(SavedataMode mode) const {
        return request_.mode == static_cast<std::uint32_t>(mode);
    }
    void start_io(kernel::GuestMemory &memory);
    void execute_io(kernel::GuestMemory &memory);
    void execute_invisible_io(kernel::GuestMemory &memory);
    // DisplaySaveList's cursor and DisplayMessage's yes/no.
    void move_list_cursor();
    void move_yes_no();
    void finish(std::int32_t result);

    SavedataStore &store_;
    SaveScreen display_ = SaveScreen::kNone;
    SavedataParam request_{};
    SavedataParam original_{};
    std::uint32_t address_ = 0;
    bool has_param_ = false;
    int current_selected_ = 0;
    int yes_no_choice_ = 1;
    IoStatus io_status_ = IoStatus::kNone;
    std::optional<std::uint32_t> unsupported_mode_;
    int up_held_ = 0;
    int down_held_ = 0;
};

} // namespace mpo::savedata
