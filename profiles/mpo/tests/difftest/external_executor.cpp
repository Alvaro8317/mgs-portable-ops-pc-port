// SPDX-License-Identifier: GPL-3.0-or-later
//
// Executor that runs in a separate process (used for Unicorn, whose engine is
// GPL-2.0-only and therefore never linked into this project).
//
// Protocol: the harness writes every case to a text file and runs
// `<command> <cases file> <results file>`. Both files are whitespace
// separated tokens, numbers in hex without prefix:
//
//   cases:   case <id> <spec> <num_ops> <op>...      results:  result <id> ok|unsupported
//            gpr <32 words>                                    gpr ... (same blocks)
//            hilo <hi> <lo>                                    ...
//            fpr <32 words>                                    faults <n>
//            fcr31 <word>                                      end
//            v <128 words, voffset order>
//            vctrl <16 words>
//            mem <base> <bytes as one hex string>
//            end
//
// An "unsupported" result carries no state blocks.

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "difftest/difftest.hpp"

namespace mpo::difftest {

namespace {

void write_words(std::ostream &out, const char *tag, const std::uint32_t *words, int count) {
    out << tag;
    char text[12];
    for (int i = 0; i < count; ++i) {
        std::snprintf(text, sizeof(text), " %x", words[i]);
        out << text;
    }
    out << '\n';
}

void write_state(std::ostream &out, const CpuState &s) {
    write_words(out, "gpr", s.gpr, 32);
    const std::uint32_t hilo[2] = {s.hi, s.lo};
    write_words(out, "hilo", hilo, 2);
    write_words(out, "fpr", s.fpr, 32);
    write_words(out, "fcr31", &s.fcr31, 1);
    std::uint32_t v[128];
    for (int i = 0; i < 128; ++i) v[i] = std::bit_cast<std::uint32_t>(s.v[i]);
    write_words(out, "v", v, 128);
    write_words(out, "vctrl", s.vfpu_ctrl, 16);
    out << "mem " << std::hex << kMemBase << ' ';
    char byte[4];
    for (const std::uint8_t b : s.mem) {
        std::snprintf(byte, sizeof(byte), "%02x", b);
        out << byte;
    }
    out << std::dec << '\n';
}

bool read_words(std::istream &in, const char *tag, std::uint32_t *words, int count) {
    std::string token;
    if (!(in >> token) || token != tag) return false;
    for (int i = 0; i < count; ++i) {
        if (!(in >> token)) return false;
        words[i] = static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16));
    }
    return true;
}

bool read_state(std::istream &in, CpuState &s) {
    std::uint32_t hilo[2];
    std::uint32_t v[128];
    if (!read_words(in, "gpr", s.gpr, 32) || !read_words(in, "hilo", hilo, 2) ||
        !read_words(in, "fpr", s.fpr, 32) || !read_words(in, "fcr31", &s.fcr31, 1) ||
        !read_words(in, "v", v, 128) || !read_words(in, "vctrl", s.vfpu_ctrl, 16)) {
        return false;
    }
    s.hi = hilo[0];
    s.lo = hilo[1];
    for (int i = 0; i < 128; ++i) s.v[i] = std::bit_cast<float>(v[i]);
    std::string token;
    std::string base;
    std::string bytes;
    if (!(in >> token >> base >> bytes) || token != "mem" || bytes.size() != kMemSize * 2) {
        return false;
    }
    for (std::uint32_t i = 0; i < kMemSize; ++i) {
        s.mem[i] =
            static_cast<std::uint8_t>(std::strtoul(bytes.substr(i * 2, 2).c_str(), nullptr, 16));
    }
    return read_words(in, "faults", &s.mem_faults, 1);
}

class ExternalExecutor final : public Executor {
public:
    ExternalExecutor(std::string name, std::string command)
        : name_(std::move(name)), command_(std::move(command)) {}

    std::string name() const override { return name_; }

    bool run(std::span<const TestCase> cases, std::span<CpuState> out,
             std::span<std::uint8_t> supported, std::string *error) override {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) {
            *error = "no temporary directory: " + ec.message();
            return false;
        }
        // Unique across processes: ctest -j runs several difftests against the same
        // executor at once, and an unseeded std::rand() gives them all one name.
        static std::atomic<unsigned> calls{0};
        const std::string stem = "mpo_difftest_" + name_ + "_" +
                                 std::to_string(std::random_device{}()) + "_" +
                                 std::to_string(calls.fetch_add(1));
        const std::filesystem::path cases_path = dir / (stem + ".cases");
        const std::filesystem::path results_path = dir / (stem + ".results");
        {
            std::ofstream file(cases_path);
            for (const TestCase &c : cases) {
                file << "case " << c.id << ' ' << c.spec << ' ' << c.num_ops;
                for (int i = 0; i < c.num_ops; ++i) {
                    file << ' ' << std::hex << c.ops[static_cast<std::size_t>(i)] << std::dec;
                }
                file << '\n';
                write_state(file, c.initial);
                file << "end\n";
            }
            if (!file) {
                *error = "cannot write " + cases_path.string();
                return false;
            }
        }
        const std::string full =
            command_ + " \"" + cases_path.string() + "\" \"" + results_path.string() + "\"";
        const int status = std::system(full.c_str());
        std::filesystem::remove(cases_path, ec);
        int code = status;
#ifndef _WIN32
        if (WIFEXITED(status)) code = WEXITSTATUS(status);
#endif
        if (code == 77) {
            // The command says it cannot run here (e.g. no unicorn module).
            *error = "skip: " + name_ + " is not available";
            std::filesystem::remove(results_path, ec);
            return false;
        }
        if (status != 0) {
            *error = name_ + ": command failed (" + std::to_string(status) + "): " + full;
            std::filesystem::remove(results_path, ec);
            return false;
        }
        std::ifstream file(results_path);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            std::string token;
            std::uint32_t id = 0;
            std::string verdict;
            if (!(file >> token >> id >> verdict) || token != "result" || id != cases[i].id) {
                *error = name_ + ": malformed results at case " + std::to_string(i);
                return false;
            }
            out[i] = cases[i].initial;
            supported[i] = verdict == "ok";
            // The protocol has no PC: the external cases have no branches.
            out[i].pc = cases[i].initial.pc + 4u * static_cast<std::uint32_t>(cases[i].num_ops);
            if (supported[i] && !read_state(file, out[i])) {
                *error = name_ + ": malformed state for case " + std::to_string(id);
                return false;
            }
            if (!(file >> token) || token != "end") {
                *error = name_ + ": missing end for case " + std::to_string(id);
                return false;
            }
        }
        file.close();
        std::filesystem::remove(results_path, ec);
        return true;
    }

private:
    std::string name_;
    std::string command_;
};

} // namespace

std::unique_ptr<Executor> make_external_executor(std::string name, std::string command) {
    return std::make_unique<ExternalExecutor>(std::move(name), std::move(command));
}

} // namespace mpo::difftest
