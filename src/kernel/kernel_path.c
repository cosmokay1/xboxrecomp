/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    /* The dashboard's custom soundtracks (XAPI reads MUSIC\ST.DB and
     * MUSIC\<soundtrack>\<song>.WMA). The host generates them from the
     * user's <GameRoot>/Music folders, so they live with the saves rather
     * than in the game dir. Listed before Partition1 so it wins. */
    { "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC\\", 1, "\\Soundtracks", "/Soundtracks" },
    /* Saved games (UDATA) and title data (TDATA) are the player's files, not
     * the game's: they go under the save dir as ordinary folders. Under the
     * game dir they landed in the disc folder -- for an install, the user's
     * own copy of the disc. Existing ones are copied over on first start
     * (xbox_path_init). No trailing separator, so opening the folder itself
     * (for enumeration) is covered too. */
    { "\\Device\\Harddisk0\\Partition1\\UDATA", 1, "\\UDATA",        "/UDATA"       },
    { "\\Device\\Harddisk0\\Partition1\\TDATA", 1, "\\TDATA",        "/TDATA"       },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. The Win32 backend fills this during translation; the POSIX one
 * does not yet, so it reads empty there. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[MAX_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void xbox_write_partition_table(const WCHAR *path)
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        *(ULONG *)(e + 16) = XBOX_PART_IN_USE;
        *(ULONG *)(e + 20) = parts[i].start;
        *(ULONG *)(e + 24) = parts[i].size;
        *(ULONG *)(e + 28) = 0;
    }

    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

/*
 * Map a partition device onto an image file.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how a
 * device is opened. Anything with a path under it -- Partition1\TDATA and the
 * like -- is an ordinary filesystem access and falls through to the rules
 * table, which puts it in a directory.
 *
 * Partition0 is the whole disk and carries the partition table written above.
 * The rest start empty, which is what an unformatted partition looks like, so
 * a title that wants a filesystem there formats one.
 */
static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    int digit;
    const char *rest;

    if (!len)
        return FALSE;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9')
        return FALSE;

    rest = xbox_path + len + 1;
    /* Nothing below it, allowing for a single trailing separator. */
    if (*rest == '\\' || *rest == '/')
        rest++;
    if (*rest != '\0')
        return FALSE;

    swprintf_s(out, n, L"%s\\Partition%c.img", s_save_dir, (WCHAR)digit);
    return TRUE;
}

/* Copy a folder tree, keeping anything already at the destination. */
static void copy_tree(const WCHAR *from, const WCHAR *to)
{
    WCHAR pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    CreateDirectoryW(to, NULL);
    swprintf_s(pat, MAX_PATH, L"%s\\*", from);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(a, MAX_PATH, L"%s\\%s", from, fd.cFileName);
        swprintf_s(b, MAX_PATH, L"%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            copy_tree(a, b);
        else
            CopyFileW(a, b, TRUE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Saves used to live in the game dir's UDATA and TDATA. The first start
 * with the save-dir mapping copies them across (the originals stay, as a
 * backup), so nobody loses a franchise to the move. */
static void migrate_save_area(const WCHAR *name)
{
    WCHAR from[MAX_PATH], to[MAX_PATH];
    swprintf_s(from, MAX_PATH, L"%s\\%s", s_game_dir, name);
    swprintf_s(to, MAX_PATH, L"%s\\%s", s_save_dir, name);
    if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES)
        return;                         /* already moved (or started fresh) */
    if (GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES)
        return;
    copy_tree(from, to);
    fprintf(stderr, "  [PATH] copied %S -> %S\n", from, to);
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        migrate_save_area(L"UDATA");
        migrate_save_area(L"TDATA");
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            static const ULONGLONG part_sectors[6] = {
                0,             /* 0: whole disk, sized below      */
                0x00465400ull, /* 1: E: game and saves            */
                0x000FA000ull, /* 2: C: system                    */
                0x00177000ull, /* 3: X: cache                     */
                0x00177000ull, /* 4: Y: cache                     */
                0x00177000ull, /* 5: Z: cache                     */
            };
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path_shared, MAX_PATH, p, _TRUNCATE);
}



/* Folders a title maps under the disc (2026-10-09): "D:\<name>\..." and
 * "\Device\CdRom0\<name>\..." open files in a host folder of the title's
 * choosing -- NFL 2K5's added teams (mods\teams) are read this way. */
static struct { char name[32]; WCHAR host[MAX_PATH]; } s_title_dirs[8];
static int s_title_dir_count;

void xbox_path_map_disc_dir(const char *name, const char *host_dir)
{
    if (s_title_dir_count >= 8 || !name || !host_dir) return;
    snprintf(s_title_dirs[s_title_dir_count].name, sizeof s_title_dirs[0].name, "%s", name);
    MultiByteToWideChar(CP_UTF8, 0, host_dir, -1, s_title_dirs[s_title_dir_count].host, MAX_PATH);
    s_title_dir_count++;
}

static int title_dir_path(const char *xbox_path, xbox_host_char *out, DWORD n)
{
    static const char *const disc[] = { "D:\\", "\\??\\D:\\", "\\Device\\CdRom0\\" };
    int i, k;
    for (k = 0; k < 3; k++) {
        int skip = match_prefix(xbox_path, disc[k]);
        if (!skip) continue;
        for (i = 0; i < s_title_dir_count; i++) {
            size_t ln = strlen(s_title_dirs[i].name);
            const char *rest = xbox_path + skip;
            if (_strnicmp(rest, s_title_dirs[i].name, ln) || (rest[ln] != '\\' && rest[ln] != 0)) continue;
            rest += ln;
            if (*rest == '\\') rest++;
            _snwprintf_s(out, n, _TRUNCATE, L"%s\\%S", s_title_dirs[i].host, rest);
            return 1;
        }
    }
    return 0;
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (title_dir_path(xbox_path, host_path_buf, buf_size))
        return TRUE;

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    /* NFL 2K5 scans hundreds of optional cache index files during startup.
     * Logging every miss changes scheduler timing more than the I/O itself.
     * Keep ordinary paths visible and make the repetitive cache scan opt-in. */
    /* The soundtrack database is re-read on every song-info call
     * (~100 times a second in the menus); keep it out of the log too. */
    static int file_trace = -1;     /* getenv per open was measurable: ST.DB is read ~100x/s */
    if (file_trace < 0) file_trace = getenv("RECOMP_FILE_TRACE") != NULL;
    if (file_trace || xbox_verbose() &&
        (!(strstr(xbox_path, "Partition5") && strstr(xbox_path, ".uh")) &&
         !strstr(xbox_path, "\\MUSIC\\ST.DB"))) {
        fprintf(stderr, "  [PATH] %s\n", xbox_path);
        fflush(stderr);
    }
    {
        WCHAR remainder_wide[MAX_PATH];
        while (*remainder == '\\' || *remainder == '/')
            remainder++;
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

static void xbox_create_partition_images(void);   /* below, with the device mapping */

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);
    xbox_create_partition_images();

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

/* Folders a title maps under the disc: "D:\\<name>\\..." and
 * "\\Device\\CdRom0\\<name>\\..." open files in a host folder of the
 * title's choosing (NFL 2K5's added teams, mods/teams). Windows version above. */
static struct { char name[32]; char host[MAX_PATH]; } s_title_dirs[8];
static int s_title_dir_count;

void xbox_path_map_disc_dir(const char *name, const char *host_dir)
{
    if (s_title_dir_count >= 8 || !name || !host_dir) return;
    snprintf(s_title_dirs[s_title_dir_count].name, sizeof s_title_dirs[0].name, "%s", name);
    snprintf(s_title_dirs[s_title_dir_count].host, sizeof s_title_dirs[0].host, "%s", host_dir);
    for (char *p = s_title_dirs[s_title_dir_count].host; *p; p++) if (*p == '\\') *p = '/';
    s_title_dir_count++;
}

static int title_dir_path(const char *xbox_path, char *out, DWORD n)
{
    static const char *const disc[] = { "D:\\", "\\??\\D:\\", "\\Device\\CdRom0\\" };
    for (int k = 0; k < 3; k++) {
        int skip = match_prefix(xbox_path, disc[k]);
        if (!skip) continue;
        for (int i = 0; i < s_title_dir_count; i++) {
            size_t ln = strlen(s_title_dirs[i].name);
            const char *rest = xbox_path + skip;
            if (strncasecmp(rest, s_title_dirs[i].name, ln) || (rest[ln] != '\\' && rest[ln] != 0)) continue;
            rest += ln;
            if (*rest == '\\') rest++;
            snprintf(out, n, "%s/%s", s_title_dirs[i].host, rest);
            for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
            return 1;
        }
    }
    return 0;
}

/* Xbox (FATX) and Windows (NTFS) paths are case-insensitive; Linux is not.
 * Titles name the same file with different capitalisation than the disc
 * folder has ("D:\VC_53450030" vs vc_53450030), so a miss is resolved one
 * component at a time against the real directory entries. Exact hits cost
 * one stat(). A component that exists under no capitalisation is left as
 * given, with everything after it, so files being created keep their name. */
#include <dirent.h>
static void resolve_case(char *path, size_t cap)
{
    struct stat st;
    char out[MAX_PATH], rest[MAX_PATH];
    char *comp, *save = NULL;
    size_t len;
    if (!path[0] || stat(path, &st) == 0) return;
    snprintf(rest, sizeof rest, "%s", path);
    out[0] = 0;
    if (rest[0] == '/') snprintf(out, sizeof out, "/");
    for (comp = strtok_r(rest, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        char probe[MAX_PATH];
        len = strlen(out);
        snprintf(probe, sizeof probe, "%s%s%s", out, (len && out[len - 1] != '/') ? "/" : "", comp);
        if (lstat(probe, &st) != 0) {
            DIR *d = opendir(len ? out : ".");
            struct dirent *e;
            int found = 0;
            if (d) {
                while ((e = readdir(d)) != NULL) {
                    if (!strcasecmp(e->d_name, comp)) {
                        snprintf(probe, sizeof probe, "%s%s%s", out,
                                 (len && out[len - 1] != '/') ? "/" : "", e->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) {
                /* Keep this component and the remainder as given. */
                const char *tail = path + (comp - rest);
                snprintf(probe, sizeof probe, "%s%s%s", out,
                         (len && out[len - 1] != '/') ? "/" : "", tail);
                snprintf(out, sizeof out, "%s", probe);
                break;
            }
        }
        snprintf(out, sizeof out, "%s", probe);
    }
    if (strlen(out) < cap) memcpy(path, out, strlen(out) + 1);
}

/* The raw disk devices, \Device\Harddisk0\PartitionN (Windows version above,
 * which explains why: XAPI's utility-drive mount reads the partition table
 * from Partition0 at offset 0x800 and the title quits to the dashboard with
 * HalReturnToFirmware when that read fails). Each device is an image file in
 * the save dir; Partition0 carries the standard retail partition table. */
#include <fcntl.h>
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void xbox_write_partition_table(const char *path)
{
    static const struct { const char *name; uint32_t start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "cannot create %s: %s", path, strerror(errno));
        return;
    }
    memset(sector, 0, sizeof sector);
    memcpy(sector, "****PARTINFO****", 16);
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        unsigned char *e = sector + 48 + i * 32;     /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        put_le32(e + 16, XBOX_PART_IN_USE);
        put_le32(e + 20, parts[i].start);
        put_le32(e + 24, parts[i].size);
        put_le32(e + 28, 0);
    }
    if (pwrite(fd, sector, sizeof sector, XBOX_PART_TABLE_OFFSET) != (ssize_t)sizeof sector)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "cannot write partition table to %s", path);
    close(fd);
}

static void xbox_create_partition_images(void)
{
    /* Sized to the geometry the table describes. ftruncate makes sparse
     * files on Linux, so the 750 MB caches cost nothing until written. */
    static const unsigned long long part_sectors[6] = {
        0, 0x00465400ull, 0x000FA000ull, 0x00177000ull, 0x00177000ull, 0x00177000ull,
    };
    static const char *subs[] = { "TitleData", "UserData", "Cache", "SystemData" };
    char path[MAX_PATH];
    mkdir_p(s_save_dir);
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", s_save_dir, subs[i]);
        mkdir_p(path);
    }
    snprintf(path, sizeof path, "%s/Partition0.img", s_save_dir);
    xbox_write_partition_table(path);
    for (int p = 1; p <= 5; p++) {
        struct stat st;
        int fd;
        snprintf(path, sizeof path, "%s/Partition%d.img", s_save_dir, p);
        if (stat(path, &st) == 0 && (unsigned long long)st.st_size >= part_sectors[p] * 512ull)
            continue;
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        if (ftruncate(fd, (off_t)(part_sectors[p] * 512ull)) != 0)
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "cannot size %s", path);
        close(fd);
    }
}

/* "\Device\Harddisk0\PartitionN" with nothing below it -> PartitionN.img.
 * Paths under a partition are filesystem accesses and go to the rules. */
static BOOL xbox_partition_device_path(const char *xbox_path, char *out, DWORD n)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    const char *rest;
    int digit;
    if (!len) return FALSE;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9') return FALSE;
    rest = xbox_path + len + 1;
    if (*rest == '\\' || *rest == '/') rest++;
    if (*rest != '\0') return FALSE;
    snprintf(out, n, "%s/Partition%c.img", s_save_dir, digit);
    return TRUE;
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        return TRUE;
    }

    if (title_dir_path(xbox_path, host_path_buf, buf_size)) {
        resolve_case(host_path_buf, buf_size);
        return TRUE;
    }

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    resolve_case(host_path_buf, buf_size);
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }

        resolve_case(host_path_buf, buf_size);
        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */

/* ---- per-user data (2026-10-09) ----------------------------------------
 *
 * Settings, caches and logs a PC game writes belong in the user's profile,
 * not beside the executable (which may be in Program Files). The title sets
 * the folder once at startup (xbox_set_user_dir); xbox_user_path() maps a
 * relative name into it, creating parent folders. When the folder is not
 * set, names stay relative to the working directory (portable layout).
 * A file that does not exist there yet but exists at the old relative
 * location is copied over once, so existing settings carry across. */
#ifdef _WIN32
#include <windows.h>
static char s_user_dir[MAX_PATH];

void xbox_set_user_dir(const char *dir)
{
    if (dir && *dir) {
        strncpy(s_user_dir, dir, sizeof s_user_dir - 1);
        CreateDirectoryA(s_user_dir, NULL);
    } else
        s_user_dir[0] = 0;
}

const char *xbox_user_dir(void) { return s_user_dir; }

static void user_make_parents(char *path)
{
    char *p;
    for (p = path + 3; *p; p++)
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = c;
        }
}

/* `rel` (e.g. "cache\\ps\\x.bin") -> out. Returns out. */
char *xbox_user_path(const char *rel, char *out, size_t n)
{
    if (!s_user_dir[0]) {
        snprintf(out, n, "%s", rel);
        return out;
    }
    snprintf(out, n, "%s\\%s", s_user_dir, rel);
    user_make_parents(out);
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(rel) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesA(rel) & FILE_ATTRIBUTE_DIRECTORY))
        CopyFileA(rel, out, TRUE);                 /* one-time migration of an existing file */
    return out;
}
#else
/* POSIX: same contract. `rel` may use backslashes. */
#include <sys/stat.h>
static char s_user_dir[MAX_PATH];

void xbox_set_user_dir(const char *dir)
{
    if (dir && *dir) {
        snprintf(s_user_dir, sizeof s_user_dir, "%s", dir);
        for (char *p = s_user_dir; *p; p++) if (*p == '\\') *p = '/';
        mkdir(s_user_dir, 0755);
    } else
        s_user_dir[0] = 0;
}

const char *xbox_user_dir(void) { return s_user_dir; }

static int user_copy_once(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    if (!in) return 0;
    out = fopen(to, "wbx");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out) == 0;
}

char *xbox_user_path(const char *rel, char *out, size_t n)
{
    char relp[MAX_PATH];
    struct stat st;
    snprintf(relp, sizeof relp, "%s", rel);
    for (char *p = relp; *p; p++) if (*p == '\\') *p = '/';
    if (!s_user_dir[0]) {
        snprintf(out, n, "%s", relp);
        return out;
    }
    snprintf(out, n, "%s/%s", s_user_dir, relp);
    for (char *p = out + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(out, 0755); *p = '/'; }
    if (stat(out, &st) != 0 && stat(relp, &st) == 0 && !S_ISDIR(st.st_mode))
        user_copy_once(relp, out);              /* one-time migration of an existing file */
    return out;
}
#endif
