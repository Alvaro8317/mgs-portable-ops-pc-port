// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceKernelModule.cpp and Core/ELF/ElfReader.cpp
// @ v1.20.4 (see modules.hpp for the functions).

#include "hle/modules.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>

#include "hle/system.hpp"
#include "psprecomp/interpreter.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kElfMagic = 0x464C457Fu; // "\x7FELF"
constexpr std::uint32_t kPspMagic = 0x5053507Eu; // "~PSP"
constexpr std::uint32_t kSceMagic = 0x4543537Eu; // "~SCE"
constexpr std::uint16_t kElfTypeExec = 2u;
constexpr std::uint32_t kPtLoad = 1u;
constexpr std::uint32_t kDefaultLoadAddress = 0x08804000u;
constexpr std::uint32_t kKernelModeAttr = 0x1000u;
constexpr std::uint32_t kThreadAttrUser = 0x80000000u;

// syslib NIDs (sceKernelModule.cpp).
constexpr std::uint32_t kNidModuleStart = 0xD632ACDBu;
constexpr std::uint32_t kNidModuleStop = 0xCEE8593Cu;
constexpr std::uint32_t kNidModuleStartThreadParameter = 0x0F7C276Cu;
constexpr std::uint32_t kNidModuleStopThreadParameter = 0xCF0CC697u;

namespace err {
constexpr std::uint32_t kError = 0x80020001u;
constexpr std::uint32_t kMemblockAllocFailed = 0x800200D9u;
constexpr std::uint32_t kUnknownModule = 0x8002012Eu;
constexpr std::uint32_t kAlreadyStopped = 0x80020135u;
constexpr std::uint32_t kUnsupportedPrxType = 0x80020148u;
} // namespace err

// PPSSPP Core/HLE/HLE.cpp g_moduleMeta: firmware modules replaced by HLE.
constexpr const char *kHleFirmwareModules[] = {
    "sceATRAC3plus_Library",
    "sceFont_Library",
    "SceFont_Library",
    "SceHttp_Library",
    "sceMpeg_library",
    "sceMp3_Library",
    "sceNetAdhocctl_Library",
    "sceNetAdhocDownload_Library",
    "sceNetAdhocMatching_Library",
    "sceNetApDialogDummy_Library",
    "sceNetAdhoc_Library",
    "sceNetApctl_Library",
    "sceNetInet_Library",
    "sceNetResolver_Library",
    "sceNet_Library",
    "sceNetAdhocAuth_Service",
    "sceNetIfhandle_Service",
    "sceSsl_Module",
    "sceDEFLATE_Library",
    "sceMD5_Library",
    "sceMemab",
    "sceAvcodec_driver",
    "sceAudiocodec_Driver",
    "sceAudiocodec",
    "sceVideocodec_Driver",
    "sceVideocodec",
    "sceMpegbase_Driver",
    "sceMpegbase",
    "scePsmf_library",
    "scePsmfP_library",
    "scePsmfPlayer",
    "sceSAScore",
    "sceCcc_Library",
    "SceParseHTTPheader_Library",
    "SceParseURI_Library",
    "sceJpeg",
};

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4u > bytes.size()) return 0u;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 2u > bytes.size()) return 0u;
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1u] << 8u));
}

bool equals_no_case(const std::string &a, const char *b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool valid_address(kernel::GuestMemory &memory, std::uint32_t address) {
    return address != 0u && memory.is_valid(address, 4u);
}

} // namespace

bool is_hle_firmware_module(const std::string &name) {
    return std::any_of(std::begin(kHleFirmwareModules), std::end(kHleFirmwareModules),
                       [&name](const char *meta) { return equals_no_case(name, meta); });
}

Module *ModuleManager::get(kernel::Uid uid) {
    const auto found = modules_.find(uid);
    return found != modules_.end() ? found->second.get() : nullptr;
}

const Module *ModuleManager::find(kernel::Uid uid) const {
    const auto found = modules_.find(uid);
    return found != modules_.end() ? found->second.get() : nullptr;
}

const Module *ModuleManager::find_by_name(const std::string &name) const {
    for (const auto &[uid, module] : modules_)
        if (module->name == name) return module.get();
    return nullptr;
}

std::vector<const Module *> ModuleManager::modules() const {
    std::vector<const Module *> out;
    for (const auto &[uid, module] : modules_) out.push_back(module.get());
    return out;
}

std::uint32_t ModuleManager::load_fake(std::span<const std::uint8_t> header,
                                       const std::string &path, bool from_top, std::string *error) {
    // The ~PSP header of an encrypted firmware module PPSSPP replaces with
    // HLE: reserve its memory like __KernelLoadELFFromPtr does, run nothing.
    char name_bytes[29]{};
    std::memcpy(name_bytes, header.data() + 0x0Au, 28u);
    const std::string name(name_bytes);
    const std::uint16_t attribute = read_u16(header, 0x04u);
    std::uint32_t total = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t seg_size = read_u32(header, 0x54u + 4u * static_cast<std::size_t>(i));
        if (seg_size == 0u) continue;
        const std::uint32_t align =
            static_cast<std::uint32_t>(read_u16(header, 0x3Cu + 2u * static_cast<std::size_t>(i))) -
            1u;
        total = ((total + align) & ~align) + seg_size;
    }
    kernel::Kernel &k = system_.kernel();
    kernel::BlockAllocator &blocks =
        (attribute & kKernelModeAttr) != 0u ? k.kernel_memory() : k.user_memory();
    std::uint32_t size = total;
    const std::string tag = "ELF/" + name;
    const std::uint32_t address = blocks.alloc(size, from_top, tag.c_str());
    if (address == kernel::BlockAllocator::kFailed) {
        if (error != nullptr) *error = "no memory for the fake module " + name;
        return err::kMemblockAllocFailed;
    }
    auto module = std::make_unique<Module>();
    module->name = name;
    module->path = path;
    module->fake = true;
    module->base = address;
    module->size = total;
    module->attributes = attribute;
    module->entry = 0xFFFFFFFFu;
    module->gp = 0xFFFFFFFFu;
    module->status = ModuleStatus::kLoaded;
    module->uid = k.create_host_object(name.c_str());
    const kernel::Uid uid = module->uid;
    system_.hle().trace_event("module %d '%s' (%s): fake (HLE firmware module), %08X..%08X", uid,
                              name.c_str(), path.c_str(), address, address + total);
    modules_.emplace(uid, std::move(module));
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t ModuleManager::load(std::span<const std::uint8_t> image, const std::string &path,
                                  std::uint32_t load_address, bool from_top, std::string *error) {
    if (read_u32(image, 0u) == kSceMagic) {
        const std::uint32_t header_size = read_u32(image, 4u);
        if (header_size < image.size()) image = image.subspan(header_size);
    }
    const std::uint32_t magic = read_u32(image, 0u);
    if (magic == kPspMagic && image.size() >= 0x150u) {
        char name_bytes[29]{};
        std::memcpy(name_bytes, image.data() + 0x0Au, 28u);
        if (is_hle_firmware_module(name_bytes)) return load_fake(image, path, from_top, error);
        if (error != nullptr)
            *error =
                std::string("encrypted module ") + name_bytes + " (" + path + ") is not supported";
        return err::kUnsupportedPrxType;
    }
    if (magic != kElfMagic) {
        if (error != nullptr) *error = path + ": not an ELF/PRX";
        return err::kUnsupportedPrxType;
    }

    kernel::Kernel &k = system_.kernel();
    psprecomp::Runtime &runtime = system_.runtime();
    std::uint32_t block = kernel::BlockAllocator::kFailed;
    kernel::BlockAllocator *blocks = nullptr;
    try {
        const psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_bytes(
            std::vector<std::uint8_t>(image.begin(), image.end()), path);
        std::uint32_t total_start = 0xFFFFFFFFu;
        std::uint32_t total_end = 0;
        for (const psprecomp::ElfSegment &segment : elf.segments()) {
            if (segment.type != kPtLoad) continue;
            total_start = std::min(total_start, segment.vaddr);
            total_end = std::max(total_end, segment.vaddr + segment.memory_size);
        }
        if (total_end <= total_start) {
            if (error != nullptr) *error = path + ": no PT_LOAD segment";
            return err::kUnsupportedPrxType;
        }
        const std::uint32_t total_size = total_end - total_start;
        const bool relocate = elf.type() != kElfTypeExec;

        // The module info's attributes decide the partition (ElfReader).
        std::uint16_t attributes = 0;
        for (const psprecomp::ElfSection &section : elf.sections()) {
            if (section.name == ".rodata.sceModuleInfo" || section.name == ".sceModuleInfo")
                attributes = read_u16(elf.bytes(), section.offset);
        }
        const bool in_user = total_start >= 0x08800000u;
        blocks = (attributes & kKernelModeAttr) != 0u && !in_user ? &k.kernel_memory()
                                                                  : &k.user_memory();
        const std::string tag = "ELF/" + path.substr(path.find_last_of("/:") + 1u);
        std::uint32_t size = total_size;
        if (!relocate) {
            block = blocks->alloc_at(total_start, total_size, tag.c_str());
        } else if (load_address != 0u) {
            block = blocks->alloc_at(load_address + total_start, total_size, tag.c_str());
        } else {
            block = blocks->alloc(size, from_top, tag.c_str());
        }
        if (block == kernel::BlockAllocator::kFailed) {
            if (error != nullptr) *error = path + ": no memory for the module";
            return err::kMemblockAllocFailed;
        }
        const std::uint32_t load_base = relocate ? block - total_start : 0u;
        (void)elf.load_and_relocate(runtime.memory(), load_base);
        const auto info = elf.find_module_info(runtime.memory(), load_base);
        if (!info) {
            blocks->free(block);
            if (error != nullptr) *error = path + ": no sceModuleInfo";
            return err::kUnsupportedPrxType;
        }

        auto module = std::make_unique<Module>();
        module->name = info->name;
        module->path = path;
        module->base = block;
        module->size = total_size;
        module->attributes = info->attributes;
        module->gp = info->gp;
        // ElfReader: e_entry -1 means none; __KernelLoadELFFromPtr falls back
        // to module_start for an entry of 0.
        module->entry = elf.entry() == 0xFFFFFFFFu
                            ? 0xFFFFFFFFu
                            : (relocate ? load_base + elf.entry() : elf.entry());
        read_exports(*module, info->ent_top, info->ent_end);
        if (module->entry == 0u) module->entry = module->start_func;
        module->imports = elf.scan_imports(runtime.memory(), *info);
        // PPSSPP loads a firmware module it replaces with HLE, but never runs it.
        module->fake = is_hle_firmware_module(module->name);
        module->uid = k.create_host_object(module->name.c_str());
        Module &m = *module;
        modules_.emplace(m.uid, std::move(module));
        link(m);
        system_.hle().cover_imports(m.imports);
        system_.hle().trace_event(
            "module %d '%s' (%s): %08X..%08X gp=%08X module_start=%08X, %zu exports, "
            "%zu imports",
            m.uid, m.name.c_str(), path.c_str(), m.base, m.base + m.size, m.gp, m.start_func,
            m.exports.size(), m.imports.size());
        const std::uint32_t code = m.start_func != 0u ? m.start_func : m.entry;
        if (!m.fake && code != 0u && code != 0xFFFFFFFFu && !runtime.has_function(code)) {
            std::fprintf(stderr,
                         "mpo: module '%s' loaded at %08X has no recompiled code at its entry %08X "
                         "(no corpus "
                         "for it, or one generated for another address)%s\n",
                         m.name.c_str(), m.base, code,
                         psprecomp::interpreter_fallback_enabled() ? "; the interpreter will run it"
                                                                   : "");
        }
        return static_cast<std::uint32_t>(m.uid);
    } catch (const std::exception &e) {
        if (block != kernel::BlockAllocator::kFailed && blocks != nullptr) blocks->free(block);
        if (error != nullptr) *error = path + ": " + e.what();
        return err::kUnsupportedPrxType;
    }
}

void ModuleManager::read_exports(Module &module, std::uint32_t ent_top, std::uint32_t ent_end) {
    kernel::GuestMemory &mem = system_.kernel().memory();
    std::uint32_t cursor = ent_top;
    while (cursor < ent_end && mem.is_valid(cursor, 16u)) {
        const std::uint32_t name_ptr = mem.read_u32(cursor);
        const std::uint32_t word2 = mem.read_u32(cursor + 8u);
        const std::uint32_t entry_words = word2 & 0xFFu;
        const std::uint32_t vcount = (word2 >> 8u) & 0xFFu;
        const std::uint32_t fcount = word2 >> 16u;
        const std::uint32_t resident = mem.read_u32(cursor + 12u);
        const std::uint32_t vcount_new =
            entry_words > 4u ? (mem.read_u32(cursor + 16u) & 0xFFFFu) : 0u;
        if (entry_words == 0u) {
            cursor += 16u;
            continue;
        }
        cursor += entry_words * 4u;
        const std::uint32_t vars = entry_words <= 4u ? vcount : std::max(vcount, vcount_new);
        const std::string library = name_ptr != 0u ? mem.read_c_string(name_ptr) : std::string();
        if (!valid_address(mem, resident)) continue;
        const std::uint32_t values = resident + 4u * (fcount + vars);
        for (std::uint32_t j = 0; j < fcount; ++j) {
            const std::uint32_t nid = mem.read_u32(resident + 4u * j);
            const std::uint32_t address = mem.read_u32(values + 4u * j);
            if (nid == kNidModuleStart) {
                module.start_func = address;
            } else if (nid == kNidModuleStop) {
                module.stop_func = address;
            } else if (!library.empty()) {
                module.exports.push_back({library, nid, address});
            }
        }
        for (std::uint32_t j = 0; j < vars; ++j) {
            const std::uint32_t nid = mem.read_u32(resident + 4u * (fcount + j));
            const std::uint32_t address = mem.read_u32(values + 4u * (fcount + j));
            const bool start = nid == kNidModuleStartThreadParameter;
            if (!start && nid != kNidModuleStopThreadParameter) continue;
            if (!valid_address(mem, address) || mem.read_u32(address) == 0u) continue;
            (start ? module.start_thread_priority : module.stop_thread_priority) =
                mem.read_u32(address + 4u);
            (start ? module.start_thread_stack_size : module.stop_thread_stack_size) =
                mem.read_u32(address + 8u);
            (start ? module.start_thread_attr : module.stop_thread_attr) =
                mem.read_u32(address + 12u);
        }
    }
}

void ModuleManager::link(Module &module) {
    HleTable &hle = system_.hle();
    for (const ModuleExport &exported : module.exports) {
        const HleTable::Entry *existing = hle.find(exported.library, exported.nid);
        if (existing != nullptr && existing->kind != HleKind::kUnimplemented &&
            existing->kind != HleKind::kLink)
            continue; // An HLE function wins over a game export.
        const std::uint32_t target = exported.address;
        hle.add(exported.library, exported.nid, hle.nid_name(exported.library, exported.nid),
                HleKind::kLink, [target](psprecomp::AllegrexContext &ctx) { ctx.pc = target; });
    }
}

void ModuleManager::unlink(const Module &module) {
    HleTable &hle = system_.hle();
    for (const ModuleExport &exported : module.exports) {
        const HleTable::Entry *existing = hle.find(exported.library, exported.nid);
        if (existing == nullptr || existing->kind != HleKind::kLink) continue;
        hle.add(exported.library, exported.nid, existing->name, HleKind::kUnimplemented, nullptr);
    }
}

std::uint32_t ModuleManager::boot(std::span<const std::uint8_t> image, const std::string &boot_path,
                                  std::string *error) {
    const std::uint32_t uid = load(image, boot_path, kDefaultLoadAddress, false, error);
    if (static_cast<std::int32_t>(uid) < 0) return uid;
    Module &m = *get(static_cast<kernel::Uid>(uid));
    if (m.fake) {
        if (error != nullptr) *error = boot_path + ": the boot module is a firmware module";
        return err::kUnsupportedPrxType;
    }
    // __KernelLoadExec's start options, replaced by the module's own.
    std::uint32_t attr = kThreadAttrUser;
    std::uint32_t priority = 0x20u;
    std::uint32_t stack_size = 0x40000u;
    if (m.start_thread_attr != 0u) attr = m.start_thread_attr;
    if (m.start_thread_priority != 0u) priority = m.start_thread_priority;
    if (m.start_thread_stack_size != 0u) stack_size = m.start_thread_stack_size;
    const std::uint32_t entry =
        m.start_func != 0u && m.start_func != 0xFFFFFFFFu ? m.start_func : m.entry;
    m.status = ModuleStatus::kStarted;

    kernel::Kernel &k = system_.kernel();
    const std::uint32_t thread =
        k.create_thread_for_module("root", m.uid, m.gp, entry, priority, stack_size, attr,
                                   (m.attributes & kKernelModeAttr) != 0u);
    if (static_cast<std::int32_t>(thread) < 0) {
        if (error != nullptr) *error = "cannot create the root thread";
        return thread;
    }
    // argv[0], copied below the root thread's stack top by StartThread.
    const std::uint32_t arg_size = static_cast<std::uint32_t>(boot_path.size() + 1u);
    const std::uint32_t args = system_.alloc_kernel_scratch(arg_size, "boot-args");
    k.memory().write_bytes(
        args, std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t *>(boot_path.c_str()), arg_size));
    function_threads_[static_cast<kernel::Uid>(thread)] = m.uid;
    const std::uint32_t started = k.start_thread(static_cast<kernel::Uid>(thread), arg_size, args);
    k.kernel_memory().free(args);
    if (started != 0u) {
        if (error != nullptr) *error = "cannot start the root thread";
        return started;
    }
    system_.hle().trace_event(
        "root thread %u: module_start %08X gp=%08X priority=%02X stack=%X attr=%08X", thread, entry,
        m.gp, priority, stack_size, attr);
    return uid;
}

std::uint32_t ModuleManager::start_module(kernel::Uid uid, std::uint32_t arg_size,
                                          std::uint32_t arg_addr, std::uint32_t status_addr,
                                          std::uint32_t option_addr) {
    kernel::Kernel &k = system_.kernel();
    Module *m = get(uid);
    if (m == nullptr) return err::kUnknownModule;
    if (m->fake) {
        if (status_addr != 0u) k.memory().write_u32(status_addr, 0u);
        return static_cast<std::uint32_t>(uid);
    }
    if (m->status == ModuleStatus::kStarted) return err::kError;

    // __KernelStartModule.
    std::uint32_t priority = 0x20u;
    std::uint32_t stack_size = 0x40000u;
    std::uint32_t attr = m->attributes;
    std::uint32_t entry = m->entry;
    if (m->start_func != 0u && m->start_func != 0xFFFFFFFFu) {
        entry = m->start_func;
        if (m->start_thread_attr != 0u) attr = m->start_thread_attr;
    }
    if (!valid_address(k.memory(), entry)) {
        if (entry == 0u || entry == 0xFFFFFFFFu) {
            m->status = ModuleStatus::kStarted;
            return static_cast<std::uint32_t>(uid);
        }
        return 0xFFFFFFFFu;
    }
    // SceKernelSMOption: size, mpidstack, stacksize, priority, attribute.
    const bool has_option = valid_address(k.memory(), option_addr);
    const std::int32_t option_priority =
        has_option ? static_cast<std::int32_t>(k.memory().read_u32(option_addr + 12u)) : 0;
    const std::int32_t option_stack =
        has_option ? static_cast<std::int32_t>(k.memory().read_u32(option_addr + 8u)) : 0;
    if (option_priority > 0) {
        priority = static_cast<std::uint32_t>(option_priority);
    } else if (static_cast<std::int32_t>(m->start_thread_priority) > 0) {
        priority = m->start_thread_priority;
    }
    if (option_stack > 0) {
        stack_size = static_cast<std::uint32_t>(option_stack);
    } else if (static_cast<std::int32_t>(m->start_thread_stack_size) > 0) {
        stack_size = m->start_thread_stack_size;
    }
    const std::uint32_t thread =
        k.create_thread_for_module(m->name.c_str(), uid, m->gp, entry, priority, stack_size, attr,
                                   (m->attributes & kKernelModeAttr) != 0u);
    if (static_cast<std::int32_t>(thread) < 0) return thread;
    function_threads_[static_cast<kernel::Uid>(thread)] = uid;
    m->status = ModuleStatus::kStarting;
    system_.hle().trace_event(
        "module %d '%s': module_start %08X on thread %u (priority %02X, stack %X)", uid,
        m->name.c_str(), entry, thread, priority, stack_size);
    // Starting a better thread switches to it at once; module_start may even
    // finish before we get the CPU back.
    (void)k.start_thread(static_cast<kernel::Uid>(thread), arg_size, arg_addr);
    m = get(uid);
    if (m == nullptr) return err::kUnknownModule;
    if (m->status != ModuleStatus::kStarting) {
        if (status_addr != 0u) k.memory().write_u32(status_addr, m->function_exit_status);
        return m->status == ModuleStatus::kStarted ? static_cast<std::uint32_t>(uid) : 0u;
    }
    m->waiters.push_back({k.current_thread(), status_addr});
    return k.wait_current_thread(kernel::WaitType::kModule, uid, 1u, 0u, false,
                                 static_cast<std::uint32_t>(uid));
}

std::uint32_t ModuleManager::stop_module(kernel::Uid uid, std::uint32_t arg_size,
                                         std::uint32_t arg_addr, std::uint32_t status_addr,
                                         std::uint32_t option_addr) {
    kernel::Kernel &k = system_.kernel();
    Module *m = get(uid);
    if (m == nullptr) return err::kUnknownModule;
    if (m->fake) {
        if (status_addr != 0u) k.memory().write_u32(status_addr, 0u);
        return 0u;
    }
    if (m->status != ModuleStatus::kStarted) return err::kAlreadyStopped;
    std::uint32_t priority = m->stop_thread_priority != 0u ? m->stop_thread_priority : 0x20u;
    std::uint32_t stack_size =
        m->stop_thread_stack_size != 0u ? m->stop_thread_stack_size : 0x40000u;
    std::uint32_t attr = m->stop_thread_attr;
    if (valid_address(k.memory(), option_addr) && k.memory().read_u32(option_addr) != 0u) {
        if (k.memory().read_u32(option_addr + 12u) != 0u)
            priority = k.memory().read_u32(option_addr + 12u);
        if (k.memory().read_u32(option_addr + 8u) != 0u)
            stack_size = k.memory().read_u32(option_addr + 8u);
        if (k.memory().read_u32(option_addr + 16u) != 0u)
            attr = k.memory().read_u32(option_addr + 16u);
    }
    if (!valid_address(k.memory(), m->stop_func)) {
        m->status = ModuleStatus::kStopped;
        return 0u;
    }
    const std::uint32_t thread =
        k.create_thread_for_module(m->name.c_str(), uid, m->gp, m->stop_func, priority, stack_size,
                                   attr, (m->attributes & kKernelModeAttr) != 0u);
    if (static_cast<std::int32_t>(thread) < 0) return thread;
    function_threads_[static_cast<kernel::Uid>(thread)] = uid;
    m->status = ModuleStatus::kStopping;
    (void)k.start_thread(static_cast<kernel::Uid>(thread), arg_size, arg_addr);
    m = get(uid);
    if (m == nullptr) return err::kUnknownModule;
    if (m->status != ModuleStatus::kStopping) {
        if (status_addr != 0u) k.memory().write_u32(status_addr, m->function_exit_status);
        return 0u;
    }
    m->waiters.push_back({k.current_thread(), status_addr});
    return k.wait_current_thread(kernel::WaitType::kModule, uid, 1u, 0u, false, 0u);
}

std::uint32_t ModuleManager::unload_module(kernel::Uid uid) {
    kernel::Kernel &k = system_.kernel();
    Module *m = get(uid);
    if (m == nullptr) return k.delay_result(err::kUnknownModule, 150u);
    unlink(*m);
    if (m->base != 0u) {
        if (!k.user_memory().free(m->base)) (void)k.kernel_memory().free(m->base);
    }
    system_.hle().trace_event("module %d '%s' unloaded", uid, m->name.c_str());
    (void)k.delete_host_object(uid);
    modules_.erase(uid);
    return k.delay_result(static_cast<std::uint32_t>(uid), 500u);
}

std::uint32_t ModuleManager::get_module_id() {
    kernel::ThreadInfo info;
    if (!system_.kernel().thread_info(system_.kernel().current_thread(), info)) return 0u;
    return static_cast<std::uint32_t>(info.module_id);
}

std::uint32_t ModuleManager::get_module_id_by_address(std::uint32_t address) const {
    for (const auto &[uid, module] : modules_) {
        if (module->base != 0u && module->base <= address && address < module->base + module->size)
            return static_cast<std::uint32_t>(uid);
    }
    return err::kUnknownModule;
}

void ModuleManager::on_thread_return(kernel::Uid thread, std::uint32_t v0) {
    // The thread exits below and its fiber is abandoned: no iterator may be
    // alive in this frame (checked iterators stay linked to their container;
    // see "Pendiente y trampas" in docs/sessions/F08b.md).
    kernel::Uid uid = 0;
    {
        const auto found = function_threads_.find(thread);
        if (found == function_threads_.end()) return;
        uid = found->second;
        function_threads_.erase(found);
    }
    Module *m = get(uid);
    if (m == nullptr) {
        system_.kernel().exit_delete_thread(v0);
        return;
    }
    finish_module_function(*m, thread, v0);
}

void ModuleManager::finish_module_function(Module &module, kernel::Uid thread,
                                           std::uint32_t exit_status) {
    // __KernelReturnFromModuleFunc: mark the module, hand the exit status to
    // the threads waiting in Start/StopModule, delete the thread.
    kernel::Kernel &k = system_.kernel();
    if (module.status == ModuleStatus::kStarting) module.status = ModuleStatus::kStarted;
    if (module.status == ModuleStatus::kStopping) module.status = ModuleStatus::kStopped;
    module.function_exit_status = exit_status;
    const kernel::Uid uid = module.uid;
    const bool started = module.status == ModuleStatus::kStarted;
    system_.hle().trace_event("module %d '%s': module function on thread %d returned %08X", uid,
                              module.name.c_str(), thread, exit_status);
    {
        // Scoped: the vector is gone before the thread exits (its fiber is
        // abandoned). resume_from_wait() may switch to a better waiter first.
        std::vector<Module::Waiter> waiters;
        waiters.swap(module.waiters);
        for (const Module::Waiter &waiter : waiters) {
            if (!k.is_waiting_for(waiter.thread, kernel::WaitType::kModule, uid)) continue;
            if (waiter.status_addr != 0u) k.memory().write_u32(waiter.status_addr, exit_status);
            k.resume_from_wait(waiter.thread, started ? static_cast<std::uint32_t>(uid) : 0u);
        }
    }
    k.exit_delete_thread(exit_status);
}

} // namespace mpo::hle
