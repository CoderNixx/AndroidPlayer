//
// Created by Administrator on 2020/3/29.
//


#include "Video.h"

Video::Video(PlayStatus* playstatus, CallJava *callJava) {
this->playstatus = playstatus;
this->callJava = callJava;
packetQueue = new PacketQueue(playstatus);
pthread_mutex_init(&mutex, NULL);
}

Video::~Video() {
    pthread_mutex_destroy(&mutex);
}

void*  thread_play(void *data) {
    Video * video = (Video*)data;
    while (video->playstatus != NULL &&  !video->playstatus->exit) {
        // 如果暂停  停止取帧
        if (video->playstatus->pause) {
            av_usleep(1000 * 100);
            continue;
        }
        if (video->playstatus->seek) {
            av_usleep(1000 * 100);
            continue;
        }
        if (video->packetQueue->getQueueSize() == 0)//加载中
        {
            av_usleep(1000 * 100);
            continue;
        }
        // packet 分配内存
        AVPacket *avPacket = av_packet_alloc();
        if (!avPacket) {
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;
            continue;
        }
        // 获取packet
        if (video->packetQueue->getAvpacket(avPacket) != 0) {
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;
            continue;
        };
        if (video->codecType == CODEC_MEDIACODEC) {
            if (av_bsf_send_packet(video->abs_ctx, avPacket) != 0) {
                av_packet_free(&avPacket);
                av_free(avPacket);
                avPacket = NULL;
                continue;
            }
            while (av_bsf_receive_packet(video->abs_ctx, avPacket) == 0) {
                loge("开始硬解码");

                double diff = video->getFrameDiffTime(NULL, avPacket);
                loge("diff is %f", diff);

                av_usleep(video->getDelayTime(diff) * 1000000);
                video->callJava->onCallDecodeAVPacket(avPacket->size, avPacket->data);

                av_packet_free(&avPacket);
                av_free(avPacket);
                continue;
            }
            avPacket = NULL;
        } else if (video->codecType == CODEC_YUV) {
            pthread_mutex_lock(&video->mutex);
            // 将packet 传递给 解码器上下文；
            if (avcodec_send_packet(video->avCodecContext, avPacket) != 0) {
                av_packet_free(&avPacket);
                av_free(avPacket);
                avPacket = NULL;
                pthread_mutex_unlock(&video->mutex);
                continue;
            }
            // 分配帧内存
            AVFrame *avFrame = av_frame_alloc();
            if (!avFrame) {
                av_packet_free(&avPacket);
                av_free(avPacket);
                avPacket = NULL;
                av_frame_free(&avFrame);
                av_free(avFrame);
                avPacket = NULL;
                pthread_mutex_unlock(&video->mutex);
                continue;
            };
            // 解码器上下文中接受帧数
            if (avcodec_receive_frame(video->avCodecContext, avFrame) != 0) {
                av_packet_free(&avPacket);
                av_free(avPacket);
                avPacket = NULL;
                av_frame_free(&avFrame);
                av_free(avFrame);
                avPacket = NULL;
                pthread_mutex_unlock(&video->mutex);
                continue;
            }
            // 新建yuv420帧内存
            AVFrame *avFrameyuv420p = av_frame_alloc();
            // 获取yuv420p的buffer size
            int num = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, video->avCodecContext->width,
                                               video->avCodecContext->height, 1);
            // 初始化buffer的内存大小
            int8_t *buffer = (int8_t *) av_malloc(num * sizeof(int8_t));
            // 设置yuv420p帧的data 和linsize
            av_image_fill_arrays(avFrameyuv420p->data, avFrameyuv420p->linesize,
                                 (uint8_t *) (buffer), AV_PIX_FMT_YUV420P,
                                 video->avCodecContext->width, video->avCodecContext->height, 1);
            // 前后转换格式上下文初始化
            SwsContext *swsContext = sws_getContext(video->avCodecContext->width,
                                                    video->avCodecContext->height,
                                                    video->avCodecContext->pix_fmt,
                                                    video->avCodecContext->width,
                                                    video->avCodecContext->height,
                                                    AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL,
                                                    NULL);
            if (!swsContext) {
                av_frame_free(&avFrameyuv420p);
                av_free(avFrameyuv420p);
                av_free(buffer);
                continue;
            }
            double distance = video->getFrameDiffTime(avFrame,NULL);
            loge("distance is %f", distance);
            av_usleep(video->getDelayTime(distance) * 1000000);
            // 开始转换成yuv420p
            sws_scale(swsContext, avFrame->data, avFrame->linesize, 0,
                      video->avCodecContext->height, avFrameyuv420p->data,
                      avFrameyuv420p->linesize);
            video->callJava->onCallRenderYUV(
                    video->avCodecContext->width,
                    video->avCodecContext->height,
                    avFrameyuv420p->data[0],
                    avFrameyuv420p->data[1],
                    avFrameyuv420p->data[2]);

            av_frame_free(&avFrameyuv420p);
            av_free(avFrameyuv420p);
            av_free(buffer);
            sws_freeContext(swsContext);
            //回收
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;
            av_frame_free(&avFrame);
            av_free(avFrame);
            avPacket = NULL;
            pthread_mutex_unlock(&video->mutex);
        }
    }
    loge("转换成yuv420p成功");
    return 0;
}



void Video::play() {
    if(playstatus != NULL && !playstatus->exit)
    {
    pthread_create(&play_Pthread,NULL,thread_play,this);
    }
}

double Video::getFrameDiffTime(AVFrame *avFrame, AVPacket *avPacket) {

    double pts = 0;
    if(avFrame != NULL)
    {
        pts = av_frame_get_best_effort_timestamp(avFrame);
    }
    if(avPacket != NULL)
    {
        pts = avPacket->pts;
    }
    if(pts == AV_NOPTS_VALUE)
    {
        pts = 0;
    }
    pts *= av_q2d(time_base);

    if(pts > 0)
    {
        clock = pts;
    }

    double diff = audio->clock - clock;
    return diff;
}

double Video::getDelayTime(double distance) {

    if(distance > 0.003)
    {
        delayTime = delayTime * 2 / 3;
        if(delayTime < defaultDelayTime / 2)
        {
            delayTime = defaultDelayTime * 2 / 3;
        }
        else if(delayTime > defaultDelayTime * 2)
        {
            delayTime = defaultDelayTime * 2;
        }
    }
    else if(distance < - 0.003)
    {
        delayTime = delayTime * 3 / 2;
        if(delayTime < defaultDelayTime / 2)
        {
            delayTime = defaultDelayTime * 2 / 3;
        }
        else if(delayTime > defaultDelayTime * 2)
        {
            delayTime = defaultDelayTime * 2;
        }
    }
    else if(distance == 0.003)
    {

    }
    if(distance >= 0.5)
    {
        delayTime = 0;
    }
    else if(distance <= -0.5)
    {
        delayTime = defaultDelayTime * 2;
    }

    if(fabs(distance) >= 10)
    {
        delayTime = defaultDelayTime;
    }
    return delayTime;
}

void Video::release() {
    loge("释放 noticeQueue");
    if(packetQueue != NULL)
    {
        packetQueue->noticeQueue();
    }
    loge("释放 play_Pthread");
    pthread_join(play_Pthread, NULL);
    loge("释放 packetQueue");
    if(packetQueue != NULL)
    {
        delete(packetQueue);
        packetQueue = NULL;
    }
    if(abs_ctx != NULL)
    {
        av_bsf_free(&abs_ctx);
        abs_ctx = NULL;
    }
    if(avCodecContext != NULL)
    {
        pthread_mutex_lock(&mutex);
        avcodec_close(avCodecContext);
        avcodec_free_context(&avCodecContext);
        avCodecContext = NULL;
        pthread_mutex_unlock(&mutex);
    }

    if(playstatus != NULL)
    {
        playstatus = NULL;
    }
    if(callJava != NULL)
    {
        callJava = NULL;
    }

}

