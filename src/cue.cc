// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/cue.h"

#include <algorithm>
#include <cstdio>
#include <print>
#include <string>
#include <system_error>

#include "src/log.h"

namespace gsradio {
namespace {

std::string FileTime(int bitrate, std::uint64_t filesize) {
    if (bitrate <= 0) return "0:0:0:0";
    const std::uint64_t seconds = filesize / ((static_cast<std::uint64_t>(bitrate) * 1000) >> 3);
    return std::format("{}:{}:{}:{}", seconds / 86400, seconds % 86400 / 3600, seconds % 3600 / 60, seconds % 60);
}

}  // namespace

void CueFile::Update(const InputSource& source, const TrackTags& tags) {
    // Write to a temporary and rename, so readers never see a partial file.
    auto tmp = path_;
    tmp += ".tmp";
    std::FILE* fp = std::fopen(tmp.c_str(), "w");
    if (!fp) {
        if (!warned_) {
            log::Info("Could not open cuefile [{}] for writing, cuefile not updated!", path_.string());
        }
        warned_ = true;
        return;
    }
    warned_ = false;
    const double percent =
        source.filesize() ? static_cast<double>(source.bytes_read()) * 100.0 / static_cast<double>(source.filesize())
                          : 0.0;
    std::print(fp, "{}\n{}\n{}\n{}\n{:.6f}\n{}\n{}\n{}\n", source.path(), source.filesize(), source.bitrate(),
               FileTime(source.bitrate(), source.filesize()), std::min(percent, 100.0), lineno_, tags.artist,
               tags.title);
    std::fclose(fp);
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
}

void CueFile::Remove() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

}  // namespace gsradio
