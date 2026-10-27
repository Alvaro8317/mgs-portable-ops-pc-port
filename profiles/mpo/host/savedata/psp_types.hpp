// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtility dialog constants and guest structure layouts (F18a).
//
// Values and layouts come from PPSSPP v1.20.4 (Core/Dialog/PSPDialog.h,
// PSPMsgDialog.h, PSPOskDialog.h, SavedataParam.h, Core/HLE/ErrorCodes.h,
// Core/HLE/sceCtrl.h), which match the PSPSDK headers. Every structure is a
// plain little-endian image of guest memory: the host is little-endian too
// (CLAUDE.md §5), so they are copied in and out with memcpy. The offsets that
// MPO touches are pinned with static_asserts (docs/analysis/utility.md).

#pragma once

#include <cstddef>
#include <cstdint>

namespace mpo::savedata {

// sceUtility*GetStatus values.
enum class DialogStatus : std::int32_t {
    kNone = 0,
    kInitialize = 1,
    kRunning = 2,
    kFinished = 3,
    kShutdown = 4,
};

// The dialog kinds that share sceUtility's "one dialog at a time" rule.
enum class DialogType : std::uint8_t { kNone, kSavedata, kMsg, kOsk };

// pspUtilityDialogCommon::result values written by the dialogs.
inline constexpr std::int32_t kDialogResultSuccess = 0;
inline constexpr std::int32_t kDialogResultCancel = 1;
inline constexpr std::int32_t kDialogResultAbort = 2;

namespace error {
// Kept as unsigned firmware codes; the HLE returns them as int32.
inline constexpr std::uint32_t kInvalidStatus = 0x80110001u;
inline constexpr std::uint32_t kInvalidParamSize = 0x80110004u;
inline constexpr std::uint32_t kWrongType = 0x80110005u;
inline constexpr std::uint32_t kMsgDialogBadOption = 0x80110501u;
inline constexpr std::uint32_t kMsgDialogErrorCodeInvalid = 0x80110502u;

inline constexpr std::uint32_t kSavedataLoadNoMs = 0x80110301u;
inline constexpr std::uint32_t kSavedataLoadDataBroken = 0x80110306u;
inline constexpr std::uint32_t kSavedataLoadNoData = 0x80110307u;
inline constexpr std::uint32_t kSavedataLoadParam = 0x80110308u;
inline constexpr std::uint32_t kSavedataLoadFileNotFound = 0x80110309u;
inline constexpr std::uint32_t kSavedataRwNoMemstick = 0x80110321u;
inline constexpr std::uint32_t kSavedataRwMemstickFull = 0x80110323u;
inline constexpr std::uint32_t kSavedataRwDataBroken = 0x80110326u;
inline constexpr std::uint32_t kSavedataRwNoData = 0x80110327u;
inline constexpr std::uint32_t kSavedataRwBadParams = 0x80110328u;
inline constexpr std::uint32_t kSavedataRwFileNotFound = 0x80110329u;
inline constexpr std::uint32_t kSavedataSaveNoMs = 0x80110381u;
inline constexpr std::uint32_t kSavedataSaveMsNoSpace = 0x80110383u;
inline constexpr std::uint32_t kSavedataSaveParam = 0x80110388u;
inline constexpr std::uint32_t kSavedataDeleteNoMs = 0x80110341u;
inline constexpr std::uint32_t kSavedataDeleteNoData = 0x80110347u;
inline constexpr std::uint32_t kSavedataSizesNoMs = 0x801103C1u;
inline constexpr std::uint32_t kSavedataSizesNoData = 0x801103C7u;
} // namespace error

// sceCtrl button bits (PSP_CTRL_*).
namespace button {
inline constexpr std::uint32_t kSelect = 0x0001u;
inline constexpr std::uint32_t kStart = 0x0008u;
inline constexpr std::uint32_t kUp = 0x0010u;
inline constexpr std::uint32_t kRight = 0x0020u;
inline constexpr std::uint32_t kDown = 0x0040u;
inline constexpr std::uint32_t kLeft = 0x0080u;
inline constexpr std::uint32_t kLTrigger = 0x0100u;
inline constexpr std::uint32_t kRTrigger = 0x0200u;
inline constexpr std::uint32_t kTriangle = 0x1000u;
inline constexpr std::uint32_t kCircle = 0x2000u;
inline constexpr std::uint32_t kCross = 0x4000u;
inline constexpr std::uint32_t kSquare = 0x8000u;
} // namespace button

// pspUtilityDialogCommon (0x30 bytes), at the start of every request.
struct DialogCommon {
    std::uint32_t size;
    std::int32_t language;
    std::int32_t button_swap; // 1: cross confirms
    std::int32_t graphics_thread;
    std::int32_t access_thread; // priority of the firmware's job thread
    std::int32_t font_thread;
    std::int32_t sound_thread;
    std::int32_t result;
    std::int32_t reserved[4];
};
static_assert(sizeof(DialogCommon) == 0x30);
static_assert(offsetof(DialogCommon, result) == 0x1C);

// --- sceUtilityMsgDialog ----------------------------------------------------

namespace msg_option {
inline constexpr std::uint32_t kErrorSound = 0x000u;
inline constexpr std::uint32_t kTextSound = 0x001u;
inline constexpr std::uint32_t kNoSound = 0x002u;
inline constexpr std::uint32_t kYesNo = 0x010u;
inline constexpr std::uint32_t kOk = 0x020u;
inline constexpr std::uint32_t kNoCancel = 0x080u;
inline constexpr std::uint32_t kDefaultNo = 0x100u;
inline constexpr std::uint32_t kSupported = 0x1B3u;
} // namespace msg_option

inline constexpr std::uint32_t kMsgDialogSizeV1 = 572;
inline constexpr std::uint32_t kMsgDialogSizeV2 = 580;
inline constexpr std::uint32_t kMsgDialogSizeV3 = 708;

struct MsgDialogParam {
    DialogCommon common;
    std::int32_t result;
    std::int32_t type; // 0: error code, 1: string
    std::uint32_t error_num;
    char string[512];
    // End of V1 (572 bytes).
    std::uint32_t options;
    std::uint32_t button_pressed; // 0 none, 1 yes/ok, 2 no, 3 back
    // End of V2 (580 bytes).
    char okay_button[64];
    char cancel_button[64];
    // End of V3 (708 bytes).
};
static_assert(sizeof(MsgDialogParam) == kMsgDialogSizeV3);
static_assert(offsetof(MsgDialogParam, options) == kMsgDialogSizeV1);
static_assert(offsetof(MsgDialogParam, okay_button) == kMsgDialogSizeV2);

// --- sceUtilityOsk ----------------------------------------------------------

namespace osk_input {
inline constexpr std::int32_t kAll = 0x00;
inline constexpr std::int32_t kLatinDigit = 0x01;
inline constexpr std::int32_t kLatinSymbol = 0x02;
inline constexpr std::int32_t kLatinLowercase = 0x04;
inline constexpr std::int32_t kLatinUppercase = 0x08;
} // namespace osk_input

namespace osk_result {
inline constexpr std::int32_t kUnchanged = 0;
inline constexpr std::int32_t kCancelled = 1;
inline constexpr std::int32_t kChanged = 2;
} // namespace osk_result

// SceUtilityOskData: one input field (0x34 bytes). The pointers are guest
// addresses of NUL-terminated UCS-2 strings.
struct OskFieldData {
    std::int32_t unk_00;
    std::int32_t unk_04;
    std::int32_t language;
    std::int32_t unk_12;
    std::int32_t input_type;
    std::int32_t lines;
    std::int32_t unk_24;
    std::uint32_t desc;
    std::uint32_t intext;
    std::uint32_t outtext_length; // in u16, including the terminator
    std::uint32_t outtext;
    std::int32_t result;
    std::uint32_t outtext_limit; // characters, not counting the terminator
};
static_assert(sizeof(OskFieldData) == 0x34);
static_assert(offsetof(OskFieldData, result) == 0x2C);

// SceUtilityOskParams (0x40 bytes).
struct OskParams {
    DialogCommon common;
    std::int32_t field_count;
    std::uint32_t fields; // guest address of OskFieldData[field_count]
    std::int32_t state;
    std::int32_t unk_60;
};
static_assert(sizeof(OskParams) == 0x40);

// --- sceUtilitySavedata -----------------------------------------------------

enum class SavedataMode : std::uint32_t {
    kAutoLoad = 0,
    kAutoSave = 1,
    kLoad = 2,
    kSave = 3,
    kListLoad = 4,
    kListSave = 5,
    kListDelete = 6,
    kListAllDelete = 7,
    kSizes = 8,
    kAutoDelete = 9,
    kDelete = 10,
    kList = 11,
    kFiles = 12,
    kMakeDataSecure = 13,
    kMakeData = 14,
    kReadDataSecure = 15,
    kReadData = 16,
    kWriteDataSecure = 17,
    kWriteData = 18,
    kEraseSecure = 19,
    kErase = 20,
    kDeleteData = 21,
    kGetSize = 22,
};

enum class SavedataFocus : std::uint32_t {
    kName = 0,
    kFirstList = 1,
    kLastList = 2,
    kLatest = 3,
    kOldest = 4,
    kFirstData = 5,
    kLastData = 6,
    kFirstEmpty = 7,
    kLastEmpty = 8,
};

inline constexpr std::uint32_t kSavedataSizeV1 = 1480;
inline constexpr std::uint32_t kSavedataSizeV2 = 1500;
inline constexpr std::uint32_t kSavedataSizeV3 = 1536;

using SaveName = char[20];

struct SavedataSfoParam {
    char title[0x80];
    char savedata_title[0x80];
    char detail[0x400];
    std::uint8_t parental_level;
    std::uint8_t unknown[3];
};
static_assert(sizeof(SavedataSfoParam) == 0x504);

struct SavedataFileData {
    std::uint32_t buf;
    std::uint32_t buf_size;
    std::uint32_t size;
    std::int32_t unknown;
};
static_assert(sizeof(SavedataFileData) == 16);

// SceUtilitySavedataParam (V3, 1536 bytes; V1 and V2 are prefixes).
struct SavedataParam {
    DialogCommon common;
    std::uint32_t mode; // SavedataMode
    std::int32_t bind;
    std::int32_t overwrite_mode;
    char game_name[13];
    char unused[3];
    SaveName save_name;
    std::uint32_t save_name_list; // guest SaveName[], ended by an empty name
    char file_name[13];
    char unused2[3];
    std::uint32_t data_buf;
    std::uint32_t data_buf_size;
    std::uint32_t data_size;
    SavedataSfoParam sfo_param;
    SavedataFileData icon0_file_data;
    SavedataFileData icon1_file_data;
    SavedataFileData pic1_file_data;
    SavedataFileData snd0_file_data;
    std::uint32_t new_data; // guest SavedataFileData*
    std::uint32_t focus;    // SavedataFocus
    std::int32_t abort_status;
    std::uint32_t ms_free;
    std::uint32_t ms_data;
    std::uint32_t utility_data;
    std::uint8_t key[16];
    std::uint32_t secure_version;
    std::int32_t multi_status;
    std::uint32_t id_list;
    std::uint32_t file_list;
    std::uint32_t size_info;
};
static_assert(sizeof(SavedataParam) == kSavedataSizeV3);
// The offsets MPO's stages write (relative to the request; see utility.md).
static_assert(offsetof(SavedataParam, mode) == 0x30);
static_assert(offsetof(SavedataParam, overwrite_mode) == 0x38);
static_assert(offsetof(SavedataParam, game_name) == 0x3C);
static_assert(offsetof(SavedataParam, save_name) == 0x4C);
static_assert(offsetof(SavedataParam, save_name_list) == 0x60);
static_assert(offsetof(SavedataParam, file_name) == 0x64);
static_assert(offsetof(SavedataParam, data_buf) == 0x74);
static_assert(offsetof(SavedataParam, data_size) == 0x7C);
static_assert(offsetof(SavedataParam, sfo_param) == 0x80);
static_assert(offsetof(SavedataParam, icon0_file_data) == 0x584);
static_assert(offsetof(SavedataParam, focus) == 0x5C8);
static_assert(offsetof(SavedataParam, key) == 0x5DC);
static_assert(offsetof(SavedataParam, secure_version) == 0x5EC);

} // namespace mpo::savedata
