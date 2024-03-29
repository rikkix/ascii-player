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

#include "apcache.h"
#include "av.h"
#include "channel/channel.h"
#include "config.h"
#include "display.h"
#include "log/log.h"

// https://stackoverflow.com/questions/35446049/port-audio-causing-loud-buzzing-50-of-tests
#define AUDIO_BUF_SIZE 1024

static void print_help();
static void print_license();
// Handle interrupt (^C)
static void handle_int(int _);
static void handle_exit(void);

int main(int argc, char *argv[]) {
    // Insucfficient program arguments.
    if (argc < 2) {
        print_help();
        return 0;
    }

    // Set interrupt handler
    signal(SIGINT, handle_int);

    // Parse program arguments into config.
    config conf = parse_config(argc, argv);

    // Initialize ncurses window
    if (!atomic_fetch_or(&ncurses_status, 1)) {
        initscr();
    }
    atexit(handle_exit);

    // Set max x and y
    getmaxyx(stdscr, conf.height, conf.width);
    conf.width--;

    logger_set_default((Logger){
        .file = conf.logfile ? fopen(conf.logfile, "a") : NULL,
        .has_color = 0,
        .has_date = 1,
        .has_time = 1,
        .has_filename = 1,
        .has_linenum = 1,
        .log_level = conf.log_level,
        .lock = PTHREAD_MUTEX_INITIALIZER,
    });

    // If --help
    if (conf.help) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        print_help();
        return 0;
    }
    // If --license
    if (conf.license) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        print_license();
        return 0;
    }

    linfo("Checking whether is an apcache file... (path: %s)", conf.filename);
    int fn_len = strlen(conf.filename);
    if (fn_len > 8) {
        if (strcmp(conf.filename + fn_len - 8, ".apcache") == 0 &&
            is_apcache(conf.filename) == 0) {
            ldebug("File detected as an apcache file");
            int err = play_from_cache(conf);
            return err;
        }
    }

    // Create Audio and Video Fromat Context.
    AVFormatContext *fmt_ctxt = NULL;
    // Create Audio and Video CoDec Context.
    AVCodecContext *a_cdc = NULL, *v_cdc = NULL;
    // Store stream index.
    int a_idx = -1, v_idx = -1;

    linfo("Finding audio and video codec centext and stream index...");
    // Find audio and video codec centext and stream index.
    int err =
        find_codec_context(&conf, &fmt_ctxt, &a_cdc, &v_cdc, &a_idx, &v_idx);
    if (err != 0) {
        return err;
    }

    ldebug("No audio");
    // If no audio
    linfo("Getting framerate...");
    AVRational framerate = fmt_ctxt->streams[v_idx]->avg_frame_rate;
    // Check if has FPS
    if (framerate.num == 0 && conf.no_audio) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unknown FPS! Exiting...\n");
        lfatal(-1, "Unknown FPS");
    } else {
        // Calculate FPS
        conf.fps = (double)framerate.num / framerate.den;
    }

    // Allocate AVPacket
    AVPacket *pckt = av_packet_alloc();
    if (!pckt) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unable to allocate AVPacket\n");
        lfatal(-2, "Unable to allocate AVPacket");
    }
    // Allocate AVFrame
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unable to allocate AVFrame\n");
        lfatal(-2, "Unable to allocate AVFrame");
    }
    // Allocate resized greyscale image frame
    AVFrame *frame_greyscale = av_frame_alloc();
    if (!frame_greyscale) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unable to allocate AVFrame for greyscale frame\n");
        lfatal(-2, "Unable to allocate AVFrame for greyscale frame");
    }
    // Initialize some fields in frame_grey
    frame_greyscale->width = conf.width;
    frame_greyscale->height = conf.height;
    // Allocate image resize context
    struct SwsContext *sws_ctxt = sws_getContext(
        v_cdc->width, v_cdc->height, v_cdc->pix_fmt, conf.width, conf.height,
        AV_PIX_FMT_GRAY8, SWS_FAST_BILINEAR, 0, 0, 0);

    // Allocate resampled audio frame
    AVFrame *frame_resampled = av_frame_alloc();
    if (!frame_resampled) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unable to allocate AVFrame for audio frame\n");
        lfatal(-2, "Unable to allocate AVFrame for audio frame");
    }
    // Allocate audio resample context
    SwrContext *resample_ctxt = swr_alloc();
    if (!resample_ctxt) {
        if (atomic_fetch_and(&ncurses_status, 0)) {
            endwin();
        }
        printf("Unable to allocate AVAudioResampleContext\n");
        lfatal(-2, "Unable to allocate AVAudioResampleContext");
    }

    // PortAudio Stream Params
    PaStreamParameters pa_stm_param;
    // PortAudio Stream
    PaStream *stream;

    ldebug("Need audio and not cache");
    // If need audio and not cache
    if (!conf.no_audio && !conf.cache) {
        linfo("Initializing PortAudio...");
        // Initialize PortAudio
        err = Pa_Initialize();
        if (err != paNoError) {
            printf("PortAudio init error(code: %d).\n", err);
            return -20;
        }
        linfo("Getting output device...");
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
        linfo("Opening audio stream...");
        // Open audio stream
        err = Pa_OpenStream(&stream, NULL, &pa_stm_param, a_cdc->sample_rate,
                            AUDIO_BUF_SIZE, paClipOff, NULL, NULL);
        if (err != paNoError) {
            if (atomic_fetch_and(&ncurses_status, 0)) {
                endwin();
            }
            printf("Error when opening audio stream. (code %d)\n", err);
            lfatal(-3, "Error when opening audio stream. (code %d)", err);
        }
    }

    ldebug("not cache");
    if (!conf.cache) {
        linfo("Allocating video channel");
        // Allocate video channel
        conf.video_ch = alloc_channel(10);
        conf.video_ch->drain_callback.callback = video_drain_callback;
        conf.video_ch->add_callback.callback = video_add_callback;
        conf.video_ch->drain_callback.arg = &conf.video_ch_status;
        conf.video_ch->add_callback.arg = &conf.video_ch_status;
    }

    // Video thread
    pthread_t th_v;

    APCache *apc = NULL;

    ldebug("is cache");
    if (conf.cache) {
        apc = apcache_alloc();
        if (!apc) {
            if (atomic_fetch_and(&ncurses_status, 0)) {
                endwin();
            }
            printf("Cannot allocate APCache\n");
            lfatal(-2, "Cannot allocate APCache");
        }
        apc->fps = conf.fps;
        apc->width = conf.width;
        apc->height = conf.height;
        apc->sample_rate = conf.no_audio ? 0 : a_cdc->sample_rate;
        linfo("Opening cache file in w mode...");
        apc->file = fopen(conf.cache, "w");
        if ((err = apcache_create(apc)) != 0) {
            if (atomic_fetch_and(&ncurses_status, 0)) {
                endwin();
            }
            printf("Error when creating apcache file. (code: %d)\n", err);
            lfatal(-2, "Error when creating apcache file. (code: %d)", err);
        }
    }

    ldebug("Ready to play...");

    int image_count = 0, audio_count = 0;
    // While not the end of file.
    while (av_read_frame(fmt_ctxt, pckt) >= 0) {
        // If is video stream.
        if (pckt->stream_index == v_idx) {
            // Send packet to video decoder
            err = avcodec_send_packet(v_cdc, pckt);
            if (err < 0) {
                if (atomic_fetch_and(&ncurses_status, 0)) {
                    endwin();
                }
                printf(
                    "Error when supplying raw packet data as input to video "
                    "decoder. (code: %d)\n",
                    err);
                lfatal(-10,
                       "Error when supplying raw packet data as input to video "
                       "decoder. (code: %d)",
                       err);
            }
            // Read all frames from decoder
            while (1) {
                // Receive frame
                err = avcodec_receive_frame(v_cdc, frame);
                if (err != 0) {
                    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                        break;
                    }
                    if (atomic_fetch_and(&ncurses_status, 0)) {
                        endwin();
                    }
                    printf("Failed when decoding video. (code: %d)\n", err);
                    lfatal(-10, "Failed when decoding video. (code: %d)", err);
                }
                int buf_size = av_image_get_buffer_size(
                    AV_PIX_FMT_GRAY8, conf.width, conf.height, 1);
                // New buf
                uint8_t *buf = (uint8_t *)av_malloc(buf_size);
                // Fill frame_greyscale
                av_image_fill_arrays(
                    frame_greyscale->data, frame_greyscale->linesize, buf,
                    AV_PIX_FMT_GRAY8, conf.width, conf.height, 1);
                // Scale raw image to target image
                sws_scale(sws_ctxt, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, v_cdc->height,
                          frame_greyscale->data, frame_greyscale->linesize);

                if (conf.cache) {
                    APFrame apf;
                    apf.type = APAV_VIDEO;
                    apf.bsize = buf_size;
                    apf.data = buf;
                    if ((err = apcache_write_frame(apc, &apf)) != 0) {
                        if (atomic_fetch_and(&ncurses_status, 0)) {
                            endwin();
                        }
                        printf(
                            "Error when writing video frame to cache file. "
                            "(code: %d)\n",
                            err);
                        lfatal(-10,
                               "Error when writing video frame to cache file. "
                               "(code: %d)",
                               err);
                    }
                    free(buf);
                    clear();
                    printw("Writing frame: %d. (video)\n", image_count);
                    refresh();
                } else {
                    // Add scaled data to video channel
                    add_element(conf.video_ch, buf);
                }
                // Reset the frame fields.
                av_frame_unref(frame_greyscale);
                if (++image_count == 1 && !conf.cache) {
                    linfo("Creating video thread...");
                    pthread_create(&th_v, NULL, play_video, &conf);
                }
            }
        } else if (!conf.no_audio && pckt->stream_index == a_idx) {
            // Send packet to audio decoder
            err = avcodec_send_packet(a_cdc, pckt);
            if (err < 0) {
                if (atomic_fetch_and(&ncurses_status, 0)) {
                    endwin();
                }
                printf(
                    "Error when supplying raw packet data as input to audio "
                    "decoder. (code: %d)\n",
                    err);
                lfatal(-10,
                       "Error when supplying raw packet data as input to audio "
                       "decoder. (code: %d)",
                       err);
            }
            // Read all frames from audio decoder
            while (1) {
                // Receive a frame from audio decoder
                err = avcodec_receive_frame(a_cdc, frame);
                if (err != 0) {
                    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                        break;
                    }
                    if (atomic_fetch_and(&ncurses_status, 0)) {
                        endwin();
                    }
                    printf("Failed when decoding audio. (code: %d)\n", err);
                    lfatal(-10, "Failed when decoding audio. (code: %d)", err);
                }
                // Unref last frame info (important!)
                av_frame_unref(frame_resampled);
                // Initialize some fields in frame_resampled
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                frame_resampled->channel_layout = AV_CH_LAYOUT_STEREO;
#pragma clang diagnostic pop
                frame_resampled->sample_rate = a_cdc->sample_rate;
                frame_resampled->format = AV_SAMPLE_FMT_FLT;
                // Resample audio data
                err = swr_convert_frame(resample_ctxt, frame_resampled, frame);
                if (err != 0) {
                    if (atomic_fetch_and(&ncurses_status, 0)) {
                        endwin();
                    }
                    print_averror(err);
                    lfatal(-10, "Error when resampling audio data. (code: %d)",
                           err);
                }
                if (++audio_count == 1 && !conf.cache) {
                    linfo("Starting audio stream...");
                    Pa_StartStream(stream);
                }
                if (conf.cache) {
                    APFrame apf;
                    apf.type = APAV_AUDIO;
                    apf.bsize = frame_resampled->nb_samples * 2 * sizeof(float);
                    apf.data = frame_resampled->data[0];
                    if ((err = apcache_write_frame(apc, &apf)) != 0) {
                        if (atomic_fetch_and(&ncurses_status, 0)) {
                            endwin();
                        }
                        printf(
                            "Error when writing audio frame to cache file. "
                            "(code: %d)\n",
                            err);
                        lfatal(-10,
                               "Error when writing audio frame to cache file. "
                               "(code: %d)",
                               err);
                    }
                    clear();
                    printw("Writing frame: %d. (audio)\n", image_count);
                    refresh();
                } else {
                    // Write data into stream
                    Pa_WriteStream(stream, frame_resampled->data[0],
                                   frame_resampled->nb_samples);
                }
            }
        }
        // Unref packet
        av_packet_unref(pckt);
        // Unref decode frame
        av_frame_unref(frame);
    }

    pthread_mutex_lock(&conf.video_ch_status.lock);
    if (conf.video_ch_status.has_data)
        pthread_cond_wait(&conf.video_ch_status.drain_cond,
                          &conf.video_ch_status.lock);
    pthread_mutex_unlock(&conf.video_ch_status.lock);

    // Exit ncurses mode
    if (atomic_fetch_and(&ncurses_status, 0)) {
        endwin();
    }
    // Free decode frame
    av_frame_free(&frame);
    // Free greyscale frame
    av_frame_free(&frame_greyscale);
    // Free packet
    av_packet_free(&pckt);
    // Free audio coDec context
    avcodec_free_context(&a_cdc);
    // Free video coDec context
    avcodec_free_context(&v_cdc);
    avformat_close_input(&fmt_ctxt);
    // Free format context
    avformat_free_context(fmt_ctxt);
    // Free image scale context
    sws_freeContext(sws_ctxt);
    // Free audio resample context
    swr_free(&resample_ctxt);
    // Free video channel
    free_channel(conf.video_ch);
    // Free resampled frame
    av_frame_free(&frame_resampled);
    // // To avoid noise at the end of the video
    // usleep(100000);
    Pa_StopStream(stream);
    // Close PortAudio stream
    Pa_CloseStream(stream);
    Pa_Terminate();
    apcache_close(apc);
    apcache_free(&apc);
    if (logger_get_default().file) fclose(logger_get_default().file);
}

/// @brief Handle interrupt (^C)
/// @param _
void handle_int(int _) {
    // Exit ncurses mode
    if (atomic_fetch_and(&ncurses_status, 0)) {
        endwin();
    }
    // Exit program
    exit(0);
}

void handle_exit() {
    // Exit ncurses mode
    if (atomic_fetch_and(&ncurses_status, 0)) {
        endwin();
    }
}

void print_help() {
    printf(
        "ASCII Player v1.0.2\n\
A media player that plays video file in ASCII characters.\n\
Usage: asciiplayer <file> [-h | --help] [-l | --license] [-c | --cache <file>]\n\
                          [-n | —no-audio] [-g | --grayscale <string>] [-r | --reverse]\n\
                          [--log <log file>] [--loglevel <level num>]\n\
\n\
       --help -h            Print this help page\n\
       --license -l         Show license and author info\n\
       --cache -c <file>    Process video into a cached file\n\
                            example: $ asciiplayer video.mp4 --cache cached.apcache\n\
       --grayscale -g <string>\n\
                            Grayscale string (default: \" .:-=+*#%%@\")\n\
       --reverse -r         Reverse grayscale string\n\
       --no-audio -n        Play video without playing audio\n\
       --log <log file>     Path to log file\n\
       --loglevel <level num>\n\
                            Log level number {TRACE: 0, DEBUG: 1, INFO: 2, WARN: 3,\n\
                                              ERROR: 4, FATAL: 5}\n");
}

void print_license() {
    printf(
        "ASCII Player is an open-source software (GNU GPLv3) written in C programming language.\n\
\n\
Author(s):\n\
    Maintainer: Zhendong Chen 221870144 @ Yuxiu College @ Nanjing University\n\
    Developer : Yuqing Tang   221870117 @ Yuxiu College @ Nanjing University\n\
    Developer : Yaqi Dong     221870103 @ Yuxiu College @ Nanjing University\n\
\n\
Special Thanks To:\n\
    GNU Project\n\
    FFmpeg\n\
    PortAudio\n");
}
