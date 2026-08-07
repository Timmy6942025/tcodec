/*
 * tcmux.c — TCodec packet container and segment utility
 *
 * This is an intentionally small, packet-preserving container for .tcv
 * access units. It is not an ISO BMFF codec registration: a player must use
 * tcdemux/tcdec (or a future libavcodec integration) to decode the payload.
 * The fragment layout is deliberately epoch-style: every record carries an
 * explicit PTS, payload length, and keyframe flag, so segments can be cut
 * without parsing codec residual syntax.
 *
 * Usage:
 *   tcmux mux     -i input.tcv -o output.tcmx -f 30
 *   tcmux demux   -i input.tcmx -o output.tcv
 *   tcmux segment -i input.tcmx -o segments -s 60 -p playlist.m3u8
 *   tcmux mp4mux  -i input.tcv  -o output.mp4 -f 30
 *   tcmux mp4demux -i output.mp4 -o output.tcv
 *
 * The mp4mux mode stores native TCV access units in a private `tcv1`
 * ISO-BMFF sample entry. Stock players cannot decode that sample entry;
 * tools/tcmux_mp4.sh is the separately tested H.264 playback bridge.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tcodec_types.h"

#define TCMX_VERSION 1u
#define TCMX_MAGIC "TCMX"
#define TCMF_MAGIC "TCMF"
#define TCMX_MAX_PACKET (100u * 1024u * 1024u)
#define TCMX_HEADER_SIZE 16u
#define TCMX_RECORD_SIZE 20u
#define TCMX_MAX_SEGMENTS (1u << 20)

struct packet {
    uint64_t pts;
    uint32_t size;
    uint8_t key;
    uint8_t *data;
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s mux -i input.tcv -o output.tcmx [-f fps]\n"
        "  %s demux -i input.tcmx -o output.tcv\n"
        "  %s segment -i input.tcmx -o directory -s frames [-p playlist.m3u8]\n"
        "  %s mp4mux -i input.tcv -o output.mp4 [-f fps]\n"
        "  %s mp4demux -i input.mp4 -o output.tcv\n",
        prog, prog, prog, prog, prog);
}

static int read_exact(FILE *f, void *buf, size_t n)
{
    return n == 0 || fread(buf, 1, n, f) == n;
}

static int write_exact(FILE *f, const void *buf, size_t n)
{
    return n == 0 || fwrite(buf, 1, n, f) == n;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) { p[i] = (uint8_t)v; v >>= 8; }
}

static int parse_uint(const char *text, unsigned min, unsigned max, unsigned *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !*text || text[0] == '-') return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max)
        return 0;
    *out = (unsigned)value;
    return 1;
}

static int write_header(FILE *f, const char magic[4], uint32_t fps)
{
    uint8_t h[TCMX_HEADER_SIZE] = {0};
    memcpy(h, magic, 4);
    put_u32(h + 4, TCMX_VERSION);
    put_u32(h + 8, fps ? fps : 30u);
    put_u32(h + 12, 0u);
    return write_exact(f, h, sizeof(h));
}

static int read_header(FILE *f, char magic_out[5], uint32_t *fps)
{
    uint8_t h[TCMX_HEADER_SIZE];
    if (!read_exact(f, h, sizeof(h))) return 0;
    memcpy(magic_out, h, 4); magic_out[4] = '\0';
    if (memcmp(h, TCMX_MAGIC, 4) != 0 && memcmp(h, TCMF_MAGIC, 4) != 0)
        return 0;
    if (get_u32(h + 4) != TCMX_VERSION) return 0;
    *fps = get_u32(h + 8);
    return *fps >= 1 && *fps <= 1000;
}

static int packet_header_valid(const struct packet *p)
{
    /* The raw .tcv stream is length-prefixed, but each payload is still
     * required to begin with the canonical TCV frame header.  This keeps
     * container keyframe metadata tied to codec syntax rather than a
     * guessed byte in arbitrary input. */
    if (p->size < 9 || p->data[0] != 'T' || p->data[1] != 'C' ||
        p->data[2] != 'V' || p->data[3] > TC_VERSION_V2)
        return 0;
    if ((p->data[3] == TC_VERSION_V0 && p->size < TC_FRAME_HEADER_SIZE_V0) ||
        (p->data[3] != TC_VERSION_V0 && p->size < TC_FRAME_HEADER_SIZE_V1))
        return 0;
    return 1;
}

static int read_raw_packet(FILE *f, struct packet *p)
{
    uint8_t nbuf[4];
    size_t got = fread(nbuf, 1, sizeof(nbuf), f);
    if (got == 0 && feof(f) && !ferror(f)) return 0; /* clean EOF */
    if (got != sizeof(nbuf)) return -1;              /* truncated length */
    if (get_u32(nbuf) == 0 || get_u32(nbuf) > TCMX_MAX_PACKET)
        return -1;
    p->size = get_u32(nbuf);
    p->data = (uint8_t *)malloc(p->size);
    if (!p->data || !read_exact(f, p->data, p->size)) {
        free(p->data); p->data = NULL; return -1;
    }
    if (!packet_header_valid(p)) { free(p->data); p->data = NULL; return -1; }
    p->key = (p->data[8] & TC_FLAG_KEY_FRAME) ? 1u : 0u;
    return 1;
}

static FILE *open_temp_output(const char *path, char *tmp_path, size_t tmp_size)
{
    int fd;
    int n = snprintf(tmp_path, tmp_size, "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= tmp_size) return NULL;
    fd = mkstemp(tmp_path);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp_path); return NULL; }
    return f;
}

static int write_record(FILE *f, const struct packet *p)
{
    uint8_t r[TCMX_RECORD_SIZE] = {0};
    memcpy(r, "PKT0", 4);
    put_u64(r + 4, p->pts);
    put_u32(r + 12, p->size);
    r[16] = p->key;
    return write_exact(f, r, sizeof(r)) && write_exact(f, p->data, p->size);
}

static int read_record(FILE *f, struct packet *p)
{
    uint8_t r[TCMX_RECORD_SIZE];
    size_t got = fread(r, 1, sizeof(r), f);
    if (got == 0 && feof(f) && !ferror(f)) return 0;
    if (got != sizeof(r) || memcmp(r, "PKT0", 4) != 0) return -1;
    p->pts = get_u64(r + 4);
    p->size = get_u32(r + 12);
    if (r[16] > 1 || r[17] != 0 || r[18] != 0 || r[19] != 0) return -1;
    p->key = r[16];
    if (p->size == 0 || p->size > TCMX_MAX_PACKET) return -1;
    p->data = (uint8_t *)malloc(p->size);
    if (!p->data || !read_exact(f, p->data, p->size)) {
        free(p->data); p->data = NULL; return -1;
    }
    if (!packet_header_valid(p) || !!(p->data[8] & TC_FLAG_KEY_FRAME) != !!p->key) {
        free(p->data); p->data = NULL; return -1;
    }
    return 1;
}

static int mux_file(const char *in_path, const char *out_path, uint32_t fps)
{
    FILE *in = fopen(in_path, "rb");
    char tmp_path[1024] = {0};
    FILE *out = open_temp_output(out_path, tmp_path, sizeof(tmp_path));
    if (!in || !out) { fprintf(stderr, "tcmux: open failed: %s\n", strerror(errno)); if (in) fclose(in); if (out) fclose(out); if (tmp_path[0]) unlink(tmp_path); return 1; }
    int rc = 0, have_packet = 0; uint64_t pts = 0; struct packet p;
    if (!write_header(out, TCMX_MAGIC, fps)) rc = 1;
    while (!rc) {
        memset(&p, 0, sizeof(p));
        int got = read_raw_packet(in, &p);
        if (got == 0) break;
        if (got < 0) { fprintf(stderr, "tcmux: malformed raw packet at PTS %llu\n", (unsigned long long)pts); rc = 1; break; }
        p.pts = pts++;
        have_packet = 1;
        if (!write_record(out, &p)) rc = 1;
        free(p.data);
    }
    if (!have_packet) rc = 1;
    if (fclose(in) != 0 || fclose(out) != 0) rc = 1;
    if (!rc && rename(tmp_path, out_path) != 0) rc = 1;
    if (rc) unlink(tmp_path);
    return rc;
}

static int demux_file(const char *in_path, const char *out_path)
{
    FILE *in = fopen(in_path, "rb");
    char tmp_path[1024];
    FILE *out = open_temp_output(out_path, tmp_path, sizeof(tmp_path));
    char magic[5]; uint32_t fps; int rc = 0, have_packet = 0;
    (void)fps;
    if (!in || !out) { fprintf(stderr, "tcmux: open failed: %s\n", strerror(errno)); if (in) fclose(in); if (out) fclose(out); return 1; }
    if (!read_header(in, magic, &fps) || strcmp(magic, TCMX_MAGIC) != 0) rc = 1;
    while (!rc) {
        struct packet p = {0}; int got = read_record(in, &p);
        if (got == 0) break;
        if (got < 0) { rc = 1; break; }
        have_packet = 1;
        uint8_t nbuf[4];
        put_u32(nbuf, p.size);
        if (!write_exact(out, nbuf, sizeof(nbuf)) || !write_exact(out, p.data, p.size)) rc = 1;
        free(p.data);
    }
    if (!have_packet) rc = 1;
    if (fclose(in) != 0 || fclose(out) != 0) rc = 1;
    if (!rc && rename(tmp_path, out_path) != 0) rc = 1;
    if (rc) unlink(tmp_path);
    return rc;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    fprintf(stderr, "tcmux: cannot create %s: %s\n", path, strerror(errno)); return 1;
}

static int write_playlist(const char *path, uint32_t fps, unsigned count,
                          const unsigned *frames)
{
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    if (fprintf(f, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%u\n#EXT-X-MEDIA-SEQUENCE:0\n", (unsigned)((frames[0] + fps - 1) / fps)) < 0) { fclose(f); return 1; }
    for (unsigned i = 0; i < count; ++i)
        if (fprintf(f, "#EXTINF:%.6f,\nseg_%04u.m4s\n", (double)frames[i] / fps, i) < 0) { fclose(f); return 1; }
    if (fprintf(f, "#EXT-X-ENDLIST\n") < 0 || fclose(f) != 0) return 1;
    return 0;
}

static int segment_file(const char *in_path, const char *dir, unsigned target,
                        const char *playlist)
{
    FILE *in = fopen(in_path, "rb"); char magic[5]; uint32_t fps;
    if (!in || !read_header(in, magic, &fps) || strcmp(magic, TCMX_MAGIC) != 0 || target == 0) {
        fprintf(stderr, "tcmux: invalid input or segment size\n"); if (in) fclose(in); return 1;
    }
    if (mkdir_one(dir)) { fclose(in); return 1; }
    FILE *seg = NULL; unsigned seg_no = 0, in_seg = 0, *counts = NULL, count_cap = 0;
    int rc = 0, have_packet = 0;
    for (;;) {
        struct packet p = {0}; int got = read_record(in, &p);
        if (got == 0) break;
        if (got < 0) { rc = 1; break; }
        if (!have_packet && !p.key) {
            fprintf(stderr, "tcmux: input does not begin with a keyframe\n");
            free(p.data); rc = 1; break;
        }
        if (seg && in_seg >= target && p.key) {
            fclose(seg); seg = NULL; in_seg = 0;
        }
        if (!seg) {
            char path[1024];
            int path_len;
            if (seg_no >= TCMX_MAX_SEGMENTS) { free(p.data); rc = 1; break; }
            path_len = snprintf(path, sizeof(path), "%s/seg_%04u.m4s", dir, seg_no);
            if (path_len < 0 || (size_t)path_len >= sizeof(path)) { free(p.data); rc = 1; break; }
            seg_no++;
            /* Never overwrite an existing segment. This makes a failed
             * segmentation unable to destroy a previous result. */
            seg = fopen(path, "wbx");
            if (!seg || !write_header(seg, TCMF_MAGIC, fps)) { free(p.data); rc = 1; break; }
            if (seg_no > count_cap) {
                unsigned ncap = count_cap ? count_cap * 2 : 8;
                if (ncap > TCMX_MAX_SEGMENTS) ncap = TCMX_MAX_SEGMENTS;
                if (ncap <= count_cap) { free(p.data); rc = 1; break; }
                unsigned *tmp = (unsigned *)realloc(counts, (size_t)ncap * sizeof(*counts));
                if (!tmp) { free(p.data); rc = 1; break; }
                counts = tmp; count_cap = ncap;
            }
            counts[seg_no - 1] = 0;
        }
        if (!write_record(seg, &p)) rc = 1;
        counts[seg_no - 1]++; in_seg++; have_packet = 1;
        free(p.data);
        if (rc) break;
    }
    if (seg) fclose(seg);
    fclose(in);
    if (!rc && seg_no == 0) rc = 1;
    if (!rc && playlist) rc = write_playlist(playlist, fps, seg_no, counts);
    if (rc && playlist) unlink(playlist);
    free(counts); return rc;
}

struct mp4_blob {
    uint8_t *data;
    size_t size;
    size_t capacity;
};

static int mp4_reserve(struct mp4_blob *b, size_t extra)
{
    if (extra > SIZE_MAX - b->size) return 0;
    size_t need = b->size + extra;
    if (need <= b->capacity) return 1;
    size_t cap = b->capacity ? b->capacity : 1024;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p; b->capacity = cap;
    return 1;
}

static int mp4_bytes(struct mp4_blob *b, const void *p, size_t n)
{
    if (!mp4_reserve(b, n)) return 0;
    memcpy(b->data + b->size, p, n); b->size += n; return 1;
}

static int mp4_zero(struct mp4_blob *b, size_t n)
{
    if (!mp4_reserve(b, n)) return 0;
    memset(b->data + b->size, 0, n); b->size += n; return 1;
}

static int mp4_u8(struct mp4_blob *b, uint8_t v) { return mp4_bytes(b, &v, 1); }

static int mp4_be16(struct mp4_blob *b, uint16_t v)
{
    uint8_t p[2] = {(uint8_t)(v >> 8), (uint8_t)v}; return mp4_bytes(b, p, 2);
}

static int mp4_be32(struct mp4_blob *b, uint32_t v)
{
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    return mp4_bytes(b, p, 4);
}

static size_t mp4_box_start(struct mp4_blob *b, const char type[4])
{
    size_t pos = b->size;
    if (!mp4_be32(b, 0) || !mp4_bytes(b, type, 4)) return SIZE_MAX;
    return pos;
}

static int mp4_box_end(struct mp4_blob *b, size_t pos)
{
    if (pos == SIZE_MAX || b->size < pos || b->size - pos > UINT32_MAX) return 0;
    uint32_t n = (uint32_t)(b->size - pos);
    b->data[pos] = (uint8_t)(n >> 24); b->data[pos + 1] = (uint8_t)(n >> 16);
    b->data[pos + 2] = (uint8_t)(n >> 8); b->data[pos + 3] = (uint8_t)n;
    return 1;
}

static int mp4_fullbox(struct mp4_blob *b, uint8_t version, uint32_t flags)
{
    return mp4_u8(b, version) && mp4_u8(b, (uint8_t)(flags >> 16)) &&
           mp4_u8(b, (uint8_t)(flags >> 8)) && mp4_u8(b, (uint8_t)flags);
}

static int mp4_matrix(struct mp4_blob *b)
{
    static const uint32_t m[9] = {
        0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000
    };
    for (size_t i = 0; i < 9; ++i) if (!mp4_be32(b, m[i])) return 0;
    return 1;
}

static int mp4_add_mvhd(struct mp4_blob *b, uint32_t fps, uint32_t frames)
{
    size_t x = mp4_box_start(b, "mvhd");
    if (!mp4_fullbox(b, 0, 0) || !mp4_zero(b, 8) || !mp4_be32(b, fps) ||
        !mp4_be32(b, frames) || !mp4_be32(b, 0x00010000) || !mp4_be16(b, 0x0100) ||
        !mp4_zero(b, 10) || !mp4_matrix(b) || !mp4_zero(b, 24) ||
        !mp4_be32(b, 2) || !mp4_box_end(b, x)) return 0;
    return 1;
}

static int mp4_add_tkhd(struct mp4_blob *b, uint32_t frames, uint16_t w, uint16_t h)
{
    size_t x = mp4_box_start(b, "tkhd");
    if (!mp4_fullbox(b, 0, 7) || !mp4_zero(b, 8) || !mp4_be32(b, 1) ||
        !mp4_zero(b, 4) || !mp4_be32(b, frames) || !mp4_zero(b, 8) ||
        !mp4_be16(b, 0) || !mp4_be16(b, 0) || !mp4_be16(b, 0x0100) || !mp4_be16(b, 0) ||
        !mp4_matrix(b) || !mp4_be32(b, (uint32_t)w << 16) ||
        !mp4_be32(b, (uint32_t)h << 16) || !mp4_box_end(b, x)) return 0;
    return 1;
}

static int mp4_add_mdhd(struct mp4_blob *b, uint32_t fps, uint32_t frames)
{
    size_t x = mp4_box_start(b, "mdhd");
    return mp4_fullbox(b, 0, 0) && mp4_zero(b, 8) && mp4_be32(b, fps) &&
           mp4_be32(b, frames) && mp4_be16(b, 0x55c4) && mp4_be16(b, 0) &&
           mp4_box_end(b, x);
}

static int mp4_add_hdlr(struct mp4_blob *b)
{
    static const char name[] = "TCodec native video";
    size_t x = mp4_box_start(b, "hdlr");
    return mp4_fullbox(b, 0, 0) && mp4_zero(b, 4) && mp4_bytes(b, "vide", 4) &&
           mp4_zero(b, 12) && mp4_bytes(b, name, sizeof(name)) && mp4_box_end(b, x);
}

static int mp4_add_stsd(struct mp4_blob *b, uint16_t w, uint16_t h)
{
    size_t x = mp4_box_start(b, "stsd");
    size_t e;
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, 1)) return 0;
    e = mp4_box_start(b, "tcv1");
    if (!mp4_zero(b, 6) || !mp4_be16(b, 1) || !mp4_zero(b, 16) ||
        !mp4_be16(b, w) || !mp4_be16(b, h) || !mp4_be32(b, 0x00480000) ||
        !mp4_be32(b, 0x00480000) || !mp4_zero(b, 4) || !mp4_be16(b, 1) ||
        !mp4_zero(b, 32) || !mp4_be16(b, 0x0018) || !mp4_be16(b, 0xffff) ||
        !mp4_box_end(b, e) || !mp4_box_end(b, x)) return 0;
    return 1;
}

static int mp4_add_stbl(struct mp4_blob *b, uint16_t w, uint16_t h,
                        uint32_t fps, uint32_t frames, const struct packet *packets,
                        size_t *stco_payload)
{
    size_t x = mp4_box_start(b, "stbl");
    size_t q;
    if (!mp4_add_stsd(b, w, h)) return 0;
    q = mp4_box_start(b, "stts");
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, 1) || !mp4_be32(b, frames) ||
        !mp4_be32(b, 1) || !mp4_box_end(b, q)) return 0;
    q = mp4_box_start(b, "stsc");
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, 1) || !mp4_be32(b, 1) ||
        !mp4_be32(b, frames) || !mp4_be32(b, 1) || !mp4_box_end(b, q)) return 0;
    q = mp4_box_start(b, "stsz");
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, 0) || !mp4_be32(b, frames)) return 0;
    for (uint32_t i = 0; i < frames; ++i) if (!mp4_be32(b, packets[i].size)) return 0;
    if (!mp4_box_end(b, q)) return 0;
    q = mp4_box_start(b, "stco");
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, frames)) return 0;
    *stco_payload = b->size;
    for (uint32_t i = 0; i < frames; ++i) if (!mp4_be32(b, 0)) return 0;
    if (!mp4_box_end(b, q) || !mp4_box_end(b, x)) return 0;
    (void)fps;
    return 1;
}

static int mp4_build_moov(struct mp4_blob *b, uint16_t w, uint16_t h,
                           uint32_t fps, uint32_t frames, const struct packet *packets,
                           size_t *stco_payload)
{
    size_t moov = mp4_box_start(b, "moov");
    size_t trak, mdia, minf, dinf, dref;
    if (!mp4_add_mvhd(b, fps, frames)) return 0;
    trak = mp4_box_start(b, "trak");
    if (!mp4_add_tkhd(b, frames, w, h)) return 0;
    mdia = mp4_box_start(b, "mdia");
    if (!mp4_add_mdhd(b, fps, frames) || !mp4_add_hdlr(b)) return 0;
    minf = mp4_box_start(b, "minf");
    { size_t vmhd = mp4_box_start(b, "vmhd");
      if (!mp4_fullbox(b, 0, 1) || !mp4_zero(b, 8) || !mp4_box_end(b, vmhd)) return 0; }
    dinf = mp4_box_start(b, "dinf"); dref = mp4_box_start(b, "dref");
    { size_t url = mp4_box_start(b, "url ");
      if (!mp4_fullbox(b, 0, 1) || !mp4_box_end(b, url)) return 0; }
    if (!mp4_fullbox(b, 0, 0) || !mp4_be32(b, 1) || !mp4_box_end(b, dref) ||
        !mp4_box_end(b, dinf) || !mp4_add_stbl(b, w, h, fps, frames, packets, stco_payload) ||
        !mp4_box_end(b, minf) || !mp4_box_end(b, mdia) || !mp4_box_end(b, trak) ||
        !mp4_box_end(b, moov)) return 0;
    return 1;
}

static int native_mp4_mux(const char *in_path, const char *out_path, uint32_t fps)
{
    FILE *in = fopen(in_path, "rb");
    struct packet *packets = NULL; size_t count = 0, cap = 0;
    struct mp4_blob ftyp = {0}, moov = {0}; size_t stco = SIZE_MAX;
    uint16_t w = 0, h = 0; int rc = 1;
    char tmp_path[1024] = {0}; FILE *out = NULL;
    if (!in) return 1;
    for (;;) {
        struct packet p = {0}; int got = read_raw_packet(in, &p);
        if (got == 0) break;
        if (got < 0) goto done;
        if (!count) {
            w = (uint16_t)(p.data[4] | (p.data[5] << 8));
            h = (uint16_t)(p.data[6] | (p.data[7] << 8));
            if (!w || !h || (w & 1u) || (h & 1u) ||
                w > TC_MAX_WIDTH || h > TC_MAX_HEIGHT) {
                free(p.data); goto done;
            }
        }
        if (count == cap) { size_t ncap = cap ? cap * 2 : 16; struct packet *q = realloc(packets, ncap * sizeof(*q)); if (!q) { free(p.data); goto done; } packets = q; cap = ncap; }
        packets[count++] = p;
    }
    fclose(in); in = NULL;
    if (!count || !w || !h || count > UINT32_MAX) goto done;
    if (!mp4_be32(&ftyp, 24) || !mp4_bytes(&ftyp, "ftypisom", 8) ||
        !mp4_be32(&ftyp, 0x200) || !mp4_bytes(&ftyp, "isomiso6", 8) ||
        !mp4_build_moov(&moov, w, h, fps, (uint32_t)count, packets, &stco)) goto done;
    if (stco == SIZE_MAX || moov.size > UINT32_MAX || ftyp.size + moov.size + 8 > UINT32_MAX) goto done;
    uint32_t payload = (uint32_t)(ftyp.size + moov.size + 8);
    uint32_t off = payload;
    for (size_t i = 0; i < count; ++i) {
        size_t pos = stco + i * 4;
        moov.data[pos] = (uint8_t)(off >> 24); moov.data[pos+1] = (uint8_t)(off >> 16);
        moov.data[pos+2] = (uint8_t)(off >> 8); moov.data[pos+3] = (uint8_t)off;
        if (packets[i].size > UINT32_MAX - off) goto done;
        off += packets[i].size;
    }
    if (access(out_path, F_OK) == 0) goto done;
    out = open_temp_output(out_path, tmp_path, sizeof(tmp_path)); if (!out) goto done;
    if (!write_exact(out, ftyp.data, ftyp.size) || !write_exact(out, moov.data, moov.size)) goto done;
    { uint32_t mdat_size = 8;
      for (size_t i = 0; i < count; ++i) {
          if (packets[i].size > UINT32_MAX - mdat_size) goto done;
          mdat_size += packets[i].size;
      }
      uint8_t hdr[8] = {(uint8_t)(mdat_size >> 24), (uint8_t)(mdat_size >> 16),
                        (uint8_t)(mdat_size >> 8), (uint8_t)mdat_size, 'm','d','a','t'};
      if (!write_exact(out, hdr, sizeof(hdr))) goto done; }
    for (size_t i = 0; i < count; ++i) if (!write_exact(out, packets[i].data, packets[i].size)) goto done;
    if (fclose(out) != 0 || rename(tmp_path, out_path) != 0) { out = NULL; goto done; }
    out = NULL; rc = 0;
done:
    if (in) fclose(in); if (out) fclose(out); if (rc) unlink(tmp_path);
    for (size_t i = 0; i < count; ++i) free(packets[i].data);
    free(packets); free(ftyp.data); free(moov.data); return rc;
}

static uint32_t mp4_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int mp4_find_box(const uint8_t *data, size_t start, size_t end, const char type[4],
                        size_t *box_start, size_t *payload_start, size_t *box_end)
{
    while (start + 8 <= end) {
        uint32_t n = mp4_read_be32(data + start); size_t total = n;
        if (n == 0) total = end - start;
        else if (n == 1 || total < 8 || total > end - start) return 0;
        if (memcmp(data + start + 4, type, 4) == 0) {
            *box_start = start; *payload_start = start + 8; *box_end = start + total; return 1;
        }
        if (n != 0 && (memcmp(data + start + 4, "moov", 4) == 0 ||
                       memcmp(data + start + 4, "trak", 4) == 0 ||
                       memcmp(data + start + 4, "mdia", 4) == 0 ||
                       memcmp(data + start + 4, "minf", 4) == 0 ||
                       memcmp(data + start + 4, "dinf", 4) == 0 ||
                       memcmp(data + start + 4, "stbl", 4) == 0 ||
                       memcmp(data + start + 4, "stsd", 4) == 0)) {
            /* stsd is a full box: skip its version/flags and entry count
             * before scanning visual sample-entry boxes. */
            size_t child = start + 8;
            if (memcmp(data + start + 4, "stsd", 4) == 0) {
                if (child + 8 > start + total) return 0;
                child += 8;
            }
            if (mp4_find_box(data, child, start + total, type,
                             box_start, payload_start, box_end)) return 1;
        }
        start += total;
    }
    return 0;
}

static int native_mp4_demux(const char *in_path, const char *out_path)
{
    FILE *in = fopen(in_path, "rb"); long length; uint8_t *data = NULL; size_t n;
    size_t moov_s, moov_p, moov_e, stbl_s, stbl_p, stbl_e;
    size_t stsd_s, stsd_p, stsd_e, tcv_s, tcv_p, tcv_e;
    size_t stsz_s, stsz_p, stsz_e, stco_s, stco_p, stco_e, mdat_s, mdat_p, mdat_e;
    char tmp_path[1024] = {0}; FILE *out = NULL; int rc = 1;
    if (!in || fseek(in, 0, SEEK_END) != 0 || (length = ftell(in)) < 0 || (unsigned long)length > SIZE_MAX ||
        fseek(in, 0, SEEK_SET) != 0) goto done;
    n = (size_t)length; data = malloc(n); if (!data || fread(data, 1, n, in) != n) goto done;
    if (!mp4_find_box(data, 0, n, "moov", &moov_s, &moov_p, &moov_e) ||
        !mp4_find_box(data, moov_p, moov_e, "stbl", &stbl_s, &stbl_p, &stbl_e) ||
        !mp4_find_box(data, stbl_p, stbl_e, "stsd", &stsd_s, &stsd_p, &stsd_e) ||
        !mp4_find_box(data, stsd_s, stsd_e, "tcv1", &tcv_s, &tcv_p, &tcv_e) ||
        !mp4_find_box(data, stbl_p, stbl_e, "stsz", &stsz_s, &stsz_p, &stsz_e) ||
        !mp4_find_box(data, stbl_p, stbl_e, "stco", &stco_s, &stco_p, &stco_e) ||
        !mp4_find_box(data, 0, n, "mdat", &mdat_s, &mdat_p, &mdat_e)) goto done;
    (void)stbl_s; (void)tcv_s; (void)tcv_p; (void)tcv_e;
    (void)stsz_s; (void)stco_s; (void)mdat_s;
    if (stsz_p + 12 > stsz_e || stco_p + 8 > stco_e ||
        stsd_p + 8 > stsd_e || mp4_read_be32(data + stsd_p + 4) != 1) goto done;
    uint32_t sample_size = mp4_read_be32(data + stsz_p + 4);
    uint32_t count = mp4_read_be32(data + stsz_p + 8);
    uint32_t offset_count = mp4_read_be32(data + stco_p + 4);
    if (sample_size != 0 || offset_count != count || stsz_p + 12 + (size_t)count * 4 > stsz_e ||
        stco_p + 8 + (size_t)count * 4 > stco_e) goto done;
    if (access(out_path, F_OK) == 0) goto done;
    out = open_temp_output(out_path, tmp_path, sizeof(tmp_path)); if (!out) goto done;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = mp4_read_be32(data + stsz_p + 12 + (size_t)i * 4);
        uint32_t off = mp4_read_be32(data + stco_p + 8 + (size_t)i * 4);
        struct packet sample = {0};
        if (!size || off < mdat_p || (size_t)off + size > mdat_e ||
            !packet_header_valid(&(struct packet){.size = size, .data = data + off})) goto done;
        sample.size = size; sample.data = data + off;
        uint8_t len[4]; put_u32(len, size);
        if (!write_exact(out, len, 4) || !write_exact(out, sample.data, sample.size)) goto done;
    }
    if (fclose(out) != 0 || rename(tmp_path, out_path) != 0) { out = NULL; goto done; }
    out = NULL; rc = 0;
done:
    if (in) fclose(in); if (out) fclose(out); if (rc) unlink(tmp_path); free(data); return rc;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 2; i + 1 < argc; ++i) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *mode = argv[1];
    const char *in = arg_value(argc, argv, "-i");
    const char *out = arg_value(argc, argv, "-o");
    if (!in || !out) { usage(argv[0]); return 1; }
    if (strcmp(mode, "mux") == 0) {
        const char *fs = arg_value(argc, argv, "-f");
        unsigned fps = 30u;
        if (fs && !parse_uint(fs, 1, 1000, &fps)) {
            fprintf(stderr, "tcmux: invalid fps\n"); return 1;
        }
        return mux_file(in, out, fps);
    }
    if (strcmp(mode, "demux") == 0) return demux_file(in, out);
    if (strcmp(mode, "mp4mux") == 0) {
        const char *fs = arg_value(argc, argv, "-f");
        unsigned fps = 30u;
        if (fs && !parse_uint(fs, 1, 1000, &fps)) { fprintf(stderr, "tcmux: invalid fps\n"); return 1; }
        return native_mp4_mux(in, out, fps);
    }
    if (strcmp(mode, "mp4demux") == 0) return native_mp4_demux(in, out);
    if (strcmp(mode, "segment") == 0) {
        const char *ss = arg_value(argc, argv, "-s");
        const char *playlist = arg_value(argc, argv, "-p");
        unsigned frames = 0;
        if (!ss || !parse_uint(ss, 1, TCMX_MAX_SEGMENTS, &frames)) {
            fprintf(stderr, "tcmux: invalid segment size\n"); return 1;
        }
        return segment_file(in, out, frames, playlist);
    }
    usage(argv[0]); return 1;
}
