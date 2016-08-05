
#include "libavutil/opt.h"
#include "config.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"

#include "smem_dec.h"
#include "smem_client.h"



struct memory_info2 {
    int index;  // stream index
    int64_t pts;
    int64_t dts;
    int stream_info_offset;
    int stream_info_number;
    int data_offset;
    int data_size;
    int is_key;
};

struct stream_info2 {
    int index; 

    enum AVMediaType codec_type;
    enum AVCodecID     codec_id;
    AVRational time_base;

    /* video */
    int width;
    int height;
    enum AVPixelFormat pix_fmt;
    int video_extradata_size;
    uint8_t video_extradata[128];

    /* audio */
    int sample_rate; ///< samples per second
    int channels;    ///< number of audio channels
    enum AVSampleFormat sample_fmt;  ///< sample format
    int audio_extradata_size;
    uint8_t audio_extradata[128];

    /* other stream not support yet */
};


#define SMEM_MAX_STREAM     64
#define SMEM_TIME_BASE      1000000
#define SMEM_TIME_BASE_Q    (AVRational){1, SMEM_TIME_BASE}

#define SMEM_NUM_IN_RANGE(n,min,max)  ((n) > (max) ? (max) : ((n) < (min) ? (min) : (n)))
#define SMEM_NUM_IF_OUT_RANGE(n,min,max)  ((n) > (max) ? 1 : ((n) < (min) ? 1 : 0))

typedef struct smem_dec_ctx {
    const AVClass *class;

    struct smemContext * sctx;

    int stream_number;
    struct stream_info2 * stream_infos;
    int stream_info_size;

    /* Params */
    char ip[64];    /* the share memory server ip address */
    int  port;      /* the share memory server port */
    char channel[128];  /* the channel in share memory server */

    int width;
    int height;

    // for timestamp
    int64_t   out_index; // the stream index of the out timestamp base used
    int64_t   last_out_ts[SMEM_MAX_STREAM]; // the last out ts for each stream,timebase={1, 1000000}
    int64_t   last_in_ts[SMEM_MAX_STREAM]; // the last in ts for each stream
    int64_t   first_in_ts[SMEM_MAX_STREAM];
    int should_duration[SMEM_MAX_STREAM];
    AVRational in_timebase[SMEM_MAX_STREAM];

    /* Options */
    int timeout;

    /* test */
    FILE * yuv_file;
}smem_dec_ctx;

//#define TEST_FILE_OUT "/root/video_out.yuv"

static int get_stream_info(AVFormatContext *avctx)
{
    struct smem_dec_ctx * ctx = avctx->priv_data;
    struct memory_info2 * m_info;

    int mem_id = -1;
    uint8_t *mem_ptr = NULL;
    int ret;

    int64_t last_time = av_gettime_relative();

    while(1){
        // to get one share memory
        mem_id = smemGetShareMemory(ctx->sctx, 0);
        if(mem_id > 0){
            av_log(avctx, AV_LOG_VERBOSE, "get memory id: %d\n", mem_id);

            // get the memory ptr
            mem_ptr = shmat(mem_id, 0, 0);
            if(mem_ptr == (void *)-1){
                av_log(avctx, AV_LOG_ERROR, "get share memory(%d) pointer error.\n", mem_id);
                smemFreeShareMemory(ctx->sctx, mem_id);
                return -1;
            }
            av_log(avctx, AV_LOG_VERBOSE, "get memory mem_ptr: %ld\n", mem_ptr);

            
            // get stream info
            m_info = (struct memory_info2 * )mem_ptr;
            ctx->stream_number = m_info->stream_info_number;

            ctx->stream_info_size = ctx->stream_number * sizeof(struct stream_info2);
            ctx->stream_infos = av_malloc(ctx->stream_info_size);


            memcpy(ctx->stream_infos, mem_ptr + m_info->stream_info_offset, ctx->stream_info_size);

            // fixme: the data should be save

            // free the share memory 
            if(mem_ptr){
                ret = shmdt(mem_ptr);
                if(ret < 0){
                    av_log(avctx, AV_LOG_ERROR, "release share memory ptr error:%d\n", ret);
                    return -1;
                }
            }

            // free the memory id
            smemFreeShareMemory(ctx->sctx, mem_id);
            av_log(avctx, AV_LOG_VERBOSE, "smemFreeShareMemory\n");

            break;
        }
        else{
            av_usleep(1);
            // fixme: should be add timeout
            // when timeout break
            if(av_gettime_relative() - last_time > ctx->timeout*1000000){
                av_log(avctx, AV_LOG_INFO, "timeout:%d\n", ctx->timeout);
                return -1;
            }

        }
    }
    return 0;
}

av_cold static int ff_smem_read_header(AVFormatContext *avctx)
{
    AVStream *stream = NULL;


    av_log(avctx, AV_LOG_VERBOSE, "ff_smem_read_header begin\n");

    struct smem_dec_ctx * ctx = avctx->priv_data;

    /* get the input stream url, the stream format should be like :  "smem://127.0.0.1:6379/channel_name"*/
    av_log(avctx, AV_LOG_VERBOSE, "channel name:%s\n", avctx->filename);


    /* connect to the server */
    ctx->sctx = smemCreateConsumer("127.0.0.1", 6379, "test");
    if(!ctx->sctx || ctx->sctx->err < 0){
        if (ctx->sctx) {
            av_log(avctx, AV_LOG_ERROR,"Connection error: %s\n", ctx->sctx->errstr);
            smemFree(ctx->sctx);
        } else {
            av_log(avctx, AV_LOG_ERROR,"Connection error: can't allocate redis context\n");
        }
    }

    /* get the stream info */
    get_stream_info(avctx);

    /* create the streams */
    int i = 0;
    struct stream_info2 * stream_info;

    for(i = 0; i < ctx->stream_number; i++){

        stream_info = &ctx->stream_infos[i];

        if(stream_info->codec_type == AVMEDIA_TYPE_VIDEO){
            av_log(avctx, AV_LOG_VERBOSE, "stream %d is video stream\n", i);

            // add video stream
            stream = avformat_new_stream(avctx, NULL);
            if (!stream) {
                av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
                goto ff_smem_read_header_error;
            }

            stream->codec->codec_type  = AVMEDIA_TYPE_VIDEO;
            stream->codec->codec_id    = stream_info->codec_id; 
            //stream->codec->time_base.den      = stream_info->time_base.den;
            //stream->codec->time_base.num      = stream_info->time_base.num;
            stream->codec->time_base = SMEM_TIME_BASE_Q;
            stream->time_base = stream->codec->time_base;

            stream->codec->pix_fmt     = stream_info->pix_fmt;
            stream->codec->width       = stream_info->width;
            stream->codec->height      = stream_info->height;
            stream->codec->extradata_size      = stream_info->video_extradata_size;
            if(stream->codec->extradata_size > 0){
                stream->codec->extradata = av_malloc(stream->codec->extradata_size);
                memcpy(stream->codec->extradata, stream_info->video_extradata, stream->codec->extradata_size);
            }

            ctx->should_duration[i] = SMEM_TIME_BASE/SMEM_NUM_IN_RANGE(25, 1, 60);  // fixme: should know the framerate
            ctx->in_timebase[i].den = stream_info->time_base.den;
            ctx->in_timebase[i].num = stream_info->time_base.num;
            ctx->last_in_ts[i] = 0;
            ctx->last_out_ts[i] = 0;
            ctx->first_in_ts[i] = 0;

            av_log(avctx, AV_LOG_INFO, "stream:%d, video, codec_id:%d, time_base:(%d,%d), pix_fmt:%d, width:%d, height:%d, should_duration=%d \n", i,
                stream_info->codec_id, stream_info->time_base.num, stream_info->time_base.den, stream_info->pix_fmt, stream_info->width, stream_info->height,
                ctx->should_duration[i]);

            //stream->codec->bit_rate    = av_image_get_buffer_size(stream->codec->pix_fmt, ctx->width, ctx->height, 1) * 1/av_q2d(stream->codec->time_base) * 8;
        }
        else if(stream_info->codec_type == AVMEDIA_TYPE_AUDIO){
            av_log(avctx, AV_LOG_VERBOSE, "stream %d is audio stream\n", i);

            // add video stream
            stream = avformat_new_stream(avctx, NULL);
            if (!stream) {
                av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
                goto ff_smem_read_header_error;
            }

            stream->codec->codec_type  = AVMEDIA_TYPE_AUDIO;
            stream->codec->codec_id    = stream_info->codec_id; 
            //stream->codec->time_base.den      = stream_info->time_base.den;
            //stream->codec->time_base.num      = stream_info->time_base.num;
            stream->codec->time_base = SMEM_TIME_BASE_Q;
            stream->time_base = stream->codec->time_base;

            stream->codec->sample_rate   = stream_info->sample_rate;
            stream->codec->channels      = stream_info->channels;
            stream->codec->sample_fmt    = stream_info->sample_fmt;

            ctx->should_duration[i] = (SMEM_TIME_BASE*1024)/SMEM_NUM_IN_RANGE(stream->codec->sample_rate, 11025, 48000);
            ctx->in_timebase[i].den = stream_info->time_base.den;
            ctx->in_timebase[i].num = stream_info->time_base.num;
            ctx->last_in_ts[i] = 0;
            ctx->last_out_ts[i] = 0;
            ctx->first_in_ts[i] = 0;

            stream->codec->extradata_size      = stream_info->audio_extradata_size;
            if(stream->codec->extradata_size > 0){
                stream->codec->extradata = av_malloc(stream->codec->extradata_size);
                memcpy(stream->codec->extradata, stream_info->audio_extradata, stream->codec->extradata_size);
            }

            av_log(avctx, AV_LOG_INFO, "stream:%d, audio, codec_id:%d, time_base:(%d,%d), sample_rate:%d, channels:%d, sample_fmt:%d, should_duration:%d \n", i,
                stream_info->codec_id, stream_info->time_base.num, stream_info->time_base.den, stream_info->sample_rate, 
                stream_info->channels, stream_info->sample_fmt, ctx->should_duration[i]);

        }else{
            av_log(avctx, AV_LOG_ERROR,"not support the type:%d\n", stream_info->codec_type);
        }
    }


    #ifdef TEST_FILE_OUT
        ctx->yuv_file = fopen(TEST_FILE_OUT, "w+");
        if(ctx->yuv_file == NULL){
            av_log(avctx, AV_LOG_ERROR,"open file:%s for test failed.\n", TEST_FILE_OUT);
        }
    #endif 

    av_log(avctx, AV_LOG_VERBOSE, "ff_smem_read_header over\n");

    return 0;

ff_smem_read_header_error:

    return AVERROR(EIO);
}


static int rebuild_timestamp(AVFormatContext *avctx, struct memory_info2 * m_info, int64_t * out_pts, int64_t * out_dts)
{
    struct smem_dec_ctx * ctx = avctx->priv_data;

    av_log(avctx, AV_LOG_INFO, "[rebuild_timestamp] before rebuild pts: %lld, dts: %lld\n", m_info->pts, m_info->dts);

    int64_t pts = av_rescale_q(m_info->pts, ctx->in_timebase[m_info->index], SMEM_TIME_BASE_Q);
    int64_t dts = av_rescale_q(m_info->dts, ctx->in_timebase[m_info->index], SMEM_TIME_BASE_Q);



    if(m_info->index == 0){
        // the first number stream is the timestamp rebuild base stream 

        if(SMEM_NUM_IF_OUT_RANGE(dts - ctx->last_in_ts[m_info->index], 0, 5*ctx->should_duration[m_info->index])){
            av_log(avctx, AV_LOG_WARNING, "[rebuild_timestamp] index:%d, last dts: %lld, the in dts: %lld is out of the range\n", m_info->index, dts, dts);
            *out_dts = ctx->last_out_ts[m_info->index] + ctx->should_duration[m_info->index];
            ctx->first_in_ts[m_info->index] = *out_dts;

        }else{
            *out_dts = ctx->last_out_ts[m_info->index] + (dts - ctx->last_in_ts[m_info->index]);
        }

        *out_pts = *out_dts + (pts - dts) + 2*ctx->should_duration[m_info->index];


    }else{
        // other number streams check the timestamp by the base stream
        if(SMEM_NUM_IF_OUT_RANGE(dts - ctx->last_in_ts[m_info->index], 0, 5*ctx->should_duration[m_info->index])){
            av_log(avctx, AV_LOG_WARNING, "[rebuild_timestamp] index:%d, last dts: %lld, the in dts: %lld is out of the range\n", m_info->index, dts, dts);
            *out_dts = ctx->last_out_ts[m_info->index] + ctx->should_duration[m_info->index];
            ctx->first_in_ts[m_info->index] = *out_dts;

        }else{
            *out_dts = ctx->last_out_ts[m_info->index] + (dts - ctx->last_in_ts[m_info->index]);
        }

        *out_pts = *out_dts + (pts - dts) + 2*ctx->should_duration[m_info->index];


    }

    ctx->last_in_ts[m_info->index] = dts;
    ctx->last_out_ts[m_info->index] = *out_dts;


    av_log(avctx, AV_LOG_INFO, "[rebuild_timestamp] after rebuild pts: %lld, dts: %lld\n", *out_pts, *out_dts);


    return 0;
}

static int ff_smem_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    av_log(avctx, AV_LOG_VERBOSE, "ff_smem_read_packet\n");
    struct smem_dec_ctx * ctx = avctx->priv_data;

    int packet_size, ret, width, height; 

    AVStream *st = avctx->streams[0];


    static uint8_t *buffer = NULL;
    int mem_id = -1;
    uint8_t *mem_ptr = NULL;
    struct memory_info2 * m_info;

    int64_t last_time = av_gettime_relative();

    int64_t out_pts;
    int64_t out_dts;

    av_init_packet(pkt);
    while(1){
        // get memory 
        mem_id = smemGetShareMemory(ctx->sctx, 0);
        if(mem_id > 0){
            //av_log(avctx, AV_LOG_VERBOSE, "get memory id: %d\n", mem_id);

            // get the memory ptr
            mem_ptr = shmat(mem_id, 0, 0);
            if(mem_ptr == (void *)-1){
                //av_log(avctx, AV_LOG_ERROR, "get share memory(%d) pointer error.\n", mem_id);
                smemFreeShareMemory(ctx->sctx, mem_id);
                //av_log(avctx, AV_LOG_ERROR, "get share memory(%d) pointer error and free over.\n", mem_id);
                av_usleep(10);
                continue;
            }
            //av_log(avctx, AV_LOG_VERBOSE, "get memory mem_ptr: %ld\n", mem_ptr);
            m_info = (struct memory_info2 * )mem_ptr;

            // fixme: need rebuild the timestamp
            if(rebuild_timestamp(avctx, m_info, &out_pts, &out_dts) < 0){
                continue;
            }

            
            av_init_packet(pkt);

            //av_log(avctx, AV_LOG_VERBOSE, "data_size: %d\n", m_info->data_size);

            av_new_packet(pkt, m_info->data_size);
            pkt->stream_index = m_info->index;
            pkt->pts = out_pts;
            pkt->dts = out_dts;
            if(m_info->is_key)
                pkt->flags |= AV_PKT_FLAG_KEY;
            
            ret = memcpy(pkt->data, mem_ptr + m_info->data_offset, m_info->data_size);

            av_log(avctx, AV_LOG_VERBOSE, "get memory packet, stream_index:%d, time_base:(%d,%d) pts:%lld, size:%d, data: %ld, mem_ptr:%lld, data_offset:%d, data_size:%d, ret=%d \n", 
                pkt->stream_index, ctx->stream_infos[pkt->stream_index].time_base.num, ctx->stream_infos[pkt->stream_index].time_base.den, pkt->pts, pkt->size, pkt->data,
                mem_ptr, m_info->data_offset, m_info->data_size, ret);

            //av_log(avctx, AV_LOG_VERBOSE, "memcpy\n");
            #ifdef TEST_FILE_OUT
                if(ctx->yuv_file && ctx->stream_infos[pkt->stream_index].codec_type == AVMEDIA_TYPE_VIDEO){
                    fwrite(pkt->data, m_info->data_size, 1, ctx->yuv_file);
                }
            #endif 

            // free the share memory 
            if(mem_ptr){
                ret = shmdt(mem_ptr);
                if(ret < 0){
                    av_log(avctx, AV_LOG_ERROR, "release share memory ptr error:%d\n", ret);
                    //return -1;
                    continue;
                }
            }

            // free the memory id
            smemFreeShareMemory(ctx->sctx, mem_id);

            break;
        
        }

        av_usleep(1);

        // when timeout break
        if(av_gettime_relative() - last_time > ctx->timeout*1000000){
            av_log(avctx, AV_LOG_INFO, "timeout:%d\n", ctx->timeout);
            return -1;
        }


    }
    av_log(avctx, AV_LOG_VERBOSE, "ff_smem_read_packet over\n");
    return 0;
}


av_cold static int ff_smem_read_close(AVFormatContext *avctx)
{
    av_log(avctx, AV_LOG_VERBOSE, "ff_smem_read_close\n");
    struct smem_dec_ctx * ctx = avctx->priv_data;

    #ifdef TEST_FILE_OUT
        if(ctx->yuv_file){
            fclose(ctx->yuv_file);
            ctx->yuv_file = NULL;
        }
    #endif 

    return 0;
}


#define OFFSET(x) offsetof(smem_dec_ctx, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "timeout", "set maximum timeout (in seconds)", OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = 5}, INT_MIN, INT_MAX, DEC },
    { NULL },
};

static const AVClass smem_demuxer_class = {
    .class_name = "smem demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_INPUT,
};


AVInputFormat ff_smem_demuxer = {
    .name           = "smem",
    .long_name      = NULL_IF_CONFIG_SMALL("Test smem input"),
    .flags          = AVFMT_NOFILE ,
    .priv_class     = &smem_demuxer_class,
    .priv_data_size = sizeof(struct smem_dec_ctx),
    .read_header   = ff_smem_read_header,
    .read_packet   = ff_smem_read_packet,
    .read_close    = ff_smem_read_close,
};
