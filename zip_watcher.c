#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
/* PS5 ZIP Watcher: unattended, update-in-place homebrew ZIP installer. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef PS5_TARGET
#include "miniz.h"
typedef mz_stream unzip_stream;
#define ZIP_OK MZ_OK
#define ZIP_END MZ_STREAM_END
#define ZIP_NO_FLUSH MZ_NO_FLUSH
#define ZIP_FINISH MZ_FINISH
#define unzip_init(s, w) mz_inflateInit2((s), (w))
#define unzip_step(s, f) mz_inflate((s), (f))
#define unzip_end(s) mz_inflateEnd((s))
#define zip_crc(c, b, n) mz_crc32((c), (b), (n))
#else
#include <zlib.h>
typedef z_stream unzip_stream;
#define ZIP_OK Z_OK
#define ZIP_END Z_STREAM_END
#define ZIP_NO_FLUSH Z_NO_FLUSH
#define ZIP_FINISH Z_FINISH
#define unzip_init(s, w) inflateInit2((s), (w))
#define unzip_step(s, f) inflate((s), (f))
#define unzip_end(s) inflateEnd((s))
#define zip_crc(c, b, n) crc32((c), (b), (n))
#endif

#define WATCH_ROOT "/mnt/usb0/homebrew"
#define MAX_PATH_LEN 2048
#define MAX_ZIP_NAME 1024
#define MAX_ENTRIES 32768
#define MAX_ARCHIVE_EXTRACT (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define IO_BUFFER 65536
#define POLL_SECONDS 3
#ifndef SETTLE_SECONDS
#define SETTLE_SECONDS 3
#endif

typedef struct {
    char name[MAX_ZIP_NAME];
    uint32_t crc, compressed, uncompressed, local_offset;
    uint16_t flags, method;
    int is_dir;
} zip_entry;

typedef struct {
    zip_entry *items;
    uint16_t count;
    int has_root_files;
    uint64_t total_size;
} archive_index;

static unsigned g_stage_serial = 0;

#ifdef PS5_TARGET
typedef struct { char reserved[45]; char message[3075]; } notify_request;
extern int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);
static void notify_ps5(const char *message) {
    notify_request req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#else
static void notify_ps5(const char *message) { (void)message; }
#endif

static uint16_t rd16(const unsigned char *b) {
    return (uint16_t)(b[0] | (uint16_t)b[1] << 8);
}
static uint32_t rd32(const unsigned char *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static int read_exact(FILE *fp, void *p, size_t n) {
    return fread(p, 1, n, fp) == n;
}
static int build_path(char *dst, size_t dst_sz, const char *a, const char *b) {
    int n = snprintf(dst, dst_sz, "%s/%s", a, b);
    return n > 0 && (size_t)n < dst_sz;
}
static int is_regular_zip(const char *name) {
    size_t n = strlen(name);
    return n > 4 && n < 200 && name[0] != '.' && strcasecmp(name + n - 4, ".zip") == 0;
}

/* ZIP paths must never be able to escape the homebrew directory. */
static int normalize_zip_name(const unsigned char *raw, size_t n, char *out,
                              size_t cap, int *is_dir) {
    if (!n || n + 1 > cap || raw[0] == '/' || raw[0] == '\\') return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = raw[i];
        if (!c || c < 32 || c == 127 || c == ':') return 0;
        out[i] = (c == '\\') ? '/' : (char)c;
    }
    out[n] = '\0';
    *is_dir = out[n-1] == '/';
    if (*is_dir) out[--n] = '\0';
    if (!n) return 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (out[i] == '/' || !out[i]) {
            size_t len = i - start;
            if (!len || (len == 1 && out[start] == '.') ||
                (len == 2 && out[start] == '.' && out[start+1] == '.'))
                return 0;
            start = i + 1;
        }
    }
    if (!strncmp(out, ".ps5zip-", 8)) return 0;
    return 1;
}

static int parse_archive(FILE *fp, archive_index *idx) {
    unsigned char *tail = NULL;
    unsigned char end[22], hdr[46];
    int ok = 0;
    off_t file_size, tail_start;
    memset(idx, 0, sizeof(*idx));
    if (fseeko(fp, 0, SEEK_END) != 0 || (file_size = ftello(fp)) < 22) goto done;
    size_t tail_len = (size_t)(file_size < 65557 ? file_size : 65557);
    tail_start = file_size - (off_t)tail_len;
    tail = malloc(tail_len);
    if (!tail || fseeko(fp, tail_start, SEEK_SET) || !read_exact(fp, tail, tail_len)) goto done;
    off_t eocd = -1;
    for (ssize_t i = (ssize_t)tail_len - 22; i >= 0; --i) {
        if (rd32(tail + i) == 0x06054b50u &&
            (size_t)i + 22 + rd16(tail + i + 20) == tail_len) {
            eocd = tail_start + i;
            memcpy(end, tail + i, sizeof(end));
            break;
        }
    }
    if (eocd < 0 || rd16(end+4) || rd16(end+6) ||
        rd16(end+8) != rd16(end+10) || rd16(end+10) == 0xffff ||
        rd32(end+12) == UINT32_MAX || rd32(end+16) == UINT32_MAX) goto done;
    uint32_t cd_size = rd32(end+12), cd_off = rd32(end+16);
    uint16_t count = rd16(end+10);
    if (!count || count > MAX_ENTRIES ||
        (uint64_t)cd_off + cd_size > (uint64_t)eocd) goto done;
    idx->items = calloc(count, sizeof(zip_entry));
    if (!idx->items) goto done;
    idx->count = count;
    if (fseeko(fp, cd_off, SEEK_SET)) goto done;
    for (uint32_t i = 0; i < count; ++i) {
        off_t current = ftello(fp);
        if (current < 0 || (uint64_t)current + 46 > (uint64_t)cd_off + cd_size ||
            !read_exact(fp, hdr, sizeof hdr) || rd32(hdr) != 0x02014b50u) goto done;
        uint16_t name_len = rd16(hdr+28), extra_len = rd16(hdr+30), comment_len = rd16(hdr+32);
        uint32_t ext_attrs = rd32(hdr+38);
        zip_entry *e = &idx->items[i];
        e->flags = rd16(hdr+8); e->method = rd16(hdr+10);
        e->crc = rd32(hdr+16); e->compressed = rd32(hdr+20);
        e->uncompressed = rd32(hdr+24); e->local_offset = rd32(hdr+42);
        if (!name_len || name_len >= MAX_ZIP_NAME || rd16(hdr+34) ||
            (e->flags & 1) || (e->method != 0 && e->method != 8) ||
            e->compressed == UINT32_MAX || e->uncompressed == UINT32_MAX ||
            e->local_offset == UINT32_MAX ||
            ((ext_attrs >> 16) & 0170000) == 0120000 ||
            (uint64_t)current + 46 + name_len + extra_len + comment_len >
                (uint64_t)cd_off + cd_size) goto done;
        unsigned char raw[MAX_ZIP_NAME];
        if (!read_exact(fp, raw, name_len) ||
            !normalize_zip_name(raw, name_len, e->name, sizeof(e->name), &e->is_dir)) goto done;
        if (fseeko(fp, (off_t)extra_len + comment_len, SEEK_CUR)) goto done;
        if (!e->is_dir && !strchr(e->name, '/')) idx->has_root_files = 1;
        if (e->is_dir && (e->uncompressed || e->compressed)) goto done;
        idx->total_size += e->uncompressed;
        if (idx->total_size > MAX_ARCHIVE_EXTRACT) goto done;
    }
    ok = 1;
done:
    free(tail);
    if (!ok) { free(idx->items); memset(idx, 0, sizeof(*idx)); }
    return ok;
}

/* Never traverse symbolic links that already exist in the destination. */
static int ensure_dir(const char *path) {
    char partial[MAX_PATH_LEN];
    size_t n = strlen(path);
    if (!n || n >= sizeof partial) return 0;
    memcpy(partial, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (partial[i] != '/' && partial[i] != '\0') continue;
        char save = partial[i]; partial[i] = '\0';
        struct stat st;
        if (lstat(partial, &st) != 0) {
            if (errno != ENOENT || mkdir(partial, 0755) != 0) {
                partial[i] = save; return 0;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            partial[i] = save; return 0;
        }
        partial[i] = save;
    }
    return 1;
}
static int ensure_parent(const char *path) {
    char tmp[MAX_PATH_LEN];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return 0;
    memcpy(tmp, path, n + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    return ensure_dir(tmp);
}

static int extract_entry(FILE *fp, off_t zip_size, const zip_entry *e, const char *output) {
    unsigned char local[30];
    if (e->is_dir) return ensure_dir(output);
    if (fseeko(fp, e->local_offset, SEEK_SET) ||
        !read_exact(fp, local, sizeof local) || rd32(local) != 0x04034b50u ||
        rd16(local+8) != e->method || (rd16(local+6) & 1)) return 0;
    uint16_t local_name_len = rd16(local+26), extra_len = rd16(local+28);
    off_t data_off = (off_t)e->local_offset + 30 + local_name_len + extra_len;
    if ((uint64_t)data_off + e->compressed > (uint64_t)zip_size ||
        local_name_len >= MAX_ZIP_NAME || !local_name_len) return 0;
    unsigned char local_name[MAX_ZIP_NAME];
    char normalized[MAX_ZIP_NAME];
    int local_dir;
    if (!read_exact(fp, local_name, local_name_len) ||
        !normalize_zip_name(local_name, local_name_len, normalized, sizeof normalized, &local_dir) ||
        strcmp(normalized, e->name) || local_dir != e->is_dir ||
        fseeko(fp, data_off, SEEK_SET) || !ensure_parent(output)) return 0;
    FILE *out = fopen(output, "wb");
    if (!out) return 0;
    uint32_t crc = (uint32_t)zip_crc(0, NULL, 0);
    uint64_t written = 0;
    int ok = 1;
    unsigned char in[IO_BUFFER], decompressed[IO_BUFFER];
    uint64_t remain = e->compressed;
    if (e->method == 0) {
        if (e->compressed != e->uncompressed) ok = 0;
        while (ok && remain) {
            size_t n = (size_t)(remain < sizeof in ? remain : sizeof in);
            if (!read_exact(fp, in, n) || fwrite(in, 1, n, out) != n) { ok = 0; break; }
            crc = (uint32_t)zip_crc(crc, in, n);
            remain -= n; written += n;
        }
    } else {
        unzip_stream stream;
        memset(&stream, 0, sizeof stream);
        if (unzip_init(&stream, -15) != ZIP_OK) ok = 0;
        if (ok) {
            int finished = 0;
            while (ok && !finished) {
                if (!stream.avail_in && remain) {
                    size_t n = (size_t)(remain < sizeof in ? remain : sizeof in);
                    if (!read_exact(fp, in, n)) { ok = 0; break; }
                    stream.next_in = in; stream.avail_in = (unsigned)n;
                    remain -= n;
                }
                stream.next_out = decompressed;
                stream.avail_out = (unsigned)sizeof decompressed;
                unsigned before_in = stream.avail_in;
                int result = unzip_step(&stream, ZIP_NO_FLUSH);
                size_t produced = sizeof decompressed - stream.avail_out;
                if (produced) {
                    if (written + produced > e->uncompressed ||
                        fwrite(decompressed, 1, produced, out) != produced) { ok = 0; break; }
                    crc = (uint32_t)zip_crc(crc, decompressed, produced);
                    written += produced;
                }
                if (result == ZIP_END) { finished = 1; break; }
                if (result != ZIP_OK || (!produced && stream.avail_in == before_in && !remain)) {
                    ok = 0; break;
                }
            }
            if (!finished || remain || stream.avail_in) ok = 0;
            unzip_end(&stream);
        }
    }
    if (written != e->uncompressed || crc != e->crc) ok = 0;
    if (fflush(out)) ok = 0;
    if (fclose(out)) ok = 0;
    if (!ok) unlink(output);
    return ok;
}

static int remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char p[MAX_PATH_LEN]; struct stat st;
        if (!build_path(p, sizeof p, dir, ent->d_name) || lstat(p, &st)) { ok = 0; continue; }
        if (S_ISDIR(st.st_mode)) { if (!remove_tree(p)) ok = 0; }
        else if (unlink(p)) ok = 0;
    }
    closedir(d);
    if (rmdir(dir)) ok = 0;
    return ok;
}

/* Merge each validated file into its destination, replacing any existing regular file.
   Existing unrelated files are intentionally preserved (update semantics). */
static int merge_tree(const char *source, const char *dest) {
    if (!ensure_dir(dest)) return 0;
    DIR *d = opendir(source);
    if (!d) return 0;
    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN]; struct stat in, out;
        if (!build_path(src, sizeof src, source, ent->d_name) ||
            !build_path(dst, sizeof dst, dest, ent->d_name) || lstat(src, &in)) { ok = 0; break; }
        int exists = lstat(dst, &out) == 0;
        if (!exists && errno != ENOENT) { ok = 0; break; }
        if (S_ISDIR(in.st_mode)) {
            if (exists && !S_ISDIR(out.st_mode)) { ok = 0; break; }
            if (!merge_tree(src, dst)) { ok = 0; break; }
        } else if (S_ISREG(in.st_mode)) {
            if (exists && !S_ISREG(out.st_mode)) { ok = 0; break; }
            if (rename(src, dst)) { ok = 0; break; }
        } else { ok = 0; break; }
    }
    closedir(d);
    if (ok && rmdir(source)) ok = 0;
    return ok;
}

static int install_zip(const char *zip_path, const char *root, const char *filename,
                       const struct stat *original) {
    int success = 0;
    FILE *fp = fopen(zip_path, "rb");
    if (!fp) return 0;
    archive_index idx;
    char stage[MAX_PATH_LEN], stage_dest[MAX_PATH_LEN], stem[200];
    stage[0] = '\0';
    if (!parse_archive(fp, &idx)) {
        printf("[ERROR] Invalid/unsupported ZIP: %s\n", filename);
        fclose(fp);
        return 0;
    }
    size_t n = strlen(filename)-4;
    memcpy(stem, filename, n); stem[n] = '\0';
    int stage_created = 0;
    for (int tries = 0; tries < 20; ++tries) {
        int x = snprintf(stage, sizeof stage, "%s/.ps5zip-stage-%ld-%u", root,
                         (long)getpid(), ++g_stage_serial);
        if (x < 0 || (size_t)x >= sizeof stage) { stage[0] = '\0'; break; }
        if (!mkdir(stage, 0700)) { stage_created = 1; break; }
        if (errno != EEXIST) { stage[0] = '\0'; break; }
    }
    if (!stage_created) { stage[0] = '\0'; goto finish; }
    /* Flat ZIPs are placed in <ZIP_NAME>/; ZIPs with directory layouts
       preserve those paths directly under homebrew/. */
    if (idx.has_root_files) {
        if (!build_path(stage_dest, sizeof stage_dest, stage, stem) ||
            !ensure_dir(stage_dest)) goto finish;
    } else {
        if (strlen(stage) >= sizeof stage_dest) goto finish;
        strcpy(stage_dest, stage);
    }
    struct stat st;
    if (fstat(fileno(fp), &st)) goto finish;
    for (uint16_t i = 0; i < idx.count; ++i) {
        char out[MAX_PATH_LEN];
        if (!build_path(out, sizeof out, stage_dest, idx.items[i].name) ||
            !extract_entry(fp, st.st_size, &idx.items[i], out)) {
            printf("[ERROR] Extraction failed: %s :: %s\n", filename, idx.items[i].name);
            goto finish;
        }
    }
    /* A ZIP being copied/rewritten must never be installed or deleted. */
    struct stat latest, handle;
    if (fstat(fileno(fp), &handle) || stat(zip_path, &latest) ||
        original->st_size != latest.st_size || original->st_mtime != latest.st_mtime ||
        original->st_ino != latest.st_ino || handle.st_size != latest.st_size ||
        handle.st_mtime != latest.st_mtime || handle.st_ino != latest.st_ino) {
        printf("[INFO] ZIP changed during extraction: %s\n", filename);
        goto finish;
    }
    if (!merge_tree(stage, root)) {
        printf("[ERROR] Update merge failed; ZIP retained: %s\n", filename);
        goto finish; /* clean up remaining staged files; original ZIP is retained */
    }
    stage[0] = '\0';
    /* Do not delete an archive replaced while we were applying updates. */
    if (stat(zip_path, &latest) || latest.st_size != original->st_size ||
        latest.st_mtime != original->st_mtime || latest.st_ino != original->st_ino) {
        printf("[WARN] ZIP changed after installation; keeping ZIP: %s\n", filename);
        goto finish;
    }
    if (unlink(zip_path)) {
        printf("[ERROR] Installed but cannot delete ZIP: %s\n", filename);
        goto finish;
    }
    printf("[OK] Updated existing files and deleted ZIP: %s\n", filename);
    char message[256];
    snprintf(message, sizeof message, "ZIP Watcher: installed %.200s", filename);
    notify_ps5(message);
    success = 1;
finish:
    fclose(fp);
    free(idx.items);
    if (stage[0]) remove_tree(stage);
    return success;
}

static int scan_once(const char *root, unsigned settle_seconds) {
    DIR *dir = opendir(root);
    if (!dir) {
        printf("[WAIT] Cannot open %s (%s)\n", root, strerror(errno));
        return 0;
    }
    int installed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!is_regular_zip(entry->d_name)) continue;
        char path[MAX_PATH_LEN];
        if (!build_path(path, sizeof path, root, entry->d_name)) continue;
        struct stat before, after;
        if (lstat(path, &before) || !S_ISREG(before.st_mode)) continue;
        if (settle_seconds) sleep(settle_seconds);
        if (lstat(path, &after) || !S_ISREG(after.st_mode) ||
            before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
            before.st_ino != after.st_ino) continue;
        printf("[INFO] Checking: %s\n", entry->d_name);
        installed += install_zip(path, root, entry->d_name, &after);
    }
    closedir(dir);
    return installed;
}

int main(int argc, char **argv) {
    const char *root = WATCH_ROOT;
    int once = 0;
#ifndef PS5_TARGET
    if (argc > 1 && !strcmp(argv[1], "--once")) {
        once = 1;
        if (argc > 2) root = argv[2];
    } else if (argc > 1 && !strcmp(argv[1], "--watch") && argc > 2) {
        root = argv[2];
    }
#else
    (void)argc; (void)argv;
#endif
    printf("PS5 ZIP Watcher v1.0 | %s | overwrite enabled\n", root);
    notify_ps5("PS5 ZIP Watcher: watching USB homebrew folder");
    do {
        scan_once(root, SETTLE_SECONDS);
        if (once) break;
        sleep(POLL_SECONDS);
    } while (1);
    return 0;
}
