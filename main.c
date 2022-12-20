#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <ncurses.h>
#include <portaudio.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "av.h"
#include "channel/channel.h"
#include "config.h"
#include "display.h"

#define AUDIO_BUF_SIZE 512

void print_help();
void print_license();
void handle_int(int _);

int main(int argc, char *argv[]) {
    // Insucfficient program arguments.
    if (argc < 2) {
        print_help();
        return 0;
    }

    // Initialize ncurses window
    initscr();

    // Set interrupt handler
    signal(SIGINT, handle_int);

    // Parse program arguments into config.
    config conf = parse_config(argc, argv);

    // If --help
    if (conf.help) {
        endwin();
        print_help();
        return 0;
    }
    // If --license
    if (conf.license) {
        endwin();
        print_license();
        return 0;
    }

    // Create Audio and Video Fromat Context.
    AVFormatContext *fmt_ctxt = NULL;
    // Create Audio and Video CoDec Context.
    AVCodecContext *a_cdc = NULL, *v_cdc = NULL;
    // Store stream index.
    int a_idx = -1, v_idx = -1;

    // Find audio and video codec centext and stream index.
    int err =
        find_codec_context(&conf, &fmt_ctxt, &a_cdc, &v_cdc, &a_idx, &v_idx);
    if (err != 0) {
        return err;
    }

    // If no audio
    if (conf.no_audio) {
        // Check if has FPS
        if (v_cdc->framerate.num == 0) {
            endwin();
            printf("Unknown FPS\n");
            exit(-1);
        }
        conf.fps = (double)v_cdc->framerate.num / v_cdc->framerate.den;
    }

    // Allocate AVPacket
    AVPacket *pckt = av_packet_alloc();
    if (!pckt) {
        printf("Unable to allocate AVPacket\n");
        return -2;
    }
    // Allocate AVFrame
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        printf("Unable to allocate AVFrame\n");
        return -2;
    }

    // Allocate resized greyscale image frame
    AVFrame *frame_greyscale = av_frame_alloc();
    if (!frame_greyscale) {
        printf("Unable to allocate AVFrame for greyscale frame\n");
        return -2;
    }
    // Initialize some fields in frame_grey
    frame_greyscale->width = conf.width;
    frame_greyscale->height = conf.height;
    // Allocate image resize context
    struct SwsContext *sws_ctxt = sws_getContext(
        v_cdc->width, v_cdc->height, v_cdc->pix_fmt, conf.width, conf.height,
        AV_PIX_FMT_GRAY8, SWS_FAST_BILINEAR, 0, 0, 0);

    // // Allocate resampled audio frame
    // AVFrame *frame_resampled = av_frame_alloc();
    // if (!frame_resampled) {
    //     printf("Unable to allocate AVFrame for audio frame\n");
    //     return -2;
    // }
    // // Initialize some fields in frame_resampled
    // frame_resampled->channel_layout = AV_CH_LAYOUT_STEREO;
    // frame_resampled->sample_rate = a_cdc->sample_rate;
    // frame_resampled->format = AV_SAMPLE_FMT_FLT;
    // Allocate audio resample context
    SwrContext *resample_ctxt = swr_alloc();
    if (!resample_ctxt) {
        printf("Unable to allocate AVAudioResampleContext\n");
        return -2;
    }

    // PortAudio Stream Params
    PaStreamParameters pa_stm_param;
    // PortAudio Stream
    PaStream *stream;
    // If need audio
    if (!conf.no_audio) {
        // Initialize PortAudio
        err = Pa_Initialize();
        if (err != paNoError) {
            printf("PortAudio init error(code: %d).\n", err);
            return -20;
        }
        // Get output device
        pa_stm_param.device = Pa_GetDefaultOutputDevice();
        if (pa_stm_param.device == paNoDevice) {
            printf("Can NOT find audio device.\n");
            return -20;
        }
        // Initialize other fields in pa_stm_param
        pa_stm_param.sampleFormat = paFloat32;
        pa_stm_param.channelCount = 2;
        pa_stm_param.suggestedLatency =
            Pa_GetDeviceInfo(pa_stm_param.device)->defaultLowOutputLatency;
        pa_stm_param.hostApiSpecificStreamInfo = NULL;
        // Open audio stream
        err = Pa_OpenStream(&stream, NULL, &pa_stm_param, a_cdc->sample_rate,
                            AUDIO_BUF_SIZE, paClipOff, NULL, NULL);
        if (err != paNoError) {
            printf("Error when opening audio stream.(code %d)\n", err);
            return -20;
        }
    }

    // Allocate video channel
    conf.video_ch = alloc_channel(10);

    // Video thread
    pthread_t th_v;

    int image_count = 0, audio_count = 0;
    // While not the end of file.
    while (av_read_frame(fmt_ctxt, pckt) >= 0) {
        // If is video stream.
        if (pckt->stream_index == v_idx) {
            // Send packet to video decoder
            err = avcodec_send_packet(v_cdc, pckt);
            if (err < 0) {
                printf(
                    "Error when supplying raw packet data as input to video "
                    "decoder.(code: %d)\n",
                    err);
                return -10;
            }
            // Read all frames from decoder
            while (1) {
                // Receive frame
                err = avcodec_receive_frame(v_cdc, frame);
                if (err != 0) {
                    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                        break;
                    }
                    printf("Failed when decoding video(code: %d)\n", err);
                    return -5;
                }
                uint8_t *buf =
                    (uint8_t *)av_malloc(av_image_get_buffer_size(
                        AV_PIX_FMT_GRAY8, conf.width, conf.height, 1));
                av_image_fill_arrays(frame_greyscale->data,
                                     frame_greyscale->linesize,
                                     buf, AV_PIX_FMT_GRAY8,
                                     conf.width, conf.height, 1);
                // Scale raw image to target image
                sws_scale(sws_ctxt, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, v_cdc->height,
                          frame_greyscale->data, frame_greyscale->linesize);
                // Add scaled data to video channel
                add_element(conf.video_ch, buf);
                // // Reset the frame fields.
                // av_frame_unref(frame_greyscale);
                // Start video thread
                if (image_count++ == 1) {
                    pthread_create(&th_v, NULL, play_video, &conf);
                }
            }
        } else if (!conf.no_audio && pckt->stream_index == a_idx) {
            err = avcodec_send_packet(a_cdc, pckt);
            if (err < 0) {
                printf(
                    "Error when supplying raw packet data as input to audio "
                    "decoder.(code: %d)\n",
                    err);
                return -10;
            }
            while (1) {
                audio_count++;
                err = avcodec_receive_frame(a_cdc, frame);
                if (err != 0) {
                    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                        break;
                    }
                    printf("Failed when decoding audio(code: %d)\n", err);
                    return -5;
                }
                // Allocate resampled audio frame
                AVFrame *frame_resampled = av_frame_alloc();
                if (!frame_resampled) {
                    printf("Unable to allocate AVFrame for audio frame\n");
                    return -2;
                }
                // Initialize some fields in frame_resampled
                frame_resampled->channel_layout = AV_CH_LAYOUT_STEREO;
                frame_resampled->sample_rate = a_cdc->sample_rate;
                frame_resampled->format = AV_SAMPLE_FMT_FLT;
                err = swr_convert_frame(resample_ctxt, frame_resampled, frame);
                if (err != 0) {
                    print_averror(err);
                    return -20;
                }
                // APAudioData *apad = (APAudioData
                // *)malloc(sizeof(APAudioData)); apad->nb_samples =
                // audio_frame->nb_samples; apad->data = (float *)
                // audio_frame->data[0];
                if (audio_count == 1) {
                    Pa_StartStream(stream);
                }
                Pa_WriteStream(stream, frame_resampled->data[0],
                               frame_resampled->nb_samples);
                av_frame_free(&frame_resampled);
            }
        }
        av_packet_unref(pckt);
        av_frame_unref(frame);
    }
    endwin();

    sleep(1);
    av_frame_free(&frame);
    av_frame_free(&frame_greyscale);
    av_packet_free(&pckt);
    avcodec_free_context(&a_cdc);
    avcodec_free_context(&v_cdc);
    avformat_free_context(fmt_ctxt);
    sws_freeContext(sws_ctxt);
    swr_free(&resample_ctxt);
    free_channel(conf.audio_ch);
    free_channel(conf.video_ch);
    Pa_CloseStream(stream);
}

/// @brief Handle interrupt (^C)
/// @param _
void handle_int(int _) {
    endwin();
    exit(0);
}

void print_help() {
    printf(
        "ASCII Player v1.0.0\n\
A media player that plays video file in ASCII characters.\n\
Usage: asciiplayer <file> [-h | --help] [-l | --license] [--cache <file>]\n\
                          [-n | —no-audio]\n\
       --help -h            Print this help page\n\
       --license -l         Show license and author info\n\
       --cache -c <file>    Process video into a cached file\n\
                            example: $ asciiplayer video.mp4 --cache cached.apcache\n\
       --no-audio -n        Play video without playing audio\n");
}

void print_license() {
    printf(
        "ASCII Player is an open-source software (GNU GPLv3) written in C programming language.\n\
\n\
Author(s):\n\
    Maintainer: Zhendong Chen 221870144 @ Nanjing University\n\
    Developer : Yuqing Tang   221870117 @ Nanjing University\n\
    Developer : Yaqi Dong     221870103 @ Nanjing University\n\
\n\
Special Thanks To:\n\
    GNU Project\n\
    FFmpeg\n\
    PortAudio\n");
}