// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/common_types.h"

struct NCCH_Header;

namespace FileSys {

struct NCCHCryptoData {
    std::array<u8, 16> primary_key{};
    std::array<u8, 16> secondary_key{};
    std::array<u8, 16> exheader_ctr{};
    std::array<u8, 16> exefs_ctr{};
    std::array<u8, 16> romfs_ctr{};
};

enum class NCCHCryptoResult {
    Success,
    MissingPrimaryKeyX,
    MissingSecondaryKeyX,
    MissingSeed,
    UnknownSecondaryKeySlot,
    UnknownVersion,
};

NCCHCryptoResult DeriveNCCHCryptoData(const NCCH_Header& ncch_header, NCCHCryptoData& out);

} // namespace FileSys
