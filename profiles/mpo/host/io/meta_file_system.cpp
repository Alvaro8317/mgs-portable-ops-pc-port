// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/FileSystems/MetaFileSystem.cpp @ v1.20.4.

#include "io/meta_file_system.hpp"

#include <cctype>

namespace mpo::io {

namespace {

bool equal_no_case(std::string_view a, std::string_view b, std::size_t length) {
    if (a.size() < length || b.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// ApplyPathStringToComponentsVector.
void apply_components(std::vector<std::string> &components, const std::string &path) {
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos) end = path.size();
        if (end > start) {
            const std::string component = path.substr(start, end - start);
            if (component == "..") {
                // The root is its own parent.
                if (!components.empty()) components.pop_back();
            } else if (component != ".") {
                components.push_back(component);
            }
        }
        start = end + 1u;
    }
}

std::string normalize_prefix(std::string prefix) {
    if (prefix == "memstick:") prefix = "ms0:";
    return prefix;
}

} // namespace

bool real_path(const std::string &current, const std::string &in, std::string &out) {
    if (in.empty()) {
        out = current;
        return true;
    }
    const std::size_t colon = in.find(':');
    if (colon != std::string::npos && colon + 1u == in.size()) {
        out = in; // Just the device, "umd0:".
        return true;
    }
    std::vector<std::string> components;
    std::string prefix;
    std::string after;
    if (colon == std::string::npos) {
        const std::size_t current_colon = current.find(':');
        if (current.empty() || current_colon == std::string::npos) return false;
        if (current_colon + 1u != current.size())
            apply_components(components, current.substr(current_colon + 1u));
        prefix = current.substr(0, current_colon + 1u);
        after = in;
    } else {
        prefix = in.substr(0, colon + 1u);
        after = in.substr(colon + 1u);
        if (after == "/") {
            out = prefix + after;
            return true;
        }
    }
    apply_components(components, after);
    out = prefix;
    for (const std::string &component : components) out += "/" + component;
    return true;
}

void MetaFileSystem::mount(std::string prefix, std::shared_ptr<FileSystem> system) {
    mounts_.push_back({std::move(prefix), std::move(system)});
}

std::uint32_t MetaFileSystem::map(const std::string &psp_path, Mapped &out) const {
    std::string in = psp_path;
    // "ms0:/file.txt" is equivalent to "   ms0:/file.txt".
    if (in.find(':') != std::string::npos) {
        const std::size_t first = in.find_first_not_of(' ');
        if (first != std::string::npos && first > 0u) in.erase(0, first);
    }
    // host0:command.txt means the current directory on the UMD.
    if (equal_no_case(in, "host0:", 6)) in.erase(0, 6);
    // No thread has a current directory: relative paths are NOCWD.
    const std::uint32_t result = in.find(':') == std::string::npos ? error::kNoCwd : 0u;
    std::string real;
    if (!real_path(starting_, in, real)) return error::kFileNotFound;
    const std::size_t colon = real.find(':');
    const std::string prefix =
        colon != std::string::npos ? normalize_prefix(real.substr(0, colon + 1u)) : real;
    for (const Mount &mount : mounts_) {
        if (equal_no_case(mount.prefix, prefix, mount.prefix.size())) {
            out.system = mount.system.get();
            out.path = colon != std::string::npos ? real.substr(colon + 1u) : std::string();
            out.device = mount.prefix;
            return result;
        }
    }
    return error::kNoDevice;
}

FileSystem *MetaFileSystem::system_for(const std::string &psp_path) const {
    Mapped mapped;
    const std::uint32_t result = map(psp_path, mapped);
    return result == 0u || result == error::kNoCwd ? mapped.system : nullptr;
}

std::uint32_t MetaFileSystem::flags_for(const std::string &psp_path) const {
    const FileSystem *system = system_for(psp_path);
    return system != nullptr ? system->flags() : 0u;
}

std::int64_t MetaFileSystem::open(const std::string &psp_path, std::uint32_t access) {
    Mapped mapped;
    const std::uint32_t result = map(psp_path, mapped);
    if (result != 0u) return error_result(result);
    return mapped.system->open(mapped.path, access, mapped.device);
}

FileSystem *MetaFileSystem::owner(std::uint32_t handle) const {
    for (const Mount &mount : mounts_)
        if (mount.system->owns(handle)) return mount.system.get();
    return nullptr;
}

void MetaFileSystem::close(std::uint32_t handle) {
    if (FileSystem *system = owner(handle)) system->close(handle);
}

FileInfo MetaFileSystem::info(const std::string &psp_path) const {
    Mapped mapped;
    if (map(psp_path, mapped) != 0u) return {};
    return mapped.system->info(mapped.path);
}

std::vector<FileInfo> MetaFileSystem::list(const std::string &psp_path, bool &exists) const {
    Mapped mapped;
    exists = false;
    if (map(psp_path, mapped) != 0u) return {};
    return mapped.system->list(mapped.path, exists);
}

std::int64_t MetaFileSystem::rename(const std::string &from, const std::string &to) {
    Mapped source;
    if (map(from, source) != 0u) return -1;
    // A relative `to` always uses the file system of `from`.
    Mapped target{source.system, to, source.device};
    if (to.find(":/") != std::string::npos && map(to, target) != 0u) return -1;
    if (target.system != source.system) return error_result(error::kCrossDevice);
    return source.system->rename(source.path, target.path);
}

std::int64_t MetaFileSystem::remove(const std::string &psp_path) {
    Mapped mapped;
    if (map(psp_path, mapped) != 0u) return error_result(error::kFileNotFound);
    return mapped.system->remove(mapped.path);
}

std::int64_t MetaFileSystem::make_directory(const std::string &psp_path) {
    Mapped mapped;
    if (map(psp_path, mapped) != 0u) return error_result(error::kFileNotFound);
    return mapped.system->make_directory(mapped.path);
}

} // namespace mpo::io
