#pragma warning(push, 0)
#pragma warning( disable : 4244 )
#pragma warning( disable : 4242 )
#pragma warning( disable : 4702 )

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>

#ifdef __cplusplus
}
#endif 

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include "misc.h"
#include "msg.h"

struct hls_args hls_args;

static void print_help(const char *filename)
{
    printf("Usage: %s url [options]\n\n"
           "--best    or -b ... Automaticly choose the best quality.\n"
           "--verbose or -v ... Verbose more information.\n"
           "--output  or -o ... Choose name of output file.\n"
           "--live    or -l ... Url is Live Streaming Url.\n"
           "--max    or -m  ... Maximum Video File Size to Download.\n"
           "--help    or -h ... Print help.\n"
           "--force   or -f ... Force overwriting the output file.\n"
           "--userAgent   or -u ...Set the user agent.\n"
           "--quiet   or -q ... Print less to the console.\n"
           "--dump-dec-cmd  ... Print the openssl decryption command.\n"
           "--dump-ts-urls  ... Print the links to the .ts files.\n", filename);
    exit(0);
}

int parse_argv(int argc, const char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            hls_args.loglevel++;
        } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            hls_args.loglevel--;
        } else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--best")) {
            hls_args.use_best = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help(argv[0]);
        } else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) {
            hls_args.force_overwrite = 1;
        } else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--userAgent")){
            hls_args.user_agent=std::string(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--live")) {
            hls_args.livestreaming = 1;
        } else if (!strcmp(argv[i], "--dump-ts-urls")) {
            hls_args.dump_ts_urls = 1;
        } else if (!strcmp(argv[i], "--dump-dec-cmd")) {
            hls_args.dump_dec_cmd = 1;
        } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--max")) {
            if ((i + 1) < argc && *argv[i + 1] != '-') {
                int tmp=0;
                if(sscanf(argv[i+1],"%d",&tmp)==1){
                    hls_args.max_size=tmp;
                }
                i++;
            }
        }else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if ((i + 1) < argc && *argv[i + 1] != '-') {
                strncpy(hls_args.filename, argv[i + 1], MAX_FILENAME_LEN);
                hls_args.custom_filename = 1;
                i++;
            }
        }
        else {
            if (strlen(argv[i]) < MAX_URL_LEN) {
                strcpy(hls_args.url, argv[i]);
                hls_args.url_passed++;
            } else {
                MSG_ERROR("URL too long.");
                exit(1);
            }
        }
    }

    if (hls_args.url_passed == 1) {
        return 0;
    }

    print_help(argv[0]);
    return 1;
}

int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    struct ByteBuffer *bb = (struct ByteBuffer *)opaque;
    int size = buf_size;

    if (bb->len - bb->pos < buf_size) {
        size = bb->len - bb->pos;
    }

    if (size > 0) {
        memcpy(buf, bb->data + bb->pos, size);
        bb->pos += size;
    }
    return size;
}

int64_t seek(void *opaque, int64_t offset, int whence)
{
    struct ByteBuffer *bb = (struct ByteBuffer *)opaque;

    switch (whence) {
        case SEEK_SET:
            bb->pos = (int)offset;
            break;
        case SEEK_CUR:
            bb->pos += offset;
            break;
        case SEEK_END:
            bb->pos = (int)(bb->len - offset);
            break;
        case AVSEEK_SIZE:
            return bb->len;
            break;
    }
    return bb->pos;
}

int bytes_remaining(uint8_t *pos, uint8_t *end)
{
    return (int)(end - pos);
}

int str_to_bin(uint8_t *data, char *hexstring, int len)
{
    char *pos = hexstring;

    for (int count = 0; count < len; count++) {
        char buf[3] = {pos[0], pos[1], 0};
        data[count] = strtol(buf, NULL, 16);
        pos += 2;
    }
    return 0;
}
