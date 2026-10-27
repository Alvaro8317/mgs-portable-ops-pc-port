// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/PSPSaveDialog.cpp @ v1.20.4 (see
// save_dialog.hpp).

#include "savedata/save_dialog.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::savedata {
namespace {

bool is_valid_size(std::uint32_t size) {
    return size == kSavedataSizeV1 || size == kSavedataSizeV2 || size == kSavedataSizeV3;
}

bool is_unsupported(std::uint32_t mode) {
    switch (static_cast<SavedataMode>(mode)) {
    case SavedataMode::kSizes:
    case SavedataMode::kList:
    case SavedataMode::kFiles:
    case SavedataMode::kGetSize:
    case SavedataMode::kEraseSecure:
    case SavedataMode::kErase:
        return true;
    default:
        return mode > static_cast<std::uint32_t>(SavedataMode::kGetSize);
    }
}

const char *message_for(SaveScreen screen) {
    switch (screen) {
    case SaveScreen::kSaveConfirm:
        return "Do you want to save this data?";
    case SaveScreen::kSaveConfirmOverwrite:
        return "Do you want to overwrite the data?";
    case SaveScreen::kSaveSaving:
        return "Saving\nPlease Wait...";
    case SaveScreen::kSaveFailed:
        return "Unable to save data.";
    case SaveScreen::kSaveDone:
        return "Save completed";
    case SaveScreen::kLoadConfirm:
        return "Load this data?";
    case SaveScreen::kLoadLoading:
        return "Loading\nPlease Wait...";
    case SaveScreen::kLoadFailed:
        return "Load failed\nThe data is corrupted.";
    case SaveScreen::kLoadDone:
        return "Load completed";
    case SaveScreen::kLoadNoData:
    case SaveScreen::kDeleteNoData:
        return "There is no data";
    case SaveScreen::kDeleteConfirm:
        return "This save data will be deleted.\nAre you sure you want to continue?";
    case SaveScreen::kDeleteDeleting:
        return "Deleting\nPlease Wait...";
    case SaveScreen::kDeleteFailed:
        return "Unable to delete data.";
    case SaveScreen::kDeleteDone:
        return "Delete completed";
    default:
        return "";
    }
}

} // namespace

int SaveDialog::init(kernel::GuestMemory &memory, std::uint32_t address) {
    if (status() != DialogStatus::kNone) return static_cast<int>(error::kInvalidStatus);
    io_status_ = IoStatus::kNone;
    unsupported_mode_.reset();

    address_ = address;
    const std::uint32_t size = memory.read_u32(address);
    if (!is_valid_size(size)) return static_cast<int>(error::kInvalidParamSize);
    const std::uint8_t *host = memory.translate(address, size);
    if (host == nullptr) return static_cast<int>(error::kInvalidParamSize);
    request_ = SavedataParam{};
    std::memcpy(&request_, host, size);
    original_ = request_;
    has_param_ = true;
    store_.set_param(memory, &request_);

    yes_no_choice_ = 1;
    current_selected_ = store_.focus_index(request_);

    switch (static_cast<SavedataMode>(request_.mode)) {
    case SavedataMode::kLoad:
        if (entry_size(0) != 0) {
            const SaveFileInfo *info = store_.entry(0);
            if (info != nullptr && info->broken) {
                request_.common.result = static_cast<std::int32_t>(error::kSavedataLoadDataBroken);
                display_ = SaveScreen::kLoadFailed;
            } else {
                display_ = SaveScreen::kLoadConfirm;
            }
        } else {
            display_ = SaveScreen::kLoadNoData;
        }
        break;
    case SavedataMode::kListLoad:
        display_ =
            store_.listed_count() == 0 ? SaveScreen::kLoadNoData : SaveScreen::kLoadListChoice;
        break;
    case SavedataMode::kSave:
        if (entry_size(0) != 0) {
            yes_no_choice_ = 0;
            display_ = SaveScreen::kSaveConfirmOverwrite;
        } else {
            display_ = SaveScreen::kSaveConfirm;
        }
        break;
    case SavedataMode::kListSave:
        display_ = SaveScreen::kSaveListChoice;
        break;
    case SavedataMode::kListAllDelete:
    case SavedataMode::kListDelete:
        display_ =
            store_.listed_count() == 0 ? SaveScreen::kDeleteNoData : SaveScreen::kDeleteListChoice;
        break;
    case SavedataMode::kDelete:
        if (entry_size(0) != 0) {
            yes_no_choice_ = 0;
            display_ = SaveScreen::kDeleteConfirm;
        } else {
            display_ = SaveScreen::kDeleteNoData;
        }
        break;
    default:
        // AUTOLOAD, AUTOSAVE, AUTODELETE and the data modes show nothing.
        display_ = SaveScreen::kNone;
        if (is_unsupported(request_.mode)) {
            unsupported_mode_ = request_.mode;
            request_.common.result = 0;
        }
        break;
    }

    change_status_init(kInitDelayUs, &request_.common);
    update_common(&request_.common);
    update_buttons({});
    start_fade(true);
    return 0;
}

std::uint64_t SaveDialog::entry_size(int idx) const {
    const SaveFileInfo *info = store_.entry(idx);
    return info != nullptr ? info->size : 0;
}

std::string SaveDialog::selected_save_dir_name() const {
    if (!SavedataStore::would_have_multi_save_name(request_)) {
        return store_.save_dir_name(request_);
    }
    return store_.save_dir_name(request_, current_selected_);
}

void SaveDialog::start_io(kernel::GuestMemory &memory) {
    io_status_ = IoStatus::kPending;
    execute_io(memory);
    io_status_ = IoStatus::kDone;
}

void SaveDialog::execute_io(kernel::GuestMemory &memory) {
    std::int32_t &result = request_.common.result;
    switch (display_) {
    case SaveScreen::kLoadLoading:
        result = store_.load(memory, request_, selected_save_dir_name());
        display_ = result == 0 ? SaveScreen::kLoadDone : SaveScreen::kLoadFailed;
        break;
    case SaveScreen::kSaveSaving:
        display_ = store_.save(memory, request_, selected_save_dir_name()) == 0
                       ? SaveScreen::kSaveDone
                       : SaveScreen::kSaveFailed;
        break;
    case SaveScreen::kDeleteDeleting:
        if (store_.remove(request_, current_selected_)) {
            result = 0;
            display_ = SaveScreen::kDeleteDone;
        } else {
            display_ = SaveScreen::kDeleteFailed;
        }
        break;
    case SaveScreen::kNone:
        execute_invisible_io(memory);
        break;
    default:
        break;
    }
}

void SaveDialog::execute_invisible_io(kernel::GuestMemory &memory) {
    std::int32_t &result = request_.common.result;
    const auto mode = static_cast<SavedataMode>(request_.mode);
    switch (mode) {
    case SavedataMode::kLoad:
    case SavedataMode::kAutoLoad:
        result = store_.load(memory, request_, selected_save_dir_name());
        break;
    case SavedataMode::kSave:
    case SavedataMode::kAutoSave:
        result = store_.save(memory, request_, selected_save_dir_name());
        break;
    case SavedataMode::kDeleteData:
        // The non-list modes have exactly one entry.
        result =
            store_.remove(request_, 0) ? 0 : static_cast<std::int32_t>(error::kSavedataRwNoData);
        break;
    case SavedataMode::kAutoDelete:
    case SavedataMode::kDelete:
        result = store_.remove(request_, 0)
                     ? 0
                     : static_cast<std::int32_t>(error::kSavedataDeleteNoData);
        break;
    case SavedataMode::kMakeData:
    case SavedataMode::kMakeDataSecure:
        result = store_.save(memory, request_, selected_save_dir_name(),
                             mode == SavedataMode::kMakeDataSecure);
        if (result == static_cast<std::int32_t>(error::kSavedataSaveMsNoSpace)) {
            result = static_cast<std::int32_t>(error::kSavedataRwMemstickFull);
        }
        break;
    case SavedataMode::kWriteData:
    case SavedataMode::kWriteDataSecure:
        result = store_.save(memory, request_, selected_save_dir_name(),
                             mode == SavedataMode::kWriteDataSecure);
        break;
    case SavedataMode::kReadData:
    case SavedataMode::kReadDataSecure:
        result = store_.load(memory, request_, selected_save_dir_name(),
                             mode == SavedataMode::kReadDataSecure);
        if (result == static_cast<std::int32_t>(error::kSavedataLoadDataBroken)) {
            result = static_cast<std::int32_t>(error::kSavedataRwDataBroken);
        }
        if (result == static_cast<std::int32_t>(error::kSavedataLoadNoData)) {
            result = static_cast<std::int32_t>(error::kSavedataRwNoData);
        }
        break;
    default:
        // Unsupported (see unsupported_mode()).
        break;
    }
}

void SaveDialog::move_list_cursor() {
    if ((is_button_pressed(button::kUp) || is_button_held(button::kUp, up_held_)) &&
        current_selected_ > 0) {
        --current_selected_;
    } else if ((is_button_pressed(button::kDown) || is_button_held(button::kDown, down_held_)) &&
               current_selected_ < store_.listed_count() - 1) {
        ++current_selected_;
    }
}

void SaveDialog::move_yes_no() {
    if (is_button_pressed(button::kLeft) && yes_no_choice_ == 0) {
        yes_no_choice_ = 1;
    } else if (is_button_pressed(button::kRight) && yes_no_choice_ == 1) {
        yes_no_choice_ = 0;
    }
}

void SaveDialog::finish(std::int32_t result) {
    request_.common.result = result;
    start_fade(false);
}

int SaveDialog::update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons) {
    if (status() != DialogStatus::kRunning) return static_cast<int>(error::kInvalidStatus);
    if (!has_param_) {
        change_status_shutdown(kShutdownDelayUs, nullptr);
        return 0;
    }
    if (pending_status() != DialogStatus::kRunning) return 0;

    // The game may change the request while the dialog runs.
    const std::uint32_t size =
        std::min<std::uint32_t>(sizeof(original_), memory.read_u32(address_));
    if (const std::uint8_t *current = memory.translate(address_, size);
        current != nullptr && std::memcmp(current, &original_, size) != 0) {
        request_ = SavedataParam{};
        std::memcpy(&request_, current, size);
        original_ = request_;
        store_.set_param(memory, &request_);
    }

    update_buttons(buttons);
    update_fade(anim_speed);
    update_common(&request_.common);
    const std::uint32_t ok = ok_button();
    const std::uint32_t cancel = cancel_button();
    const bool list_delete =
        is_mode(SavedataMode::kListDelete) || is_mode(SavedataMode::kListAllDelete);

    switch (display_) {
    case SaveScreen::kSaveListChoice:
        move_list_cursor();
        if (is_button_pressed(cancel)) {
            finish(kDialogResultCancel);
        } else if (is_button_pressed(ok)) {
            if (entry_size(current_selected_) > 0) {
                yes_no_choice_ = 0;
                display_ = SaveScreen::kSaveConfirmOverwrite;
            } else {
                display_ = SaveScreen::kSaveSaving;
                start_io(memory);
            }
        }
        break;
    case SaveScreen::kSaveConfirm:
        move_yes_no();
        if (is_button_pressed(cancel) || (is_button_pressed(ok) && yes_no_choice_ == 0)) {
            finish(kDialogResultCancel);
        } else if (is_button_pressed(ok)) {
            display_ = SaveScreen::kSaveSaving;
            start_io(memory);
        }
        break;
    case SaveScreen::kSaveConfirmOverwrite:
        move_yes_no();
        if (is_button_pressed(cancel) || (is_button_pressed(ok) && yes_no_choice_ == 0)) {
            if (!is_mode(SavedataMode::kSave)) {
                display_ = SaveScreen::kSaveListChoice;
            } else {
                finish(kDialogResultCancel);
            }
        } else if (is_button_pressed(ok)) {
            display_ = SaveScreen::kSaveSaving;
            start_io(memory);
        }
        break;
    case SaveScreen::kSaveFailed:
        if (is_button_pressed(cancel)) {
            if (!is_mode(SavedataMode::kSave)) {
                display_ = SaveScreen::kSaveListChoice;
            } else {
                finish(kDialogResultCancel);
            }
        }
        break;
    case SaveScreen::kSaveDone:
        if (io_status_ == IoStatus::kDone) {
            // PPSSPP refreshes the list once its IO thread is joined.
            io_status_ = IoStatus::kNone;
            store_.set_param(memory, &request_);
        }
        if (is_button_pressed(cancel)) {
            if (const SaveFileInfo *info = store_.entry(current_selected_)) {
                store_.set_selected_save(info->idx);
            }
            finish(kDialogResultSuccess);
        }
        break;

    case SaveScreen::kLoadListChoice:
        move_list_cursor();
        if (is_button_pressed(cancel)) {
            finish(kDialogResultCancel);
        } else if (is_button_pressed(ok)) {
            display_ = SaveScreen::kLoadLoading;
            start_io(memory);
        }
        break;
    case SaveScreen::kLoadConfirm:
        move_yes_no();
        if (is_button_pressed(cancel) || (is_button_pressed(ok) && yes_no_choice_ == 0)) {
            finish(kDialogResultCancel);
        } else if (is_button_pressed(ok)) {
            display_ = SaveScreen::kLoadLoading;
            start_io(memory);
        }
        break;
    case SaveScreen::kLoadFailed:
        if (is_button_pressed(cancel)) {
            if (!is_mode(SavedataMode::kLoad)) {
                display_ = SaveScreen::kLoadListChoice;
            } else {
                start_fade(false); // the failure code stays in common.result
            }
        }
        break;
    case SaveScreen::kLoadDone:
        if (is_button_pressed(cancel) || is_button_pressed(ok)) {
            if (const SaveFileInfo *info = store_.entry(current_selected_)) {
                store_.set_selected_save(info->idx);
            }
            finish(kDialogResultSuccess);
        }
        break;
    case SaveScreen::kLoadNoData:
        if (is_button_pressed(cancel)) {
            finish(static_cast<std::int32_t>(error::kSavedataLoadNoData));
        }
        break;

    case SaveScreen::kDeleteListChoice:
        move_list_cursor();
        if (is_button_pressed(cancel)) {
            finish(kDialogResultCancel);
        } else if (is_button_pressed(ok)) {
            yes_no_choice_ = 0;
            display_ = SaveScreen::kDeleteConfirm;
        }
        break;
    case SaveScreen::kDeleteConfirm:
        move_yes_no();
        if (is_button_pressed(cancel) || (is_button_pressed(ok) && yes_no_choice_ == 0)) {
            if (list_delete) {
                display_ = SaveScreen::kDeleteListChoice;
            } else {
                finish(kDialogResultCancel);
            }
        } else if (is_button_pressed(ok)) {
            display_ = SaveScreen::kDeleteDeleting;
            start_io(memory);
        }
        break;
    case SaveScreen::kDeleteFailed:
        if (is_button_pressed(cancel)) {
            if (list_delete) {
                display_ = SaveScreen::kDeleteListChoice;
            } else {
                start_fade(false);
            }
        }
        break;
    case SaveScreen::kDeleteDone:
        if (io_status_ == IoStatus::kDone) {
            io_status_ = IoStatus::kNone;
            store_.set_param(memory, &request_);
        }
        if (is_button_pressed(cancel)) {
            if (store_.listed_count() == 0) {
                display_ = SaveScreen::kDeleteNoData;
            } else if (list_delete) {
                current_selected_ = std::min(current_selected_, store_.listed_count() - 1);
                display_ = SaveScreen::kDeleteListChoice;
            } else {
                finish(kDialogResultSuccess);
            }
        }
        break;
    case SaveScreen::kDeleteNoData:
        if (is_button_pressed(cancel)) {
            finish(static_cast<std::int32_t>(error::kSavedataDeleteNoData));
        }
        break;

    case SaveScreen::kNone:
        if (io_status_ == IoStatus::kNone) {
            start_io(memory);
        } else {
            change_status(DialogStatus::kFinished, 0);
        }
        break;
    default:
        // kSaveSaving, kLoadLoading and kDeleteDeleting never last past
        // start_io (the work is synchronous).
        change_status(DialogStatus::kFinished, 0);
        break;
    }

    if (read_status() == DialogStatus::kFinished || pending_status() == DialogStatus::kFinished) {
        const std::uint32_t out = std::min<std::uint32_t>(request_.common.size, sizeof(request_));
        memory.write_bytes(address_, {reinterpret_cast<const std::uint8_t *>(&request_), out});
    }
    return 0;
}

int SaveDialog::shutdown(bool force) {
    if (status() != DialogStatus::kFinished && !force) {
        return static_cast<int>(error::kInvalidStatus);
    }
    io_status_ = IoStatus::kNone;
    shutdown_base(force);
    if (!force) change_status_shutdown(kShutdownDelayUs, &request_.common);
    has_param_ = false;
    return 0;
}

SaveDialogView SaveDialog::view() const {
    SaveDialogView out;
    out.screen = display_;
    switch (display_) {
    case SaveScreen::kSaveListChoice:
    case SaveScreen::kSaveConfirm:
    case SaveScreen::kSaveConfirmOverwrite:
    case SaveScreen::kSaveSaving:
    case SaveScreen::kSaveDone:
    case SaveScreen::kSaveFailed:
        out.banner = SaveBanner::kSave;
        break;
    case SaveScreen::kLoadListChoice:
    case SaveScreen::kLoadConfirm:
    case SaveScreen::kLoadLoading:
    case SaveScreen::kLoadDone:
    case SaveScreen::kLoadNoData:
    case SaveScreen::kLoadFailed:
        out.banner = SaveBanner::kLoad;
        break;
    case SaveScreen::kNone:
        out.banner = SaveBanner::kNone;
        break;
    default:
        out.banner = SaveBanner::kDelete;
        break;
    }
    const auto &entries = store_.entries();
    const auto count = std::min<std::size_t>(
        entries.size(), static_cast<std::size_t>(std::max(store_.listed_count(), 0)));
    out.entries.assign(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(count));
    out.selected = current_selected_;
    out.message = message_for(display_);
    out.has_yes_no = display_ == SaveScreen::kSaveConfirm ||
                     display_ == SaveScreen::kSaveConfirmOverwrite ||
                     display_ == SaveScreen::kLoadConfirm || display_ == SaveScreen::kDeleteConfirm;
    out.yes_selected = yes_no_choice_ != 0;
    const bool list_or_confirm = display_ == SaveScreen::kSaveListChoice ||
                                 display_ == SaveScreen::kLoadListChoice ||
                                 display_ == SaveScreen::kDeleteListChoice || out.has_yes_no;
    out.shows_ok_button = list_or_confirm;
    out.shows_back_button = display_ != SaveScreen::kNone && display_ != SaveScreen::kSaveSaving &&
                            display_ != SaveScreen::kLoadLoading &&
                            display_ != SaveScreen::kDeleteDeleting;
    out.fade = fade_value();
    out.ok_button = ok_button();
    out.cancel_button = cancel_button();
    return out;
}

} // namespace mpo::savedata
