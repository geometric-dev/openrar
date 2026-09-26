#include "header_reader.hpp"
#include "../core/vint.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/aes256.hpp"
#include "../crypto/sha256.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace openrar::format {

namespace {

// Parse type / common flags / extra / data-size from a block body. Shared by
// the plaintext and the decrypted read paths.
bool parse_block_fields(const std::vector<core::byte>& body, core::uint64& out_type,
                        core::uint64& out_flags, core::uint64& out_data_size) {
    size_t offset = 0;
    size_t read_bytes = 0;
    if (!core::read_vint(body.data() + offset, body.size() - offset, out_type, read_bytes))
        return false;
    offset += read_bytes;

    if (!core::read_vint(body.data() + offset, body.size() - offset, out_flags, read_bytes))
        return false;
    offset += read_bytes;

    if (out_flags & HFL_EXTRA) {
        core::uint64 extra_size = 0;
        if (!core::read_vint(body.data() + offset, body.size() - offset, extra_size, read_bytes))
            return false;
        offset += read_bytes;
    }

    out_data_size = 0;
    if (out_flags & HFL_DATA) {
        if (!core::read_vint(body.data() + offset, body.size() - offset, out_data_size, read_bytes))
            return false;
        offset += read_bytes;
    }
    return true;
}

} // namespace

bool HeaderCryptReader::init(const std::string& password, const CryptBlock& crypt) {
    active = false;
    bad_password = false;
    if (crypt.crypt_version != 0 || crypt.lg2_count > 24) return false;

    crypto::Pbkdf2Rar5::derive_keys(password, crypt.salt.data(), crypt.salt.size(),
                                    1U << crypt.lg2_count, keys);

    // Checksum gate: only trust the stored PswCheck when its SHA256 checksum
    // matches; a damaged PswCheck is treated as absent and the header CRC32
    // becomes the password verdict instead.
    bool use_check = false;
    if (crypt.has_psw_check) {
        core::byte digest[crypto::Sha256::DIGEST_SIZE];
        crypto::Sha256::compute(crypt.psw_check.data(), crypt.psw_check.size(), digest);
        if (std::memcmp(digest, crypt.psw_check_csum.data(), crypt.psw_check_csum.size()) == 0) {
            use_check = true;
        }
    }
    // Constant-time compare (report L3): an early-exit memcmp here would let
    // header-decode timing reveal how many leading PswCheck bytes match.
    if (use_check && !crypto::Pbkdf2Rar5::constant_time_equal(
                         keys.psw_check, crypt.psw_check.data(), sizeof(keys.psw_check))) {
        bad_password = true;
        return false;
    }
    active = true;
    return true;
}

bool HeaderReader::read_signature(io::FileStream& src) {
    core::byte buf[RAR5_SIGNATURE_SIZE];
    if (src.read(buf, sizeof(buf)) != sizeof(buf)) {
        return false;
    }
    return std::memcmp(buf, rar5_signature(), RAR5_SIGNATURE_SIZE) == 0;
}

HeaderResult HeaderReader::read_block_raw(io::ReadSource& src, core::uint64& out_type,
                                          core::uint64& out_flags,
                                          std::vector<core::byte>& out_body,
                                          core::uint64& out_data_size, HeaderCryptReader* crypt) {
    out_body.clear();
    out_data_size = 0;

    if (crypt && crypt->active) {
        // Encrypted header layout: [16-byte clear IV][AES-256-CBC ciphertext
        // of the plaintext [CRC32 | size vint | body] zero-padded to 16].
        // The CBC chain carries over from the first cipher block into the
        // remainder, so decrypt the first block, recover the header size and
        // only then read the rest of the ciphertext.
        core::byte iv[16];
        if (src.read(iv, sizeof(iv)) != sizeof(iv)) return HeaderResult::Eof;

        std::vector<core::byte> plain_first(16);
        if (src.read(plain_first.data(), 16) != 16) return HeaderResult::Error;
        crypto::Aes256 aes(crypt->keys.aes_key);
        if (!aes.decrypt_cbc(plain_first.data(), plain_first.size(), iv))
            return HeaderResult::Error;

        core::uint32 expected_crc = core::read_le32(plain_first.data());

        // Recover the body-size vint from the decrypted first block. It
        // starts at plaintext offset 4.
        // 10 = maximum length of a 64-bit VINT; always fits in first 16-byte AES block (bytes 4..15 = 12 bytes).
        std::vector<core::byte> size_vint_bytes;
        size_t sv_len = 0;
        core::uint64 body_size = 0;
        size_t read_bytes = 0;
        while (sv_len < (plain_first.size() - 4) && sv_len < 10) {
            core::byte b = plain_first[4 + sv_len];
            size_vint_bytes.push_back(b);
            sv_len++;
            if ((b & 0x80) == 0) break;
        }
        if (!core::read_vint(size_vint_bytes.data(), size_vint_bytes.size(), body_size,
                             read_bytes)) {
            crypt->bad_password = true; // Unterminated or garbage VINT indicates bad password
            return HeaderResult::Error;
        }
        if (body_size > 2 * 1024 * 1024) {
            // Deliberate UX heuristic: garbage from wrong password frequently decodes to enormous body_size
            crypt->bad_password = true;
            return HeaderResult::Error;
        }

        // Total plaintext = CRC32(4) + size vint + body; ciphertext is the
        // zero-padded plaintext rounded up to whole cipher blocks. The
        // plaintext buffer always holds at least the decrypted first block.
        size_t total_plain = 4 + sv_len + static_cast<size_t>(body_size);
        size_t cipher_total = (total_plain + 15u) & ~size_t(15);
        if (cipher_total < 16) {
            crypt->bad_password = true;
            return HeaderResult::Error;
        }
        size_t rest = cipher_total - 16;
        std::vector<core::byte> plain(std::max(total_plain, size_t(16)));
        std::memcpy(plain.data(), plain_first.data(), 16);
        if (rest > 0) {
            std::vector<core::byte> cipher_rest(rest);
            if (src.read(cipher_rest.data(), rest) != rest) return HeaderResult::Error;
            // `iv` now holds the previous cipher block written back by
            // decrypt_cbc, which is exactly the chaining value needed here.
            if (!aes.decrypt_cbc(cipher_rest.data(), rest, iv)) return HeaderResult::Error;
            std::memcpy(plain.data() + 16, cipher_rest.data(), total_plain - 16);
        }

        // CRC32 covers the size vint and body of the plaintext header.
        crypto::Crc32 crc;
        crc.update(size_vint_bytes.data(), size_vint_bytes.size());
        crc.update(plain.data() + 4 + sv_len, static_cast<size_t>(body_size));
        if (crc.get() != expected_crc) {
            crypt->bad_password = true;
            return HeaderResult::Error;
        }

        out_body.assign(plain.begin() + 4 + sv_len, plain.begin() + total_plain);
        return parse_block_fields(out_body, out_type, out_flags, out_data_size)
                   ? HeaderResult::Ok
                   : HeaderResult::Error;
    }

    // Read 4-byte CRC32
    core::byte crc_buf[4];
    if (src.read(crc_buf, 4) != 4) {
        return HeaderResult::Eof;
    }
    core::uint32 expected_crc = core::read_le32(crc_buf);

    // Read body_size vint. One read call for a 10-byte prefix instead of one
    // syscall per byte: every header previously cost up to 10 token reads
    // through the unbuffered FileStream. Over-consumption beyond the vint is
    // undone with a seek so nested/header scanning positions stay exact.
    std::vector<core::byte> size_vint_bytes;
    core::uint64 body_size = 0;
    {
        core::byte prefix[10];
        size_t got = src.read(prefix, sizeof(prefix));
        if (got == 0) return HeaderResult::Eof;
        size_t vint_len = 0;
        for (size_t i = 0; i < got; ++i) {
            size_vint_bytes.push_back(prefix[i]);
            if ((prefix[i] & 0x80) == 0) {
                vint_len = i + 1;
                break;
            }
        }
        if (vint_len == 0) return HeaderResult::Error; // 10 continuation bytes: malformed
        if (got > vint_len &&
            !src.seek(static_cast<core::int64>(vint_len) - static_cast<core::int64>(got),
                      io::SeekOrigin::Current)) {
            return HeaderResult::Error;
        }
        size_vint_bytes.resize(vint_len);
    }

    size_t read_bytes = 0;
    if (!core::read_vint(size_vint_bytes.data(), size_vint_bytes.size(), body_size, read_bytes)) {
        return HeaderResult::Error;
    }
    // Spec 00-overview: Header size max 2 MiB (3-byte vint) — reject larger to avoid OOM / DoS
    if (body_size > 2 * 1024 * 1024) {
        return HeaderResult::Error;
    }

    // Read body
    out_body.resize(static_cast<size_t>(body_size));
    if (src.read(out_body.data(), out_body.size()) != out_body.size()) {
        return HeaderResult::Error;
    }

    // Verify CRC32
    crypto::Crc32 crc;
    crc.update(size_vint_bytes.data(), size_vint_bytes.size());
    crc.update(out_body.data(), out_body.size());
    if (crc.get() != expected_crc) {
        return HeaderResult::HeaderCrcMismatch;
    }

    return parse_block_fields(out_body, out_type, out_flags, out_data_size) ? HeaderResult::Ok
                                                                            : HeaderResult::Error;
}

HeaderResult HeaderReader::read_block_raw_mem(const core::byte* data, size_t size,
                                              size_t& cur_offset, core::uint64& out_type,
                                              core::uint64& out_flags,
                                              std::vector<core::byte>& out_body,
                                              core::uint64& out_data_size) {
    out_body.clear();
    out_data_size = 0;

    if (cur_offset + 4 > size) return HeaderResult::Error;
    core::uint32 expected_crc = core::read_le32(data + cur_offset);
    cur_offset += 4;

    core::uint64 body_size = 0;
    size_t read_bytes = 0;
    if (!core::read_vint(data + cur_offset, size - cur_offset, body_size, read_bytes)) {
        return HeaderResult::Error;
    }
    if (body_size > 2 * 1024 * 1024) {
        return HeaderResult::Error;
    }
    size_t size_vint_len = read_bytes;
    const core::byte* size_vint_ptr = data + cur_offset;
    cur_offset += read_bytes;

    if (cur_offset + body_size > size) return HeaderResult::Error;
    out_body.assign(data + cur_offset, data + cur_offset + body_size);

    crypto::Crc32 crc;
    crc.update(size_vint_ptr, size_vint_len);
    crc.update(out_body.data(), out_body.size());
    if (crc.get() != expected_crc) {
        return HeaderResult::HeaderCrcMismatch;
    }
    cur_offset += static_cast<size_t>(body_size);

    size_t offset = 0;
    if (!core::read_vint(out_body.data() + offset, out_body.size() - offset, out_type, read_bytes))
        return HeaderResult::Error;
    offset += read_bytes;

    if (!core::read_vint(out_body.data() + offset, out_body.size() - offset, out_flags, read_bytes))
        return HeaderResult::Error;
    offset += read_bytes;

    if (out_flags & HFL_EXTRA) {
        core::uint64 extra_size = 0;
        if (!core::read_vint(out_body.data() + offset, out_body.size() - offset, extra_size,
                             read_bytes))
            return HeaderResult::Error;
        offset += read_bytes;
    }

    if (out_flags & HFL_DATA) {
        if (!core::read_vint(out_body.data() + offset, out_body.size() - offset, out_data_size,
                             read_bytes))
            return HeaderResult::Error;
        offset += read_bytes;
    }

    return HeaderResult::Ok;
}

bool HeaderReader::parse_main_header(const core::byte* body, size_t body_size,
                                     MainBlock& out_block) {
    out_block = MainBlock{};
    size_t offset = 0;
    size_t read_bytes = 0;

    core::uint64 type = 0, flags = 0;
    if (!core::read_vint(body + offset, body_size - offset, type, read_bytes) || type != HEAD_MAIN)
        return false;
    offset += read_bytes;

    if (!core::read_vint(body + offset, body_size - offset, flags, read_bytes)) return false;
    offset += read_bytes;

    core::uint64 extra_size = 0;
    if (flags & HFL_EXTRA) {
        if (!core::read_vint(body + offset, body_size - offset, extra_size, read_bytes))
            return false;
        offset += read_bytes;
    }

    if (!core::read_vint(body + offset, body_size - offset, out_block.arc_flags, read_bytes))
        return false;
    offset += read_bytes;

    if (out_block.arc_flags & MHFL_VOLNUMBER) {
        if (!core::read_vint(body + offset, body_size - offset, out_block.vol_number, read_bytes))
            return false;
        offset += read_bytes;
    }

    // Parse extra area if present. The extra area ends where the header ends,
    // so locate it from the end rather than assuming it follows the last
    // fixed field (future fields must be skipped, not misparsed).
    if (flags & HFL_EXTRA && extra_size > 0 && offset < body_size) {
        size_t extra_start = offset;
        if (extra_size <= body_size) {
            size_t from_end = body_size - static_cast<size_t>(extra_size);
            if (from_end > offset) extra_start = from_end;
        }
        offset = extra_start;
        size_t extra_end = body_size;
        while (offset < extra_end) {
            size_t old_offset = offset;
            core::uint64 record_size = 0;
            if (!core::read_vint(body + offset, extra_end - offset, record_size, read_bytes)) break;
            offset += read_bytes;

            if (offset >= extra_end || record_size < 1) break;
            // Overflow-safe clamp: record_size comes straight from a vint and
            // can be up to 2^64-1, so the naive `offset + record_size` can
            // wrap and underflow rec_end below offset (huge rec_end - offset
            // in the field reads below). extra_end > offset is invariant here.
            size_t rec_end = record_size <= extra_end - offset
                                 ? offset + static_cast<size_t>(record_size)
                                 : extra_end;
            core::byte rec_type = body[offset++];

            if (rec_type == MHEXTRA_LOCATOR && record_size >= 2) {
                out_block.has_locator = true;
                core::uint64 loc_flags = 0;
                if (core::read_vint(body + offset, rec_end - offset, loc_flags, read_bytes)) {
                    offset += read_bytes;
                    if (loc_flags & 0x01) { // QLIST
                        core::uint64 qo_off = 0;
                        if (core::read_vint(body + offset, rec_end - offset, qo_off, read_bytes)) {
                            // Spec: 0 means absent/preallocation insufficient — ignore
                            if (qo_off != 0)
                                out_block.locator_qo_offset = static_cast<core::int64>(qo_off);
                            else
                                out_block.locator_qo_offset = -1;
                            offset += read_bytes;
                        }
                    }
                    if (loc_flags & 0x02) { // RR
                        core::uint64 rr_off = 0;
                        if (core::read_vint(body + offset, rec_end - offset, rr_off, read_bytes)) {
                            if (rr_off != 0)
                                out_block.locator_rr_offset = static_cast<core::int64>(rr_off);
                            else
                                out_block.locator_rr_offset = -1;
                            offset += read_bytes;
                        }
                    }
                }
            } else if (rec_type == MHEXTRA_METADATA) {
                size_t cur = offset;
                core::uint64 meta_flags = 0;
                size_t rb = 0;
                if (core::read_vint(body + cur, rec_end - cur, meta_flags, rb)) {
                    cur += rb;
                    out_block.has_metadata = true;
                    out_block.metadata_is_unix_time = (meta_flags & 0x04) != 0;
                    out_block.metadata_is_nanoseconds = (meta_flags & 0x08) != 0;
                    if (meta_flags & 0x01) {
                        core::uint64 name_len = 0;
                        if (core::read_vint(body + cur, rec_end - cur, name_len, rb)) {
                            cur += rb;
                            // Subtraction form: name_len is a raw vint that can
                            // approach 2^64, so cur + name_len wraps and passes
                            // an addition-based guard, then an ~2^64-length
                            // assign throws (or aborts the wasm module).
                            if (name_len <= static_cast<core::uint64>(rec_end - cur)) {
                                out_block.metadata_name.assign(
                                    reinterpret_cast<const char*>(body + cur),
                                    static_cast<size_t>(name_len));
                                size_t null_pos = out_block.metadata_name.find('\0');
                                if (null_pos != std::string::npos) {
                                    out_block.metadata_name.resize(null_pos);
                                }
                                cur += static_cast<size_t>(name_len);
                            }
                        }
                    }
                    if (meta_flags & 0x02) {
                        if (out_block.metadata_is_unix_time) {
                            if (out_block.metadata_is_nanoseconds && cur + 8 <= rec_end) {
                                out_block.metadata_ctime = core::read_le64(body + cur);
                                cur += 8;
                            } else if (!out_block.metadata_is_nanoseconds && cur + 4 <= rec_end) {
                                out_block.metadata_ctime = core::read_le32(body + cur);
                                cur += 4;
                            }
                        } else if (cur + 8 <= rec_end) {
                            out_block.metadata_ctime = core::read_le64(body + cur);
                            cur += 8;
                        }
                    }
                }
            }
            offset = rec_end;
            if (offset <= old_offset) return false;
        }
    }

    return true;
}

bool HeaderReader::parse_file_header(const core::byte* body, size_t body_size,
                                     FileBlock& out_block) {
    out_block = FileBlock{};
    size_t offset = 0;
    size_t read_bytes = 0;

    core::uint64 type = 0, flags = 0;
    if (!core::read_vint(body + offset, body_size - offset, type, read_bytes)) return false;
    if (type != HEAD_FILE && type != HEAD_SERVICE) return false;
    out_block.is_service = (type == HEAD_SERVICE);
    offset += read_bytes;

    if (!core::read_vint(body + offset, body_size - offset, flags, read_bytes)) return false;
    offset += read_bytes;

    core::uint64 extra_size = 0;
    if (flags & HFL_EXTRA) {
        if (!core::read_vint(body + offset, body_size - offset, extra_size, read_bytes))
            return false;
        offset += read_bytes;
    }

    if (flags & HFL_DATA) {
        core::uint64 pack_sz = 0;
        if (!core::read_vint(body + offset, body_size - offset, pack_sz, read_bytes)) return false;
        out_block.pack_size = static_cast<core::int64>(pack_sz);
        offset += read_bytes;
    } else {
        out_block.pack_size = -1;
    }

    if (!core::read_vint(body + offset, body_size - offset, out_block.file_flags, read_bytes))
        return false;
    offset += read_bytes;

    if (!core::read_vint(body + offset, body_size - offset, out_block.unp_size, read_bytes))
        return false;
    offset += read_bytes;
    out_block.unp_unknown = (out_block.file_flags & FHFL_UNPUNKNOWN) != 0;

    if (!core::read_vint(body + offset, body_size - offset, out_block.attributes, read_bytes))
        return false;
    offset += read_bytes;

    if (out_block.file_flags & FHFL_UTIME) {
        if (offset + 4 > body_size) return false;
        out_block.utime_unix = core::read_le32(body + offset);
        offset += 4;
    }

    if (out_block.file_flags & FHFL_CRC32) {
        if (offset + 4 > body_size) return false;
        out_block.has_crc32 = true;
        out_block.data_crc32 = core::read_le32(body + offset);
        offset += 4;
    }

    core::uint64 comp_info = 0;
    if (!core::read_vint(body + offset, body_size - offset, comp_info, read_bytes)) return false;
    offset += read_bytes;

    // RAR5 format spec for Compression information:
    // bits 0..5: version of compression algorithm (0 = RAR5, 1 = RAR7)
    // bit 6 (0x40): solid flag
    // bits 7..9 (0x380): compression method (0..5)
    // bits 10..14 (0x7C00): dictionary size base power
    // bits 15..19 (0xF8000): dictionary size fraction (RAR7)
    // bit 20 (0x100000): RAR5 compat flag
    out_block.unp_ver = static_cast<core::uint32>(comp_info & 0x3F);
    out_block.is_solid = (comp_info & 0x40) != 0;
    out_block.method = static_cast<core::uint32>((comp_info >> 7) & 7);

    if (out_block.unp_ver > 1 || (out_block.method == 0 && (comp_info & 0x7C00) == 0)) {
        out_block.win_size = 0;
    } else {
        // Spec 01-headers.md:95 – dictionary size 128 KiB << N  ; N max 15 version0 (4096 MiB), N max 23 version1 (1 TB)
        // with FCI_DICT_FRACT (bits 15-19) for version1. Validate against spec max and alloc limit 1 GiB.
        // Dictionaries above the spec max fail instead of truncating.
        core::uint32 raw_dict_bits = static_cast<core::uint32>((comp_info >> 10) & 0x1F);
        if (out_block.unp_ver == 0 && raw_dict_bits > 15) {
            // Version0 max N=15 ; raw >15 is spec-invalid -> treat as dictionary too large (fail not truncate)
            // Keep win_size as spec-invalid sentinel > RAR_DICT_MAX_V0 so decompressor will report "dictionary too large"
            out_block.win_size = RAR_DICT_MAX_V0 + 1;
        } else if (out_block.unp_ver == 1 && raw_dict_bits > 23) {
            // Version1 max N=23 (1 TB)
            out_block.win_size = RAR_DICT_MAX_V1 + 1;
        } else {
            core::uint32 dict_bits = raw_dict_bits;
            // For version0, mask was 0x0F, already validated raw<=15; for version1, allow 0..23
            core::uint64 base = RAR_DICT_BASE << dict_bits;
            core::uint64 win = base;
            if (out_block.unp_ver == 1) {
                core::uint32 frac = static_cast<core::uint32>((comp_info >> 15) & 0x1F);
                win += (base / 32) * frac;
                // FCI_RAR5_COMPAT (0x100000) indicates RAR5 compression algorithm compatibility.
                // For version1, validate effective dict against 1 TB spec max (with fract)
                if (win > RAR_DICT_MAX_V1) {
                    win = RAR_DICT_MAX_V1 + 1; // sentinel for too large
                }
            } else {
                if (win > RAR_DICT_MAX_V0) {
                    win = RAR_DICT_MAX_V0 + 1;
                }
            }
            out_block.win_size = win;
            // Note: win up to 1 TB fits in uint64; decompressor will further check > RAR_DICT_ALLOC_LIMIT
            // and return false with "dictionary too large" instead of truncating (spec 01:95).
        }
    }

    core::uint64 host_os = 0;
    if (!core::read_vint(body + offset, body_size - offset, host_os, read_bytes)) return false;
    out_block.host_os = static_cast<core::uint32>(host_os);
    offset += read_bytes;

    core::uint64 name_len = 0;
    if (!core::read_vint(body + offset, body_size - offset, name_len, read_bytes)) return false;
    offset += read_bytes;
    // Overflow-safe form: name_len from a 10-byte vint can be near 2^64-1 and
    // make `offset + name_len` wrap, passing the old check; the subtraction
    // form cannot wrap (offset <= body_size is maintained by read_vint bounds).
    if (name_len > body_size - offset) return false;
    out_block.file_name.assign(reinterpret_cast<const char*>(body + offset),
                               static_cast<size_t>(name_len));
    // v1.24 plan §4.3: names that are not well-formed UTF-8 are accepted
    // byte-losslessly here; the extraction pipeline percent-encodes the
    // invalid sequences (%XX) so the escaped name IS the on-disk name
    // (previously the whole header parse failed, making such archives
    // unreadable). UI paths sanitize for display independently.
    offset += static_cast<size_t>(name_len);

    if (out_block.is_service) {
        out_block.service_type = out_block.file_name;
    }

    // Parse extra records. Same end-anchored layout as the main header.
    if (flags & HFL_EXTRA && extra_size > 0 && offset < body_size) {
        size_t extra_start = offset;
        if (extra_size <= body_size) {
            size_t from_end = body_size - static_cast<size_t>(extra_size);
            if (from_end > offset) extra_start = from_end;
        }
        offset = extra_start;
        size_t extra_end = body_size;
        while (offset < extra_end) {
            size_t old_offset = offset;
            core::uint64 rec_size = 0;
            if (!core::read_vint(body + offset, extra_end - offset, rec_size, read_bytes)) break;
            offset += read_bytes;

            if (offset >= extra_end || rec_size < 1) break;
            // Overflow-safe clamp (see parse_main_header): rec_size is an
            // unchecked vint, naive addition wraps to a rec_end below offset.
            size_t rec_end =
                rec_size <= extra_end - offset ? offset + static_cast<size_t>(rec_size) : extra_end;
            core::byte rec_type = body[offset++];

            if (rec_type == FHEXTRA_HASH && rec_size >= 34) {
                // rec_size claims 34 bytes (type + hash_type + 32 digest), but
                // rec_end may be clamped back to extra_end — verify actual
                // remaining room (hash_type byte + 32-byte digest) before the
                // reads, otherwise an oversized rec_size near the end of the
                // extra area reads past the end of body.
                if (rec_end - offset >= 33) {
                    core::byte hash_type = body[offset++];
                    if (hash_type == 0) { // BLAKE2sp
                        out_block.has_blake2sp = true;
                        std::memcpy(out_block.blake2sp.data(), body + offset, 32);
                        offset += 32;
                    }
                }
            } else if (rec_type == FHEXTRA_CRYPT) {
                out_block.is_encrypted = true;
                core::uint64 cver = 0, cflags = 0;
                core::read_vint(body + offset, rec_end - offset, cver, read_bytes);
                offset += read_bytes;
                core::read_vint(body + offset, rec_end - offset, cflags, read_bytes);
                offset += read_bytes;
                out_block.crypt_version = static_cast<core::uint32>(cver);
                out_block.crypt_flags = static_cast<core::uint32>(cflags);
                if (offset < rec_end) out_block.lg2_count = body[offset++];
                if (offset + 16 <= rec_end) {
                    std::memcpy(out_block.salt.data(), body + offset, 16);
                    offset += 16;
                }
                if (offset + 16 <= rec_end) {
                    std::memcpy(out_block.init_v.data(), body + offset, 16);
                    offset += 16;
                }
                if (cflags & 0x01) { // FHEXTRA_CRYPT_PSWCHECK
                    out_block.has_psw_check = true;
                    if (offset + 8 <= rec_end) {
                        std::memcpy(out_block.psw_check.data(), body + offset, 8);
                        offset += 8;
                    }
                    if (offset + 4 <= rec_end) {
                        std::memcpy(out_block.psw_check_csum.data(), body + offset, 4);
                        offset += 4;
                    }
                    // An all-zero check field means no explicit password check was
                    // stored; leave verification to the data checksum instead of
                    // rejecting the correct password here.
                    bool all_zero = true;
                    for (int i = 0; i < 8; ++i) all_zero &= (out_block.psw_check[i] == 0);
                    if (all_zero) out_block.has_psw_check = false;
                }
            } else if (rec_type == FHEXTRA_REDIR ||
                       rec_type == 0x04) { // 0x04 compat for early prototype archives
                // Type collision: spec FHEXTRA_VERSION shares 0x04 with the
                // redirect record shape early prototype archives used. A version
                // record is exactly [flags vint == 0][version vint] filling
                // the record; anything else parses as the legacy redirect.
                // Without this probe, records the writer itself emits
                // (header_writer.cpp FHEXTRA_VERSION) misparsed as redirects
                // and has_file_version was never set.
                {
                    core::uint64 vflags = 0, vnum = 0;
                    size_t v1 = 0, v2 = 0;
                    bool is_version =
                        core::read_vint(body + offset, rec_end - offset, vflags, v1) &&
                        vflags == 0 && offset + v1 <= rec_end &&
                        core::read_vint(body + offset + v1, rec_end - offset - v1, vnum, v2) &&
                        offset + v1 + v2 == rec_end;
                    if (is_version) {
                        offset = rec_end;
                        out_block.has_file_version = true;
                        out_block.file_version = vnum;
                        continue;
                    }
                }
                core::uint64 rtype = 0, rflags = 0, tlen = 0;
                // Spec 07-services: NameSize <2048 else ignore record
                if (!core::read_vint(body + offset, rec_end - offset, rtype, read_bytes)) {
                    offset = rec_end;
                    continue;
                }
                offset += read_bytes;
                if (!core::read_vint(body + offset, rec_end - offset, rflags, read_bytes)) {
                    offset = rec_end;
                    continue;
                }
                offset += read_bytes;
                if (!core::read_vint(body + offset, rec_end - offset, tlen, read_bytes)) {
                    offset = rec_end;
                    continue;
                }
                offset += read_bytes;
                if (tlen >= 2048) {
                    offset = rec_end;
                    continue;
                } // ignore per spec
                out_block.redir_type = static_cast<core::uint8>(rtype);
                out_block.redir_dir_target = (rflags & 0x01) != 0;
                size_t actual_tlen =
                    std::min(static_cast<size_t>(tlen), rec_end >= offset ? rec_end - offset : 0);
                out_block.redir_target.assign(reinterpret_cast<const char*>(body + offset),
                                              actual_tlen);
                offset += actual_tlen;
            } else if (rec_type == FHEXTRA_HTIME) {
                core::uint64 tflags = 0;
                size_t tf_read = 0;
                if (!core::read_vint(body + offset, rec_end - offset, tflags, tf_read)) {
                    offset = rec_end;
                    continue;
                }
                offset += tf_read;
                // Compat: legacy prototype format uses 0x01=mtime/0x02=ctime/0x04=atime (FILETIME 8B)
                // Spec uses 0x01=Unix, 0x02=mtime,0x04=ctime,0x08=atime,0x10=ns
                bool is_legacy = (tflags <= 0x07) && (tflags & 0x01) && !(tflags & 0x02) &&
                                 ((rec_end - offset) % 8 == 0) && (rec_end - offset) > 0;
                if (is_legacy) {
                    if ((tflags & 0x01) && offset + 8 <= rec_end) {
                        out_block.mtime_win = core::read_le64(body + offset);
                        offset += 8;
                        out_block.has_mtime_ns = false;
                    }
                    if ((tflags & 0x02) && offset + 8 <= rec_end) {
                        out_block.ctime_win = core::read_le64(body + offset);
                        offset += 8;
                    }
                    if ((tflags & 0x04) && offset + 8 <= rec_end) {
                        out_block.atime_win = core::read_le64(body + offset);
                        offset += 8;
                    }
                    out_block.htime_is_unix = false;
                } else {
                    bool is_unix = (tflags & 0x01) != 0;
                    bool has_mtime = (tflags & 0x02) != 0;
                    bool has_ctime = (tflags & 0x04) != 0;
                    bool has_atime = (tflags & 0x08) != 0;
                    bool has_ns = (tflags & 0x10) != 0;
                    out_block.htime_is_unix = is_unix;
                    if (has_mtime) {
                        if (is_unix) {
                            if (offset + 4 <= rec_end) {
                                out_block.htime_mtime_unix = core::read_le32(body + offset);
                                offset += 4;
                            }
                        } else if (offset + 8 <= rec_end) {
                            out_block.mtime_win = core::read_le64(body + offset);
                            offset += 8;
                        }
                    }
                    if (has_ctime) {
                        if (is_unix) {
                            if (offset + 4 <= rec_end) {
                                out_block.htime_ctime_unix = core::read_le32(body + offset);
                                offset += 4;
                            }
                        } else if (offset + 8 <= rec_end) {
                            out_block.ctime_win = core::read_le64(body + offset);
                            offset += 8;
                        }
                    }
                    if (has_atime) {
                        if (is_unix) {
                            if (offset + 4 <= rec_end) {
                                out_block.htime_atime_unix = core::read_le32(body + offset);
                                offset += 4;
                            }
                        } else if (offset + 8 <= rec_end) {
                            out_block.atime_win = core::read_le64(body + offset);
                            offset += 8;
                        }
                    }
                    if (has_ns && is_unix) {
                        if (has_mtime && offset + 4 <= rec_end) {
                            core::uint32 raw = core::read_le32(body + offset);
                            raw &= 0x3FFFFFFFu;
                            if (raw < 1000000000u) {
                                out_block.mtime_ns = raw;
                                out_block.has_mtime_ns = true;
                            }
                            offset += 4;
                        }
                        if (has_ctime && offset + 4 <= rec_end) {
                            core::uint32 raw = core::read_le32(body + offset);
                            raw &= 0x3FFFFFFFu;
                            if (raw < 1000000000u) {
                                out_block.ctime_ns = raw;
                                out_block.has_ctime_ns = true;
                            }
                            offset += 4;
                        }
                        if (has_atime && offset + 4 <= rec_end) {
                            core::uint32 raw = core::read_le32(body + offset);
                            raw &= 0x3FFFFFFFu;
                            if (raw < 1000000000u) {
                                out_block.atime_ns = raw;
                                out_block.has_atime_ns = true;
                            }
                            offset += 4;
                        }
                    } else if (has_ns) {
                        // Spec: ns only meaningful with Unix; if has_ns without is_unix, just skip bytes for present stamps
                        int ns_cnt =
                            (has_mtime ? 1 : 0) + (has_ctime ? 1 : 0) + (has_atime ? 1 : 0);
                        size_t need = static_cast<size_t>(ns_cnt) * 4;
                        if (offset + need <= rec_end) offset += need;
                    }
                }
            } else if (rec_type == FHEXTRA_SUBDATA) {
                if (rec_end >= offset) {
                    out_block.sub_data.assign(body + offset, body + rec_end);
                    // Some older writers store the size one byte short; fold a
                    // single trailing byte left over at the end of the extra
                    // area back into this final record.
                    if (rec_end < extra_end && extra_end - rec_end == 1) {
                        out_block.sub_data.push_back(body[rec_end]);
                        rec_end = extra_end;
                    }
                }
            } else if (rec_type == FHEXTRA_OWNER) {
                core::uint64 oflags = 0;
                size_t orr = 0;
                if (!core::read_vint(body + offset, rec_end - offset, oflags, orr)) {
                    offset = rec_end;
                    continue;
                }
                offset += orr;
                // Spec limit: owner name fields are at most 255 bytes. A name
                // length beyond that makes the record malformed — discard the
                // whole record rather than clamping, which would parse the
                // remaining fields out of the middle of the name bytes and
                // feed wrong ownership data into chown (v1.21.2 fix).
                {
                    bool name_ok = true;
                    core::uint64 probe_len = 0;
                    size_t probe_rr = 0;
                    size_t probe_off = offset;
                    if (oflags & 0x0001) {
                        if (!core::read_vint(body + probe_off, rec_end - probe_off, probe_len,
                                             probe_rr) ||
                            probe_len > 255) {
                            name_ok = false;
                        } else {
                            probe_off += probe_rr + probe_len;
                        }
                    }
                    if (name_ok && (oflags & 0x0002)) {
                        if (!core::read_vint(body + probe_off, rec_end - probe_off, probe_len,
                                             probe_rr) ||
                            probe_len > 255) {
                            name_ok = false;
                        }
                    }
                    if (!name_ok) {
                        out_block.has_owner = false;
                        offset = rec_end;
                        continue;
                    }
                }
                out_block.has_owner = true;
                if (oflags & 0x0001) {
                    core::uint64 ulen = 0;
                    if (core::read_vint(body + offset, rec_end - offset, ulen, orr)) {
                        offset += orr;
                        size_t take = std::min(static_cast<size_t>(ulen),
                                               rec_end >= offset ? rec_end - offset : 0);
                        out_block.owner_user.assign(reinterpret_cast<const char*>(body + offset),
                                                    take);
                        offset += take;
                    }
                }
                if (oflags & 0x0002) {
                    core::uint64 glen = 0;
                    if (core::read_vint(body + offset, rec_end - offset, glen, orr)) {
                        offset += orr;
                        size_t take = std::min(static_cast<size_t>(glen),
                                               rec_end >= offset ? rec_end - offset : 0);
                        out_block.owner_group.assign(reinterpret_cast<const char*>(body + offset),
                                                     take);
                        offset += take;
                    }
                }
                if (oflags & 0x0004) {
                    core::uint64 uid = 0;
                    if (core::read_vint(body + offset, rec_end - offset, uid, orr)) {
                        offset += orr;
                        out_block.owner_uid = uid;
                        out_block.has_owner_uid = true;
                    }
                }
                if (oflags & 0x0008) {
                    core::uint64 gid = 0;
                    if (core::read_vint(body + offset, rec_end - offset, gid, orr)) {
                        offset += orr;
                        out_block.owner_gid = gid;
                        out_block.has_owner_gid = true;
                    }
                }
            } else if (rec_type == FHEXTRA_XATTR) {
                // v1.27: extended attributes (FHEXTRA_XATTR). Parsed into
                // FileBlock::xattrs; ANY validation failure (flags != 0,
                // name/value length violations, count or length overrun,
                // trailing bytes, duplicate names) captures the record
                // VERBATIM as an unknown extra — the record is never
                // partially applied and never aborts the header parse
                // (v1.27 plan §1.1). Hostile-input allocation is bounded
                // by the header-size cap: every read is clamped to
                // rec_end, and the entry count to the remaining bytes.
                std::vector<FileBlock::FileXattr> parsed;
                std::unordered_set<std::string> seen_names;
                bool ok = true;
                size_t xoff = offset; // first byte after the type vint
                size_t xr = 0;
                core::uint64 xflags = 0, xcount = 0;
                if (!core::read_vint(body + xoff, rec_end - xoff, xflags, xr) || xflags != 0) {
                    ok = false;
                } else {
                    xoff += xr;
                    if (!core::read_vint(body + xoff, rec_end - xoff, xcount, xr)) {
                        ok = false;
                    } else {
                        xoff += xr;
                        // Each entry consumes at least 3 bytes (name-length
                        // vint + 1 name byte + value-length vint); a count
                        // beyond that cannot be satisfiable.
                        if (xcount > (rec_end - xoff) / 3) {
                            ok = false;
                        } else {
                            seen_names.reserve(static_cast<size_t>(xcount));
                            for (core::uint64 i = 0; ok && i < xcount; ++i) {
                                core::uint64 nlen = 0, vlen = 0;
                                if (!core::read_vint(body + xoff, rec_end - xoff, nlen, xr) ||
                                    nlen == 0 || nlen > format::FHEXTRA_XATTR_NAME_MAX) {
                                    ok = false;
                                    break;
                                }
                                xoff += xr;
                                if (static_cast<size_t>(nlen) > rec_end - xoff) {
                                    ok = false;
                                    break;
                                }
                                FileBlock::FileXattr xa;
                                xa.name.assign(reinterpret_cast<const char*>(body + xoff),
                                               static_cast<size_t>(nlen));
                                xoff += static_cast<size_t>(nlen);
                                if (!core::read_vint(body + xoff, rec_end - xoff, vlen, xr) ||
                                    vlen > format::FHEXTRA_XATTR_VALUE_MAX) {
                                    ok = false;
                                    break;
                                }
                                xoff += xr;
                                if (static_cast<size_t>(vlen) > rec_end - xoff) {
                                    ok = false;
                                    break;
                                }
                                xa.value.assign(body + xoff,
                                                body + xoff + static_cast<size_t>(vlen));
                                xoff += static_cast<size_t>(vlen);
                                if (!seen_names.insert(xa.name).second) ok = false;
                                if (ok) parsed.push_back(std::move(xa));
                            }
                            if (ok && xoff != rec_end) ok = false; // trailing bytes
                        }
                    }
                }
                if (ok) {
                    out_block.xattrs.insert(out_block.xattrs.end(),
                                            std::make_move_iterator(parsed.begin()),
                                            std::make_move_iterator(parsed.end()));
                } else {
                    out_block.unknown_extras.push_back(
                        {rec_type, std::vector<core::byte>(body + old_offset, body + rec_end)});
                }
                offset = rec_end;
            } else {
                // v1.24 plan §7.3: unknown extra records are captured
                // VERBATIM (type vint + size vint + payload) and re-encoded
                // byte-identically during mutations — a roundtrip through
                // OpenRAR never destroys data a newer producer wrote.
                out_block.unknown_extras.push_back(
                    {rec_type, std::vector<core::byte>(body + old_offset, body + rec_end)});
            }
            offset = rec_end;
            if (offset <= old_offset) return false;
        }
    }

    return true;
}

bool HeaderReader::parse_crypt_header(const core::byte* body, size_t body_size,
                                      CryptBlock& out_block) {
    out_block = CryptBlock{};
    size_t offset = 0;
    size_t read_bytes = 0;

    core::uint64 type = 0, flags = 0;
    if (!core::read_vint(body + offset, body_size - offset, type, read_bytes) || type != HEAD_CRYPT)
        return false;
    offset += read_bytes;

    if (!core::read_vint(body + offset, body_size - offset, flags, read_bytes)) return false;
    offset += read_bytes;

    core::uint64 cver = 0, eflags = 0;
    if (!core::read_vint(body + offset, body_size - offset, cver, read_bytes)) return false;
    offset += read_bytes;
    out_block.crypt_version = static_cast<core::uint32>(cver);

    if (!core::read_vint(body + offset, body_size - offset, eflags, read_bytes)) return false;
    offset += read_bytes;
    out_block.enc_flags = static_cast<core::uint32>(eflags);

    if (offset >= body_size) return false;
    out_block.lg2_count = body[offset++];

    if (offset + 16 > body_size) return false;
    std::memcpy(out_block.salt.data(), body + offset, 16);
    offset += 16;

    if (out_block.enc_flags & 0x01) {
        if (offset + 12 > body_size) return false;
        out_block.has_psw_check = true;
        std::memcpy(out_block.psw_check.data(), body + offset, 8);
        offset += 8;
        std::memcpy(out_block.psw_check_csum.data(), body + offset, 4);
        offset += 4;
    }

    return true;
}

bool HeaderReader::parse_end_header(const core::byte* body, size_t body_size,
                                    EndArcBlock& out_block) {
    out_block = EndArcBlock{};
    size_t offset = 0;
    size_t read_bytes = 0;

    core::uint64 type = 0, flags = 0;
    if (!core::read_vint(body + offset, body_size - offset, type, read_bytes) ||
        type != HEAD_ENDARC)
        return false;
    offset += read_bytes;

    if (!core::read_vint(body + offset, body_size - offset, flags, read_bytes)) return false;
    offset += read_bytes;

    if (offset < body_size) {
        core::read_vint(body + offset, body_size - offset, out_block.end_flags, read_bytes);
    }
    return true;
}

} // namespace openrar::format
