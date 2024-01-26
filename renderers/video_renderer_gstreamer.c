/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include "video_renderer.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#define SECOND_IN_NSECS 1000000000UL

static video_renderer_t *renderer = NULL;
static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
// 添加全局变量以跟踪上次保存图片的时间
static GstClockTime last_saved_time = 0;
// 在函数外部定义这些变量，以保持解码器上下文
static AVCodecContext *codecContext = NULL;
static const AVCodec *codec = NULL;
static bool codec_initialized = false;

struct video_renderer_s {
    GstElement *appsrc, *pipeline, *sink;
    GstBus *bus;
};

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip method=clockwise ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip method=counterclockwise ! ");
            break;
        default:
	    g_string_append(launch, "videoflip method=rotate-180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip method=upper-left-diagonal ! ");
            break;
        case RIGHT: 
            g_string_append(launch, "videoflip method=upper-right-diagonal ! ");
            break;
        default:
            g_string_append(launch, "videoflip method=horizontal-flip ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip method=upper-right-diagonal ! ");
            break;
        case RIGHT: 
            g_string_append(launch, "videoflip method=upper-left-diagonal ! ");
            break;
        default:
            g_string_append(launch, "videoflip method=vertical-flip ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip method=counterclockwise ! ");
            break;
        case RIGHT: 
            g_string_append(launch, "videoflip method=clockwise ! ");
            break;
        default:
            break;
        }
        break;
    }
}	

/* apple uses colorimetry=1:3:5:1                                *
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 5 -> GST_VIDEO_TRANSFER_BT709                      *
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    *                            *
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

void  video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                          const char *decoder, const char *converter, const char *videosink, const bool *initial_fullscreen,
                          const bool *video_sync) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    GstClock *clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);

    logger = render_logger;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;

    renderer = calloc(1, sizeof(video_renderer_t));
    g_assert(renderer);

    GString *launch = g_string_new("appsrc name=video_source ! ");
    g_string_append(launch, "queue ! ");
    g_string_append(launch, parser);
    g_string_append(launch, " ! ");
    g_string_append(launch, decoder);
    g_string_append(launch, " ! ");
    g_string_append(launch, converter);
    g_string_append(launch, " ! ");    
    append_videoflip(launch, &videoflip[0], &videoflip[1]);
    g_string_append(launch, videosink);
    g_string_append(launch, " name=video_sink");
    if (*video_sync) {
        g_string_append(launch, " sync=true");
        sync = true;
    } else {
        g_string_append(launch, " sync=false");
        sync = false;
    }
    logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline will be:\n\"%s\"", launch->str);
    renderer->pipeline = gst_parse_launch(launch->str, &error);
    if (error) {
        g_error ("get_parse_launch error (video) :\n %s\n",error->message);
        g_clear_error (&error);
    }
    g_assert (renderer->pipeline);
    gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer->pipeline), clock);

    renderer->appsrc = gst_bin_get_by_name (GST_BIN (renderer->pipeline), "video_source");
    g_assert(renderer->appsrc);
    caps = gst_caps_from_string(h264_caps);
    g_object_set(renderer->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
    g_string_free(launch, TRUE);
    gst_caps_unref(caps);
    gst_object_unref(clock);

    renderer->sink = gst_bin_get_by_name (GST_BIN (renderer->pipeline), "video_sink");
    g_assert(renderer->sink);

    gst_element_set_state (renderer->pipeline, GST_STATE_READY);
    GstState state;
    if (gst_element_get_state (renderer->pipeline, &state, NULL, 0)) {
        if (state == GST_STATE_READY) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer");
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
        }
    } else {
        logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
    }
}

void video_renderer_pause() {
    logger_log(logger, LOGGER_DEBUG, "video renderer paused");
    gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
}

void video_renderer_resume() {
    if (video_renderer_is_paused()) {
        logger_log(logger, LOGGER_DEBUG, "video renderer resumed");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

bool video_renderer_is_paused() {
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    return (state == GST_STATE_PAUSED);
}

void video_renderer_start() {
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    first_packet = true;
}

void video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    GstClockTime current_time = gst_clock_get_time(gst_element_get_clock(renderer->pipeline));
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_ERR, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }

        // 确保 NTP 时间戳有效
        if (*ntp_time < gst_video_pipeline_base_time) {
            logger_log(logger, LOGGER_ERR, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                    ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return;
        }

        // 确保数据有效性
        if (!(data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)) {
            logger_log(logger, LOGGER_ERR, "*** ERROR: Invalid NAL start code");
            return;
        }

        // buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        // g_assert(buffer != NULL);
        // //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        // if (sync) {
        //     GST_BUFFER_PTS(buffer) = pts;
        // }
        // gst_buffer_fill(buffer, 0, data, *data_len);

        // // 更新最后保存时间
        // last_saved_time = current_time;

        // gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);

        if (!codec_initialized) {
            // FFmpeg 解码器和解码上下文初始化
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            if (!codec) {
                logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to find H264 codec");
                return;
            }

            codecContext = avcodec_alloc_context3(codec);
            if (!codecContext) {
                logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to allocate codec context");
                return;
            }

            if (avcodec_open2(codecContext, codec, NULL) < 0) {
                logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to open codec");
                avcodec_free_context(&codecContext);
                return;
            }
            codec_initialized = true;
        }
        

        // 创建一个新的 AVPacket 用于存储解码前的数据
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to allocate packet");
            avcodec_free_context(&codecContext);
            return;
        }

        av_new_packet(pkt, *data_len);
        memcpy(pkt->data, data, *data_len);

        // 创建一个 AVFrame 用于存储解码后的数据
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to allocate frame");
            av_packet_free(&pkt);
            avcodec_free_context(&codecContext);
            return;
        }

        // 解码数据
        int ret = avcodec_send_packet(codecContext, pkt);
        if (ret < 0) {
            logger_log(logger, LOGGER_ERR, "*** ERROR: avcodec_send_packet failed, code %d", ret);
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_close(codecContext);
            avcodec_free_context(&codecContext);
            return;
        }

        if (avcodec_receive_frame(codecContext, frame) == 0) {
            // 检查是否已经过去了至少两秒
            if (current_time - last_saved_time > 2 * GST_SECOND) {
                last_saved_time = current_time;
                // 找到JPEG编码器
                const AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);

                if (!jpegCodec) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to find JPEG codec");
                    // 清理代码
                    return;
                }

                // 创建JPEG编码器的上下文
                AVCodecContext *jpegContext = avcodec_alloc_context3(jpegCodec);
                if (!jpegContext) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to allocate JPEG codec context");
                    // 清理代码
                    return;
                }

                // 配置编码器上下文
                jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;
                jpegContext->height = frame->height;
                jpegContext->width = frame->width;
                jpegContext->time_base = (AVRational){1, 25};
                jpegContext->qmin = 10;
                jpegContext->qmax = 10;

                // 打开编码器
                if (avcodec_open2(jpegContext, jpegCodec, NULL) < 0) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to open JPEG codec");
                    avcodec_free_context(&jpegContext);
                    // 清理代码
                    return;
                }

                // 创建JPEG数据包
                AVPacket *jpegPkt = av_packet_alloc();
                if (!jpegPkt) {
                    // 错误处理
                    return;
                }

                // 发送帧到编码器
                if (avcodec_send_frame(jpegContext, frame) < 0) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to send frame to JPEG codec");
                    av_packet_unref(jpegPkt);
                    avcodec_close(jpegContext);
                    avcodec_free_context(&jpegContext);
                    // 清理代码
                    return;
                }

                // 接收编码后的数据
                if (avcodec_receive_packet(jpegContext, jpegPkt) < 0) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to receive packet from JPEG codec");
                    av_packet_unref(jpegPkt);
                    avcodec_close(jpegContext);
                    avcodec_free_context(&jpegContext);
                    // 清理代码
                    return;
                }

                // 保存JPEG图片
                FILE *jpegFile = fopen("output.jpg", "wb");
                if (!jpegFile) {
                    logger_log(logger, LOGGER_ERR, "*** ERROR: Failed to open file for JPEG output");
                } else {
                    fwrite(jpegPkt->data, 1, jpegPkt->size, jpegFile);
                    fclose(jpegFile);
                }

                // 清理JPEG编码器
                av_packet_free(&jpegPkt);
                avcodec_close(jpegContext);
                avcodec_free_context(&jpegContext);
            }
        }

        // 清理。注意：不要关闭和释放codecContext，除非你确定不再需要它
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
}

void video_renderer_flush() {
}

void video_renderer_stop() {
  if (renderer) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
	    gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
  }   
}

void video_renderer_destroy() {
    if (renderer) {
        GstState state;
        gst_element_get_state(renderer->pipeline, &state, NULL, 0);
        if (state != GST_STATE_NULL) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
	    gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        }
        gst_object_unref(renderer->bus);
        gst_object_unref(renderer->sink);
        gst_object_unref (renderer->appsrc);
        gst_object_unref (renderer->pipeline);
        free (renderer);
        renderer = NULL;

        codec_initialized = false;
        avcodec_close(codecContext);
        avcodec_free_context(&codecContext);
    }
}

/* not implemented for gstreamer */
void video_renderer_update_background(int type) {
}

gboolean gstreamer_pipeline_bus_callback(GstBus *bus, GstMessage *message, gpointer loop) {
    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gboolean flushing;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error: %s", err->message);
        if (strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi OS with (unpatched) GStreamer-1.18.4 needs \"-bt709\" uxplay option");
        }
	g_error_free (err);
        g_free (debug);
        gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
	flushing = TRUE;
        gst_bus_set_flushing(bus, flushing);
 	gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	g_main_loop_quit( (GMainLoop *) loop);
        break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
         logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
	//   g_main_loop_quit( (GMainLoop *) loop);
        break;
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

unsigned int video_renderer_listen(void *loop) {
    return (unsigned int) gst_bus_add_watch(renderer->bus, (GstBusFunc)
                                            gstreamer_pipeline_bus_callback, (gpointer) loop);    
}  
