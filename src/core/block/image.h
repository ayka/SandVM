#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/block/disk.h"

namespace sandvm::block {

enum class ImageFormat { Raw, Vhd, Vmdk };

const char* to_string(ImageFormat format);
// From the file name: .vhd -> Vhd, .vmdk -> Vmdk, anything else (.img, .raw) -> Raw.
ImageFormat format_from_name(const std::wstring& path);

// Opens a disk image of any supported format, detected from its contents, with its parent chain
// (differencing VHD, VMDK snapshot) opened read-only. Throws std::runtime_error.
std::unique_ptr<Disk> open_image(const std::wstring& path, bool read_only, int depth = 0);
ImageFormat probe_format(const std::wstring& path);

// A new empty image: raw (an NTFS sparse file), dynamic VHD or monolithicSparse VMDK.
void create_image(const std::wstring& path, ImageFormat format, uint64_t size_bytes);

// A differencing VHD at `path` on top of `parent` (any format): a snapshot or a linked clone. The
// parent must not be written to afterwards.
void create_differencing_image(const std::wstring& path, const std::wstring& parent);

struct ImageInfo {
  std::wstring path;
  ImageFormat format = ImageFormat::Raw;
  std::string subformat;  // "fixed" / "dynamic" / "differencing", the VMDK createType, ...
  uint64_t virtual_size = 0;
  uint64_t file_size = 0;  // on disk, as the file system reports it (sparse files: allocated)
  std::wstring parent;     // empty without a parent
};
// The image and its ancestors, the image first.
std::vector<ImageInfo> image_chain(const std::wstring& path);

using Progress = std::function<void(uint64_t done, uint64_t total)>;

// Copies `child`'s own sectors into its parent (which becomes writable for this); the child is then
// redundant and can be deleted. Every other image on that parent becomes invalid (VHD identity checks
// catch it). Child must be a differencing VHD.
void commit_image(const std::wstring& child, const Progress& progress = {});
// The opposite direction, for deleting a snapshot in the middle of a chain: copies the sectors of
// child's parent (a differencing VHD) that the child does not have into the child, then points the
// child at its grandparent. The parent file can be deleted afterwards if nothing else uses it.
void merge_parent_into(const std::wstring& child, const Progress& progress = {});
// Copies an image (the whole chain flattened) into a new file of the given format; all-zero areas
// are left unallocated.
void convert_image(const std::wstring& src, const std::wstring& dst, ImageFormat format,
                   const Progress& progress = {});

// "20G", "512M", "4096" (MiB if no unit), "1T". Returns 0 on a parse error.
uint64_t parse_size(const std::string& text);

}  // namespace sandvm::block
