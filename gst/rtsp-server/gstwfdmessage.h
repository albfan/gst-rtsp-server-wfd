/* GStreamer
 * Copyright (C) <2005,2006> Wim Taymans <wim@fluendo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/*
 * Unless otherwise indicated, Source Code is licensed under MIT license.
 * See further explanation attached in License Statement (distributed in the file
 * LICENSE).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __GST_WFD_MESSAGE_H__
#define __GST_WFD_MESSAGE_H__

#include <glib.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/**
 * GstWFDResult:
 * @GST_WFD_OK: A successful return value
 * @GST_WFD_EINVAL: a function was given invalid parameters
 *
 * Return values for the WFD functions.
 */
typedef enum {
  GST_WFD_OK     = 0,
  GST_WFD_EINVAL = -1
} GstWFDResult;


typedef enum {
  GST_WFD_AUDIO_UNKNOWN 	= 0,
  GST_WFD_AUDIO_LPCM		= (1 << 0),
  GST_WFD_AUDIO_AAC		= (1 << 1),
  GST_WFD_AUDIO_AC3		= (1 << 2)
} GstWFDAudioFormats;

typedef enum {
  GST_WFD_FREQ_UNKNOWN = 0,
  GST_WFD_FREQ_44100 	 = (1 << 0),
  GST_WFD_FREQ_48000	 = (1 << 1)
} GstWFDAudioFreq;

typedef enum {
  GST_WFD_CHANNEL_UNKNOWN = 0,
  GST_WFD_CHANNEL_2 	 	= (1 << 0),
  GST_WFD_CHANNEL_4		= (1 << 1),
  GST_WFD_CHANNEL_6		= (1 << 2),
  GST_WFD_CHANNEL_8		= (1 << 3)
} GstWFDAudioChannels;


typedef enum {
  GST_WFD_VIDEO_UNKNOWN = 0,
  GST_WFD_VIDEO_H264	  = (1 << 0)
} GstWFDVideoCodecs;

typedef enum {
  GST_WFD_VIDEO_CEA_RESOLUTION = 0,
  GST_WFD_VIDEO_VESA_RESOLUTION,
  GST_WFD_VIDEO_HH_RESOLUTION
} GstWFDVideoNativeResolution;

typedef enum {
  GST_WFD_CEA_UNKNOWN		= 0,
  GST_WFD_CEA_640x480P60 	= (1 << 0),
  GST_WFD_CEA_720x480P60	= (1 << 1),
  GST_WFD_CEA_720x480I60	= (1 << 2),
  GST_WFD_CEA_720x576P50	= (1 << 3),
  GST_WFD_CEA_720x576I50	= (1 << 4),
  GST_WFD_CEA_1280x720P30	= (1 << 5),
  GST_WFD_CEA_1280x720P60	= (1 << 6),
  GST_WFD_CEA_1920x1080P30= (1 << 7),
  GST_WFD_CEA_1920x1080P60= (1 << 8),
  GST_WFD_CEA_1920x1080I60= (1 << 9),
  GST_WFD_CEA_1280x720P25	= (1 << 10),
  GST_WFD_CEA_1280x720P50	= (1 << 11),
  GST_WFD_CEA_1920x1080P25= (1 << 12),
  GST_WFD_CEA_1920x1080P50= (1 << 13),
  GST_WFD_CEA_1920x1080I50= (1 << 14),
  GST_WFD_CEA_1280x720P24	= (1 << 15),
  GST_WFD_CEA_1920x1080P24= (1 << 16)
} GstWFDVideoCEAResolution;

typedef enum {
  GST_WFD_VESA_UNKNOWN		= 0,
  GST_WFD_VESA_800x600P30 	= (1 << 0),
  GST_WFD_VESA_800x600P60		= (1 << 1),
  GST_WFD_VESA_1024x768P30	= (1 << 2),
  GST_WFD_VESA_1024x768P60	= (1 << 3),
  GST_WFD_VESA_1152x864P30	= (1 << 4),
  GST_WFD_VESA_1152x864P60	= (1 << 5),
  GST_WFD_VESA_1280x768P30	= (1 << 6),
  GST_WFD_VESA_1280x768P60	= (1 << 7),
  GST_WFD_VESA_1280x800P30	= (1 << 8),
  GST_WFD_VESA_1280x800P60	= (1 << 9),
  GST_WFD_VESA_1360x768P30	= (1 << 10),
  GST_WFD_VESA_1360x768P60	= (1 << 11),
  GST_WFD_VESA_1366x768P30	= (1 << 12),
  GST_WFD_VESA_1366x768P60	= (1 << 13),
  GST_WFD_VESA_1280x1024P30	= (1 << 14),
  GST_WFD_VESA_1280x1024P60	= (1 << 15),
  GST_WFD_VESA_1400x1050P30	= (1 << 16),
  GST_WFD_VESA_1400x1050P60	= (1 << 17),
  GST_WFD_VESA_1440x900P30	= (1 << 18),
  GST_WFD_VESA_1440x900P60	= (1 << 19),
  GST_WFD_VESA_1600x900P30	= (1 << 20),
  GST_WFD_VESA_1600x900P60	= (1 << 21),
  GST_WFD_VESA_1600x1200P30	= (1 << 22),
  GST_WFD_VESA_1600x1200P60	= (1 << 23),
  GST_WFD_VESA_1680x1024P30	= (1 << 24),
  GST_WFD_VESA_1680x1024P60	= (1 << 25),
  GST_WFD_VESA_1680x1050P30	= (1 << 26),
  GST_WFD_VESA_1680x1050P60	= (1 << 27),
  GST_WFD_VESA_1920x1200P30	= (1 << 28),
  GST_WFD_VESA_1920x1200P60	= (1 << 29)
} GstWFDVideoVESAResolution;

typedef enum {
  GST_WFD_HH_UNKNOWN		= 0,
  GST_WFD_HH_800x480P30 	= (1 << 0),
  GST_WFD_HH_800x480P60	= (1 << 1),
  GST_WFD_HH_854x480P30	= (1 << 2),
  GST_WFD_HH_854x480P60	= (1 << 3),
  GST_WFD_HH_864x480P30	= (1 << 4),
  GST_WFD_HH_864x480P60	= (1 << 5),
  GST_WFD_HH_640x360P30	= (1 << 6),
  GST_WFD_HH_640x360P60	= (1 << 7),
  GST_WFD_HH_960x540P30	= (1 << 8),
  GST_WFD_HH_960x540P60	= (1 << 9),
  GST_WFD_HH_848x480P30	= (1 << 10),
  GST_WFD_HH_848x480P60	= (1 << 11)
} GstWFDVideoHHResolution;

typedef enum {
  GST_WFD_H264_UNKNOWN_PROFILE= 0,
  GST_WFD_H264_BASE_PROFILE	= (1 << 0),
  GST_WFD_H264_HIGH_PROFILE	= (1 << 1)
} GstWFDVideoH264Profile;

typedef enum {
  GST_WFD_H264_LEVEL_UNKNOWN = 0,
  GST_WFD_H264_LEVEL_3_1   = (1 << 0),
  GST_WFD_H264_LEVEL_3_2   = (1 << 1),
  GST_WFD_H264_LEVEL_4       = (1 << 2),
  GST_WFD_H264_LEVEL_4_1   = (1 << 3),
  GST_WFD_H264_LEVEL_4_2   = (1 << 4)
} GstWFDVideoH264Level;

typedef enum {
  GST_WFD_HDCP_NONE	= 0,
  GST_WFD_HDCP_2_0	= (1 << 0),
  GST_WFD_HDCP_2_1	= (1 << 1)
} GstWFDHDCPProtection;

typedef enum {
  GST_WFD_SINK_UNKNOWN = -1,
  GST_WFD_SINK_NOT_COUPLED	= 0,
  GST_WFD_SINK_COUPLED,
  GST_WFD_SINK_TEARDOWN_COUPLING,
  GST_WFD_SINK_RESERVED
} GstWFDCoupledSinkStatus;

typedef enum {
  GST_WFD_TRIGGER_UNKNOWN = 0,
  GST_WFD_TRIGGER_SETUP,
  GST_WFD_TRIGGER_PAUSE,
  GST_WFD_TRIGGER_TEARDOWN,
  GST_WFD_TRIGGER_PLAY
} GstWFDTrigger;

typedef enum {
  GST_WFD_RTSP_TRANS_UNKNOWN =  0,
  GST_WFD_RTSP_TRANS_RTP     = (1 << 0),
  GST_WFD_RTSP_TRANS_RDT     = (1 << 1)
} GstWFDRTSPTransMode;

typedef enum {
  GST_WFD_RTSP_PROFILE_UNKNOWN =  0,
  GST_WFD_RTSP_PROFILE_AVP     = (1 << 0),
  GST_WFD_RTSP_PROFILE_SAVP    = (1 << 1)
} GstWFDRTSPProfile;

typedef enum {
  GST_WFD_RTSP_LOWER_TRANS_UNKNOWN   = 0,
  GST_WFD_RTSP_LOWER_TRANS_UDP       = (1 << 0),
  GST_WFD_RTSP_LOWER_TRANS_UDP_MCAST = (1 << 1),
  GST_WFD_RTSP_LOWER_TRANS_TCP       = (1 << 2),
  GST_WFD_RTSP_LOWER_TRANS_HTTP      = (1 << 3)
} GstWFDRTSPLowerTrans;

typedef enum {
  GST_WFD_PRIMARY_SINK   = 0,
  GST_WFD_SECONDARY_SINK
} GstWFDSinkType;

typedef enum {
  GST_WFD_UIBC_INPUT_CAT_UNKNOWN   = 0,
  GST_WFD_UIBC_INPUT_CAT_GENERIC   = (1 << 0),
  GST_WFD_UIBC_INPUT_CAT_HIDC      = (1 << 1),
} GstWFDUibcinput_cat;

typedef enum {
  GST_WFD_UIBC_INPUT_TYPE_UNKNOWN        = 0,
  GST_WFD_UIBC_INPUT_TYPE_KEYBOARD       = (1 << 0),
  GST_WFD_UIBC_INPUT_TYPE_MOUSE          = (1 << 1),
  GST_WFD_UIBC_INPUT_TYPE_SINGLETOUCH    = (1 << 2),
  GST_WFD_UIBC_INPUT_TYPE_MULTITOUCH     = (1 << 3),
  GST_WFD_UIBC_INPUT_TYPE_JOYSTICK       = (1 << 4),
  GST_WFD_UIBC_INPUT_TYPE_CAMERA         = (1 << 5),
  GST_WFD_UIBC_INPUT_TYPE_GESTURE        = (1 << 6),
  GST_WFD_UIBC_INPUT_TYPE_REMOTECONTROL  = (1 << 7)
} GstWFDUibcinp_type;

typedef enum {
  GST_WFD_UIBC_INPUT_PATH_UNKNOWN   = 0,
  GST_WFD_UIBC_INPUT_PATH_INFRARED  = (1 << 0),
  GST_WFD_UIBC_INPUT_PATH_USB       = (1 << 1),
  GST_WFD_UIBC_INPUT_PATH_BT        = (1 << 2),
  GST_WFD_UIBC_INPUT_PATH_ZIGBEE    = (1 << 3),
  GST_WFD_UIBC_INPUT_PATH_WIFI      = (1 << 4),
  GST_WFD_UIBC_INPUT_PATH_NOSP      = (1 << 5)
} GstWFDUibcinp_path;

typedef enum {
  GST_WFD_CONNECTOR_VGA           = 0,
  GST_WFD_CONNECTOR_S,
  GST_WFD_CONNECTOR_COMPOSITE,
  GST_WFD_CONNECTOR_COMPONENT,
  GST_WFD_CONNECTOR_DVI,
  GST_WFD_CONNECTOR_HDMI,
  GST_WFD_CONNECTOR_LVDS,
  GST_WFD_CONNECTOR_RESERVED_7,
  GST_WFD_CONNECTOR_JAPANESE_D,
  GST_WFD_CONNECTOR_SDI,
  GST_WFD_CONNECTOR_DP,
  GST_WFD_CONNECTOR_RESERVED_11,
  GST_WFD_CONNECTOR_UDI,
  GST_WFD_CONNECTOR_NO           = 254,
  GST_WFD_CONNECTOR_PHYSICAL     = 255
} GstWFDConnector;


typedef struct {
  gchar	*audio_format;
  guint32 modes;
  guint latency;
} GstWFDAudioCodec;

typedef struct {
  guint	count;
  GstWFDAudioCodec *list;
} GstWFDAudioCodeclist;


typedef struct {
  guint CEA_Support;
  guint VESA_Support;
  guint HH_Support;
  guint latency;
  guint min_slice_size;
  guint slice_enc_params;
  guint frame_rate_control_support;
} GstWFDVideoH264MiscParams;

typedef struct {
  guint profile;
  guint level;
  guint max_hres;
  guint max_vres;
  GstWFDVideoH264MiscParams misc_params;
} GstWFDVideoH264Codec;

typedef struct {
  guint	native;
  guint preferred_display_mode_supported;
  GstWFDVideoH264Codec H264_codec;
} GstWFDVideoCodec;

typedef struct {
  guint			count;
  GstWFDVideoCodec *list;
} GstWFDVideoCodeclist;

typedef struct {
  guint video_3d_capability;
  guint latency;
  guint min_slice_size;
  guint slice_enc_params;
  guint frame_rate_control_support;
} GstWFD3DVideoH264MiscParams;

typedef struct {
  guint profile;
  guint level;
  GstWFD3DVideoH264MiscParams misc_params;
  guint max_hres;
  guint max_vres;
} GstWFD3DVideoH264Codec;

typedef struct {
  guint native;
  guint preferred_display_mode_supported;
  GstWFD3DVideoH264Codec H264_codec;
} GstWFD3dCapList;

typedef struct {
  guint			count;
  GstWFD3dCapList *list;
} GstWFD3DFormats;

typedef struct {
  gchar *hdcpversion;
  gchar *TCPPort;
} GstWFDHdcp2Spec;

typedef struct {
  GstWFDHdcp2Spec *hdcp2_spec;
} GstWFDContentProtection;

typedef struct {
  guint edid_supported;
  guint edid_block_count;
  gchar *edid_payload;
} GstWFDDisplayEdid;


typedef struct {
  guint status;
  gchar *sink_address;
} GstWFDCoupled_sink_cap;

typedef struct {
  GstWFDCoupled_sink_cap *coupled_sink_cap;
} GstWFDCoupledSink;

typedef struct {
  gchar *wfd_trigger_method;
} GstWFDTriggerMethod;

typedef struct {
  gchar *wfd_url0;
  gchar *wfd_url1;
} GstWFDPresentationUrl;

typedef struct {
  gchar *profile;
  guint32 rtp_port0;
  guint32 rtp_port1;
  gchar *mode;
} GstWFDClientRtpPorts;

typedef struct {
 gchar *destination;
} GstWFDRoute;

typedef struct {
  gboolean I2CPresent;
  guint32 I2C_port;
} GstWFDI2C;

typedef struct {
  guint64 PTS;
  guint64 DTS;
} GstWFDAVFormatChangeTiming;

typedef struct {
  gboolean displaymodesupported;
  guint64 p_clock;
  guint32 H;
  guint32 HB;
  guint32 HSPOL_HSOFF;
  guint32 HSW;
  guint32 V;
  guint32 VB;
  guint32 VSPOL_VSOFF;
  guint32 VSW;
  guint VBS3D;
  guint R;
  guint V2d_s3d_modes;
  guint P_depth;
  GstWFDVideoH264Codec H264_codec;
} GstWFDPreferredDisplayMode;

typedef struct {
  guint32 input_cat;
} GstWFDInputCategoryList;

typedef struct {
  guint32 inp_type;
} GstWFDGenericCategoryList;

typedef struct _detailed_cap detailed_cap;

typedef struct {
  GstWFDUibcinp_type inp_type;
  GstWFDUibcinp_path inp_path;
} GstWFDHIDCTypePathPair;

struct _detailed_cap {
  GstWFDHIDCTypePathPair p;
  detailed_cap *next;
};

typedef struct {
  guint cap_count;
  detailed_cap *next;
} GstWFDHIDCCategoryList;

typedef struct {
  gboolean uibcsupported;
  GstWFDInputCategoryList input_category_list;
  GstWFDGenericCategoryList generic_cap_list;
  GstWFDHIDCCategoryList hidc_cap_list;
  guint32 tcp_port;
} GstWFDUibcCapability;

typedef struct {
  gboolean uibc_setting;
} GstWFDUibcSetting;

typedef struct {
  gboolean standby_resume_cap;
} GstWFDStandbyResumeCapability;

typedef struct {
  gboolean wfd_standby;
} GstWFDStandby;

typedef struct {
  gboolean supported;
  gint32 connector_type;
} GstWFDConnectorType;

typedef struct {
  gboolean idr_request;
} GstWFDIdrRequest;

/**
 * GstWFDMessage:
 * @version: the protocol version
 * @origin: owner/creator and session identifier
 * @session_name: session name
 * @information: session information
 * @uri: URI of description
 * @emails: array of #gchar with email addresses
 * @phones: array of #gchar with phone numbers
 * @connection: connection information for the session
 * @bandwidths: array of #GstWFDBandwidth with bandwidth information
 * @times: array of #GstWFDTime with time descriptions
 * @zones: array of #GstWFDZone with time zone adjustments
 * @key: encryption key
 * @attributes: array of #GstWFDAttribute with session attributes
 * @medias: array of #GstWFDMedia with media descriptions
 *
 * The contents of the WFD message.
 */
typedef struct {
  GstWFDAudioCodeclist *audio_codecs;
  GstWFDVideoCodeclist *video_formats;
  GstWFD3DFormats *video_3d_formats;
  GstWFDContentProtection *content_protection;
  GstWFDDisplayEdid *display_edid;
  GstWFDCoupledSink *coupled_sink;
  GstWFDTriggerMethod *trigger_method;
  GstWFDPresentationUrl *presentation_url;
  GstWFDClientRtpPorts *client_rtp_ports;
  GstWFDRoute *route;
  GstWFDI2C *I2C;
  GstWFDAVFormatChangeTiming *av_format_change_timing;
  GstWFDPreferredDisplayMode *preferred_display_mode;
  GstWFDUibcCapability *uibc_capability;
  GstWFDUibcSetting *uibc_setting;
  GstWFDStandbyResumeCapability *standby_resume_capability;
  GstWFDStandby *standby;
  GstWFDConnectorType *connector_type;
  GstWFDIdrRequest *idr_request;
} GstWFDMessage;

GType                   gst_wfd_message_get_type            (void);

#define GST_TYPE_WFD_MESSAGE           (gst_wfd_message_get_type())
#define GST_WFD_MESSAGE_CAST(object)   ((GstWFDMessage *)(object))
#define GST_WFD_MESSAGE(object)        (GST_WFD_MESSAGE_CAST(object))

/* Session descriptions */
GstWFDResult            gst_wfd_message_new                 (GstWFDMessage **msg);
GstWFDResult            gst_wfd_message_init                (GstWFDMessage *msg);
GstWFDResult            gst_wfd_message_uninit              (GstWFDMessage *msg);
GstWFDResult            gst_wfd_message_free                (GstWFDMessage *msg);
GstWFDResult            gst_wfd_message_copy                (const GstWFDMessage *msg, GstWFDMessage **copy);

GstWFDResult            gst_wfd_message_parse_buffer        (const guint8 *data, guint size, GstWFDMessage *msg);
gchar*                  gst_wfd_message_as_text             (const GstWFDMessage *msg);
gchar*                  gst_wfd_message_param_names_as_text (const GstWFDMessage *msg);
GstWFDResult            gst_wfd_message_dump                (const GstWFDMessage *msg);


GstWFDResult gst_wfd_message_set_supported_audio_format(GstWFDMessage *msg,
                                        GstWFDAudioFormats a_codec,
                                        guint a_freq, guint a_channels,
                                        guint a_bitwidth, guint32 a_latency);

GstWFDResult gst_wfd_message_set_prefered_audio_format(GstWFDMessage *msg,
                                        GstWFDAudioFormats a_codec,
                                        GstWFDAudioFreq a_freq,
                                        GstWFDAudioChannels a_channels,
                                        guint a_bitwidth, guint32 a_latency);

GstWFDResult gst_wfd_message_get_supported_audio_format (GstWFDMessage *msg,
                                        guint *a_codec,
                                        guint *a_freq,
                                        guint *a_channels,
                                        guint *a_bitwidth,
                                        guint32 *a_latency);

GstWFDResult gst_wfd_message_get_prefered_audio_format (GstWFDMessage *msg,
                                        GstWFDAudioFormats *a_codec,
                                        GstWFDAudioFreq *a_freq,
                                        GstWFDAudioChannels *a_channels,
                                        guint *a_bitwidth, guint32 *a_latency);

GstWFDResult gst_wfd_message_set_supported_video_format (GstWFDMessage *msg,
                                        GstWFDVideoCodecs v_codec,
                                        GstWFDVideoNativeResolution v_native,
                                        guint64 v_native_resolution,
                                        guint64 v_cea_resolution,
                                        guint64 v_vesa_resolution,
                                        guint64 v_hh_resolution,
                                        guint v_profile,
                                        guint v_level,
                                        guint32 v_latency,
                                        guint32 v_max_height,
                                        guint32 v_max_width,
                                        guint32 min_slice_size,
                                        guint32 slice_enc_params,
                                        guint frame_rate_control);

GstWFDResult gst_wfd_message_set_prefered_video_format(GstWFDMessage *msg,
                                        GstWFDVideoCodecs v_codec,
                                        GstWFDVideoNativeResolution v_native,
                                        guint64 v_native_resolution,
                                        GstWFDVideoCEAResolution v_cea_resolution,
                                        GstWFDVideoVESAResolution v_vesa_resolution,
                                        GstWFDVideoHHResolution v_hh_resolution,
                                        GstWFDVideoH264Profile v_profile,
                                        GstWFDVideoH264Level v_level,
                                        guint32 v_latency,
                                        guint32 v_max_height,
                                        guint32 v_max_width,
                                        guint32 min_slice_size,
                                        guint32 slice_enc_params,
                                        guint frame_rate_control);

GstWFDResult gst_wfd_message_get_supported_video_format(GstWFDMessage *msg,
                                        GstWFDVideoCodecs *v_codec,
                                        GstWFDVideoNativeResolution *v_native,
                                        guint64 *v_native_resolution,
                                        guint64 *v_cea_resolution,
                                        guint64 *v_vesa_resolution,
                                        guint64 *v_hh_resolution,
                                        guint *v_profile,
                                        guint *v_level,
                                        guint32 *v_latency,
                                        guint32 *v_max_height,
                                        guint32 *v_max_width,
                                        guint32 *min_slice_size,
                                        guint32 *slice_enc_params,
                                        guint *frame_rate_control);

GstWFDResult gst_wfd_message_get_prefered_video_format(GstWFDMessage *msg,
                                        GstWFDVideoCodecs *v_codec,
                                        GstWFDVideoNativeResolution *v_native,
                                        guint64 *v_native_resolution,
                                        GstWFDVideoCEAResolution *v_cea_resolution,
                                        GstWFDVideoVESAResolution *v_vesa_resolution,
                                        GstWFDVideoHHResolution *v_hh_resolution,
                                        GstWFDVideoH264Profile *v_profile,
                                        GstWFDVideoH264Level *v_level,
                                        guint32 *v_latency,
                                        guint32 *v_max_height,
                                        guint32 *v_max_width,
                                        guint32 *min_slice_size,
                                        guint32 *slice_enc_params,
                                        guint *frame_rate_control);

GstWFDResult gst_wfd_message_set_display_edid (GstWFDMessage *msg,
                                        gboolean edid_supported,
                                        guint32 edid_blockcount,
                                        gchar *edid_playload);

GstWFDResult gst_wfd_message_get_display_edid (GstWFDMessage *msg,
                                        gboolean *edid_supported,
                                        guint32 *edid_blockcount,
                                        gchar **edid_playload);

GstWFDResult gst_wfd_message_set_contentprotection_type (GstWFDMessage *msg,
                                        GstWFDHDCPProtection hdcpversion,
                                        guint32 TCPPort);

GstWFDResult gst_wfd_message_get_contentprotection_type (GstWFDMessage *msg,
                                        GstWFDHDCPProtection *hdcpversion,
                                        guint32 *TCPPort);

GstWFDResult gst_wfd_messge_set_prefered_rtp_ports (GstWFDMessage *msg,
                                        GstWFDRTSPTransMode trans,
                                        GstWFDRTSPProfile profile,
                                        GstWFDRTSPLowerTrans lowertrans,
                                        guint32 rtp_port0,
                                        guint32 rtp_port1);

GstWFDResult gst_wfd_message_get_prefered_rtp_ports (GstWFDMessage *msg,
                                        GstWFDRTSPTransMode *trans,
                                        GstWFDRTSPProfile *profile,
                                        GstWFDRTSPLowerTrans *lowertrans,
                                        guint32 *rtp_port0,
                                        guint32 *rtp_port1);
G_END_DECLS

#endif /* __GST_WFD_MESSAGE_H__ */
