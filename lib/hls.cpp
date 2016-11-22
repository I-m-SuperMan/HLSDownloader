#pragma warning( disable : 4244 )
#pragma warning( disable : 4242 )
#pragma warning( disable : 4996 )
#pragma warning( disable : 4706 )
#pragma warning( disable : 4505 )
#ifdef __cplusplus 
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
}
#endif 

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "curls.h"
#include "hls.h"
#include "msg.h"
#include "misc.h"
#include "aes.h"
#include <io.h>

#include <windows.h>
#include <chrono>
#include <thread>


#include <memory>

#include <vector>

#include "MultipleThreadQueue.h"

/*
 * The memmem() function finds the start of the first occurrence of the
 * substring 'needle' of length 'nlen' in the memory area 'haystack' of
 * length 'hlen'.
 *
 * The return value is a pointer to the beginning of the sub-string, or
 * NULL if the substring is not found.
 */
uint8_t *memmem( const uint8_t *haystack, size_t hlen,  const uint8_t *needle, size_t nlen)
{
    int needle_first;
    const uint8_t *p = haystack;
    size_t plen = hlen;

    if (!nlen)
        return NULL;

    needle_first = *(unsigned char *)needle;

    while (plen >= nlen && (p = (const uint8_t *)memchr(p, needle_first, plen - nlen + 1)))
    {
        if (!memcmp(p, needle, nlen))
            return (uint8_t *)p;

        p++;
        plen = hlen - (p - haystack);
    }

    return NULL;
}


int get_playlist_type(char *source)
{
    if (strncmp("#EXTM3U", source, 7) != 0) {
        MSG_WARNING("Not a valid M3U8 file. Exiting.\n");
        return -1;
    }

    if (strstr(source, "#EXT-X-STREAM-INF")) {
        return 0;
    }

    return 1;
}

static int extend_url(char **url, const char *baseurl)
{
    size_t max_length = strlen(*url) + strlen(baseurl) + 10;

    if (!strncmp(*url, "http://", 7) || !strncmp(*url, "https://", 8)) {
        return 0;
    }

    else if (**url == '/') {
        char *domain = (char *)malloc(max_length);
        strcpy(domain, baseurl);

        if (!sscanf(baseurl, "http://%[^/]", domain)) {
            sscanf(baseurl, "https://%[^/]", domain);
        }

        char *buffer = (char *)malloc(max_length);
        snprintf(buffer, max_length, "%s%s", domain, *url);
        *url = (char *)realloc(*url, strlen(buffer) + 1);
        strcpy(*url, buffer);
        free(buffer);
        free(domain);
        return 0;
    }
    
    else {
        // URLs can have '?'. To make /../ work, remove it.
        char *find_questionmark = (char *)strchr(baseurl, '?');
        if (find_questionmark) {
            *find_questionmark = '\0';
        }

        char *buffer = (char *)malloc(max_length);
        snprintf(buffer, max_length, "%s/../%s", baseurl, *url);
        *url = (char *)realloc(*url, strlen(buffer) + 1);
        strcpy(*url, buffer);
        free(buffer);
        return 0;
    }
}

static int parse_playlist_tag(struct hls_media_playlist *me, char *tag,std::string user_agent)
{
    int enc_type;

    if (!strncmp(tag, "#EXT-X-KEY:METHOD=AES-128", 25)) {
        enc_type = ENC_AES128;
    } else if (!strncmp(tag, "#EXT-X-KEY:METHOD=SAMPLE-AES", 28)) {
        enc_type = ENC_AES_SAMPLE;
    } else  {
        return 1;
    }

    me->encryption = true;
    me->encryptiontype = enc_type;
    me->enc_aes.iv_is_static = false;
        
    char *link_to_key = (char *)malloc(strlen(tag) + strlen(me->url) + 10);
    char iv_str[STRLEN_BTS(KEYLEN)];
        
    if (sscanf(tag, "#EXT-X-KEY:METHOD=AES-128,URI=\"%[^\"]\",IV=0x%s", link_to_key, iv_str) == 2 ||
        sscanf(tag, "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"%[^\"]\",IV=0x%s", link_to_key, iv_str) == 2)
    {
        uint8_t *iv_bin = (uint8_t *)malloc(KEYLEN);
        str_to_bin(iv_bin, iv_str, KEYLEN);
        memcpy(me->enc_aes.iv_value, iv_bin, KEYLEN);
        me->enc_aes.iv_is_static = true;
        free(iv_bin);
    }
        
    extend_url(&link_to_key, me->url);

    uint8_t *decrypt;
    if (get_data_from_url(link_to_key, NULL, &decrypt, BINKEY,user_agent) == 0) {
        MSG_ERROR("Getting key-file failed.\n");
        free(link_to_key);
        return 1;
    }

    memcpy(me->enc_aes.key_value, decrypt, KEYLEN);
    free(link_to_key);
    free(decrypt);
    return 0;
}


char * next_line(char * src){
    char * next = strchr(src, '\n');
    if(next != NULL)
        next++;
    return next;
}

static int get_link_count(char *src)
{
    int linkcount = 0;

    while (src != NULL) {
        if (*src == '#'||*src=='\n') {
            src = next_line(src);
            continue;
        }
        if (*src == '\0') {
            break;
        }
        linkcount++;
        src = next_line(src);
    }

    return linkcount;
}

static int media_playlist_get_media_sequence(char *source)
{
    int j = 0;
    char *p_media_sequence = strstr(source, "#EXT-X-MEDIA-SEQUENCE:");

    if (p_media_sequence) {
        if (sscanf(p_media_sequence, "#EXT-X-MEDIA-SEQUENCE:%d", &j) != 1) {
            MSG_ERROR("Could not read EXT-X-MEDIA-SEQUENCE\n");
            return 0;
        }
    }
    return j;
}

static int media_playlist_get_media_target_duration(char *source)
{
    int j = 1;
    char *p_media_sequence = strstr(source, "#EXT-X-TARGETDURATION:");

    if (p_media_sequence) {
        if (sscanf(p_media_sequence, "#EXT-X-TARGETDURATION:%d", &j) != 1) {
            MSG_ERROR("Could not read EXT-X-MEDIA-TARGETDURATION\n");
            return 0;
        }
    }
    return j;
}



static int media_playlist_get_links(struct hls_media_playlist *me,std::string user_agent)
{
    int ms_init = media_playlist_get_media_sequence(me->source);
    int target_duration=media_playlist_get_media_target_duration(me->source);
    if(target_duration!=0){
        me->target_duration=target_duration;
    }
    struct hls_media_segment *ms = me->media_segment;
    char *src = me->source;
    char * tmp_url = (char *)malloc(strlen(src));
    
    for (int i = 0; i < me->count && src!=NULL; i++) {
        while (src != NULL) {
            if (*src == '\n') {
                src = next_line(src);
                continue;
            }
            if (*src == '#') {
                parse_playlist_tag(me, src,user_agent);
                src = next_line(src);
                continue;
            }
            if (*src == '\0') {
                goto finish;
            }
            if (sscanf(src, "%[^\n]", tmp_url) == 1) {
                ms[i].sequence_number = i + ms_init;
                extend_url(&tmp_url, me->url);
                ms[i].url=std::string(tmp_url);

                if (me->encryptiontype == ENC_AES128 || me->encryptiontype == ENC_AES_SAMPLE) {
                    memcpy(ms[i].enc_aes.key_value, me->enc_aes.key_value, KEYLEN);
                    if (me->enc_aes.iv_is_static == false) {
                        char iv_str[STRLEN_BTS(KEYLEN)];
                        snprintf(iv_str, STRLEN_BTS(KEYLEN), "%032x\n", ms[i].sequence_number);
                        uint8_t *iv_bin = (uint8_t *)malloc(KEYLEN);
                        str_to_bin(iv_bin, iv_str, KEYLEN);
                        memcpy(ms[i].enc_aes.iv_value, iv_bin, KEYLEN);
                        free(iv_bin);
                    }
                }
                src = next_line(src);
                break;
            }
        }
    }

finish:
    // Extend the individual urls.
    // for (int i = 0; i < me->count; i++) {
    //     extend_url(&ms[i].url, me->url);
    // }
    free(tmp_url);
    return 0;
}

int handle_hls_media_playlist(struct hls_media_playlist *me,std::string user_agent)
{
    me->encryption = false;
    me->encryptiontype = ENC_NONE;

    if(me->media_segment !=NULL){
        delete [] me->media_segment;
    }

    if(me->source != NULL){
        free(me->source) ;
    }

    get_data_from_url(me->url, &me->source, NULL, STRING,user_agent);

    if (get_playlist_type(me->source) != MEDIA_PLAYLIST) {
        return 1;
    }

    me->count = get_link_count(me->source);

    me->media_segment = new hls_media_segment[me->count];//(hls_media_segment *)malloc(sizeof(struct hls_media_segment) * me->count);

    if (media_playlist_get_links(me,user_agent)) {
        MSG_ERROR("Could not parse links. Exiting.\n");
        return 1;
    }
    return 0;
}

static int master_playlist_get_bitrate(struct hls_master_playlist *ma)
{
    struct hls_media_playlist *me = ma->media_playlist;

    char *src = ma->source;

    for (int i = 0; i < ma->count && src; i++) {
        if ((src = strstr(src, "BANDWIDTH="))) {
            if ((sscanf(src, "BANDWIDTH=%u", &me[i].bitrate)) == 1) {
                src++;
                continue;
            }
        }
    }
    return 0;
}

static int master_playlist_get_links(struct hls_master_playlist *ma)
{
    struct hls_media_playlist *me = ma->media_playlist;
    char *src = ma->source;

    for (int i = 0; i < ma->count; i++) {
        me[i].url = (char *)malloc(strlen(src));
    }

    for (int i = 0; i < ma->count; i++) {
        while (src != NULL) {
            if (*src == '#' || *src == '\n') {
                src = next_line(src);
                continue;
            }
            if (*src == '\0') {
                goto finish;
            }
            if (sscanf(src, "%[^\n]", me[i].url) == 1) {
                src = next_line(src);
                break;
            }
        }
    }

finish:
    for (int i = 0; i < ma->count; i++) {
        extend_url(&me[i].url, ma->url);
    }
    return 0;
}

int handle_hls_master_playlist(struct hls_master_playlist *ma)
{
    ma->count = get_link_count(ma->source);
    ma->media_playlist = new hls_media_playlist[ma->count];//(hls_media_playlist *)malloc(sizeof(struct hls_media_playlist) * ma->count);
    if (master_playlist_get_links(ma)) {
        MSG_ERROR("Could not parse links. Exiting.\n");
        return 1;
    }

    for (int i = 0; i < ma->count; i++) {
        ma->media_playlist[i].bitrate = 0;
    }

    if (master_playlist_get_bitrate(ma)) {
        MSG_ERROR("Could not parse bitrate. Exiting.\n");
        return 1;
    }
    return 0;
}

void print_hls_master_playlist(struct hls_master_playlist *ma)
{
    int i;
    MSG_VERBOSE("Found %d Qualitys\n\n", ma->count);
    for (i = 0; i < ma->count; i++) {
        MSG_PRINT("%d: Bandwidth: %d\n", i, ma->media_playlist[i].bitrate);
    }
}

static int decrypt_sample_aes(struct hls_media_segment *s, struct ByteBuffer *buf)
{
    // SAMPLE AES works by encrypting small segments (blocks).
    // Blocks have a size of 16 bytes.
    // Only 1 in 10 blocks of the video stream are encrypted,
    // while every single block of the audio stream is encrypted.
    int audio_index = -1, video_index = -1;
    static int audio_sample_rate = 0, audio_frame_size = 0;

    struct ByteBuffer input_buffer = {buf->data, buf->len, 0};

    AVInputFormat *ifmt = av_find_input_format("mpegts");
    uint8_t *input_avbuff = (uint8_t *)av_malloc(4096);
    AVIOContext *input_io_ctx = avio_alloc_context(input_avbuff, 4096, 0, &input_buffer,
                                                   read_packet, NULL, seek);
    AVFormatContext *ifmt_ctx = avformat_alloc_context();
    ifmt_ctx->pb = input_io_ctx;

    AVOutputFormat *ofmt = av_guess_format("mpegts", NULL, NULL);

    AVFormatContext *ofmt_ctx = avformat_alloc_context();
    ofmt_ctx->oformat = ofmt;
    
    if (avformat_open_input(&ifmt_ctx, "file.h264", ifmt, NULL) != 0) {
        MSG_ERROR("Opening input file failed\n");
    }

    // avformat_find_stream_info() throws useless warnings because the data is encrypted.
    av_log_set_level(AV_LOG_QUIET);
    avformat_find_stream_info(ifmt_ctx, NULL);
    av_log_set_level(AV_LOG_WARNING);

    for (int i = 0; i < (int)ifmt_ctx->nb_streams; i++) {
        AVCodecContext *in_c = ifmt_ctx->streams[i]->codec;
        if (in_c->codec_type == AVMEDIA_TYPE_AUDIO) {
            avformat_new_stream(ofmt_ctx, in_c->codec);
            avcodec_copy_context(ofmt_ctx->streams[i]->codec, in_c);
            audio_index = i;
            if (s->sequence_number == 1) {
                audio_frame_size = in_c->frame_size;
                audio_sample_rate = in_c->sample_rate;
            }
        } else if (in_c->codec_id == AV_CODEC_ID_H264) {
            avformat_new_stream(ofmt_ctx, in_c->codec);
            avcodec_copy_context(ofmt_ctx->streams[i]->codec, in_c);
            video_index = i;
        }
    }

    if (video_index < 0 || audio_index < 0) {
        MSG_ERROR("Video or Audio missing.");
    }


    // It can happen that only the first segment contains
    // useful sample_rate/frame_size data.
    if (ofmt_ctx->streams[audio_index]->codec->sample_rate == 0) {
        ofmt_ctx->streams[audio_index]->codec->sample_rate = audio_sample_rate;
    }
    if (ofmt_ctx->streams[audio_index]->codec->frame_size == 0) {
        ofmt_ctx->streams[audio_index]->codec->frame_size = audio_frame_size;
    }

    if (avio_open_dyn_buf(&ofmt_ctx->pb) != 0) {
        MSG_ERROR("Could not open output memory stream.\n");
    }

    AVPacket pkt;
    uint8_t packet_iv[16];

    if (avformat_write_header(ofmt_ctx, NULL) != 0) {
        MSG_ERROR("Writing header failed.\n");
    }

    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == audio_index) {
            // The IV must be reset at the beginning of every packet.
            memcpy(packet_iv, s->enc_aes.iv_value, 16);

            uint8_t *audio_frame = pkt.data;
            uint8_t *p_frame = audio_frame;

            enum AVCodecID cid = ifmt_ctx->streams[audio_index]->codec->codec_id;
            if (cid == AV_CODEC_ID_AAC) {
                // ADTS headers can contain CRC checks.
                // If the CRC check bit is 0, CRC exists.
                //
                // Header (7 or 9 byte) + unencrypted leader (16 bytes)
                p_frame += (p_frame[1] & 0x01) ? 23 : 25;
            } else if (cid == AV_CODEC_ID_AC3) {
                // AC3 Audio is untested. Sample streams welcome.
                //
                // unencrypted leader
                p_frame += 16;
            } else {
                MSG_ERROR("This audio codec is unsupported.\n");
                exit(1);
            }

            while (bytes_remaining(p_frame, (audio_frame + pkt.size)) >= 16 ) {
                uint8_t *dec_tmp = (uint8_t *)malloc(16);
                AES128_CBC_decrypt_buffer(dec_tmp, p_frame, 16, s->enc_aes.key_value, packet_iv);

                // CBC requires the unencrypted data from the previous
                // decryption as IV.
                memcpy(packet_iv, p_frame, 16);

                memcpy(p_frame, dec_tmp, 16);
                free(dec_tmp);
                p_frame += 16;
            }
            if (av_interleaved_write_frame(ofmt_ctx, &pkt)) {
                MSG_WARNING("Writing audio frame failed.\n");
            }
        } else if (pkt.stream_index == video_index) {
            // av_return_frame returns whole h264 frames. SAMPLE-AES
            // encrypts NAL units instead of frames. Fortunatly, a frame
            // contains multiple NAL units, so av_return_frame can be used.
            uint8_t *h264_frame = pkt.data;
            uint8_t *p = h264_frame;
            uint8_t *end = p + pkt.size;
            uint8_t *nal_end;

            int block;

            while (p < end) {
                block = 0;
                // Reset the IV at the beginning of every NAL unit.
                memcpy(packet_iv, s->enc_aes.iv_value, 16);
                p = memmem(p, (end - p), h264_nal_init, 3);
                if (!p) {
                    break;
                }
                nal_end = memmem(p + 3, (end - (p + 3)), h264_nal_init, 3);
                if (!nal_end) {
                    nal_end = end;
                }

                if (bytes_remaining(p, nal_end) <= (16 + 3 + 32)) {
                    p = nal_end;
                    continue;
                } else {
                    // Remove the start code emulation prevention.
                    for (int i = 0; i < 4; i++) {
                        uint8_t *tmp = p;
                        while ((tmp = memmem(tmp, nal_end - tmp, h264_scep_search[i], 4))) {
                            memcpy(tmp, h264_scep_replace[i], 3);
                            tmp += 3;
                            end -= 1;
                            memcpy(tmp, tmp + 1, (end - tmp));
                        }
                    }

                    // NALinit bytes + NAL_unit_type_byte + unencrypted_leader
                    p += 3 + 32;

                    nal_end = memmem(p, (end - p), h264_nal_init, 3);
                    if (!nal_end) {
                        nal_end = end;
                    }
                }

                while (bytes_remaining(p, nal_end) > 16) {
                    block++;
                    if (block == 1) {
                        uint8_t *output = (uint8_t *)malloc(16);
                        AES128_CBC_decrypt_buffer(output, p, 16, s->enc_aes.key_value, packet_iv);

                        // CBC requires the unencrypted data from the previous
                        // decryption as IV.
                        memcpy(packet_iv, p, 16);

                        memcpy(p, output, 16);
                        free (output);
                    } else if (block == 10) {
                        block = 0;
                    }
                    p += 16; //blocksize
                }
                p += bytes_remaining(p, nal_end); //unencryted trailer
            }
            pkt.size = bytes_remaining(pkt.data, end);
            if (av_interleaved_write_frame(ofmt_ctx, &pkt)) {
                MSG_WARNING("Writing video frame failed.\n");
            }
        }
        av_packet_unref(&pkt);
    }
    if (av_write_trailer(ofmt_ctx) != 0) {
        MSG_ERROR("Writing trailer failed.\n");
    }

    uint8_t *outbuf;
    buf->len = avio_close_dyn_buf(ofmt_ctx->pb, &outbuf);
    buf->data = (uint8_t *)realloc(buf->data, buf->len);
    memcpy(buf->data, outbuf, buf->len);
    av_free(outbuf);

    av_free(input_io_ctx->buffer);
    av_free(input_io_ctx);
    input_avbuff = NULL;
    avformat_free_context(ifmt_ctx);
    avformat_free_context(ofmt_ctx);
    return 0;
}

static int decrypt_aes128(struct hls_media_segment *s, struct ByteBuffer *buf)
{
    // The AES128 method encrypts whole segments.
    // Simply decrypting them is enough.
    uint8_t *db = (uint8_t *)malloc(buf->len);
    AES128_CBC_decrypt_buffer(db, buf->data, (uint32_t)buf->len,
                              s->enc_aes.key_value, s->enc_aes.iv_value);
    memcpy(buf->data, db, buf->len);
    free(db);
    return 0;
}

int download_hls(struct hls_media_playlist *me,bool live_streamming,int max_file_size,const char* custom_filename, std::string user_agent, bool force_overwrite=true)
{
    if(live_streamming){
        MSG_VERBOSE("Downloading Live Streaming\n");
    }else{
        MSG_VERBOSE("Downloading %d segments.\n", me->count);
    }

    char filename[MAX_FILENAME_LEN];

    if (custom_filename!=NULL && *custom_filename!='\0') {
        strcpy(filename, custom_filename);
    } else {
        strcpy(filename, "000_hls_output.ts");
    }

    if (access(filename, F_OK) != -1) {
        if (force_overwrite) {
            if (remove(filename) != 0) {
                MSG_ERROR("Error overwriting file");
                exit(1);
            }
        } else {
            char userchoice;
            MSG_PRINT("File already exists. Overwrite? (y/n) ");
            scanf("\n%c", &userchoice);
            if (userchoice == 'y') {
                if (remove(filename) != 0) {
                    MSG_ERROR("Error overwriting file");
                    exit(1);
                }
            } else {
                MSG_WARNING("Choose a different filename. Exiting.\n");
                exit(0);
            }
        }
    }

    std::shared_ptr<Queue<std::shared_ptr<hls_media_segment>>> segment_queue(new Queue<std::shared_ptr<hls_media_segment>>());
    std::mutex mutex_;
    std::condition_variable cond_;
    bool flag_finish=false;

    int lattest_sequence_number=-1;
    std::shared_ptr<std::thread> update_playlist_thread=std::make_shared<std::thread>([&]{    
        bool first_download_iteration = true;
        int wait_second= me->target_duration > 0 ?me->target_duration:1;
        while(first_download_iteration || live_streamming){
            {
                std::lock_guard<std::mutex> mlock(mutex_);
                if(flag_finish){
                    break;
                }
            }

            first_download_iteration = false;
            int i=0,current_file_count=0;
            
            for (; i < me->count&&me->media_segment[i].sequence_number <= lattest_sequence_number; i++);
            
            //Build downloading queue
            current_file_count = i;
            for (; i < me->count; i++) {
                //MSG_PRINT(me->media_segment[i].url); 
                segment_queue->push(std::make_shared<hls_media_segment>(me->media_segment[i]));
                lattest_sequence_number= me->media_segment[i].sequence_number;
            }


            MSG_PRINT("Update %d ts to queue\n",me->count - current_file_count);
            {
                std::unique_lock<std::mutex> mlock(mutex_);
                cond_.notify_one();
            }
            if(live_streamming){
                if(me->count - current_file_count < 1){    
                    
                    wait_second = wait_second*2;
                }else{
                    wait_second = me->target_duration;
                }
                MSG_PRINT("Sleep %d Seconds\n",wait_second);
                std::this_thread::sleep_for(std::chrono::seconds(wait_second));
                

                if (handle_hls_media_playlist(me,user_agent)) {
                    break;
                }
                MSG_PRINT("Reload m3u3 Playlist\n");
            }
        }
    });


    FILE *pFile = fopen(filename, "wb");
    int file_size=0;
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        cond_.wait(mlock,[segment_queue]()->bool{return !(segment_queue->empty());});
    }
    while(!segment_queue->empty()){
        auto segment=segment_queue->pop();

        MSG_PRINT("Thread %d Downloading segment %d\n", 0, segment->sequence_number);
        auto buffer = std::make_shared<ByteBuffer>();

        buffer->len = (int)get_data_from_url(segment->url.c_str(), NULL, &(buffer->data), BINARY,user_agent);
       
        if (me->encryption == true && me->encryptiontype == ENC_AES128) {
            decrypt_aes128(segment.get(), buffer.get());
        } else if (me->encryption == true && me->encryptiontype == ENC_AES_SAMPLE) {
            decrypt_sample_aes(segment.get(), buffer.get());
        }
        fwrite(buffer->data, 1, buffer->len, pFile);
        fflush(pFile);
        free(buffer->data);
        file_size=file_size+buffer->len;
        MSG_PRINT("Totally Downloading Size: %d....Max size %d\n", file_size,max_file_size);
        if(max_file_size>0 && file_size >= max_file_size){
            std::lock_guard<std::mutex> mlock(mutex_);
            flag_finish = true;
            MSG_PRINT("Reach Max Size %d! Finish.\n", max_file_size);
            break;
        }
        //delete segment;
    }


    fclose(pFile);
    update_playlist_thread->join();
    
    return 0;
}

int print_enc_keys(struct hls_media_playlist *me)
{
    for (int i = 0; i < me->count; i++) {
        if (me->encryption == true) {
            MSG_PRINT("[AES-128]KEY: 0x");
            for(size_t count = 0; count < KEYLEN; count++) {
                MSG_PRINT("%02x", me->media_segment[i].enc_aes.key_value[count]);
            }
            MSG_PRINT(" IV: 0x");
            for(size_t count = 0; count < KEYLEN; count++) {
                MSG_PRINT("%02x", me->media_segment[i].enc_aes.iv_value[count]);
            }
            MSG_PRINT("\n");
        }
    }
    return 0;
}


void media_playlist_cleanup(hls_media_playlist *me)
{
    if(me->source != NULL)
        free(me->source);
    if(me->url != NULL)
        free(me->url);
    if(me->media_segment != NULL)
        delete [] me->media_segment;
}


void master_playlist_cleanup(hls_master_playlist *ma)
{

    if(ma->source != NULL)
        free(ma->source);
    if(ma->url != NULL)
        free(ma->url);
    if(ma->media_playlist != NULL )
        delete [] ma->media_playlist;
}
