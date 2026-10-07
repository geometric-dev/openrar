#include "../src/io/file_stream.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <windows.h>
using namespace openrar;
namespace fs = std::filesystem;

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* NtSetInfoFn)(HANDLE, PVOID, PVOID, ULONG, ULONG);

static bool nt_rename(HANDLE fh, HANDLE parent, const std::wstring& leaf) {
    struct RI {
        DWORD Flags;
        HANDLE RootDirectory;
        DWORD FileNameLength;
        WCHAR FileName[1];
    };
    std::vector<BYTE> b(sizeof(RI) + leaf.size() * 2 + 2);
    RI* ri = (RI*)b.data();
    ri->Flags = 0x3;
    ri->RootDirectory = parent;
    ri->FileNameLength = (DWORD)(leaf.size() * 2);
    memcpy(ri->FileName, leaf.c_str(), (leaf.size() + 1) * 2);
    static NtSetInfoFn set_info = []() -> NtSetInfoFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? (NtSetInfoFn)(void*)GetProcAddress(ntdll, "NtSetInformationFile") : nullptr;
    }();
    if (!set_info) {
        printf("  no ntdll\n");
        return false;
    }
    struct {
        LONG Status;
        ULONG_PTR Information;
    } iosb{0, 0};
    LONG st = set_info(fh, &iosb, ri, (ULONG)b.size(), 65 /* FileRenameInformationEx */);
    printf("  NtSetInformationFile(65) -> 0x%08lX\n", (unsigned long)st);
    return st == 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wprintf(L"temp dir: %s\n", tmp);
    fs::path dir = fs::temp_directory_path() / "openrar_ren_iso";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    HANDLE fh = CreateFileW((dir / L"a.bin").wstring().c_str(), GENERIC_WRITE | DELETE,
                            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    DWORD w;
    WriteFile(fh, buf, sizeof buf, &w, nullptr);
    HANDLE parent =
        CreateFileW(dir.wstring().c_str(), FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool ok = nt_rename(fh, parent, L"b.bin");
    CloseHandle(parent);
    CloseHandle(fh);
    printf("nt rename: %d, b.bin exists: %d\n", (int)ok, (int)fs::exists(dir / "b.bin"));
    // What filesystem is %TEMP% on?
    DWORD spc = GetDriveTypeW(fs::temp_directory_path().root_name().wstring().c_str());
    printf("drive type: %lu (3=fixed)\n", spc);
    char fsname[MAX_PATH + 1] = {};
    ULARGE_INTEGER free_, total;
    GetVolumeInformationW(fs::temp_directory_path().root_path().wstring().c_str(), nullptr, 0,
                          nullptr, nullptr, nullptr, (LPWSTR)fsname, MAX_PATH);
    printf("fs name: %s\n", fsname);
    return ok ? 0 : 2;
}
