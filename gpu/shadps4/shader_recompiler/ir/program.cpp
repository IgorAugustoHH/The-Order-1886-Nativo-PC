// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>
#include <string>

#include <fmt/format.h>

#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::IR {

void DumpProgram(const Program& program, const Info& info, const std::string& type) {
    using namespace Common::FS;

    // bbport: BB_DUMP_ONLY=hash,hash (hex) dumps the IR of just those programs, without the
    // rest of BB_DUMP_SHADERS (studying a few shaders out of thousands).
    static const std::vector<u64> only = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("BB_DUMP_ONLY")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 value = std::strtoull(p, &end, 16);
                if (end == p) {
                    ++p;
                    continue;
                }
                list.push_back(value);
                p = end;
            }
        }
        return list;
    }();
    if (!only.empty()) {
        if (std::find(only.begin(), only.end(), info.pgm_hash) == only.end()) {
            return;
        }
    } else if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto ir_filename =
        fmt::format("{}_{:#018x}.{}irprogram.txt", info.hw_stage, info.pgm_hash, type);
    const auto ir_file = IOFile{dump_dir / ir_filename, FileAccessMode::Create, FileType::TextFile};

    size_t index{0};
    std::map<const IR::Inst*, size_t> inst_to_index;
    std::map<const IR::Block*, size_t> block_to_index;

    for (const IR::Block* const block : program.blocks) {
        block_to_index.emplace(block, index);
        ++index;
    }

    for (const auto& block : program.blocks) {
        std::string s = IR::DumpBlock(*block, block_to_index, inst_to_index, index) + '\n';
        ir_file.WriteString(s);
    }

    const auto asl_filename =
        fmt::format("{}_{:#018x}.{}asl.txt", info.hw_stage, info.pgm_hash, type);
    const auto asl_file =
        IOFile{dump_dir / asl_filename, FileAccessMode::Create, FileType::TextFile};

    for (const auto& node : program.syntax_list) {
        std::string s = IR::DumpASLNode(node, block_to_index, inst_to_index) + '\n';
        asl_file.WriteString(s);
    }
}

} // namespace Shader::IR
