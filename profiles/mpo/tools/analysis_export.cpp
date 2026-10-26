// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_analysis_export: runs PSPRecomp's automatic analysis (the same
// analyze_program() that psp_analyze and psp_recomp --auto use) on a decrypted
// PSP module and writes everything the comparison tools need as JSON:
//
//   - the executable ranges and the ELF sections (names, ranges and flags);
//   - every function seed with its source;
//   - every function with its entry and its decoded instructions as address
//     runs (delay slots are added explicitly, psp_analyze leaves them out);
//   - the block entries of all functions (the dispatcher entries of the AOT);
//   - a census of the decoder over every word of the executable ranges: the
//     OpcodeKind of each word and, for the kinds the emitter cannot lower
//     (Vfpu, Unsupported, Syscall), the raw word so a tool can name it.
//
// With a framework that has the analysis-hints patch (framework/patches/,
// PSPRECOMP_ANALYSIS_HINTS), an optional fourth argument names a hints file
// (profiles/mpo/tools/recomp_hints.py) and the analysis uses it, which is how
// F06 measured the effect of the Ghidra data.
//
// The output contains instruction words of the user's executable, so it is
// local analysis data (analysis/, ignored by git) and never goes to the repo.
// See docs/analysis/psp_analyze.md.

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/program_analysis.hpp"
#include "psprecomp/sha256.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Highest OpcodeKind value; the census stores one byte per word.
constexpr int kMaxKind = static_cast<int>(psprecomp::OpcodeKind::Unsupported);

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) {
                out += '?';
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

std::string quoted_hex(std::uint32_t value) {
    return "\"" + psprecomp::hex32(value) + "\"";
}

// Turns a sorted address set into [start, end) runs of consecutive words.
std::vector<std::pair<std::uint32_t, std::uint32_t>> runs_of(const std::set<std::uint32_t> &set) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    for (const auto address : set) {
        if (!runs.empty() && runs.back().second == address) {
            runs.back().second = address + 4u;
        } else {
            runs.emplace_back(address, address + 4u);
        }
    }
    return runs;
}

// The mnemonics the decoder gives each kind ("vadd/vdiv/vsub"), taken from
// the decoder itself so the names cannot drift from PSPRecomp.
std::map<int, std::string> kind_names(const psprecomp::GuestMemory &memory,
                                      const std::vector<psprecomp::ExecutableRange> &ranges) {
    std::map<int, std::set<std::string>> seen;
    for (const auto &range : ranges) {
        for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
            const auto decoded = psprecomp::decode_allegrex(memory.load32(pc));
            seen[static_cast<int>(decoded.kind)].insert(decoded.mnemonic);
        }
    }
    std::map<int, std::string> names;
    for (const auto &[kind, mnemonics] : seen) {
        std::string joined;
        for (const auto &mnemonic : mnemonics) {
            joined += (joined.empty() ? "" : "/") + mnemonic;
        }
        names[kind] = joined;
    }
    return names;
}

bool is_generic_kind(psprecomp::OpcodeKind kind) {
    return kind == psprecomp::OpcodeKind::Vfpu || kind == psprecomp::OpcodeKind::Unsupported ||
           kind == psprecomp::OpcodeKind::Syscall;
}

void write_report(std::ostream &out, const std::filesystem::path &input,
                  const psprecomp::Elf32Image &elf, const psprecomp::GuestMemory &memory,
                  std::uint32_t load_base, const psprecomp::ProgramAnalysis &program) {
    out << "{\n";
    out << "  \"tool\": \"mpo_analysis_export\",\n";
    out << "  \"input_sha256\": \"" << psprecomp::sha256_file(input) << "\",\n";
    out << "  \"load_base\": " << quoted_hex(load_base) << ",\n";
    out << "  \"entry\": " << quoted_hex(elf.runtime_entry(load_base)) << ",\n";

    out << "  \"executable_ranges\": [";
    for (std::size_t i = 0; i < program.executable_ranges.size(); ++i) {
        const auto &range = program.executable_ranges[i];
        out << (i ? ", " : "") << "[" << quoted_hex(range.start) << ", " << quoted_hex(range.end)
            << "]";
    }
    out << "],\n";

    out << "  \"sections\": [\n";
    bool first = true;
    for (const auto &section : elf.sections()) {
        if (section.size == 0u || (section.flags & 2u) == 0u) continue; // SHF_ALLOC only
        const std::uint32_t start = elf.section_runtime_address(section, load_base);
        out << (first ? "" : ",\n") << "    {\"name\": \"" << json_escape(section.name)
            << "\", \"start\": " << quoted_hex(start)
            << ", \"end\": " << quoted_hex(start + section.size) << ", \"flags\": " << section.flags
            << "}";
        first = false;
    }
    out << "\n  ],\n";

    out << "  \"seeds\": [\n";
    first = true;
    for (const auto &[address, source] : program.seeds) {
        out << (first ? "" : ",\n") << "    [" << quoted_hex(address) << ", \""
            << json_escape(source) << "\"]";
        first = false;
    }
    out << "\n  ],\n";

    out << "  \"functions\": [\n";
    first = true;
    for (const auto &function : program.functions) {
        // psp_analyze's labels leave out delay slots; the emitter reads them
        // from pc + 4. Add them so coverage compares like with like.
        std::set<std::uint32_t> words = function.labels;
        for (const auto pc : function.labels) {
            if (psprecomp::decode_allegrex(memory.load32(pc)).has_delay_slot()) {
                words.insert(pc + 4u);
            }
        }
        out << (first ? "" : ",\n") << "    {\"entry\": " << quoted_hex(function.entry)
            << ", \"block_entries\": " << function.entry_labels.size()
            << ", \"direct_calls\": " << function.direct_calls.size()
            << ", \"unsupported\": " << function.unsupported_instruction_count
            << ", \"truncated\": " << (function.truncated ? "true" : "false")
            << ", \"indirect_call_sites\": [";
        bool first_site = true;
        for (const auto site : function.indirect_call_sites) {
            out << (first_site ? "" : ", ") << quoted_hex(site);
            first_site = false;
        }
        out << "], \"runs\": [";
        bool first_run = true;
        for (const auto &[start, end] : runs_of(words)) {
            out << (first_run ? "" : ", ") << "[" << quoted_hex(start) << ", " << quoted_hex(end)
                << "]";
            first_run = false;
        }
        out << "]}";
        first = false;
    }
    out << "\n  ],\n";

    // Block entries of every function: the addresses psp_recomp --auto turns
    // into dispatcher entries (a jr into a jump table needs its targets here).
    out << "  \"entry_labels\": [";
    first = true;
    for (const auto label : program.covered_entry_labels) {
        out << (first ? "" : ", ") << quoted_hex(label);
        first = false;
    }
    out << "],\n";

    out << "  \"kind_names\": {";
    first = true;
    for (const auto &[kind, name] : kind_names(memory, program.executable_ranges)) {
        out << (first ? "" : ", ") << "\"" << kind << "\": \"" << json_escape(name) << "\"";
        first = false;
    }
    out << "},\n";

    // One hex byte per word with its OpcodeKind, per executable range.
    out << "  \"census\": [\n";
    for (std::size_t i = 0; i < program.executable_ranges.size(); ++i) {
        const auto &range = program.executable_ranges[i];
        static constexpr char kHex[] = "0123456789abcdef";
        std::string kinds;
        kinds.reserve((range.end - range.start) / 2u);
        for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
            const int kind = static_cast<int>(psprecomp::decode_allegrex(memory.load32(pc)).kind);
            kinds += kHex[(kind >> 4) & 0xF];
            kinds += kHex[kind & 0xF];
        }
        out << (i ? ",\n" : "") << "    {\"start\": " << quoted_hex(range.start)
            << ", \"kinds\": \"" << kinds << "\"}";
    }
    out << "\n  ],\n";

    // Raw words of the kinds the generic emitter turns into rt.unsupported().
    out << "  \"generic_words\": [\n";
    first = true;
    for (const auto &range : program.executable_ranges) {
        for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
            const std::uint32_t word = memory.load32(pc);
            const auto decoded = psprecomp::decode_allegrex(word);
            if (!is_generic_kind(decoded.kind)) continue;
            out << (first ? "" : ",\n") << "    [" << quoted_hex(pc) << ", " << quoted_hex(word)
                << ", \"" << json_escape(decoded.mnemonic) << "\"]";
            first = false;
        }
    }
    out << "\n  ]\n}\n";
}

} // namespace

int main(int argc, char **argv) {
    static_assert(kMaxKind < 256, "the census stores one byte per OpcodeKind");
    try {
        if (argc != 4 && argc != 5) {
            std::cerr << "Usage: mpo_analysis_export <decrypted ELF/PRX> <load_base_hex> "
                         "<out.json> [hints.txt]\n";
            return 2;
        }
        const std::filesystem::path input = argv[1];
        const auto load_base = static_cast<std::uint32_t>(std::stoul(argv[2], nullptr, 0));
        const std::filesystem::path output = argv[3];

        const auto elf = psprecomp::Elf32Image::from_file(input);
        psprecomp::GuestMemory memory(32u * 1024u * 1024u);
        const auto relocations = elf.load_and_relocate(memory, load_base);
        if (relocations.invalid != 0u || relocations.unsupported != 0u) {
            std::cerr << "mpo_analysis_export: " << relocations.invalid << " invalid and "
                      << relocations.unsupported << " unsupported relocations\n";
            return 1;
        }
#if defined(PSPRECOMP_ANALYSIS_HINTS)
        const auto hints =
            argc == 5 ? psprecomp::load_analysis_hints(argv[4]) : psprecomp::AnalysisHints{};
        const auto program = psprecomp::analyze_program(elf, memory, load_base, hints);
#else
        if (argc == 5) {
            std::cerr << "mpo_analysis_export: this framework has no analysis hints "
                         "(framework/patches/0001-analysis-hints.patch)\n";
            return 2;
        }
        const auto program = psprecomp::analyze_program(elf, memory, load_base);
#endif

        std::ofstream out(output);
        if (!out) throw psprecomp::Error("Cannot create " + output.string());
        write_report(out, input, elf, memory, load_base, program);
        if (!out) throw psprecomp::Error("Failed writing " + output.string());
        std::cout << "functions " << program.functions.size() << ", seeds " << program.seeds.size()
                  << ", labels " << program.covered_labels.size() << " -> " << output.string()
                  << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "mpo_analysis_export: " << error.what() << "\n";
        return 1;
    }
}
