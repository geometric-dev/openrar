#ifndef OPENRAR_FORMAT_HEADERS_HPP
#define OPENRAR_FORMAT_HEADERS_HPP

#include "../core/types.hpp"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace openrar::format {

// RAR 5.0 Magic Signature: "Rar!\x1A\x07\x01\x00"
//
// The signature must never appear as a literal byte sequence in compiled
// binaries: the Default.SFX stub links this header's users, and an SFX module
// that carries an embedded signature breaks reference tools — UnRAR locates
// the archive behind an SFX prefix with a naive first-match scan and stops
// inside the module, reporting the main header as corrupt. (WinRAR's own
// stubs avoid embedding the signature for the same reason.) The bytes are
// stored XOR-masked and assembled at runtime; the volatile loads defeat
// constant folding back into .rdata.
inline constexpr std::size_t RAR5_SIGNATURE_SIZE = 8;

inline const core::byte* rar5_signature() {
    static const volatile core::byte masked[8] = {0x52 ^ 0x5B, 0x61 ^ 0x5B, 0x72 ^ 0x5B,
                                                  0x21 ^ 0x5B, 0x1A ^ 0x5B, 0x07 ^ 0x5B,
                                                  0x01 ^ 0x5B, 0x00 ^ 0x5B};
    static const std::array<core::byte, 8> sig = [] {
        std::array<core::byte, 8> s{};
        for (int i = 0; i < 8; ++i) s[i] = static_cast<core::byte>(masked[i] ^ 0x5B);
        return s;
    }();
    return sig.data();
}

// Header Block Types
enum class HeaderType : core::uint64 { Main = 1, File = 2, Service = 3, Crypt = 4, EndArc = 5 };

inline constexpr core::uint64 HEAD_MAIN = static_cast<core::uint64>(HeaderType::Main);
inline constexpr core::uint64 HEAD_FILE = static_cast<core::uint64>(HeaderType::File);
inline constexpr core::uint64 HEAD_SERVICE = static_cast<core::uint64>(HeaderType::Service);
inline constexpr core::uint64 HEAD_CRYPT = static_cast<core::uint64>(HeaderType::Crypt);
inline constexpr core::uint64 HEAD_ENDARC = static_cast<core::uint64>(HeaderType::EndArc);

// Common Header Block Flags
namespace header_flags {
inline constexpr core::uint64 EXTRA = 0x0001;           // Extra area follows header
inline constexpr core::uint64 DATA = 0x0002;            // Data area follows header
inline constexpr core::uint64 SKIP_IF_UNKNOWN = 0x0004; // Skip block if type is unknown
inline constexpr core::uint64 SPLIT_BEFORE = 0x0008;    // Data continues from previous volume
inline constexpr core::uint64 SPLIT_AFTER = 0x0010;     // Data continues in next volume
inline constexpr core::uint64 CHILD = 0x0020;           // Child block (e.g. STM or ACL service)
inline constexpr core::uint64 INHERITED = 0x0040;       // Inherited from previous block
} // namespace header_flags

inline constexpr core::uint64 HFL_EXTRA = header_flags::EXTRA;
inline constexpr core::uint64 HFL_DATA = header_flags::DATA;
inline constexpr core::uint64 HFL_SKIP_IF_UNKNOWN = header_flags::SKIP_IF_UNKNOWN;
inline constexpr core::uint64 HFL_SPLITBEFORE = header_flags::SPLIT_BEFORE;
inline constexpr core::uint64 HFL_SPLITAFTER = header_flags::SPLIT_AFTER;
inline constexpr core::uint64 HFL_CHILD = header_flags::CHILD;
inline constexpr core::uint64 HFL_INHERITED = header_flags::INHERITED;

// Main Archive Flags
namespace main_flags {
inline constexpr core::uint64 VOLUME = 0x0001;     // Multi-volume archive
inline constexpr core::uint64 VOL_NUMBER = 0x0002; // Volume number field present
inline constexpr core::uint64 SOLID = 0x0004;      // Solid archive
inline constexpr core::uint64 PROTECT = 0x0008;    // Recovery record present
inline constexpr core::uint64 LOCK = 0x0010;       // Archive locked against mutations
} // namespace main_flags

inline constexpr core::uint64 MHFL_VOLUME = main_flags::VOLUME;
inline constexpr core::uint64 MHFL_VOLNUMBER = main_flags::VOL_NUMBER;
inline constexpr core::uint64 MHFL_SOLID = main_flags::SOLID;
inline constexpr core::uint64 MHFL_PROTECT = main_flags::PROTECT;
inline constexpr core::uint64 MHFL_LOCK = main_flags::LOCK;

// File Header Flags
namespace file_flags {
inline constexpr core::uint64 DIRECTORY = 0x0001;   // Directory entry
inline constexpr core::uint64 UTIME = 0x0002;       // 32-bit Unix time present
inline constexpr core::uint64 CRC32 = 0x0004;       // 32-bit CRC present
inline constexpr core::uint64 UNP_UNKNOWN = 0x0008; // Unpacked size is unknown
} // namespace file_flags

inline constexpr core::uint64 FHFL_DIRECTORY = file_flags::DIRECTORY;
inline constexpr core::uint64 FHFL_UTIME = file_flags::UTIME;
inline constexpr core::uint64 FHFL_CRC32 = file_flags::CRC32;
inline constexpr core::uint64 FHFL_UNPUNKNOWN = file_flags::UNP_UNKNOWN;

// Extra Field Types
// Spec 01-headers/06-encoding: 0x01 Crypt, 0x02 Hash, 0x03 HTime, 0x04 Version, 0x05 Redir, 0x06 Owner, 0x07 SubData,
//   0x08 Xattr (v1.27 — verified free against unrar headers5.hpp + bitplane rar-research; skipped
//   without error by readers that do not implement it, per the spec's unknown-record contract)
// Main extra: 0x01 Locator, 0x02 Metadata
enum class ExtraType : core::uint8 {
    Crypt = 0x01,   // File encryption record (FHEXTRA_CRYPT)
    Hash = 0x02,    // Data checksum (BLAKE2sp) (FHEXTRA_HASH)
    Htime = 0x03,   // High precision timestamps (FHEXTRA_HTIME)
    Version = 0x04, // File version (FHEXTRA_VERSION) — flags(0) + version vint
    Redir = 0x05,   // Redirection (symlink/junction/hardlink) (FHEXTRA_REDIR)
    Owner = 0x06,   // Unix owner (FHEXTRA_OWNER) — flags + names + UID/GID
    SubData = 0x07, // Service subdata (FHEXTRA_SUBDATA)
    Xattr = 0x08,   // Extended attributes (FHEXTRA_XATTR) — v1.27
    Locator =
        0x01, // Main header locator (MHEXTRA_LOCATOR) — alias 0x01, distinct namespace from file extras
    Metadata = 0x02 // Main header metadata (MHEXTRA_METADATA) — name + time
};

inline constexpr core::uint8 FHEXTRA_CRYPT = static_cast<core::uint8>(ExtraType::Crypt);
inline constexpr core::uint8 FHEXTRA_HASH = static_cast<core::uint8>(ExtraType::Hash);
inline constexpr core::uint8 FHEXTRA_HTIME = static_cast<core::uint8>(ExtraType::Htime);
inline constexpr core::uint8 FHEXTRA_VERSION = static_cast<core::uint8>(ExtraType::Version);
inline constexpr core::uint8 FHEXTRA_REDIR = static_cast<core::uint8>(ExtraType::Redir);
inline constexpr core::uint8 FHEXTRA_OWNER = static_cast<core::uint8>(ExtraType::Owner);
inline constexpr core::uint8 FHEXTRA_SUBDATA = static_cast<core::uint8>(ExtraType::SubData);
inline constexpr core::uint8 FHEXTRA_XATTR = static_cast<core::uint8>(ExtraType::Xattr);
inline constexpr core::uint8 MHEXTRA_LOCATOR = static_cast<core::uint8>(ExtraType::Locator);
inline constexpr core::uint8 MHEXTRA_METADATA = static_cast<core::uint8>(ExtraType::Metadata);

// Main Header Model
struct MainBlock {
    core::uint64 arc_flags{0};
    core::uint64 vol_number{0};
    bool has_locator{false};
    // Locator extra record offsets (MHEXTRA_LOCATOR, §01-headers.md:§2.1).
    // Sentinel -1 means absent: write_main_block emits a locator vint for the
    // field only when the value is >= 0 (has_qo = locator_qo_offset >= 0,
    // has_rr = locator_rr_offset >= 0 in header_writer.cpp). Callers that
    // strip QuickOpen on mutation MUST set locator_qo_offset = -1 explicitly
    // — do NOT leave it at 0, which write_main_block interprets as "QO block
    // is located at offset 0 from main_header_pos" (i.e. points into the RAR5
    // signature, producing a corrupt locator that WinRAR will reject).
    core::int64 locator_qo_offset{-1};
    core::int64 locator_rr_offset{-1};

    // Metadata extra record (MHEXTRA_METADATA, 0x02)
    bool has_metadata{false};
    std::string metadata_name;
    core::uint64 metadata_ctime{0};      // Windows FILETIME or Unix timestamp
    bool metadata_is_unix_time{false};   // Flag bit 0x04
    bool metadata_is_nanoseconds{false}; // Flag bit 0x08
};

// Dictionary limits per spec 01-headers.md:95
// 128 KiB << N  ;  N max 15 version0 (4096 MiB), N max 23 version1 (1 TB) with FCI_DICT_FRACT
inline constexpr core::uint64 RAR_DICT_BASE = 128ULL * 1024;
inline constexpr core::uint64 RAR_DICT_MAX_V0 = RAR_DICT_BASE << 15; // 4096 MiB = 4294967296
inline constexpr core::uint64 RAR_DICT_MAX_V1 = RAR_DICT_BASE << 23; // 1 TiB = 1099511627776
inline constexpr core::uint64 RAR_DICT_SPEC_MAX_ABSOLUTE = RAR_DICT_MAX_V1;

// Compression Information bit flags & masks (spec 01-headers.md:85-92 / 06-encoding.md:86-91)
inline constexpr core::uint32 FCI_ALGO_MASK =
    0x003F; // Version of compression algorithm (0=RAR5, 1=RAR7)
inline constexpr core::uint32 FCI_SOLID = 0x0040;       // Solid flag
inline constexpr core::uint32 FCI_METHOD_MASK = 0x0380; // Method 0..5
inline constexpr core::uint32 FCI_DICT_MASK = 0x7C00; // Dictionary size power 0..23 (128 KiB << N)
inline constexpr core::uint32 FCI_DICT_FRACT_MASK =
    0xF8000; // Dictionary fraction (version 1) in 1/32 of size
inline constexpr core::uint32 FCI_RAR5_COMPAT =
    0x100000; // RAR7 dict sizing with RAR5 compression algorithm
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(_M_IX86) || defined(__i386__) ||       \
    defined(__arm__) || defined(_M_ARM)
inline constexpr size_t RAR_DICT_ALLOC_LIMIT =
    1ULL * 1024 * 1024 * 1024; // implementation alloc limit 1 GiB on 32-bit / WASM
#else
inline constexpr size_t RAR_DICT_ALLOC_LIMIT =
    64ULL * 1024 * 1024 * 1024; // implementation alloc limit 64 GiB on 64-bit native
#endif
// ILP32 guard: on a 32-bit size_t the 64 GiB constant would silently truncate
// to 0, making every dictionary look "too large" (armv7 leg finding).
static_assert(RAR_DICT_ALLOC_LIMIT == 1ULL * 1024 * 1024 * 1024 ||
                  RAR_DICT_ALLOC_LIMIT == 64ULL * 1024 * 1024 * 1024,
              "RAR_DICT_ALLOC_LIMIT does not fit this target's size_t");

inline bool is_dictionary_too_large_for_alloc(core::uint64 win_size) {
    return win_size > RAR_DICT_ALLOC_LIMIT;
}
inline bool is_dictionary_spec_max_exceeded(core::uint64 win_size, core::uint32 unp_ver) {
    if (unp_ver == 0) return win_size > RAR_DICT_MAX_V0;
    if (unp_ver == 1) return win_size > RAR_DICT_MAX_V1;
    return win_size > RAR_DICT_SPEC_MAX_ABSOLUTE;
}

// File & Service Header Model
struct FileBlock {
    bool is_service{false};
    std::string service_type; // e.g. "CMT", "QO", "RR", "STM", "ACL"

    core::uint64 file_flags{0};
    core::uint64 unp_size{0};
    bool unp_unknown{false};   // FHFL_UNPUNKNOWN: field present but ignored; decode to stream end
    core::int64 pack_size{-1}; // -1 if no data area (e.g. directories)
    core::uint64 attributes{0};

    bool has_crc32{false};
    core::uint32 data_crc32{0};

    bool has_blake2sp{false};
    std::array<core::byte, 32> blake2sp{};

    // Timestamps
    core::uint64 mtime_win{0};
    core::uint64 ctime_win{0};
    core::uint64 atime_win{0};
    core::uint32 utime_unix{0};
    // High-precision timestamp (FHEXTRA_HTIME) decoded details
    bool htime_is_unix{false};
    core::uint32 htime_mtime_unix{0};
    core::uint32 htime_ctime_unix{0};
    core::uint32 htime_atime_unix{0};
    core::uint32 mtime_ns{0};
    core::uint32 ctime_ns{0};
    core::uint32 atime_ns{0};
    bool has_mtime_ns{false};
    bool has_ctime_ns{false};
    bool has_atime_ns{false};

    // Unix owner (FHEXTRA_OWNER)
    bool has_owner{false};
    std::string owner_user;
    std::string owner_group;
    core::uint64 owner_uid{0};
    core::uint64 owner_gid{0};
    bool has_owner_uid{false};
    bool has_owner_gid{false};

    // File version (FHEXTRA_VERSION)
    bool has_file_version{false};
    core::uint64 file_version{0};

    // Compression info
    core::uint32 method{0}; // 0 = store, 1..5 = compressed
    core::uint64 win_size{0};
    bool is_solid{false};
    core::uint32 unp_ver{0}; // 0 = RAR5, 1 = RAR7

    core::uint32 host_os{0}; // 0 = Windows, 1 = Unix
    std::string file_name;

    // Encryption
    bool is_encrypted{false};
    core::uint32 crypt_version{0};
    core::uint32 crypt_flags{0};
    core::uint8 lg2_count{0};
    std::array<core::byte, 16> salt{};
    std::array<core::byte, 16> init_v{};
    bool has_psw_check{false};
    std::array<core::byte, 8> psw_check{};
    std::array<core::byte, 4> psw_check_csum{};

    // Redirection / Symlink
    core::uint8 redir_type{0};
    bool redir_dir_target{false};
    std::string redir_target;

    // Service SubData (e.g. CMT)
    std::vector<core::byte> sub_data;

    // Extended attributes (FHEXTRA_XATTR, v1.27): POSIX user./security./
    // trusted.* and macOS com.apple.metadata.* namespaces. Names carry
    // their namespace prefix; values are opaque bytes. Well-formed
    // records parse here; malformed ones fall back to unknown_extras
    // (verbatim) — never a partial parse, never an abort.
    struct FileXattr {
        std::string name;
        std::vector<core::byte> value;
    };
    std::vector<FileXattr> xattrs;

    // Unknown extra records (v1.24 plan §7.3): records whose type this
    // build does not implement are captured VERBATIM (type vint + size vint
    // + payload) and re-serialized byte-identically during mutations, so a
    // roundtrip through OpenRAR never destroys data a newer producer wrote.
    struct UnknownExtra {
        core::uint64 type{0};
        std::vector<core::byte> raw; // full record bytes
    };
    std::vector<UnknownExtra> unknown_extras;
};

// Encryption Header Model (HEAD_CRYPT)
struct CryptBlock {
    core::uint32 crypt_version{0};
    core::uint32 enc_flags{0};
    core::uint8 lg2_count{0};
    std::array<core::byte, 16> salt{};
    bool has_psw_check{false};
    std::array<core::byte, 8> psw_check{};
    std::array<core::byte, 4> psw_check_csum{};
};

// End Archive Model (HEAD_ENDARC)
struct EndArcBlock {
    core::uint64 end_flags{0}; // EARCF_NEXTVOL = 0x0001
};

} // namespace openrar::format

#endif // OPENRAR_FORMAT_HEADERS_HPP
