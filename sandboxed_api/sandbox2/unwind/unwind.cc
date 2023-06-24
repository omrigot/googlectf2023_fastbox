// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sandboxed_api/sandbox2/unwind/unwind.h"

#include <cxxabi.h>
#include <sys/ptrace.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "libunwind-ptrace.h"
#include "sandboxed_api/config.h"
#include "sandboxed_api/sandbox2/comms.h"
#include "sandboxed_api/sandbox2/unwind/ptrace_hook.h"
#include "sandboxed_api/sandbox2/unwind/unwind.pb.h"
#include "sandboxed_api/sandbox2/util/maps_parser.h"
#include "sandboxed_api/sandbox2/util/minielf.h"
#include "sandboxed_api/util/file_helpers.h"
#include "sandboxed_api/util/raw_logging.h"
#include "sandboxed_api/util/status_macros.h"

namespace sandbox2 {
namespace {

std::string DemangleSymbol(const std::string& maybe_mangled) {
  int status;
  size_t length;
  std::unique_ptr<char, decltype(&std::free)> symbol(
      abi::__cxa_demangle(maybe_mangled.c_str(), /*output_buffer=*/nullptr,
                          &length, &status),
      std::free);
  if (symbol && status == 0) {
    return std::string(symbol.get(), length);
  }
  return maybe_mangled;
}

absl::StatusOr<uintptr_t> ReadMemory(pid_t pid, uintptr_t addr) {
  errno = 0;
  uintptr_t val = ptrace(PTRACE_PEEKDATA, pid, addr, 0);
  if (errno != 0) {
    return absl::ErrnoToStatus(errno, "ptrace() failed");
  }
  return val;
}

absl::StatusOr<std::vector<uintptr_t>> UnwindUsingFramePointer(pid_t pid,
                                                               int max_frames,
                                                               uintptr_t fp) {
#if defined(SAPI_PPC64_LE)
  constexpr int kIPOffset = 2;
#else
  constexpr int kIPOffset = 1;
#endif
  std::vector<uintptr_t> ips;
  for (int i = 0; fp != 0 && i < max_frames; ++i) {
    SAPI_ASSIGN_OR_RETURN(uintptr_t ip,
                          ReadMemory(pid, fp + kIPOffset * sizeof(void*)));
    ips.push_back(ip);
    SAPI_ASSIGN_OR_RETURN(fp, ReadMemory(pid, fp));
  }
  return ips;
}

absl::StatusOr<std::vector<uintptr_t>> RunLibUnwind(pid_t pid, int max_frames) {
  static unw_addr_space_t as =
      unw_create_addr_space(&_UPT_accessors, 0 /* byte order */);
  if (as == nullptr) {
    return absl::InternalError("unw_create_addr_space() failed");
  }

  void* context = _UPT_create(pid);
  if (context == nullptr) {
    return absl::InternalError("_UPT_create() failed");
  }
  absl::Cleanup context_cleanup = [&context] { _UPT_destroy(context); };

  unw_cursor_t cursor;
  if (int rc = unw_init_remote(&cursor, as, context); rc < 0) {
    // Could be UNW_EINVAL (8), UNW_EUNSPEC (1) or UNW_EBADREG (3).
    return absl::InternalError(
        absl::StrCat("unw_init_remote() failed with error ", rc));
  }
  std::vector<uintptr_t> ips;
  for (int i = 0; i < max_frames; ++i) {
    unw_word_t ip;
    unw_word_t fp = 0;
    int rc = unw_get_reg(&cursor, UNW_REG_IP, &ip);
    if (rc < 0) {
      // Could be UNW_EUNSPEC or UNW_EBADREG.
      SAPI_RAW_LOG(WARNING, "unw_get_reg() failed with error %d", rc);
      break;
    }
#if defined(SAPI_ARM64)
    constexpr int kFpReg = UNW_AARCH64_X29;
#elif defined(SAPI_ARM)
    constexpr int kFpReg = UNW_ARM_R11;
#elif defined(SAPI_X86_64)
    constexpr int kFpReg = UNW_X86_64_RBP;
#elif defined(SAPI_PPC64_LE)
    constexpr int kFpReg = UNW_PPC64_R1;
#endif
    rc = unw_get_reg(&cursor, kFpReg, &fp);
    if (rc < 0) {
      SAPI_RAW_LOG(WARNING, "unw_get_reg() failed with error %d", rc);
    }
    ips.push_back(ip);
    rc = unw_step(&cursor);
    if (rc <= 0) {
      if (rc < 0) {
        SAPI_RAW_LOG(WARNING, "unw_step() failed with error %d", rc);
      }
      if (fp != 0) {
        SAPI_RAW_LOG(INFO, "Falling back to frame based unwinding at FP: %lx",
                     fp);
        absl::StatusOr<std::vector<uintptr_t>> fp_ips =
            UnwindUsingFramePointer(pid, max_frames - ips.size(), fp);
        if (!fp_ips.ok()) {
          SAPI_RAW_LOG(WARNING, "FP based unwinding failed: %s",
                       std::string(fp_ips.status().message()).c_str());
          break;
        }
        ips.insert(ips.end(), fp_ips->begin(), fp_ips->end());
      }
      break;
    }
  }
  return ips;
}

absl::StatusOr<std::vector<std::string>> SymbolizeStacktrace(
    pid_t pid, const std::vector<uintptr_t>& ips) {
  SAPI_ASSIGN_OR_RETURN(auto addr_to_symbol, LoadSymbolsMap(pid));
  std::vector<std::string> stack_trace;
  stack_trace.reserve(ips.size());
  // Symbolize stacktrace
  for (uintptr_t ip : ips) {
    const std::string symbol =
        GetSymbolAt(addr_to_symbol, static_cast<uint64_t>(ip));
    stack_trace.push_back(absl::StrCat(symbol, "(0x", absl::Hex(ip), ")"));
  }
  return stack_trace;
}

}  // namespace

std::string GetSymbolAt(const SymbolMap& addr_to_symbol, uint64_t addr) {
  auto entry_for_next_symbol = addr_to_symbol.lower_bound(addr);
  if (entry_for_next_symbol != addr_to_symbol.end() &&
      entry_for_next_symbol != addr_to_symbol.begin()) {
    // Matches the addr exactly:
    if (entry_for_next_symbol->first == addr) {
      return DemangleSymbol(entry_for_next_symbol->second);
    }

    // Might be inside a function, return symbol+offset;
    const auto entry_for_previous_symbol = --entry_for_next_symbol;
    if (!entry_for_previous_symbol->second.empty()) {
      return absl::StrCat(DemangleSymbol(entry_for_previous_symbol->second),
                          "+0x",
                          absl::Hex(addr - entry_for_previous_symbol->first));
    }
  }
  return "";
}

absl::StatusOr<SymbolMap> LoadSymbolsMap(pid_t pid) {
  const std::string maps_filename = absl::StrCat("/proc/", pid, "/maps");
  std::string maps_content;
  SAPI_RETURN_IF_ERROR(sapi::file::GetContents(maps_filename, &maps_content,
                                               sapi::file::Defaults()));

  SAPI_ASSIGN_OR_RETURN(std::vector<MapsEntry> maps,
                        ParseProcMaps(maps_content));

  // Get symbols for each file entry in the maps entry.
  // This is not a very efficient way, so we might want to optimize it.
  SymbolMap addr_to_symbol;
  for (const MapsEntry& entry : maps) {
    if (!entry.is_executable ||
        entry.inode == 0 ||  // Only parse file-backed entries
        entry.path.empty() ||
        absl::EndsWith(entry.path, " (deleted)")  // Skip deleted files
    ) {
      continue;
    }

    // Store details about start + end of this map.
    // The maps entries are ordered and thus sorted with increasing adresses.
    // This means if there is a symbol @ entry.end, it will be overwritten in
    // the next iteration.
    std::string map = absl::StrCat("map:", entry.path);
    if (entry.pgoff) {
      absl::StrAppend(&map, "+0x", absl::Hex(entry.pgoff));
    }
    addr_to_symbol[entry.start] = map;
    addr_to_symbol[entry.end] = "";

    absl::StatusOr<ElfFile> elf =
        ElfFile::ParseFromFile(entry.path, ElfFile::kLoadSymbols);
    if (!elf.ok()) {
      SAPI_RAW_LOG(WARNING, "Could not load symbols for %s: %s",
                   entry.path.c_str(),
                   std::string(elf.status().message()).c_str());
      continue;
    }

    for (const ElfFile::Symbol& symbol : elf->symbols()) {
      // Skip Mapping Symbols on ARM
      // ARM documentation for Mapping Symbols:
      // https://developer.arm.com/documentation/dui0803/a/Accessing-and-managing-symbols-with-armlink/About-mapping-symbols
      if constexpr (sapi::host_cpu::IsArm64() || sapi::host_cpu::IsArm()) {
        if (absl::StartsWith(symbol.name, "$x") ||
            absl::StartsWith(symbol.name, "$d") ||
            absl::StartsWith(symbol.name, "$t") ||
            absl::StartsWith(symbol.name, "$a") ||
            absl::StartsWith(symbol.name, "$v")) {
          continue;
        }
      }

      if (elf->position_independent()) {
        if (symbol.address >= entry.pgoff &&
            symbol.address - entry.pgoff < entry.end - entry.start) {
          addr_to_symbol[symbol.address + entry.start - entry.pgoff] =
              symbol.name;
        }
      } else {
        if (symbol.address >= entry.start && symbol.address < entry.end) {
          addr_to_symbol[symbol.address] = symbol.name;
        }
      }
    }
  }
  return addr_to_symbol;
}

bool RunLibUnwindAndSymbolizer(Comms* comms) {
  UnwindSetup setup;
  if (!comms->RecvProtoBuf(&setup)) {
    return false;
  }
  int mem_fd;
  if (!comms->RecvFD(&mem_fd)) {
    return false;
  }

  EnablePtraceEmulationWithUserRegs(setup.pid(), setup.regs(), mem_fd);

  absl::StatusOr<std::vector<std::string>> stack_trace =
      RunLibUnwindAndSymbolizer(setup.pid(), setup.default_max_frames());

  if (!comms->SendStatus(stack_trace.status())) {
    return false;
  }

  if (!stack_trace.ok()) {
    return true;
  }

  UnwindResult msg;
  *msg.mutable_stacktrace() = {stack_trace->begin(), stack_trace->end()};
  return comms->SendProtoBuf(msg);
}

absl::StatusOr<std::vector<std::string>> RunLibUnwindAndSymbolizer(
    pid_t pid, int max_frames) {
  SAPI_ASSIGN_OR_RETURN(std::vector<uintptr_t> ips,
                        RunLibUnwind(pid, max_frames));
  return SymbolizeStacktrace(pid, ips);
}

}  // namespace sandbox2
