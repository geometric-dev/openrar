#ifndef OPENRAR_CORE_ERROR_HPP
#define OPENRAR_CORE_ERROR_HPP

#include "types.hpp"

namespace openrar::core {

enum class ExitCode : int {
    Success = 0,
    Warning = 1,
    FatalError = 2,
    CrcError = 3,
    LockedArchive = 4,
    WriteError = 5,
    OpenError = 6,
    UserError = 7,
    OutOfMemory = 8,
    CreateError = 9,
    NoFilesFound = 10,
    BadPassword = 11,
    ReadError = 12,
    CorruptArchive = 13,
    UserBreak = 255
};

inline const char* error_string(ExitCode code) {
    switch (code) {
    case ExitCode::Success:
        return "Success";
    case ExitCode::Warning:
        return "Non-fatal error / warning";
    case ExitCode::FatalError:
        return "Fatal error";
    case ExitCode::CrcError:
        return "CRC or checksum verification failure";
    case ExitCode::LockedArchive:
        return "Archive is locked against modification";
    case ExitCode::WriteError:
        return "Write error";
    case ExitCode::OpenError:
        return "File open error";
    case ExitCode::UserError:
        return "Command-line syntax or user error";
    case ExitCode::OutOfMemory:
        return "Out of memory";
    case ExitCode::CreateError:
        return "File creation error";
    case ExitCode::NoFilesFound:
        return "No files found matching criteria";
    case ExitCode::BadPassword:
        return "Incorrect password or header decryption failure";
    case ExitCode::ReadError:
        return "Read error";
    case ExitCode::CorruptArchive:
        return "Corrupt archive";
    case ExitCode::UserBreak:
        return "Operation cancelled by user";
    default:
        return "Unknown error";
    }
}

} // namespace openrar::core

#endif // OPENRAR_CORE_ERROR_HPP
