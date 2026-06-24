#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/*
 * s192cmd - Convert Motorola S19 files to FLEX binary (.CMD) format.
 *
 * FLEX binary record format:
 *   0: 0x02 record marker
 *   1: load address high byte
 *   2: load address low byte
 *   3: data length (1..255)
 *   4..n: data bytes
 *
 * Optional execute-address trailer:
 *   0: 0x16
 *   1: exec address high byte
 *   2: exec address low byte
 */

static void usage(void)
{
    fprintf(stderr, "s192cmd [-x execaddr] [s19file [output.cmd]]\n");
    fprintf(stderr, "  no file args : read S19 from stdin, write CMD to stdout\n");
    fprintf(stderr, "  one file arg : read S19 from file,  write CMD to stdout\n");
    fprintf(stderr, "  -x execaddr : override execution address (decimal or 0xHEX)\n");
    exit(1);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_byte(const char *p, uint8_t *out)
{
    int hi = hexval((unsigned char)p[0]);
    int lo = hexval((unsigned char)p[1]);
    if (hi < 0 || lo < 0)
        return -1;
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

static int parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!s[0] || (end && *end) || v > 0xFFFFUL)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

static int rstrip_line(char *line)
{
    size_t len = strlen(line);

    while (len > 0) {
        char c = line[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            line[--len] = '\0';
            continue;
        }
        break;
    }

    return (int)len;
}

static int emit_flex_record(FILE *out, uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;

    while (pos < len) {
        uint16_t chunk = (uint16_t)(len - pos);
        if (chunk > 255)
            chunk = 255;

        if (fputc(0x02, out) == EOF)
            return -1;
        if (fputc((addr >> 8) & 0xFF, out) == EOF)
            return -1;
        if (fputc(addr & 0xFF, out) == EOF)
            return -1;
        if (fputc(chunk, out) == EOF)
            return -1;

        for (uint16_t i = 0; i < chunk; i++) {
            if (fputc(data[pos + i], out) == EOF)
                return -1;
        }

        addr = (uint16_t)(addr + chunk);
        pos = (uint16_t)(pos + chunk);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int opt;
    int setexec = 0;
    uint16_t exec = 0;
    FILE *in = NULL;
    FILE *out = NULL;
    const char *in_name = "<stdin>";
    const char *out_name = "<stdout>";
    char line[2048];
    int lineno = 0;

    while ((opt = getopt(argc, argv, "x:")) != -1) {
        switch (opt) {
        case 'x':
            if (parse_u16(optarg, &exec) < 0) {
                fprintf(stderr, "s192cmd: invalid exec address '%s'.\n", optarg);
                return 1;
            }
            setexec = 1;
            break;
        default:
            usage();
        }
    }

    if (optind > argc || optind + 2 < argc)
        usage();

    if (optind < argc) {
        in_name = argv[optind];
        in = fopen(in_name, "r");
        if (in == NULL) {
            perror(in_name);
            return 1;
        }
    } else {
        in = stdin;
    }

    if (optind + 1 < argc) {
        out_name = argv[optind + 1];
        out = fopen(out_name, "wb");
        if (out == NULL) {
            perror(out_name);
            if (in != stdin)
                fclose(in);
            return 1;
        }
    } else {
        out = stdout;
    }

    while (fgets(line, sizeof(line), in) != NULL) {
        uint8_t  bytes[255];
        int      linelen;
        uint8_t  count;
        int      addr_len;
        uint16_t addr = 0;
        int      payload_len;
        uint8_t  sum;
        int      nbytes;
        char     rtype;

        lineno++;
        linelen = rstrip_line(line);

        if (linelen == 0)
            continue;

        if ((line[0] != 'S' && line[0] != 's') || linelen < 4) {
            fprintf(stderr, "s192cmd: line %d is not a valid S-record line.\n", lineno);
            goto fail;
        }

        rtype = line[1];
        if (rtype >= 'a' && rtype <= 'z')
            rtype = (char)(rtype - 'a' + 'A');

        switch (rtype) {
        case '0':
            addr_len = 2;
            break;
        case '1':
            addr_len = 2;
            break;
        case '5':
            addr_len = 2;
            break;
        case '9':
            addr_len = 2;
            break;
        default:
            fprintf(stderr, "s192cmd: line %d uses unsupported record type S%c (expect S19 input).\n", lineno, line[1]);
            goto fail;
        }

        if (parse_hex_byte(line + 2, &count) < 0) {
            fprintf(stderr, "s192cmd: line %d has invalid count field.\n", lineno);
            goto fail;
        }

        nbytes = (int)count;
        if (nbytes <= 0 || nbytes > 255) {
            fprintf(stderr, "s192cmd: line %d has invalid byte count.\n", lineno);
            goto fail;
        }
        if (4 + nbytes * 2 != linelen) {
            fprintf(stderr, "s192cmd: line %d has invalid length/count.\n", lineno);
            goto fail;
        }
        if (nbytes < addr_len + 1) {
            fprintf(stderr, "s192cmd: line %d count too small for record type.\n", lineno);
            goto fail;
        }

        sum = count;
        for (int i = 0; i < nbytes; i++) {
            if (parse_hex_byte(line + 4 + i * 2, &bytes[i]) < 0) {
                fprintf(stderr, "s192cmd: line %d has invalid hex data.\n", lineno);
                goto fail;
            }
            sum = (uint8_t)(sum + bytes[i]);
        }

        if (sum != 0xFF) {
            fprintf(stderr, "s192cmd: line %d checksum mismatch.\n", lineno);
            goto fail;
        }

        for (int i = 0; i < addr_len; i++) {
            addr = (addr << 8) | bytes[i];
        }

        payload_len = nbytes - addr_len - 1; /* Exclude checksum byte */

        if (rtype == '1') {
            if (payload_len > 0) {
                if (emit_flex_record(out, (uint16_t)addr, bytes + addr_len, (uint16_t)payload_len) < 0) {
                    perror("s192cmd: write error");
                    goto fail;
                }
            }
        } else if (!setexec && rtype == '9') {
            exec = (uint16_t)addr;
            setexec = 1;
        }
    }

    if (ferror(in)) {
        perror(in_name);
        goto fail;
    }

    if (setexec) {
        if (fputc(0x16, out) == EOF ||
            fputc((exec >> 8) & 0xFF, out) == EOF ||
            fputc(exec & 0xFF, out) == EOF) {
            perror("s192cmd: write error");
            goto fail;
        }
    }

    if (out != stdout) {
        if (fclose(out) == EOF) {
            perror(out_name);
            if (in != stdin)
                fclose(in);
            return 1;
        }
    } else if (fflush(out) == EOF) {
        perror(out_name);
        if (in != stdin)
            fclose(in);
        return 1;
    }

    if (in != stdin)
        fclose(in);
    return 0;

fail:
    if (out && out != stdout)
        fclose(out);
    if (in && in != stdin)
        fclose(in);
    return 1;
}
