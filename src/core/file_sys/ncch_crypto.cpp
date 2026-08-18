// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/file_sys/ncch_crypto.h"

#include <algorithm>
#include <cstring>
#include <cryptopp/sha.h>
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/seed_db.h"
#include "core/hw/aes/key.h"

namespace FileSys {

namespace {

constexpr u32 NCCH_MEDIA_UNIT_SIZE = 0x200;

std::array<u8, 4> U32ToBEArray(u32 value) {
    return std::array<u8, 4>{
        static_cast<u8>(value >> 24),
        static_cast<u8>((value >> 16) & 0xFF),
        static_cast<u8>((value >> 8) & 0xFF),
        static_cast<u8>(value & 0xFF),
    };
}

HW::AES::KeySlotID GetSecondaryKeySlot(u8 secondary_key_slot) {
    using namespace HW::AES;

    switch (secondary_key_slot) {
    case 0:
        return KeySlotID::NCCHSecure1;
    case 1:
        return KeySlotID::NCCHSecure2;
    case 10:
        return KeySlotID::NCCHSecure3;
    case 11:
        return KeySlotID::NCCHSecure4;
    default:
        return KeySlotID::MaxKeySlotID;
    }
}

} // namespace

NCCHCryptoResult DeriveNCCHCryptoData(const NCCH_Header& ncch_header, NCCHCryptoData& out) {
    if (ncch_header.fixed_key) {
        out.primary_key.fill(0);
        out.secondary_key.fill(0);
    } else {
        using namespace HW::AES;

        InitKeys();

        std::array<u8, 16> key_y_primary{};
        std::array<u8, 16> key_y_secondary{};
        std::copy(ncch_header.signature, ncch_header.signature + key_y_primary.size(),
                  key_y_primary.begin());

        if (!ncch_header.seed_crypto) {
            key_y_secondary = key_y_primary;
        } else {
            auto seed = GetSeed(ncch_header.program_id);
            if (!seed.has_value()) {
                return NCCHCryptoResult::MissingSeed;
            }

            std::array<u8, 32> input{};
            std::memcpy(input.data(), key_y_primary.data(), key_y_primary.size());
            std::memcpy(input.data() + key_y_primary.size(), seed->data(), seed->size());

            CryptoPP::SHA256 sha;
            std::array<u8, CryptoPP::SHA256::DIGESTSIZE> hash{};
            sha.CalculateDigest(hash.data(), input.data(), input.size());
            std::memcpy(key_y_secondary.data(), hash.data(), key_y_secondary.size());
        }

        SetKeyY(KeySlotID::NCCHSecure1, key_y_primary);
        if (!IsNormalKeyAvailable(KeySlotID::NCCHSecure1)) {
            return NCCHCryptoResult::MissingPrimaryKeyX;
        }
        out.primary_key = GetNormalKey(KeySlotID::NCCHSecure1);

        const auto secondary_slot = GetSecondaryKeySlot(ncch_header.secondary_key_slot);
        if (secondary_slot == KeySlotID::MaxKeySlotID) {
            return NCCHCryptoResult::UnknownSecondaryKeySlot;
        }

        SetKeyY(secondary_slot, key_y_secondary);
        if (!IsNormalKeyAvailable(secondary_slot)) {
            return NCCHCryptoResult::MissingSecondaryKeyX;
        }
        out.secondary_key = GetNormalKey(secondary_slot);
    }

    if (ncch_header.version == 0 || ncch_header.version == 2) {
        std::reverse_copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                          out.exheader_ctr.begin());
        out.exefs_ctr = out.romfs_ctr = out.exheader_ctr;
        out.exheader_ctr[8] = 1;
        out.exefs_ctr[8] = 2;
        out.romfs_ctr[8] = 3;
    } else if (ncch_header.version == 1) {
        std::copy(ncch_header.partition_id, ncch_header.partition_id + 8, out.exheader_ctr.begin());
        out.exefs_ctr = out.romfs_ctr = out.exheader_ctr;

        const auto offset_exheader = U32ToBEArray(0x200);
        const auto offset_exefs = U32ToBEArray(ncch_header.exefs_offset * NCCH_MEDIA_UNIT_SIZE);
        const auto offset_romfs = U32ToBEArray(ncch_header.romfs_offset * NCCH_MEDIA_UNIT_SIZE);

        std::copy(offset_exheader.begin(), offset_exheader.end(), out.exheader_ctr.begin() + 12);
        std::copy(offset_exefs.begin(), offset_exefs.end(), out.exefs_ctr.begin() + 12);
        std::copy(offset_romfs.begin(), offset_romfs.end(), out.romfs_ctr.begin() + 12);
    } else {
        return NCCHCryptoResult::UnknownVersion;
    }

    return NCCHCryptoResult::Success;
}

} // namespace FileSys
