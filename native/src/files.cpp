// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's files.
//
// The PS4 gives a game a few mount points. /app0/ is the game's own package, read-only: here,
// the folders given with --app0 (by default work/update, then work/base: the 1.09 update's
// files over the base game's), first match wins. /download0/, /temp0/ and /data/ are writable;
// here each is a folder in the run's output folder (<out>/files/download0, ...). /savedata0/
// and the like are the saves mounted there (savedata.cpp). Any other path doesn't exist.
//
// Three families of calls reach them: the kernel's (sceKernelOpen, Read, Lseek, Stat, ...,
// returning SCE_KERNEL_ERROR_* on failure), POSIX's (stat, mkdir, ..., returning -1 and setting
// errno), and the C library's streams (fopen, fread, ...). All share one table of open files.
// Structures are FreeBSD 9's, as the PS4's are.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>

#include "loader.hpp"

namespace fs = std::filesystem;

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

// FreeBSD's errno values
constexpr int E_NOENT = 2, E_BADF = 9, E_ACCES = 13, E_EXIST = 17, E_NOTDIR = 20,
              E_ISDIR = 21, E_INVAL = 22, E_ROFS = 30, E_NOTEMPTY = 66;

i64 sce(int e) { return e ? i64(std::int32_t(0x80020000u | u32(e))) : 0; }

// FreeBSD's open flags
constexpr u64 O_ACCMODE = 3, O_WRONLY = 1, O_RDWR = 2, O_APPEND = 0x8, O_CREAT = 0x200,
              O_TRUNC = 0x400, O_EXCL = 0x800, O_DIRECTORY = 0x20000;

std::string cstr(u64 a) { return a ? std::string(ptr<char>(a)) : std::string(); }

// ---- paths ----

struct Resolved {
    std::string mount;  // "app0", "download0", ...; empty: no such place
    bool writable = false;
    std::vector<fs::path> candidates;  // where it may be, in order
};

Resolved resolve(const std::string& game_path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= game_path.size()) {
        size_t j = game_path.find('/', i);
        if (j == std::string::npos) j = game_path.size();
        std::string p = game_path.substr(i, j - i);
        if (p == "..") {
            if (parts.size() > 1) parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    Resolved r;
    if (parts.empty() || game_path.empty() || game_path[0] != '/') return r;
    fs::path rest;
    for (size_t k = 1; k < parts.size(); k++) rest /= parts[k];
    if (parts[0] == "app0") {
        r.mount = "app0";
        for (const auto& root : M.app0) r.candidates.push_back(fs::path(root) / rest);
    } else if (parts[0] == "download0" || parts[0] == "temp0" || parts[0] == "data") {
        r.mount = parts[0];
        r.writable = true;
        fs::path root = fs::path(M.out_dir) / "files" / parts[0];
        std::error_code ec;
        fs::create_directories(root, ec);
        r.candidates.push_back(root / rest);
    } else if (std::string folder = savedata_folder(parts[0], r.writable); !folder.empty()) {
        r.mount = parts[0];
        r.candidates.push_back(fs::path(folder) / rest);
    }
    return r;
}

// The first candidate that exists, or empty.
fs::path existing(const Resolved& r) {
    std::error_code ec;
    for (const auto& c : r.candidates)
        if (fs::exists(c, ec)) return c;
    return {};
}

std::FILE* open_host(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

int seek_host(std::FILE* f, i64 off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, off, whence);
#endif
}

i64 tell_host(std::FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

// ---- open files ----

struct File {
    std::mutex m;
    std::string game_path;
    fs::path host;
    std::FILE* f = nullptr;
    bool append = false, writable = false;
    bool dir = false;
    std::vector<std::pair<std::string, bool>> entries;  // a directory's: (name, is a folder)
    size_t next = 0;
    bool eof = false, error = false;
    int ungot = -1;
    ~File() {
        if (f) std::fclose(f);
    }
};

std::mutex g_fds_lock;
std::map<int, std::shared_ptr<File>> g_fds;
int g_next_fd = 3;  // 0, 1 and 2 are stdin, stdout and stderr

std::shared_ptr<File> file(i64 fd) {
    std::lock_guard lock(g_fds_lock);
    auto it = g_fds.find(int(fd));
    return it == g_fds.end() ? nullptr : it->second;
}

int add(std::shared_ptr<File> f) {
    std::lock_guard lock(g_fds_lock);
    int fd = g_next_fd++;
    g_fds[fd] = std::move(f);
    return fd;
}

// A directory's entries, across every folder it may be in (for /app0/, the update's and the
// base game's), with . and .. first.
std::vector<std::pair<std::string, bool>> list(const Resolved& r) {
    std::set<std::string> seen;
    std::vector<std::pair<std::string, bool>> out = {{".", true}, {"..", true}};
    std::error_code ec;
    for (const auto& c : r.candidates) {
        if (!fs::is_directory(c, ec)) continue;
        for (const auto& e : fs::directory_iterator(c, ec)) {
            std::string name = e.path().filename().string();
            if (seen.insert(name).second) out.emplace_back(name, e.is_directory(ec));
        }
    }
    std::sort(out.begin() + 2, out.end());
    return out;
}

// Opens a game path; returns the fd, or minus a FreeBSD errno.
int open_path(const std::string& path, u64 flags, std::string& note) {
    Resolved r = resolve(path);
    if (r.mount.empty()) {
        note = std::format("'{}': no such place", path);
        return -E_NOENT;
    }
    fs::path host = existing(r);
    std::error_code ec;
    bool want_write = (flags & O_ACCMODE) != 0 || (flags & (O_CREAT | O_TRUNC));
    if (want_write && !r.writable) {
        note = std::format("'{}': read-only", path);
        return -E_ROFS;
    }
    auto f = std::make_shared<File>();
    f->game_path = path;
    if (!host.empty() && fs::is_directory(host, ec)) {
        if (want_write) return -E_ISDIR;
        f->dir = true;
        f->host = host;
        f->entries = list(r);
        note = std::format("'{}': folder, {} entries", path, f->entries.size());
        return add(f);
    }
    if (flags & O_DIRECTORY) return host.empty() ? -E_NOENT : -E_NOTDIR;
    const char* mode = "rb";
    if (!want_write) {
        if (host.empty()) {
            note = std::format("'{}': not found", path);
            return -E_NOENT;
        }
    } else {
        if (host.empty()) {
            if (!(flags & O_CREAT)) return -E_NOENT;
            host = r.candidates.front();
            fs::create_directories(host.parent_path(), ec);
            mode = "w+b";
        } else {
            if ((flags & O_CREAT) && (flags & O_EXCL)) return -E_EXIST;
            mode = (flags & O_TRUNC) ? "w+b" : "r+b";
        }
        f->writable = true;
        f->append = (flags & O_APPEND) != 0;
    }
    f->host = host;
    f->f = open_host(host, mode);
    if (!f->f) {
        note = std::format("'{}': can't open {}", path, host.string());
        return -E_ACCES;
    }
    note = std::format("'{}' -> {}", path, host.string());
    return add(f);
}

// ---- stat ----

// FreeBSD 9's struct stat (120 bytes)
struct Stat {
    u32 dev, ino;
    std::uint16_t mode, nlink;
    u32 uid, gid, rdev;
    i64 atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    i64 size, blocks;
    u32 blksize, flags, gen;
    std::int32_t lspare;
    i64 birthtime, birthtime_ns;
};
static_assert(sizeof(Stat) == 120);

// 0, or a FreeBSD errno
int stat_host(const fs::path& host, bool writable, u64 out) {
    std::error_code ec;
    auto st = fs::status(host, ec);
    if (ec || !fs::exists(st)) return E_NOENT;
    Stat s{};
    bool dir = fs::is_directory(st);
    s.dev = 1;
    s.ino = u32(std::hash<std::string>{}(host.string()));
    s.mode = std::uint16_t((dir ? 0040000 : 0100000) | (writable ? 0777 : 0555));
    s.nlink = 1;
    s.size = dir ? 0 : i64(fs::file_size(host, ec));
    s.blksize = 0x10000;
    s.blocks = (s.size + 511) / 512;
    auto t = fs::last_write_time(host, ec);
    auto sys = std::chrono::system_clock::now() + (t - fs::file_time_type::clock::now());
    i64 sec = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    s.atime = s.mtime = s.ctime = s.birthtime = sec;
    std::memcpy(ptr<void>(out), &s, sizeof s);
    return 0;
}

int stat_path(const std::string& path, u64 out, std::string& note) {
    Resolved r = resolve(path);
    fs::path host = existing(r);
    if (r.mount.empty() || host.empty()) {
        note = std::format("'{}': not found", path);
        return E_NOENT;
    }
    note = std::format("'{}' -> {}", path, host.string());
    return stat_host(host, r.writable, out);
}

// ---- the kernel's calls ----

// sceKernelOpen(path, flags, mode)
i64 k_open(const u64* a, std::string& note) {
    int fd = open_path(cstr(a[0]), a[1], note);
    return fd < 0 ? sce(-fd) : fd;
}

i64 k_close(const u64* a, std::string&) {
    std::lock_guard lock(g_fds_lock);
    return g_fds.erase(int(a[0])) ? 0 : sce(E_BADF);
}

// Reads up to n bytes; minus an errno on failure.
i64 read_file(File& f, void* buf, u64 n) {
    std::lock_guard lock(f.m);
    if (f.dir) return -E_ISDIR;
    u64 got = 0;
    auto* out = static_cast<unsigned char*>(buf);
    if (n && f.ungot >= 0) {
        out[got++] = static_cast<unsigned char>(f.ungot);
        f.ungot = -1;
    }
    got += std::fread(out + got, 1, n - got, f.f);
    if (got < n) f.eof = true;
    return i64(got);
}

i64 write_file(File& f, const void* buf, u64 n) {
    std::lock_guard lock(f.m);
    if (f.dir || !f.writable) return -E_BADF;
    if (f.append) seek_host(f.f, 0, SEEK_END);
    return i64(std::fwrite(buf, 1, n, f.f));
}

// sceKernelRead(fd, buf, n)
i64 k_read(const u64* a, std::string&) {
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    i64 r = read_file(*f, ptr<void>(a[1]), a[2]);
    return r < 0 ? sce(int(-r)) : r;
}

// sceKernelWrite(fd, buf, n): 1 and 2 (stdout, stderr) go to stdout.txt
i64 k_write(const u64* a, std::string&) {
    if (a[0] == 1 || a[0] == 2) {
        std::lock_guard lock(M.log_lock);
        M.stdout_text.append(ptr<char>(a[1]), a[2]);
        return i64(a[2]);
    }
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    i64 r = write_file(*f, ptr<void>(a[1]), a[2]);
    return r < 0 ? sce(int(-r)) : r;
}

// Seeks; returns the new position, or minus an errno.
i64 seek_file(File& f, i64 off, int whence) {
    std::lock_guard lock(f.m);
    if (f.dir) return -E_ISDIR;
    if (whence < 0 || whence > 2 || seek_host(f.f, off, whence) != 0) return -E_INVAL;
    f.eof = false;
    f.ungot = -1;
    return tell_host(f.f);
}

// sceKernelLseek(fd, offset, whence)
i64 k_lseek(const u64* a, std::string&) {
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    i64 r = seek_file(*f, i64(a[1]), int(a[2]));
    return r < 0 ? sce(int(-r)) : r;
}

// sceKernelStat(path, SceKernelStat *)
i64 k_stat(const u64* a, std::string& note) { return sce(stat_path(cstr(a[0]), a[1], note)); }

// sceKernelFstat(fd, SceKernelStat *)
i64 k_fstat(const u64* a, std::string&) {
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    std::lock_guard lock(f->m);
    if (f->f) std::fflush(f->f);
    return sce(stat_host(f->host, f->writable, a[1]));
}

// sceKernelGetdirentries(fd, buf, nbytes, long *basep): FreeBSD's struct dirent, one after
// another: u32 fileno, u16 reclen, u8 type, u8 namlen, the name and its 0, rounded to 4 bytes
i64 k_getdirentries(const u64* a, std::string&) {
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    std::lock_guard lock(f->m);
    if (!f->dir) return sce(E_NOTDIR);
    if (a[3]) *ptr<i64>(a[3]) = i64(f->next);
    u64 at = a[1], end = a[1] + a[2];
    while (f->next < f->entries.size()) {
        const auto& [name, dir] = f->entries[f->next];
        u64 namlen = std::min<u64>(name.size(), 255);
        u64 reclen = (8 + namlen + 1 + 3) & ~u64(3);
        if (at + reclen > end) break;
        std::memset(ptr<void>(at), 0, reclen);
        *ptr<u32>(at) = u32(f->next + 1);
        *ptr<std::uint16_t>(at + 4) = std::uint16_t(reclen);
        *ptr<unsigned char>(at + 6) = dir ? 4 : 8;  // DT_DIR, DT_REG
        *ptr<unsigned char>(at + 7) = static_cast<unsigned char>(namlen);
        std::memcpy(ptr<void>(at + 8), name.data(), namlen);
        at += reclen;
        f->next++;
    }
    return i64(at - a[1]);
}

// The calls that change the file system: 0 or a FreeBSD errno. Only the writable places.
int mkdir_path(const std::string& path, std::string& note) {
    Resolved r = resolve(path);
    note = std::format("'{}'", path);
    if (r.mount.empty()) return E_NOENT;
    if (!r.writable) return E_ROFS;
    std::error_code ec;
    if (fs::exists(r.candidates.front(), ec)) return E_EXIST;
    return fs::create_directory(r.candidates.front(), ec) ? 0 : E_NOENT;
}

int remove_path(const std::string& path, bool dir, std::string& note) {
    Resolved r = resolve(path);
    note = std::format("'{}'", path);
    if (r.mount.empty()) return E_NOENT;
    if (!r.writable) return E_ROFS;
    std::error_code ec;
    const fs::path& p = r.candidates.front();
    if (!fs::exists(p, ec)) return E_NOENT;
    if (fs::is_directory(p, ec) != dir) return dir ? E_NOTDIR : E_ISDIR;
    if (dir && !fs::is_empty(p, ec)) return E_NOTEMPTY;
    return fs::remove(p, ec) ? 0 : E_ACCES;
}

int rename_path(const std::string& from, const std::string& to, std::string& note) {
    Resolved a = resolve(from), b = resolve(to);
    note = std::format("'{}' -> '{}'", from, to);
    if (a.mount.empty() || b.mount.empty()) return E_NOENT;
    if (!a.writable || !b.writable) return E_ROFS;
    std::error_code ec;
    fs::rename(a.candidates.front(), b.candidates.front(), ec);
    return ec ? E_NOENT : 0;
}

int truncate_host(const fs::path& p, u64 len) {
    std::error_code ec;
    fs::resize_file(p, len, ec);
    return ec ? E_INVAL : 0;
}

i64 k_mkdir(const u64* a, std::string& note) { return sce(mkdir_path(cstr(a[0]), note)); }
i64 k_rmdir(const u64* a, std::string& note) { return sce(remove_path(cstr(a[0]), true, note)); }
i64 k_unlink(const u64* a, std::string& note) {
    return sce(remove_path(cstr(a[0]), false, note));
}
i64 k_rename(const u64* a, std::string& note) {
    return sce(rename_path(cstr(a[0]), cstr(a[1]), note));
}
// sceKernelTruncate(path, length)
i64 k_truncate(const u64* a, std::string& note) {
    Resolved r = resolve(cstr(a[0]));
    note = std::format("'{}'", cstr(a[0]));
    if (r.mount.empty()) return sce(E_NOENT);
    if (!r.writable) return sce(E_ROFS);
    return sce(truncate_host(r.candidates.front(), a[1]));
}
// sceKernelFtruncate(fd, length)
i64 k_ftruncate(const u64* a, std::string&) {
    auto f = file(a[0]);
    if (!f) return sce(E_BADF);
    std::lock_guard lock(f->m);
    if (!f->writable) return sce(E_BADF);
    std::fflush(f->f);
    return sce(truncate_host(f->host, a[1]));
}

// ---- POSIX's: -1 and errno on failure ----

i64 posix(int e) {
    if (!e) return 0;
    set_errno(e);
    return -1;
}

i64 p_stat(const u64* a, std::string& note) { return posix(stat_path(cstr(a[0]), a[1], note)); }
i64 p_mkdir(const u64* a, std::string& note) { return posix(mkdir_path(cstr(a[0]), note)); }
i64 p_rmdir(const u64* a, std::string& note) { return posix(remove_path(cstr(a[0]), true, note)); }
i64 p_rename(const u64* a, std::string& note) {
    return posix(rename_path(cstr(a[0]), cstr(a[1]), note));
}
// remove(path): a file or an empty folder
i64 p_remove(const u64* a, std::string& note) {
    Resolved r = resolve(cstr(a[0]));
    std::error_code ec;
    bool dir = !r.candidates.empty() && fs::is_directory(r.candidates.front(), ec);
    return posix(remove_path(cstr(a[0]), dir, note));
}

// __error(): where the calling thread's errno is
i64 p_error(const u64*, std::string&) {
    GameThread* t = current_thread();
    if (!t->errno_at) t->errno_at = sync_alloc(16, 16);
    return i64(t->errno_at);
}

// ---- the C library's streams ----
//
// A FILE* the game is given points at a Stream in the loader's own area, naming an open file.

struct Stream {
    static constexpr u32 TAG = 0x4d525453;  // "STRM"
    u32 tag = TAG;
    int fd = -1;
};

std::shared_ptr<File> stream(u64 s) {
    if (s < SYNC_BASE || s >= SYNC_BASE + SYNC_RESERVE || ptr<Stream>(s)->tag != Stream::TAG)
        return nullptr;
    return file(ptr<Stream>(s)->fd);
}

// fopen(path, mode)
i64 c_fopen(const u64* a, std::string& note) {
    std::string mode = cstr(a[1]);
    bool plus = mode.find('+') != std::string::npos;
    u64 flags = 0;
    switch (mode.empty() ? 'r' : mode[0]) {
        case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
        case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
        default: flags = plus ? O_RDWR : 0;
    }
    int fd = open_path(cstr(a[0]), flags, note);
    if (fd < 0) {
        set_errno(-fd);
        return 0;
    }
    Stream* s = new (ptr<void>(sync_alloc(sizeof(Stream), 16))) Stream();
    s->fd = fd;
    return i64(reinterpret_cast<u64>(s));
}

i64 c_fclose(const u64* a, std::string&) {
    if (!stream(a[0])) return -1;
    std::lock_guard lock(g_fds_lock);
    g_fds.erase(ptr<Stream>(a[0])->fd);
    ptr<Stream>(a[0])->tag = 0;
    return 0;
}

// fread(buf, size, count, FILE*): whole items read
i64 c_fread(const u64* a, std::string&) {
    auto f = stream(a[3]);
    if (!f || !a[1]) return 0;
    i64 r = read_file(*f, ptr<void>(a[0]), a[1] * a[2]);
    if (r < 0) {
        f->error = true;
        return 0;
    }
    return r / i64(a[1]);
}

// fwrite(buf, size, count, FILE*)
i64 c_fwrite(const u64* a, std::string&) {
    std::string text(ptr<char>(a[0]), a[1] * a[2]);
    if (!a[1]) return 0;
    if (!stream(a[3])) {  // stdout or stderr
        std::lock_guard lock(M.log_lock);
        M.stdout_text += text;
        return i64(a[2]);
    }
    i64 r = write_file(*stream(a[3]), text.data(), text.size());
    return r < 0 ? 0 : r / i64(a[1]);
}

// fseek(FILE*, long offset, whence): 0 or -1
i64 c_fseek(const u64* a, std::string&) {
    auto f = stream(a[0]);
    if (!f) return -1;
    return seek_file(*f, i64(a[1]), int(a[2])) < 0 ? -1 : 0;
}

i64 c_ftell(const u64* a, std::string&) {
    auto f = stream(a[0]);
    if (!f) return -1;
    std::lock_guard lock(f->m);
    return f->dir ? -1 : tell_host(f->f) - (f->ungot >= 0 ? 1 : 0);
}

i64 c_fflush(const u64* a, std::string&) {
    if (auto f = stream(a[0])) {
        std::lock_guard lock(f->m);
        if (f->f) std::fflush(f->f);
    }
    return 0;
}

i64 c_feof(const u64* a, std::string&) {
    auto f = stream(a[0]);
    return f && f->eof && f->ungot < 0;
}

i64 c_ferror(const u64* a, std::string&) {
    auto f = stream(a[0]);
    return f && f->error;
}

// fgetc / getc(FILE*): a byte, or -1 (EOF)
i64 c_fgetc(const u64* a, std::string&) {
    auto f = stream(a[0]);
    if (!f) return -1;
    unsigned char c;
    return read_file(*f, &c, 1) == 1 ? c : -1;
}

// ungetc(c, FILE*)
i64 c_ungetc(const u64* a, std::string&) {
    auto f = stream(a[1]);
    if (!f || int(a[0]) == -1) return -1;
    std::lock_guard lock(f->m);
    f->ungot = int(a[0] & 0xff);
    f->eof = false;
    return i64(a[0] & 0xff);
}

// fgets(buf, n, FILE*): up to n - 1 bytes, through the first newline
i64 c_fgets(const u64* a, std::string&) {
    auto f = stream(a[2]);
    int n = int(a[1]);
    if (!f || n <= 0) return 0;
    char* out = ptr<char>(a[0]);
    int got = 0;
    while (got < n - 1) {
        unsigned char c;
        if (read_file(*f, &c, 1) != 1) break;
        out[got++] = char(c);
        if (c == '\n') break;
    }
    if (!got) return 0;
    out[got] = 0;
    return i64(a[0]);
}

// fputs(text, FILE*), puts(text)
i64 c_fputs(const u64* a, std::string&) {
    std::string text = cstr(a[0]);
    if (stream_write(a[1], text)) return 0;
    std::lock_guard lock(M.log_lock);
    M.stdout_text += text;
    return 0;
}
i64 c_puts(const u64* a, std::string&) {
    std::lock_guard lock(M.log_lock);
    M.stdout_text += cstr(a[0]) + "\n";
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }

}  // namespace

bool stream_write(u64 s, const std::string& text) {
    auto f = stream(s);
    if (!f) return false;
    write_file(*f, text.data(), text.size());
    return true;
}

void set_errno(int e) {
    std::string unused;
    *ptr<int>(u64(p_error(nullptr, unused))) = e;
}

void add_file_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceKernelOpen", k_open},
        {"sceKernelClose", k_close},
        {"sceKernelRead", k_read},
        {"sceKernelWrite", k_write},
        {"sceKernelLseek", k_lseek},
        {"sceKernelStat", k_stat},
        {"sceKernelFstat", k_fstat},
        {"sceKernelGetdirentries", k_getdirentries},
        {"sceKernelMkdir", k_mkdir},
        {"sceKernelRmdir", k_rmdir},
        {"sceKernelUnlink", k_unlink},
        {"sceKernelRename", k_rename},
        {"sceKernelTruncate", k_truncate},
        {"sceKernelFtruncate", k_ftruncate},
        {"stat", p_stat},
        {"mkdir", p_mkdir},
        {"rmdir", p_rmdir},
        {"rename", p_rename},
        {"remove", p_remove},
        {"__error", p_error},
        {"fopen", c_fopen},
        {"fclose", c_fclose},
        {"fread", c_fread},
        {"fwrite", c_fwrite},
        {"fseek", c_fseek},
        {"ftell", c_ftell},
        {"fflush", c_fflush},
        {"feof", c_feof},
        {"ferror", c_ferror},
        {"fgetc", c_fgetc},
        {"getc", c_fgetc},
        {"ungetc", c_ungetc},
        {"fgets", c_fgets},
        {"fputs", c_fputs},
        {"puts", c_puts},
        {"setvbuf", nothing},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
