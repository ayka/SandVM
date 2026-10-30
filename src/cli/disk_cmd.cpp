// sandvm disk: image tools (create, snapshot chains, conversion), see usage below.

#include "cli/disk_cmd.h"

#include <windows.h>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "core/block/host_file.h"
#include "core/block/image.h"

namespace sandvm::cli {
namespace {
using namespace block;

int disk_usage() {
  std::fprintf(stderr,
               "usage: sandvm disk <command>\n"
               "  create <file> <size> [--format raw|vhd|vmdk]   new empty image (size: 20G, 512M, ...);\n"
               "                                                 format from the extension if not given\n"
               "  snapshot <new.vhd> <parent>                    differencing VHD on top of parent (any format);\n"
               "                                                 the parent must not be written to afterwards\n"
               "  info <file>                                    format, sizes and the parent chain\n"
               "  commit <child.vhd>                             write the child's changes into its parent\n"
               "  merge <child.vhd>                              pull the (differencing) parent into the child and\n"
               "                                                 attach the child to the grandparent\n"
               "  convert <src> <dst> [--format raw|vhd|vmdk]    flatten src (with its parents) into a new image\n");
  return 2;
}

std::string human(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GiB", bytes / double(1ull << 30));
  else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MiB", bytes / double(1ull << 20));
  else std::snprintf(buf, sizeof(buf), "%llu bytes", static_cast<unsigned long long>(bytes));
  return buf;
}

bool parse_format(const std::wstring& s, ImageFormat* out) {
  if (s == L"raw" || s == L"img") *out = ImageFormat::Raw;
  else if (s == L"vhd") *out = ImageFormat::Vhd;
  else if (s == L"vmdk") *out = ImageFormat::Vmdk;
  else return false;
  return true;
}

// Prints a percentage on one console line, at most every whole percent.
Progress progress_printer() {
  return [last = -1](uint64_t done, uint64_t total) mutable {
    const int pct = total ? static_cast<int>(done * 100 / total) : 100;
    if (pct == last) return;
    last = pct;
    std::fprintf(stderr, "\r  %3d%%", pct);
    if (pct == 100) std::fprintf(stderr, "\n");
  };
}
}  // namespace

int cmd_disk(const std::vector<std::wstring>& args) {
  if (args.empty()) return disk_usage();
  const std::wstring& sub = args[0];
  std::vector<std::wstring> pos;
  bool have_format = false;
  ImageFormat format = ImageFormat::Raw;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--format" && i + 1 < args.size()) {
      if (!parse_format(args[++i], &format)) return disk_usage();
      have_format = true;
    } else {
      pos.push_back(args[i]);
    }
  }
  try {
    if (sub == L"create" && pos.size() == 2) {
      const uint64_t size = parse_size(narrow(pos[1]));
      if (!size) {
        std::fprintf(stderr, "disk create: bad size '%s'\n", narrow(pos[1]).c_str());
        return 2;
      }
      if (!have_format) format = format_from_name(pos[0]);
      create_image(pos[0], format, size);
      std::printf("created %s (%s, %s)\n", narrow(pos[0]).c_str(), to_string(format), human(size).c_str());
      return 0;
    }
    if (sub == L"snapshot" && pos.size() == 2) {
      if (format_from_name(pos[0]) != ImageFormat::Vhd) {
        std::fprintf(stderr, "disk snapshot: the new image must be a .vhd\n");
        return 2;
      }
      create_differencing_image(pos[0], pos[1]);
      std::printf("created %s on top of %s\n", narrow(pos[0]).c_str(), narrow(absolute_path(pos[1])).c_str());
      if (probe_format(pos[1]) != ImageFormat::Vhd) {
        std::printf("note: the parent is not a VHD, so only SandVM can open this chain (Windows needs VHD parents)\n");
      }
      return 0;
    }
    if (sub == L"info" && pos.size() == 1) {
      const auto chain = image_chain(pos[0]);
      for (size_t i = 0; i < chain.size(); ++i) {
        const auto& c = chain[i];
        std::printf("%s%s\n", i ? "parent: " : "image:  ", narrow(c.path).c_str());
        std::printf("  format: %s (%s)\n", to_string(c.format), c.subformat.c_str());
        std::printf("  virtual size: %s (%llu bytes)\n", human(c.virtual_size).c_str(),
                    static_cast<unsigned long long>(c.virtual_size));
        std::printf("  on disk: %s\n", human(c.file_size).c_str());
      }
      return 0;
    }
    if (sub == L"commit" && pos.size() == 1) {
      commit_image(pos[0], progress_printer());
      std::printf("committed %s into its parent; the child can be deleted\n", narrow(pos[0]).c_str());
      return 0;
    }
    if (sub == L"merge" && pos.size() == 1) {
      merge_parent_into(pos[0], progress_printer());
      std::printf("merged the parent into %s\n", narrow(pos[0]).c_str());
      return 0;
    }
    if (sub == L"convert" && pos.size() == 2) {
      if (!have_format) format = format_from_name(pos[1]);
      convert_image(pos[0], pos[1], format, progress_printer());
      std::printf("converted %s -> %s (%s)\n", narrow(pos[0]).c_str(), narrow(pos[1]).c_str(), to_string(format));
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "disk %s: %s\n", narrow(sub).c_str(), e.what());
    return 1;
  }
  return disk_usage();
}

}  // namespace sandvm::cli
