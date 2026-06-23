/**
 * flexfs - FLEX disk image manipulation tool
 *
 * Purpose:
 *   flexfs provides list, get, put, and delete operations on FLEX disk images.
 *   Supports sector size detection (128 or 256 bytes), directory scanning, and
 *   basic disk validation via block chain checking.
 *
 * Directory Alignment (Critical):
 *   FLEX directory sectors (T0,S5 onwards) have a 16-byte header followed by
 *   24-byte directory entries:
 *   - Entry 0: bytes 16-39
 *   - Entry 1: bytes 40-63
 *   - Entry 2: bytes 64-87
 *   Entry offset: 16 + (i * 24) where i is the 0-based slot index.
 *   
 *   This aligns with flexadd.c. Directory traversal (dir_begin, dir_get, dir_next)
 *   now correctly starts iteration at slot 0 (not slot 1).
 *
 * Sector Layout (Data Sectors):
 *   - Bytes 0-1: Next track/sector link (0,0 for chain end)
 *   - Bytes 2-3: Logical record number (1-based counter matching sector position)
 *   - Bytes 4-255: Data payload (252 bytes)
 *
 * Version: Aligned with flexadd 1.1.2 directory structure
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>
#include "flexfs.h"

#define VERSION "1.1.2"

// Sector size constants
#define SECTOR_SIZE_128     128
#define SECTOR_SIZE_256     256
#define DEFAULT_SECTOR_SIZE 256

// Global sector size variable
static int sector_size = DEFAULT_SECTOR_SIZE;

/* FLEX stores text files in a slightly weird 'space compressed' format. This
   is the default automatic behaviour of FLEX and done by the OS itself so
   generally any 'text' file is in this format.
   
   0x00 is padding (usually to the end of sector as FLEX lacks byte level
   file sizes). It's not an EOF marker so you can APPEND files
   0x18 works like 0xDD (no idea why!)
   0x0D is a newline - we turn it into \n for Linux
   0x09 is a space compression marker and followed by a byte showing the
   number of spaces to replace it with (2-127)
   
   We blindly obey nonsense expansion sizes, it's better to have 250 spaces
   than an error code usually */
   
static uint8_t decompstate;

static void reset_decompress(void)
{
    decompstate = 0;
}

static void decompbyte(uint8_t c, FILE *fp)
{
    switch(decompstate) {
    case 0:
        if (c == 0)
            return;
        if (c == 0x18)
            return;
        if (c == 0x09) {
            decompstate = 1;
            return;
        }
        if (c == 0x0D)
            c = '\n';
        fputc(c, fp);
        break;
    case 1:
        while(c--)
            fputc(' ', fp);
        decompstate = 0;
        break;
    }
}

static void decompress(uint8_t *buf, int len, FILE *fp)
{
    while(len--)
        decompbyte(*buf++, fp);
}

/* Low level disk I/O */
static struct Xsir sir;
static int disk_fd;

static void sir_setsecfree(uint16_t secs)
{
    sir.secfreel = secs;
    sir.secfreeh = secs >> 8;
}

static off_t disk_offset(int track, int sec)
{
    off_t pos = track * sir.endsector;
    if (track == 0 && sec < 2)
        sec++;
    pos += sec - 1;
    pos *= sector_size;
    return pos;
}

static void disk_read(int track, int sec, uint8_t *buf)
{
    off_t pos = disk_offset(track, sec);
    int l;
    if (lseek(disk_fd, pos, SEEK_SET) < 0) {
        perror("lseek");
        exit(1);
    }
    if ((l = read(disk_fd, buf, sector_size)) != sector_size) {
        if (l < 0)
            perror("read");
        else
            fprintf(stderr, "read: short read (%d,%d)->%ld.\n", track, sec, pos);
        exit(1);
    }
}

static void disk_write(int track, int sec, uint8_t *buf)
{
    off_t pos = disk_offset(track, sec);
    int l;
    if (lseek(disk_fd, pos, SEEK_SET) < 0) {
        perror("lseek");
        exit(1);
    }
    if ((l = write(disk_fd, buf, sector_size)) != sector_size) {
        if (l < 0)
            perror("write");
        else
            fprintf(stderr, "write: short write.\n");
        exit(1);
    }
}

static int disk_read_next(uint8_t *buf)
{
    if (buf[0] ==0 && buf[1] == 0)
        return 0;
    disk_read(buf[0], buf[1], buf);
    return 1;
}

static uint8_t *workbuf = NULL;
static uint8_t *dirbuf = NULL;
static uint8_t dirtrk;
static uint8_t dirsec;
static int dirpt;
static uint16_t *flex_map;

static void allocate_buffers(void)
{
    if (workbuf) free(workbuf);
    if (dirbuf) free(dirbuf);
    
    workbuf = calloc(1, sector_size);
    dirbuf = calloc(1, sector_size);
    
    if (!workbuf || !dirbuf) {
        fprintf(stderr, "Out of memory allocating sector buffers.\n");
        exit(1);
    }
}

static int dir_entries_per_sector = 10; // Will be calculated based on sector size

/**
 * @brief Initialize directory scanning.
 * 
 * Sets up directory iterator to start at T0,S5 (first directory sector),
 * and initializes dirpt to 0 (first slot in sector, NOT slot 1).
 * Calculates dir_entries_per_sector based on current sector size.
 * 
 * Directory entries are located at: offset 16 + (dirpt * 24)
 * where dirpt is now 0-based (was 1-based in older versions).
 */
static void dir_begin(void)
{
    dir_entries_per_sector = (sector_size - 16) / 24;
    disk_read(0, 5, dirbuf);
    dirtrk = 0;
    dirsec = 5;
    dirpt = 0;  // Start at slot 0 (byte 16), not slot 1 (byte 40)
}

/**
 * @brief Get current directory entry pointer.
 * 
 * Returns pointer to DIR_struct at current iterator position.
 * Offset calculation: 16 + (dirpt * 24) ensures first entry at byte 16,
 * with 24-byte intervals for subsequent entries.
 */
static struct dir *dir_get(void)
{
    return (struct dir *)(dirbuf + 24 * dirpt + 16);  // Start at byte 16 for dirpt=0
}

/**
 * @brief Advance to next directory entry.
 * 
 * Increments dirpt and handles wrap-around to next directory sector.
 * When dirpt >= entries_per_sector, resets to 0 and moves to next sector.
 * Stops iteration when encountering sector 0 (wrap-around guard).
 * 
 * @return 1 if successful (more entries available), 0 if end of directory
 */
static int dir_next(void)
{
    dirpt++;
    if (dirpt >= dir_entries_per_sector) {  // Move to next sector when full
        dirpt = 0;  // Reset to first entry (slot 0) in next sector
        dirtrk = dirbuf[0];
        dirsec = dirbuf[1];
        return disk_read_next(dirbuf);
    } else
        return 1;
}

static void dir_write(void)
{
    disk_write(dirtrk, dirsec, dirbuf);
}

static int dir_match(const char *name, const char *ext)
{
    struct dir *d = dir_get();
    if (strncmp(name, d->name, 8) == 0 && strncmp(ext, d->ext, 3) == 0)
        return 1;
    return 0;
}

static struct dir *dir_find(const char *name, const char *ext)
{
    dir_begin();
    do {
        if (dir_match(name, ext))
            return dir_get();
    } while(dir_next());
    return NULL;
}

/**
 * @brief Find first free directory entry slot.
 * 
 * Scans all directory entries looking for an unused slot marked by either:
 * - First byte == 0x00 (empty, never used)
 * - First byte with 0x80 bit set (0x80-0xFF range includes old high-bit markers
 *   and the standard 0xFF deleted marker from flexadd)
 * 
 * Uses dir_begin() to start at T0,S5 and dir_next() to iterate through all
 * entries across all directory sectors.
 * 
 * @return Pointer to first free DIR_struct entry, or NULL if directory is full
 */
static struct dir *dir_findfree(void)
{
    dir_begin();
    do {
        struct dir *d = dir_get();
        // Check for empty (0x00) or any deleted marker (0x80-0xFF range)
        // This includes both old high-bit markers and standard 0xFF markers
        if (d->name[0] == 0 || d->name[0] & 0x80)
            return d;
    } while(dir_next());
    return NULL;
}

static void timestamp(struct dir *d)
{
    long t = time(NULL);
    struct tm *tm = localtime(&t);
    d->day = tm->tm_mday;
    d->month = tm->tm_mon + 1;
    d->year = tm->tm_year % 100;
}

static int read_sir(void)
{
    off_t sir_offset = (sector_size == SECTOR_SIZE_128) ? (2 * sector_size + 16) : (512 + 16);
    if (lseek(disk_fd, sir_offset, SEEK_SET) < 0)
        return -1;
    if (read(disk_fd, &sir, sizeof(struct Xsir)) != sizeof(struct Xsir))
        return -1;
    return 0;
}

static void write_sir(void)
{
    off_t sir_offset = (sector_size == SECTOR_SIZE_128) ? (2 * sector_size + 16) : (512 + 16);
    if (lseek(disk_fd, sir_offset, SEEK_SET) < 0 || write(disk_fd, &sir, sizeof(struct Xsir)) != sizeof(struct Xsir)) {
        perror("write sir");
        exit(1);
    }
}

static int flex_mount(void)
{
    allocate_buffers();
    if (read_sir() < 0)
        return -1;
//    if (sir.month > 12 || sir.day > 31 || sir.day == 0)
//        return -1;
    if (sir.endtrack < 34 || sir.endsector < 9)
        return -1;
    printf("Mounting volume %-11.11s serial %d  %02d/%02d/%02d\n",
        sir.label, (sir.volh << 8) | sir.voll, 
        sir.day, sir.month, sir.year);
    printf("Disk geometry is %d tracks, %d sectors per track, %d bytes/sector.\n",
        sir.endtrack + 1, sir.endsector, sector_size);
    return 0;
}

static int mark_block_chain(const char *name, uint16_t code, uint8_t track, uint8_t sec, uint8_t etrack, uint8_t esec)
{
    int count = 0;
    int pos;
    while(track || sec) {
        if (sec == 0 || sec > sir.endsector || track == 0 || track > sir.endtrack) {
            fprintf(stderr, "%s: corrupt sector chain reference (%d,%d)\n",
                name, track, sec);
            break;
        }
        disk_read(track, sec, workbuf);
        pos = track * sir.endsector + (sec - 1);
        switch (flex_map[pos]) {
            case 0xFFFF:
                flex_map[pos] = code;
                break;
            case 0xFFFE:
                fprintf(stderr, "%s: block (%d,%d) is on free chain.\n", name, track, sec);
                break;
            case 0x0001:
                fprintf(stderr, "%s: block (%d,%d) is in another file.\n", name, track, sec);
                break;
            /* TODO: relace 0x0001 etc with the directory count from start of
               dir so we can report which file */
            default:
                fprintf(stderr, "%s: bad value %04X in map.\n", name, flex_map[pos]);
        }
        count++;
        if (*workbuf == 0 && workbuf[1] == 0)
            break;
        track = *workbuf;
        sec = workbuf[1];
    }
    if (track != etrack || sec != esec)
        fprintf(stderr, "%s: end of chain is (%d,%d) but should be (%d,%d).\n",
            name, track, sec, etrack, esec);
    return count;
}

static void mark_blocks_used(struct dir *d)
{
    char buf[16];
    int count;
    snprintf(buf, 16, "%.8s.%.3s", d->name, d->ext);
    count = mark_block_chain(buf, 0x0001, d->strack, d->ssec, d->etrack, d->esec);
    if (count != ((d->sech << 8) | d->secl))
        fprintf(stderr, "%s: block chain length does not match sectors (%d v %d).\n",
            buf, (d->sech << 8) | d->secl, count);
}

static void flex_buildmap(void)
{
    struct dir *d;
    int count;
    if (flex_map)
        free(flex_map);
    flex_map = calloc((sir.endtrack + 1) * sir.endsector, sizeof(uint16_t));
    if (flex_map == NULL) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }
    memset(flex_map, 0xFF, (sir.endtrack + 1) * sir.endsector * sizeof(uint16_t));

    dir_begin();
    do {
        d = dir_get();
        if (d->name[0] && !(d->name[0] & 0x80))
            mark_blocks_used(d);
    } while(dir_next());
    count = mark_block_chain("free", 0xFFFE, sir.ffreetrack, sir.ffreesec, sir.lfreetrack, sir.lfreesec);
    if (count != sir_secfree()) {
        fprintf(stderr, "%d blocks in the free chain, space free claims to be %d blocks.\n",
            count, sir_secfree());
    }
}

static int flex_unlink(const char *name, const char *ext)
{
    struct dir *d = dir_find(name, ext);
    uint16_t freesec;
    if (d == NULL)
        return -1;
    d->name[0] |= 0x80;
    if (d->etrack || d->esec) {
        disk_read(d->etrack, d->esec, workbuf);
        /* Hook the existing free list onto the end of the file chain */
        *workbuf = sir.ffreetrack;
        workbuf[1] = sir.ffreesec;
        /* Update the free sector count
         */
        freesec = sir_secfree();
        freesec += dir_sectors(d);
        sir_setsecfree(freesec);
        disk_write(d->etrack, d->esec, workbuf);
        /* Now add it to the SIR */
        sir.ffreetrack = d->strack;
        sir.ffreesec = d->ssec;
        write_sir();
    }
    d->etrack = d->esec = d->ssec = d->strack = 0;
    dir_write();
    return 0;
}

static struct dir *flex_create(const char *name, const char *ext)
{
    struct dir *d = dir_find(name, ext);
    if (d != NULL)
        return NULL;		/* Exists */
    d = dir_findfree();
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, 8);
    strncpy(d->ext, ext, 3);
    timestamp(d);
    d->strack = 0;
    d->ssec = 0;
    d->etrack = 0;
    d->esec = 0;
    d->rndf = 0;
    dir_write();
    return d;
}

/* Add a sector to a file */
static int flex_append(struct dir *d, const char *buf)
{
    uint8_t trk,sec;
    int data_space = sector_size - 4;
    
    /* Space ? */
    if (sir_secfree() == 0)
        return -1;
    /* If we have sectors already then change the end pointer of the last one */
    if (d->esec || d->etrack) {
        disk_read(d->etrack, d->esec, workbuf);
        workbuf[0] = sir.ffreetrack;
        workbuf[1] = sir.ffreesec;
        disk_write(d->etrack, d->esec, workbuf);
    }
    /* Update the sir, dir and new sector */
    trk = sir.ffreetrack;
    sec = sir.ffreesec;
    disk_read(trk, sec, workbuf);
    sir.ffreetrack = *workbuf;
    sir.ffreesec = workbuf[1];
    /* First block - update the header */
    if (d->etrack == 0 && d->esec == 0) {
        d->strack = trk;
        d->ssec = sec;
    }
    d->etrack = trk;
    d->esec = sec;
    /* Adjust sec count in directory */
    d->secl++;
    if (d->secl == 0)
        d->sech++;
    *workbuf = 0;
    workbuf[1] = 0;
    /* Sectors have logical record numbers 1+ */
    workbuf[2] = d->sech;
    workbuf[3] = d->secl;
    /* Add the data */
    memcpy(workbuf + 4, buf, data_space);
    disk_write(trk, sec, workbuf);
    /* Adjust sir.secfree */
    sir_setsecfree(sir_secfree() - 1);
    dir_write();
    write_sir();
    return 0;
}

static int flex_addfile(const char *name, const char *ext, FILE *inf, int translate)
{
    char *buf;
    int data_space = sector_size - 4;
    int l;
    struct dir *d;
    
    buf = malloc(data_space);
    if (!buf) {
        fprintf(stderr, "Out of memory allocating file buffer.\n");
        return -1;
    }
    
    d = flex_create(name, ext);
    if (d == NULL) {
        free(buf);
        return -1;
    }
    
    while((l = fread(buf, 1, data_space, inf)) > 0) {
        if (translate) {
            int out = 0;
            for (int i = 0; i < l; i++) {
                uint8_t c = (uint8_t)buf[i];

                if (c == '\r')
                    continue;
                if (c == '\n')
                    c = 0x0D;

                if (c == ' ') {
                    int run = 1;
                    while ((i + 1) < l && buf[i + 1] == ' ' && run < 127) {
                        run++;
                        i++;
                    }
                    if (run >= 2) {
                        buf[out++] = 0x09;
                        buf[out++] = (char)run;
                    } else {
                        buf[out++] = ' ';
                    }
                } else {
                    buf[out++] = (char)c;
                }
            }
            l = out;
        }
        /* Flex zeroes unused space and the Flex file formats need that */
        if (l != data_space)
            memset(buf + l, 0, data_space - l);
        flex_append(d, buf);
    }
    if (l == -1) {
        perror("read");
        free(buf);
        exit(1);
    }
    free(buf);
    return 0;
}

static int flex_dump(struct dir *d, FILE *outf, int ascii, int translate)
{
    int count = 0;
    int data_space = sector_size - 4;
    int pending_space_count = 0;
    reset_decompress();
    /* Dump each sector in turn */
    if (d->strack == 0 && d->ssec == 0)
        return 0;
    disk_read(d->strack, d->ssec, workbuf);
    do {
        count++;
        if (((workbuf[2] << 8) | workbuf[3]) != count)
            fprintf(stderr, "%s.%s: sector %d has a sector count of %d.\n",
                d->name, d->ext, count, (workbuf[2] << 8) | workbuf[3]);
        if (ascii)
            decompress(workbuf + 4, data_space, outf);
        else if (translate) {
            for (int i = 0; i < data_space; i++) {
                uint8_t c = workbuf[4 + i];

                if (pending_space_count) {
                    int count_spaces = c;
                    while (count_spaces-- > 0) {
                        if (fputc(' ', outf) == EOF) {
                            fprintf(stderr, "%s.%s: write error.\n", d->name, d->ext);
                            exit(1);
                        }
                    }
                    pending_space_count = 0;
                    continue;
                }

                if (c == 0x09) {
                    pending_space_count = 1;
                    continue;
                }
                if (c == 0x0D)
                    c = 0x0A;
                if (fputc(c, outf) == EOF) {
                    fprintf(stderr, "%s.%s: write error.\n", d->name, d->ext);
                    exit(1);
                }
            }
        } else if (fwrite(workbuf + 4, data_space, 1, outf) != 1) {
            fprintf(stderr, "%s.%s: write error.\n", d->name, d->ext);
            exit(1);
        }
    } while(disk_read_next(workbuf));
    return 0;
}

static int flex_get(const char *name, const char *ext, FILE *outf, int ascii, int translate)
{
    struct dir *d = dir_find(name, ext);
    if (d == NULL) {
        fprintf(stderr, "File not found.\n");
        return -1;
    }
    return flex_dump(d, outf, ascii, translate);
}

static void flex_get_all(int translate)
{
    FILE *outf;
    char buf[16];
    dir_begin();
    int txt;
    do {
        struct dir *d = dir_get();
        if (d->name[0] == 0 || d->name[0] & 0x80)
            continue;
        snprintf(buf, 16, "%.8s.%s.3s", d->name, d->ext);
        outf = fopen(buf, "w");
        if (outf == NULL) {
            perror(buf);
            continue;
        }
        txt = !memcmp(d->ext, "TXT", 3);
        flex_dump(d, outf, txt, translate);
        fclose(outf);
    } while(dir_next());
}

static void flex_ls(void)
{
    struct dir *d;
    printf("Volume: %-11.11s   (%02d/%02d/%02d)\n",
        sir.label, sir.day, sir.month, sir.year);
    printf("Media format %d tracks, %d sectors per track.\n",
        sir.endtrack+1, sir.endsector);
    dir_begin();
    do {
        d = dir_get();
        if (*d->name != 0 && (*d->name & 0x80) == 0) {
            printf("  %-8.8s.%-3.3s     %5d %02d/%02d/%02d\n",
                d->name, d->ext, dir_sectors(d),
                d->day, d->month, d->year);
        }
    } while(dir_next());
    printf("%d sectors free.\n", sir_secfree());
}

static void flex_showmap(void)
{
    int t, s;
    uint16_t *p;

    flex_buildmap();
    
    p = flex_map;

    for (t = 0; t <= sir.endtrack; t++) {
        for (s = 0; s < sir.endsector - 1; s++) {
            switch(*p++) {
                case 0xFFFF:
                    putchar('.');
                    break;
                case 0xFFFE:
                    putchar('-');
                    break;
                case 0x0001:
                    putchar('F');
                    break;
                default:
                    putchar('?');
                    break;
            }
        }
        putchar('\n');
    }
}

static void usage(void)
{
    fprintf(stderr, "flexfs version %s\n", VERSION);
    fprintf(stderr, "-a: do space compressed to ASCII conversion.\n");
    fprintf(stderr, "-t: text translation (get: CR->LF + 0x09 space expansion, put: LF->CR + 0x09 space compression).\n");
    fprintf(stderr, "-y: auto-delete existing target before put.\n");
    fprintf(stderr, "-d disk.dsk file.ext            : delete a file.\n");
    fprintf(stderr, "-g disk.dsk file.ext linuxfile  : get a file.\n");
    fprintf(stderr, "-g -A disk.dsk                  : extract all of the files.\n");
    fprintf(stderr, "-l disk.dsk                     : list contents of disk.\n");
    fprintf(stderr, "-m disk.dsk                     : check disk and show map.\n");
    fprintf(stderr, "-p disk.dsk linuxfile file.ext  : put a file.\n");
    fprintf(stderr, "-z <sector_size>                : sector size in bytes (128 or 256, defaults to 256).\n");
    exit(1);
}

enum command {
    LIST,
    GET,
    PUT,
    DELETE,
    MAP
};

int main(int argc, char *argv[])
{
    int opt;
    int all = 0;
    int ascii = 0;
    int translate = 0;
    int auto_yes = 0;
    enum command cmd = LIST;
    char *ext;
    char *name;

    assert(sizeof(struct dir) == 24);
    
    while((opt = getopt(argc, argv, "lgmpdaAtyz:")) != -1) {
        switch(opt) {
        case 'l':
            cmd = LIST;
            break;
        case 'g':
            cmd = GET;
            break;
        case 'p':
            cmd = PUT;
            break;
        case 'd':
            cmd = DELETE;
            break;
        case 'm':
            cmd = MAP;
            break;
        case 'a':
            ascii = 1;
            break;
        case 't':
            translate = 1;
            break;
        case 'y':
            auto_yes = 1;
            break;
        case 'A':
            all = 1;
            break;
        case 'z':
            {
                int temp_sector_size = atoi(optarg);
                if (temp_sector_size != SECTOR_SIZE_128 && temp_sector_size != SECTOR_SIZE_256) {
                    fprintf(stderr, "Error: Sector size (-z) must be either 128 or 256 bytes.\n");
                    exit(1);
                }
                sector_size = temp_sector_size;
            }
            break;
        default:
            usage();
        }
    }
    if (all && cmd != GET) {
        fprintf(stderr, "flexfs: -A only supported with -g.\n");
        exit(1);
    }
    if (cmd == LIST || cmd == MAP || all == 1 ) {
        if (optind + 1 != argc)
            usage();
    } else {
        if (cmd == DELETE && optind + 2 != argc)
            usage();
        else if (optind + 3 != argc)
            usage();
        if (cmd == PUT)
            name = argv[optind + 2];
        else
            name = argv[optind + 1];
        ext = strchr(name, '.');
        if (ext)
            *ext++ = 0;
        else
            ext = "";
    }

    disk_fd = open(argv[optind], O_RDWR);
    if (disk_fd == -1) {
        perror(argv[optind]);
        exit(1);
    }
    if (flex_mount() < 0) {
        fprintf(stderr, "%s: not a FLEX volume.\n", argv[optind]);
        exit(1);
    }
    switch(cmd) {
        case LIST:
            flex_ls();
            break;
        case GET:
            if (all)
                flex_get_all(translate);
            else {
                FILE *fp = fopen(argv[optind + 2], "wb");
                if (fp == NULL) {
                    perror(argv[optind + 2]);
                    exit(1);
                }
                flex_get(name, ext, fp, ascii, translate);
                if (fclose(fp) < 0) {
                    perror(argv[optind + 2]);
                    exit(1);
                }
            }
            break;
        case PUT:
            {
                FILE *fp = fopen(argv[optind + 1], "rb");
                if (fp == NULL) {
                    perror(argv[optind + 1]);
                    exit(1);
                }
                if (auto_yes) {
                    /* Ignore not-found: -y means delete if present. */
                    (void)flex_unlink(name, ext);
                }
                if (flex_addfile(name, ext, fp, translate) < 0) {
                    fprintf(stderr, "put failed: destination %s.%s already exists (use -y to replace) or no free directory slot.\n", name, ext);
                    fclose(fp);
                    exit(1);
                }
                if (fclose(fp) < 0) {
                    perror(argv[optind + 1]);
                    exit(1);
                }
            }
            break;
        case DELETE:
            flex_unlink(name, ext);
            break;
        case MAP:
            flex_showmap();
    }
    
    // Cleanup allocated buffers
    if (workbuf) free(workbuf);
    if (dirbuf) free(dirbuf);
    if (flex_map) free(flex_map);
    
    return 0;
}
