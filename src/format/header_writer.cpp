#include "header_writer.hpp"
#include "../core/vint.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/aes256.hpp"
#include "../crypto/sha256.hpp"
#include "../crypto/rng.hpp"
#include <cstring>

namespace openrar::format {

namespace {

inline void push_le32(std::vector<core::byte>& vec, core::uint32 val) {
    vec.push_back(static_cast<core::byte>(val & 0xFF));
    vec.push_back(static_cast<core::byte>((val >> 8) & 0xFF));
    vec.push_back(static_cast<core::byte>((val >> 16) & 0xFF));
    vec.push_back(static_cast<core::byte>((val >> 24) & 0xFF));
}

inline void push_le64(std::vector<core::byte>& vec, core::uint64 val) {
    for (int i = 0; i < 8; ++i) {
        vec.push_back(static_cast<core::byte>((val >> (8 * i)) & 0xFF));
    }
}

} // namespace

bool HeaderCryptWriter::init_new(const std::string& password, CryptBlock& out_crypt) {
    out_crypt = CryptBlock{};
    core::byte salt[16];
    if (!crypto::secure_random_bytes(salt, sizeof(salt))) return false;

    // Default of 2^15 = 32768 PBKDF2 rounds.
    constexpr core::uint8 LG2_COUNT = 15;
    crypto::Rar5Keys keys;
    crypto::Pbkdf2Rar5::derive_keys(password, salt, sizeof(salt), 1U << LG2_COUNT, keys);

    out_crypt.crypt_version = 0;
    out_crypt.enc_flags = 0x0001; // PswCheck present
    out_crypt.lg2_count = LG2_COUNT;
    std::memcpy(out_crypt.salt.data(), salt, sizeof(salt));
    out_crypt.has_psw_check = true;
    std::memcpy(out_crypt.psw_check.data(), keys.psw_check, sizeof(keys.psw_check));
    std::memcpy(out_crypt.psw_check_csum.data(), keys.psw_check_csum, sizeof(keys.psw_check_csum));

    keys_ = keys;
    active = true;
    return true;
}

bool HeaderCryptWriter::init_existing(const std::string& password, const CryptBlock& crypt) {
    if (crypt.crypt_version != 0 || crypt.lg2_count > 24) return false;
    crypto::Rar5Keys keys;
    crypto::Pbkdf2Rar5::derive_keys(password, crypt.salt.data(), crypt.salt.size(),
                                    1U << crypt.lg2_count, keys);
    keys_ = keys;
    active = true;
    return true;
}

bool HeaderCryptWriter::write_block(io::FileStream& dest, const std::vector<core::byte>& wrapped) {
    // Zero-pad the plaintext header to the 16-byte cipher block boundary.
    size_t padded = (wrapped.size() + 15u) & ~size_t(15);
    std::vector<core::byte> plain(padded, 0);
    std::memcpy(plain.data(), wrapped.data(), wrapped.size());

    core::byte iv[16];
    if (!crypto::secure_random_bytes(iv, sizeof(iv))) return false;
    core::byte iv_copy[16];
    std::memcpy(iv_copy, iv, sizeof(iv));

    crypto::Aes256 aes(keys_.aes_key);
    if (!aes.encrypt_cbc(plain.data(), padded, iv_copy)) return false;

    if (dest.write(iv, sizeof(iv)) != sizeof(iv)) return false;
    return dest.write(plain.data(), padded) == padded;
}

bool HeaderWriter::emit_block(io::FileStream& dest, const std::vector<core::byte>& wrapped,
                              HeaderCryptWriter* crypt) {
    if (crypt && crypt->active) {
        return crypt->write_block(dest, wrapped);
    }
    return dest.write(wrapped.data(), wrapped.size()) == wrapped.size();
}

bool HeaderWriter::write_signature(io::FileStream& dest) {
    return dest.write(rar5_signature(), RAR5_SIGNATURE_SIZE) == RAR5_SIGNATURE_SIZE;
}

std::vector<core::byte> HeaderWriter::wrap_block(const std::vector<core::byte>& body) {
    std::vector<core::byte> size_vint;
    core::push_vint(size_vint, body.size());

    crypto::Crc32 crc;
    crc.update(size_vint.data(), size_vint.size());
    crc.update(body.data(), body.size());
    core::uint32 crc_val = crc.get();

    std::vector<core::byte> block;
    push_le32(block, crc_val);
    block.insert(block.end(), size_vint.begin(), size_vint.end());
    block.insert(block.end(), body.begin(), body.end());
    return block;
}

bool HeaderWriter::write_main_block(io::FileStream& dest, const MainBlock& block,
                                    HeaderCryptWriter* crypt) {
    std::vector<core::byte> extra;

    if (block.has_locator) {
        // INTENDED (over-wide vint): locator offsets are written with
        // push_vint_fixed(..., 10) even when they would fit in fewer bytes.
        // add_recovery_record back-patches the RR offset by rewriting the
        // whole main header in a second pass; the fixed field width keeps
        // the main header's byte size identical across both passes, so the
        // size vint never changes shape. Readers accept <= 10-byte vints
        // (see core/vint.cpp read_vint). Do not shrink the width.
        bool has_qo = block.locator_qo_offset >= 0;
        bool has_rr = block.locator_rr_offset >= 0;

        if (has_qo && has_rr) {
            // RAR5 extra-record Size covers type + flags + payload and
            // excludes the size vint itself (same arithmetic as the
            // single-offset branches below): 1 + 1 + 10 + 10 = 22.
            core::push_vint(extra, 22);
            extra.push_back(MHEXTRA_LOCATOR);
            core::push_vint(extra, 0x01 | 0x02); // QLIST | RR
            core::push_vint_fixed(extra, static_cast<core::uint64>(block.locator_qo_offset), 10);
            core::push_vint_fixed(extra, static_cast<core::uint64>(block.locator_rr_offset), 10);
        } else if (has_qo) {
            core::push_vint(extra, 12);
            extra.push_back(MHEXTRA_LOCATOR);
            core::push_vint(extra, 0x01); // QLIST
            core::push_vint_fixed(extra, static_cast<core::uint64>(block.locator_qo_offset), 10);
        } else if (has_rr) {
            core::push_vint(extra, 12);
            extra.push_back(MHEXTRA_LOCATOR);
            core::push_vint(extra, 0x02); // RR
            core::push_vint_fixed(extra, static_cast<core::uint64>(block.locator_rr_offset), 10);
        }
    }

    if (block.has_metadata) {
        std::vector<core::byte> meta_payload;
        core::uint64 flags = 0;
        if (!block.metadata_name.empty()) flags |= 0x01;
        if (block.metadata_ctime != 0) flags |= 0x02;
        if (block.metadata_is_unix_time) flags |= 0x04;
        if (block.metadata_is_nanoseconds) flags |= 0x08;

        core::push_vint(meta_payload, flags);
        if (flags & 0x01) {
            core::push_vint(meta_payload, block.metadata_name.size());
            meta_payload.insert(meta_payload.end(), block.metadata_name.begin(),
                                block.metadata_name.end());
        }
        if (flags & 0x02) {
            if (block.metadata_is_unix_time) {
                if (block.metadata_is_nanoseconds) {
                    for (int b = 0; b < 8; ++b) {
                        meta_payload.push_back(
                            static_cast<core::byte>((block.metadata_ctime >> (8 * b)) & 0xFF));
                    }
                } else {
                    core::uint32 sec = static_cast<core::uint32>(block.metadata_ctime);
                    for (int b = 0; b < 4; ++b) {
                        meta_payload.push_back(static_cast<core::byte>((sec >> (8 * b)) & 0xFF));
                    }
                }
            } else {
                for (int b = 0; b < 8; ++b) {
                    meta_payload.push_back(
                        static_cast<core::byte>((block.metadata_ctime >> (8 * b)) & 0xFF));
                }
            }
        }
        core::push_vint(extra, 1 + meta_payload.size());
        extra.push_back(MHEXTRA_METADATA);
        extra.insert(extra.end(), meta_payload.begin(), meta_payload.end());
    }

    core::uint64 common_flags = extra.empty() ? 0 : HFL_EXTRA;

    std::vector<core::byte> body;
    core::push_vint(body, HEAD_MAIN);
    core::push_vint(body, common_flags);
    if (common_flags & HFL_EXTRA) {
        core::push_vint(body, extra.size());
    }
    core::push_vint(body, block.arc_flags);
    if (block.arc_flags & MHFL_VOLNUMBER) {
        core::push_vint(body, block.vol_number);
    }
    if (!extra.empty()) {
        body.insert(body.end(), extra.begin(), extra.end());
    }

    auto wrapped = wrap_block(body);
    return emit_block(dest, wrapped, crypt);
}

bool HeaderWriter::write_file_block(io::FileStream& dest, const FileBlock& block) {
    return write_file_block(dest, block, 0, nullptr);
}

std::vector<core::byte> HeaderWriter::serialize_file_block(const FileBlock& block,
                                                           core::uint64 extra_head_flags,
                                                           bool fixed_pack_size_vint) {
    std::vector<core::byte> extra;

    // Encryption Extra Record
    if (block.is_encrypted) {
        std::vector<core::byte> crypt_content;
        core::push_vint(crypt_content, block.crypt_version);
        core::push_vint(crypt_content, block.crypt_flags);
        crypt_content.push_back(block.lg2_count);
        crypt_content.insert(crypt_content.end(), block.salt.begin(), block.salt.end());
        crypt_content.insert(crypt_content.end(), block.init_v.begin(), block.init_v.end());
        if (block.has_psw_check) {
            crypt_content.insert(crypt_content.end(), block.psw_check.begin(),
                                 block.psw_check.end());
            crypt_content.insert(crypt_content.end(), block.psw_check_csum.begin(),
                                 block.psw_check_csum.end());
        }
        core::push_vint(extra, crypt_content.size() + 1);
        extra.push_back(FHEXTRA_CRYPT);
        extra.insert(extra.end(), crypt_content.begin(), crypt_content.end());
    }

    // Redirection / Symlink Extra Record
    if (block.redir_type != 0) {
        std::vector<core::byte> redir_content;
        core::push_vint(redir_content, block.redir_type);
        core::push_vint(redir_content, block.redir_dir_target ? 1 : 0);
        core::push_vint(redir_content, block.redir_target.size());
        redir_content.insert(redir_content.end(), block.redir_target.begin(),
                             block.redir_target.end());

        core::push_vint(extra, redir_content.size() + 1);
        extra.push_back(FHEXTRA_REDIR);
        extra.insert(extra.end(), redir_content.begin(), redir_content.end());
    }

    // BLAKE2sp Hash Extra Record
    if (block.has_blake2sp) {
        core::push_vint(extra, 2 + 32); // size: type(1) + hash_type(1) + 32-byte digest
        extra.push_back(FHEXTRA_HASH);
        extra.push_back(0); // 0 = BLAKE2sp
        extra.insert(extra.end(), block.blake2sp.begin(), block.blake2sp.end());
    }

    // High Precision Timestamps (FHEXTRA_HTIME) â€” spec 01-headers.md:103
    {
        bool is_unix = block.htime_is_unix;
        bool has_mtime = false, has_ctime = false, has_atime = false;
        bool has_ns = false;
        if (is_unix) {
            has_mtime = block.htime_mtime_unix != 0 || block.has_mtime_ns;
            has_ctime = block.htime_ctime_unix != 0 || block.has_ctime_ns;
            has_atime = block.htime_atime_unix != 0 || block.has_atime_ns;
            has_ns = block.has_mtime_ns || block.has_ctime_ns || block.has_atime_ns;
            // Fallback: if unix mode but no unix times set, fall back to win times
            if (!has_mtime && !has_ctime && !has_atime && block.mtime_win != 0) {
                is_unix = false;
                has_mtime = block.mtime_win != 0;
                has_ctime = block.ctime_win != 0;
                has_atime = block.atime_win != 0;
                has_ns = false;
            }
        } else {
            has_mtime = block.mtime_win != 0;
            has_ctime = block.ctime_win != 0;
            has_atime = block.atime_win != 0;
            has_ns = false;
            if (block.has_mtime_ns || block.has_ctime_ns || block.has_atime_ns) {
                // ns requires unix mode, but if caller set ns without unix flag, enable unix mode
                // Keep as FILETIME if no unix times set â€” ns will be ignored per spec
            }
            // If unix times present but win flag not set, switch to unix
            if (!has_mtime && !has_ctime && !has_atime &&
                (block.htime_mtime_unix != 0 || block.has_mtime_ns)) {
                is_unix = true;
                has_mtime = block.htime_mtime_unix != 0 || block.has_mtime_ns;
                has_ctime = block.htime_ctime_unix != 0 || block.has_ctime_ns;
                has_atime = block.htime_atime_unix != 0 || block.has_atime_ns;
                has_ns = block.has_mtime_ns || block.has_ctime_ns || block.has_atime_ns;
            }
        }
        // Also handle mixed case where caller set both win and unix times â€” prefer is_unix path already
        if (has_mtime || has_ctime || has_atime) {
            core::uint64 tflags = 0;
            if (is_unix) {
                tflags |= 0x01;
                if (has_mtime) tflags |= 0x02;
                if (has_ctime) tflags |= 0x04;
                if (has_atime) tflags |= 0x08;
                if (has_ns) tflags |= 0x10;
            } else {
                // Official RAR5 spec for Windows FILETIME:
                // Bit 0 = 0 (Windows FILETIME, 8 bytes per field)
                // Bit 1 = mtime present (0x02)
                // Bit 2 = ctime present (0x04)
                // Bit 3 = atime present (0x08)
                if (has_mtime) tflags |= 0x02;
                if (has_ctime) tflags |= 0x04;
                if (has_atime) tflags |= 0x08;
            }

            std::vector<core::byte> htime_content;
            core::push_vint(htime_content, tflags);
            if (has_mtime) {
                if (is_unix)
                    push_le32(htime_content, block.htime_mtime_unix);
                else
                    push_le64(htime_content, block.mtime_win);
            }
            if (has_ctime) {
                if (is_unix)
                    push_le32(htime_content, block.htime_ctime_unix);
                else
                    push_le64(htime_content, block.ctime_win);
            }
            if (has_atime) {
                if (is_unix)
                    push_le32(htime_content, block.htime_atime_unix);
                else
                    push_le64(htime_content, block.atime_win);
            }
            if (has_ns && is_unix) {
                if (has_mtime) push_le32(htime_content, block.mtime_ns);
                if (has_ctime) push_le32(htime_content, block.ctime_ns);
                if (has_atime) push_le32(htime_content, block.atime_ns);
            }

            core::push_vint(extra, htime_content.size() + 1);
            extra.push_back(FHEXTRA_HTIME);
            extra.insert(extra.end(), htime_content.begin(), htime_content.end());
        }
    }

    // File Version (FHEXTRA_VERSION 0x04)
    if (block.has_file_version) {
        std::vector<core::byte> ver_content;
        core::push_vint(ver_content, 0); // flags
        core::push_vint(ver_content, block.file_version);
        core::push_vint(extra, ver_content.size() + 1);
        extra.push_back(FHEXTRA_VERSION);
        extra.insert(extra.end(), ver_content.begin(), ver_content.end());
    }

    // Unix Owner (FHEXTRA_OWNER 0x06)
    if (block.has_owner || !block.owner_user.empty() || !block.owner_group.empty() ||
        block.has_owner_uid || block.has_owner_gid) {
        core::uint64 oflags = 0;
        if (!block.owner_user.empty()) oflags |= 0x0001;
        if (!block.owner_group.empty()) oflags |= 0x0002;
        if (block.has_owner_uid) oflags |= 0x0004;
        if (block.has_owner_gid) oflags |= 0x0008;

        std::vector<core::byte> owner_content;
        core::push_vint(owner_content, oflags);
        // Spec limit: name fields are at most 255 bytes. Truncate over-long
        // names at serialization — a longer field makes the record malformed
        // for readers (ours discards such records rather than misparse)
        // (v1.21.2).
        auto push_owner_name = [&owner_content](const std::string& name) {
            size_t len = std::min<size_t>(name.size(), 255);
            core::push_vint(owner_content, len);
            owner_content.insert(owner_content.end(), name.begin(), name.begin() + len);
        };
        if (oflags & 0x0001) push_owner_name(block.owner_user);
        if (oflags & 0x0002) push_owner_name(block.owner_group);
        if (oflags & 0x0004) core::push_vint(owner_content, block.owner_uid);
        if (oflags & 0x0008) core::push_vint(owner_content, block.owner_gid);

        core::push_vint(extra, owner_content.size() + 1);
        extra.push_back(FHEXTRA_OWNER);
        extra.insert(extra.end(), owner_content.begin(), owner_content.end());
    }

    // Service SubData (e.g. CMT)
    if (block.is_service && !block.sub_data.empty()) {
        core::push_vint(extra, block.sub_data.size() + 1);
        extra.push_back(FHEXTRA_SUBDATA);
        extra.insert(extra.end(), block.sub_data.begin(), block.sub_data.end());
    }

    // Unknown extra records (v1.24 plan §7.3): captured verbatim on parse
    // and re-encoded byte-identically here, appended after the known
    // records (extra-area record order is not significant per spec — each
    // record is self-describing).
    for (const auto& unk : block.unknown_extras) {
        extra.insert(extra.end(), unk.raw.begin(), unk.raw.end());
    }

    core::uint64 head_flags =
        (block.pack_size >= 0 ? HFL_DATA : 0) | (extra.empty() ? 0 : HFL_EXTRA) |
        (extra_head_flags & (HFL_SPLITBEFORE | HFL_SPLITAFTER | HFL_CHILD | HFL_INHERITED));

    std::vector<core::byte> body;
    core::push_vint(body, block.is_service ? HEAD_SERVICE : HEAD_FILE);
    core::push_vint(body, head_flags);
    if (head_flags & HFL_EXTRA) {
        core::push_vint(body, extra.size());
    }
    if (block.pack_size >= 0) {
        if (fixed_pack_size_vint) {
            core::push_vint_fixed(body, static_cast<core::uint64>(block.pack_size), 10);
        } else {
            core::push_vint(body, static_cast<core::uint64>(block.pack_size));
        }
    }

    core::uint64 eff_file_flags = block.file_flags;
    if (block.has_crc32) eff_file_flags |= FHFL_CRC32;
    if (block.utime_unix != 0) eff_file_flags |= FHFL_UTIME;

    core::push_vint(body, eff_file_flags);
    core::push_vint(body, block.unp_size);
    core::push_vint(body, block.attributes);

    if (eff_file_flags & FHFL_UTIME) {
        push_le32(body, block.utime_unix);
    }
    if (eff_file_flags & FHFL_CRC32) {
        push_le32(body, block.data_crc32);
    }

    // Compression info:
    // bits 0..5: version of compression algorithm (0 = RAR5, 1 = RAR7)
    // bit 6 (0x40): solid flag
    // bits 7..9 (0x380): compression method (0..5)
    // bits 10..14 (0x7C00): dictionary size base power
    // bits 15..19 (0xF8000): dictionary size fraction (RAR7)
    core::uint32 comp_info =
        (block.unp_ver & 0x3F) | (block.is_solid ? 0x40 : 0) | ((block.method & 7) << 7);
    if (block.win_size >= 0x20000) {
        core::uint64 pow2 = 0x20000;
        core::uint32 dict_bits = 0;
        while (2 * pow2 <= block.win_size && dict_bits < (block.unp_ver == 1 ? 23u : 15u)) {
            pow2 *= 2;
            dict_bits++;
        }
        comp_info |= (dict_bits << 10);
        if (block.unp_ver == 1) {
            if (block.win_size > pow2) {
                core::uint64 fraction = (block.win_size - pow2) * 32 / pow2;
                if (fraction > 31) fraction = 31;
                comp_info |= ((static_cast<core::uint32>(fraction) & 0x1F) << 15);
            }
            // RAR7 dictionary sizing with RAR5 compression algorithm
            comp_info |= FCI_RAR5_COMPAT;
        }
    }
    core::push_vint(body, comp_info);

    core::uint64 host_os = block.host_os;
    core::push_vint(body, host_os);

    core::push_vint(body, block.file_name.size());
    body.insert(body.end(), block.file_name.begin(), block.file_name.end());

    if (!extra.empty()) {
        body.insert(body.end(), extra.begin(), extra.end());
    }

    return wrap_block(body);
}

bool HeaderWriter::write_file_block(io::FileStream& dest, const FileBlock& block,
                                    core::uint64 extra_head_flags, HeaderCryptWriter* crypt,
                                    bool fixed_pack_size_vint) {
    auto wrapped = serialize_file_block(block, extra_head_flags, fixed_pack_size_vint);
    return emit_block(dest, wrapped, crypt);
}

bool HeaderWriter::write_crypt_block(io::FileStream& dest, const CryptBlock& block) {
    std::vector<core::byte> body;
    core::push_vint(body, HEAD_CRYPT);
    core::push_vint(body, 0); // Common flags: HEAD_CRYPT has no extra, no data

    core::push_vint(body, block.crypt_version);
    core::uint32 enc_flags = block.has_psw_check ? 1 : 0; // CHFL_CRYPT_PSWCHECK = 0x01
    core::push_vint(body, enc_flags);
    body.push_back(block.lg2_count);
    body.insert(body.end(), block.salt.begin(), block.salt.end());
    if (block.has_psw_check) {
        body.insert(body.end(), block.psw_check.begin(), block.psw_check.end());
        body.insert(body.end(), block.psw_check_csum.begin(), block.psw_check_csum.end());
    }

    auto wrapped = wrap_block(body);
    return dest.write(wrapped.data(), wrapped.size()) == wrapped.size();
}

bool HeaderWriter::write_end_block(io::FileStream& dest, const EndArcBlock& block,
                                   HeaderCryptWriter* crypt) {
    std::vector<core::byte> body;
    core::push_vint(body, HEAD_ENDARC);
    core::push_vint(body, 0); // Common flags
    core::push_vint(body, block.end_flags);

    auto wrapped = wrap_block(body);
    return emit_block(dest, wrapped, crypt);
}

} // namespace openrar::format
