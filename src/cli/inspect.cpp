#include "inspect.hpp"

#include "../archive/archive_reader.hpp"
#include "../archive/rar_errors.hpp"
#include "../format/headers.hpp"
#include "../io/file_stream.hpp"
#include "json_output.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace openrar::cli {

namespace {

struct RecoveryForensics {
    bool present{false};
    uint32_t sector_size{512};
    uint32_t data_sectors{0};
    uint32_t recovery_sectors{0};
    uint64_t protected_bytes{0};
    uint64_t rr_offset{0};
    uint64_t rr_size{0};
    double parity_percent{0.0};
};

struct ArchiveForensics {
    std::string path;
    uint64_t file_size{0};
    std::string format{"RAR 5.0"};
    bool is_solid{false};
    bool is_locked{false};
    bool is_volume{false};
    bool has_sfx{false};
    uint64_t sfx_offset{0};

    bool header_encrypted{false};
    std::string cipher{"None"};
    std::string kdf{"None"};
    std::string salt_hex;
    uint32_t kdf_iterations{0};

    bool has_quick_open{false};
    uint64_t qo_offset{0};
    uint64_t qo_size{0};

    bool has_comment{false};
    uint64_t comment_size{0};

    RecoveryForensics recovery;

    size_t file_count{0};
    size_t dir_count{0};
    size_t symlink_count{0};
    size_t encrypted_files{0};

    uint64_t total_unpacked_bytes{0};
    uint64_t total_packed_bytes{0};
    uint64_t max_dictionary_bytes{0};
    double compression_ratio{0.0};

    bool truncated{false};
    bool read_error{false};
    std::string status{"OK"};
};

RecoveryForensics parse_recovery_forensics(archive::ArchiveReader& reader, io::FileStream& fs) {
    RecoveryForensics rf;
    if (!reader.has_recovery_record()) return rf;

    rf.present = true;
    for (const auto& entry : reader.entries()) {
        if (entry.header.is_service && entry.header.service_type == "RR") {
            rf.rr_offset = entry.data_offset;
            rf.rr_size = entry.data_size;

            // Attempt to inspect first 64 bytes of the RR service block payload
            core::byte buf[64] = {0};
            if (fs.is_open() &&
                fs.seek(static_cast<core::int64>(entry.data_offset), io::SeekOrigin::Begin)) {
                size_t got = fs.read(buf, sizeof(buf));
                if (got >= 16) {
                    // Check magic "RB" (0x52, 0x42)
                    if (buf[0] == 0x52 && buf[1] == 0x42) {
                        rf.sector_size = 512;
                        // Read declared counts if valid
                        rf.recovery_sectors =
                            static_cast<uint32_t>(buf[2]) | (static_cast<uint32_t>(buf[3]) << 8);
                    }
                }
            }
            break;
        }
    }

    if (rf.recovery_sectors == 0 && rf.rr_size > 0) {
        // Fallback: estimate from RR payload size (512 B sectors)
        rf.recovery_sectors = static_cast<uint32_t>(rf.rr_size / 512);
    }

    // Main header protect bit and archive size estimate data sectors
    if (reader.main_block().has_locator && reader.main_block().locator_rr_offset > 0) {
        rf.protected_bytes = static_cast<uint64_t>(reader.main_block().locator_rr_offset);
        if (rf.sector_size > 0) {
            rf.data_sectors =
                static_cast<uint32_t>((rf.protected_bytes + rf.sector_size - 1) / rf.sector_size);
        }
    } else {
        rf.protected_bytes = rf.rr_offset > 0 ? rf.rr_offset : 0;
        if (rf.sector_size > 0) {
            rf.data_sectors =
                static_cast<uint32_t>((rf.protected_bytes + rf.sector_size - 1) / rf.sector_size);
        }
    }

    if (rf.data_sectors > 0 && rf.recovery_sectors > 0) {
        rf.parity_percent =
            (static_cast<double>(rf.recovery_sectors) / static_cast<double>(rf.data_sectors)) *
            100.0;
    }
    return rf;
}

} // namespace

int inspect_archive(const InspectOptions& opt) {
    if (!std::filesystem::exists(opt.arc_path)) {
        std::cerr << "Error: File not found: " << opt.arc_path << "\n";
        return 10; // EXIT_NO_FILES
    }

    ArchiveForensics f;
    f.path = opt.arc_path;
    std::error_code ec;
    f.file_size = std::filesystem::file_size(opt.arc_path, ec);

    archive::ArchiveReader reader;
    int status = archive::RAR_OK;
    std::string detail;

    // Directive 5: Permissive diagnostic inspection
    bool opened = reader.open_ex(opt.arc_path, opt.password, status, detail);

    io::FileStream fs;
    fs.open(opt.arc_path, io::FileMode::ReadOnly);

    if (!opened) {
        f.read_error = true;
        if (status == archive::RAR_ERR_ENCRYPTED || status == archive::RAR_ERR_BAD_PASSWORD ||
            reader.saw_crypt_header()) {
            f.header_encrypted = true;
            f.cipher = "AES-256-CBC";
            f.kdf = "PBKDF2-HMAC-SHA256";
            f.status = "PASSWORD_REQUIRED";

            // Extract salt and iteration workbound from CryptBlock
            const format::CryptBlock& cb = reader.header_crypt();
            std::ostringstream s_ss;
            for (size_t i = 0; i < sizeof(cb.salt); ++i) {
                s_ss << std::hex << std::setw(2) << std::setfill('0')
                     << static_cast<int>(cb.salt[i]);
            }
            f.salt_hex = s_ss.str();
            f.kdf_iterations = 1u << cb.lg2_count;
        } else if (status == archive::RAR_ERR_NOT_RAR) {
            std::cerr << "Error: Not a RAR archive: " << opt.arc_path << "\n";
            return 13; // EXIT_BAD_ARCHIVE
        } else {
            f.status = "DAMAGED_OR_TRUNCATED";
        }
    }

    if (opened || f.header_encrypted) {
        f.is_solid = reader.is_solid();
        f.is_locked = reader.is_locked();
        f.is_volume = reader.is_volume();
        f.sfx_offset = reader.sfx_offset();
        f.has_sfx = (f.sfx_offset > 0);
        f.truncated = reader.has_truncated_data();

        if (reader.is_header_encrypted()) {
            f.header_encrypted = true;
            f.cipher = "AES-256-CBC";
            f.kdf = "PBKDF2-HMAC-SHA256";
            const format::CryptBlock& cb = reader.header_crypt();
            std::ostringstream s_ss;
            for (size_t i = 0; i < sizeof(cb.salt); ++i) {
                s_ss << std::hex << std::setw(2) << std::setfill('0')
                     << static_cast<int>(cb.salt[i]);
            }
            f.salt_hex = s_ss.str();
            f.kdf_iterations = 1u << cb.lg2_count;
        }

        // Tally service blocks and entry stats
        for (const auto& entry : reader.entries()) {
            if (entry.header.is_service) {
                if (entry.header.service_type == "CMT") {
                    f.has_comment = true;
                    f.comment_size = entry.data_size;
                } else if (entry.header.service_type == "QO") {
                    f.has_quick_open = true;
                    f.qo_offset = entry.data_offset;
                    f.qo_size = entry.data_size;
                }
                continue;
            }

            if ((entry.header.file_flags & format::file_flags::DIRECTORY) != 0) {
                f.dir_count++;
            } else if (entry.header.redir_type != 0) {
                f.symlink_count++;
            } else {
                f.file_count++;
            }

            if (entry.header.is_encrypted) {
                f.encrypted_files++;
            }

            f.total_unpacked_bytes += entry.header.unp_size;
            f.total_packed_bytes += entry.header.pack_size;
            if (entry.header.win_size > f.max_dictionary_bytes) {
                f.max_dictionary_bytes = entry.header.win_size;
            }
        }

        if (f.total_unpacked_bytes > 0) {
            f.compression_ratio = (static_cast<double>(f.total_packed_bytes) /
                                   static_cast<double>(f.total_unpacked_bytes)) *
                                  100.0;
        }

        f.recovery = parse_recovery_forensics(reader, fs);
    }

    if (opt.json) {
        std::cout << "{\n"
                  << "  \"command\": \"inspect\",\n"
                  << "  \"version\": \"2.0.0\",\n"
                  << "  \"archive\": \"" << escape_json_string(f.path) << "\",\n"
                  << "  \"status\": \"" << f.status << "\",\n"
                  << "  \"file_size_bytes\": " << f.file_size << ",\n"
                  << "  \"envelope\": {\n"
                  << "    \"format\": \"" << f.format << "\",\n"
                  << "    \"solid\": " << (f.is_solid ? "true" : "false") << ",\n"
                  << "    \"locked\": " << (f.is_locked ? "true" : "false") << ",\n"
                  << "    \"volume\": " << (f.is_volume ? "true" : "false") << ",\n"
                  << "    \"has_sfx\": " << (f.has_sfx ? "true" : "false") << ",\n"
                  << "    \"sfx_offset_bytes\": " << f.sfx_offset << ",\n"
                  << "    \"truncated\": " << (f.truncated ? "true" : "false") << "\n"
                  << "  },\n"
                  << "  \"security\": {\n"
                  << "    \"header_encrypted\": " << (f.header_encrypted ? "true" : "false")
                  << ",\n"
                  << "    \"cipher\": \"" << f.cipher << "\",\n"
                  << "    \"kdf\": \"" << f.kdf << "\",\n"
                  << "    \"kdf_iterations\": " << f.kdf_iterations << ",\n"
                  << "    \"salt\": \"" << f.salt_hex << "\",\n"
                  << "    \"encrypted_files_count\": " << f.encrypted_files << "\n"
                  << "  },\n"
                  << "  \"recovery_record\": {\n"
                  << "    \"present\": " << (f.recovery.present ? "true" : "false") << ",\n"
                  << "    \"algorithm\": \"Cauchy RS16 Reed-Solomon\",\n"
                  << "    \"sector_size_bytes\": " << f.recovery.sector_size << ",\n"
                  << "    \"data_sectors_k\": " << f.recovery.data_sectors << ",\n"
                  << "    \"parity_sectors_m\": " << f.recovery.recovery_sectors << ",\n"
                  << "    \"parity_percent\": " << std::fixed << std::setprecision(2)
                  << f.recovery.parity_percent << ",\n"
                  << "    \"protected_bytes\": " << f.recovery.protected_bytes << ",\n"
                  << "    \"payload_offset\": " << f.recovery.rr_offset << ",\n"
                  << "    \"payload_size_bytes\": " << f.recovery.rr_size << "\n"
                  << "  },\n"
                  << "  \"service_blocks\": {\n"
                  << "    \"quick_open\": " << (f.has_quick_open ? "true" : "false") << ",\n"
                  << "    \"quick_open_bytes\": " << f.qo_size << ",\n"
                  << "    \"comment\": " << (f.has_comment ? "true" : "false") << ",\n"
                  << "    \"comment_bytes\": " << f.comment_size << "\n"
                  << "  },\n"
                  << "  \"catalog_summary\": {\n"
                  << "    \"files\": " << f.file_count << ",\n"
                  << "    \"directories\": " << f.dir_count << ",\n"
                  << "    \"symlinks\": " << f.symlink_count << ",\n"
                  << "    \"total_unpacked_bytes\": " << f.total_unpacked_bytes << ",\n"
                  << "    \"total_packed_bytes\": " << f.total_packed_bytes << ",\n"
                  << "    \"compression_ratio_percent\": " << std::fixed << std::setprecision(2)
                  << f.compression_ratio << ",\n"
                  << "    \"max_dictionary_bytes\": " << f.max_dictionary_bytes << "\n"
                  << "  }\n"
                  << "}\n";
        return f.read_error ? 1 : 0;
    }

    // Terminal Forensic Dashboard
    std::cout
        << "\n  \033[1;36mOpenRAR 2.0.0 Archive Forensics & Health X-Ray\033[0m\n"
        << "  \033[90m==================================================================\033[0m\n"
        << "  Archive: \033[1;37m" << f.path << "\033[0m (" << f.file_size << " bytes)\n"
        << "  Status:  " << (f.read_error ? "\033[1;33m" : "\033[1;32m") << f.status
        << "\033[0m\n\n"
        << "  \033[1;37m[Envelope & Physical Layout]\033[0m\n"
        << "    Format:          " << f.format << "\n"
        << "    Solid Chain:     "
        << (f.is_solid ? "Enabled (Inter-file dictionary history)"
                       : "Disabled (Independent entries)")
        << "\n"
        << "    Multi-Volume:    "
        << (f.is_volume ? "Yes (Spanned volume member)" : "No (Standalone archive)") << "\n"
        << "    SFX Stub:        "
        << (f.has_sfx ? ("Yes (" + std::to_string(f.sfx_offset) + " bytes prepended)") : "None")
        << "\n"
        << "    Lock Status:     " << (f.is_locked ? "Locked against modification" : "Unlocked")
        << "\n"
        << "    Integrity State: "
        << (f.truncated ? "\033[1;31mTruncated Archive (Declared data exceeds volume)\033[0m"
                        : "\033[1;32mClean (Valid boundaries)\033[0m")
        << "\n\n";

    std::cout << "  \033[1;37m[Security & Cryptography]\033[0m\n"
              << "    Header Cipher:   "
              << (f.header_encrypted ? ("\033[1;33m" + f.cipher + " (Encrypted Headers)\033[0m")
                                     : "Plaintext Headers")
              << "\n";
    if (f.header_encrypted) {
        std::cout << "    KDF / Derivation:" << f.kdf << " (" << f.kdf_iterations
                  << " iterations)\n"
                  << "    Salt (Hex):      " << f.salt_hex << "\n";
    }
    std::cout << "    Payload Encr.:   " << f.encrypted_files << " / "
              << (f.file_count + f.dir_count) << " entries encrypted\n\n";

    std::cout << "  \033[1;37m[Cauchy RS16 Recovery Geometry]\033[0m\n";
    if (f.recovery.present) {
        std::cout << "    Protection:      \033[1;32mActive (Cauchy RS16 Reed-Solomon)\033[0m\n"
                  << "    Sector Size:     " << f.recovery.sector_size << " bytes\n"
                  << "    Geometry:        K = " << f.recovery.data_sectors
                  << " data sectors, M = " << f.recovery.recovery_sectors << " parity sectors ("
                  << std::fixed << std::setprecision(1) << f.recovery.parity_percent
                  << "% parity)\n"
                  << "    Protected Range: 0 .. " << f.recovery.protected_bytes << " bytes\n"
                  << "    Repair Capacity: Can reconstruct up to " << f.recovery.recovery_sectors
                  << " missing/punctured sectors\n";
    } else {
        std::cout << "    Protection:      \033[90mNone (No recovery record present)\033[0m\n";
    }

    std::cout
        << "\n  \033[1;37m[Catalog & Storage Attribution]\033[0m\n"
        << "    Files / Dirs:    " << f.file_count << " files, " << f.dir_count << " dirs, "
        << f.symlink_count << " links\n"
        << "    Unpacked Size:   " << f.total_unpacked_bytes << " bytes\n"
        << "    Packed Size:     " << f.total_packed_bytes << " bytes (" << std::fixed
        << std::setprecision(1) << f.compression_ratio << "% ratio)\n"
        << "    Max Dictionary:  " << (f.max_dictionary_bytes / 1024) << " KiB\n"
        << "    QuickOpen Block: "
        << (f.has_quick_open ? ("Present (" + std::to_string(f.qo_size) + " bytes)") : "None")
        << "\n"
        << "    Archive Comment: "
        << (f.has_comment ? ("Present (" + std::to_string(f.comment_size) + " bytes)") : "None")
        << "\n"
        << "  "
           "\033[90m==================================================================\033[0m\n\n";

    return f.read_error ? 1 : 0;
}

} // namespace openrar::cli
