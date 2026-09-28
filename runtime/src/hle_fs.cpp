// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied)
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <nx/runtime.h>

namespace fs = std::filesystem;

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
std::map<uint64_t, FILE*> g_files;
struct Dir {
    std::vector<fs::directory_entry> entries;
    size_t pos = 0;
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? fs::path(r) : fs::path("private");
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    return it->second / fs::u8path(rest);
}

void mount(const std::string& name, const fs::path& p) {
    fs::create_directories(p);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "extract" / "romfs");
    c.x[0] = kOk;
}

void EnsureSaveData(CPU& c) { c.x[0] = kOk; }

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, data_root() / "user" / "save");
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

void Commit(CPU& c) { c.x[0] = kOk; }
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (f) {
        if (size > 0) {
            _fseeki64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        if (g_trace) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = _wfopen(p.c_str(), (mode & 2) ? L"r+b" : L"rb");
    if (!f) {
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = f;
    st<uint64_t>(c.x[0], h);
    if (g_trace) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

FILE* file(uint64_t h) {
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    if (FILE* f = file(c.x[0])) fclose(f);
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    std::lock_guard lk(g_fs);
    FILE* f = file(c.x[1]);
    if (!f) {
        c.x[0] = kTargetNotFound;
        return;
    }
    int64_t cur = _ftelli64(f);
    _fseeki64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], _ftelli64(f));
    _fseeki64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

uint64_t read_at(FILE* f, int64_t off, uint64_t buf, uint64_t size) {
    _fseeki64(f, off, SEEK_SET);
    return fread(reinterpret_cast<void*>(buf), 1, size, f);
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    std::lock_guard lk(g_fs);
    FILE* f = file(c.x[1]);
    if (!f) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(f, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    std::lock_guard lk(g_fs);
    FILE* f = file(c.x[0]);
    if (!f) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(f, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    std::lock_guard lk(g_fs);
    FILE* f = file(c.x[0]);
    if (!f) {
        c.x[0] = kTargetNotFound;
        return;
    }
    _fseeki64(f, int64_t(c.x[1]), SEEK_SET);
    fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], f);
    if (c.x[4] && (ld<uint32_t>(c.x[4]) & 1)) fflush(f);
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    std::lock_guard lk(g_fs);
    if (FILE* f = file(c.x[0])) fflush(f);
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    Dir d;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool dir = e.is_directory();
        if ((dir && (mode & 1)) || (!dir && (mode & 2))) d.entries.push_back(e);
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        auto u8 = e.path().filename().u8string();
        std::string name(u8.begin(), u8.end());
        std::memcpy(reinterpret_cast<void*>(out), name.c_str(), std::min<size_t>(name.size(), 0x300));
        bool dir = e.is_directory();
        st<int8_t>(out + 0x304, dir ? 0 : 1);
        st<int64_t>(out + 0x308, dir ? 0 : int64_t(e.file_size()));
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);

}  // namespace nx
