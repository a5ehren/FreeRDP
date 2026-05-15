/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 Bitmap Compression
 *
 * Copyright 2026 Armin Novak <anovak@thincast.com>
 * Copyright 2026 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/codec/av1.h>
#include <freerdp/log.h>
#include <freerdp/primitives.h>

#include <winpr/assert.h>
#include <winpr/crt.h>

#if defined(WITH_LIBAOM)
#include <aom/aom.h>

#include <aom/aom_decoder.h>
#include <aom/aom_encoder.h>

#include <aom/aom_image.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>
#endif

#if defined(WITH_LIBSVTAV1)
#include <EbSvtAv1.h>
#include <EbSvtAv1Enc.h>
#endif

#if defined(WITH_LIBDAV1D)
#include <errno.h>

#include <dav1d/dav1d.h>
#include <dav1d/data.h>
#include <dav1d/picture.h>
#endif

#if defined(WITH_LIBYUV)
#include <libyuv.h>
#endif

#define TAG FREERDP_TAG("codec.av1")

typedef enum
{
	FREERDP_AV1_BACKEND_NONE,
	FREERDP_AV1_BACKEND_AOM,
	FREERDP_AV1_BACKEND_SVTAV1,
	FREERDP_AV1_BACKEND_DAV1D
} FREERDP_AV1_BACKEND;

struct S_FREERDP_AV1_CONTEXT
{
	wLog* log;
	bool encoder;
	bool initialized;
	FREERDP_AV1_BACKEND backend;

	UINT32 profile;
	UINT32 ratecontrol;
	UINT32 bitrate;
	UINT32 usagetype;
	UINT32 width;
	UINT32 height;
	UINT64 framecount;

	BYTE* yuvdata[3];
	UINT32 yuvStride[3];
	UINT32 yuvHeight[3];

	BYTE* bitstream;
	UINT32 bitstreamSize;
	UINT32 bitstreamCapacity;

#if defined(WITH_LIBAOM)
	aom_codec_ctx_t aom;
	aom_codec_enc_cfg_t aomEcfg;
	aom_codec_dec_cfg_t aomDcfg;
	aom_enc_frame_flags_t aomEflags;
	aom_codec_flags_t aomFlags;
#endif

#if defined(WITH_LIBSVTAV1)
	EbComponentType* svt;
	bool svtInitialized;
	EbSvtAv1EncConfiguration svtCfg;
	EbBufferHeaderType svtInput;
	EbSvtIOFormat svtPicture;
#endif

#if defined(WITH_LIBDAV1D)
	Dav1dContext* dav1d;
#endif
};

WINPR_ATTR_NODISCARD
static BOOL allocate_h264_metablock(UINT32 QP, RECTANGLE_16* rectangles,
                                    RDPGFX_H264_METABLOCK* meta, size_t count)
{
	/* [MS-RDPEGFX] 2.2.4.4.2 RDPGFX_AVC420_QUANT_QUALITY */
	if (!meta || (QP > UINT8_MAX) || (count > UINT32_MAX))
	{
		free(rectangles);
		return FALSE;
	}

	meta->regionRects = rectangles;
	if (count == 0)
		return TRUE;

	meta->quantQualityVals = calloc(count, sizeof(RDPGFX_H264_QUANT_QUALITY));

	if (!meta->quantQualityVals || !meta->regionRects)
	{
		free(meta->quantQualityVals);
		free(meta->regionRects);
		*meta = (RDPGFX_H264_METABLOCK)WINPR_C_ARRAY_INIT;
		return FALSE;
	}

	meta->numRegionRects = (UINT32)count;
	for (size_t x = 0; x < count; x++)
	{
		RDPGFX_H264_QUANT_QUALITY* cur = &meta->quantQualityVals[x];
		cur->qp = (UINT8)QP;

		/* qpVal bit 6 and 7 are flags, so mask them out here.
		 * qualityVal is [0-100] so 100 - qpVal [0-64] is always in range */
		cur->qualityVal = 100 - (QP & 0x3F);
	}
	return TRUE;
}

WINPR_ATTR_NODISCARD
static const char* av1_backend_name(FREERDP_AV1_BACKEND backend)
{
	switch (backend)
	{
		case FREERDP_AV1_BACKEND_AOM:
			return "libaom";
		case FREERDP_AV1_BACKEND_SVTAV1:
			return "SVT-AV1";
		case FREERDP_AV1_BACKEND_DAV1D:
			return "dav1d";
		case FREERDP_AV1_BACKEND_NONE:
		default:
			return "none";
	}
}

WINPR_ATTR_NODISCARD
static UINT32 av1_default_usage(void)
{
#if defined(WITH_LIBAOM)
	return AOM_USAGE_REALTIME;
#else
	return 1;
#endif
}

WINPR_ATTR_NODISCARD
static BOOL av1_validate_ratecontrol(FREERDP_AV1_CONTEXT* av1, UINT32 value)
{
	switch (value)
	{
		case FREERDP_AV1_VBR:
		case FREERDP_AV1_CBR:
		case FREERDP_AV1_CQ:
		case FREERDP_AV1_Q:
			return TRUE;
		default:
			WLog_Print(av1->log, WLOG_WARN,
			           "Unknown FREERDP_AV1_CONTEXT_OPTION_RATECONTROL value [0x%08" PRIx32 "]",
			           value);
			return FALSE;
	}
}

static void av1_free_yuv(FREERDP_AV1_CONTEXT* av1)
{
	for (size_t x = 0; x < ARRAYSIZE(av1->yuvdata); x++)
	{
		winpr_aligned_free(av1->yuvdata[x]);
		av1->yuvdata[x] = nullptr;
		av1->yuvStride[x] = 0;
		av1->yuvHeight[x] = 0;
	}
}

WINPR_ATTR_NODISCARD
static BOOL av1_allocate_yuv(FREERDP_AV1_CONTEXT* av1, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);

	const BOOL i420 = av1->profile == 0;
	UINT32 strides[3] = { width, i420 ? (width + 1) / 2 : width, i420 ? (width + 1) / 2 : width };
	const UINT32 heights[3] = { height, i420 ? (height + 1) / 2 : height,
		                        i420 ? (height + 1) / 2 : height };

	for (size_t x = 0; x < ARRAYSIZE(strides); x++)
	{
		if (strides[x] % 16 != 0)
			strides[x] += 16 - (strides[x] % 16);
	}

	BYTE* data[3] = WINPR_C_ARRAY_INIT;
	for (size_t x = 0; x < ARRAYSIZE(data); x++)
	{
		if ((strides[x] == 0) || (heights[x] == 0))
			goto fail;

		data[x] = winpr_aligned_malloc(1ull * strides[x] * heights[x], 64);
		if (!data[x])
			goto fail;
	}

	av1_free_yuv(av1);
	for (size_t x = 0; x < ARRAYSIZE(data); x++)
	{
		av1->yuvdata[x] = data[x];
		av1->yuvStride[x] = strides[x];
		av1->yuvHeight[x] = heights[x];
	}
	return TRUE;

fail:
	for (size_t x = 0; x < ARRAYSIZE(data); x++)
		winpr_aligned_free(data[x]);
	return FALSE;
}

WINPR_ATTR_NODISCARD
static BOOL av1_bitstream_reserve(FREERDP_AV1_CONTEXT* av1, size_t size)
{
	WINPR_ASSERT(av1);

	if (size > UINT32_MAX)
		return FALSE;
	if (size <= av1->bitstreamCapacity)
		return TRUE;

	BYTE* tmp = realloc(av1->bitstream, size);
	if (!tmp)
		return FALSE;
	av1->bitstream = tmp;
	av1->bitstreamCapacity = (UINT32)size;
	return TRUE;
}

WINPR_ATTR_NODISCARD
static BOOL av1_bitstream_append(FREERDP_AV1_CONTEXT* av1, const BYTE* data, size_t size)
{
	WINPR_ASSERT(av1);

	if (size == 0)
		return TRUE;
	if (!data)
		return FALSE;

	const size_t oldSize = av1->bitstreamSize;
	const size_t newSize = oldSize + size;
	if ((newSize < oldSize) || !av1_bitstream_reserve(av1, newSize))
		return FALSE;

	memcpy(&av1->bitstream[oldSize], data, size);
	av1->bitstreamSize = (UINT32)newSize;
	return TRUE;
}

WINPR_ATTR_NODISCARD
static pstatus_t av1_convert_rgb_to_yuv(FREERDP_AV1_CONTEXT* av1, const BYTE* pSrcData,
                                        DWORD SrcFormat, UINT32 nSrcStep, UINT32 width,
                                        UINT32 height)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(pSrcData);

	const prim_size_t roi = { .width = width, .height = height };

#if defined(WITH_LIBYUV)
	WINPR_UNUSED(SrcFormat);
	switch (av1->profile)
	{
		case 0:
			return ARGBToI420(pSrcData, nSrcStep, av1->yuvdata[0], av1->yuvStride[0],
			                  av1->yuvdata[1], av1->yuvStride[1], av1->yuvdata[2],
			                  WINPR_ASSERTING_INT_CAST(int, av1->yuvStride[2]),
			                  WINPR_ASSERTING_INT_CAST(int, roi.width),
			                  WINPR_ASSERTING_INT_CAST(int, roi.height));
		case 1:
			return ARGBToI444(pSrcData, nSrcStep, av1->yuvdata[0], av1->yuvStride[0],
			                  av1->yuvdata[1], av1->yuvStride[1], av1->yuvdata[2],
			                  WINPR_ASSERTING_INT_CAST(int, av1->yuvStride[2]),
			                  WINPR_ASSERTING_INT_CAST(int, roi.width),
			                  WINPR_ASSERTING_INT_CAST(int, roi.height));
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unsupported AV1 profile %" PRIu32, av1->profile);
			return -1;
	}
#else
	primitives_t* primitives = primitives_get();
	if (!primitives)
	{
		WLog_Print(av1->log, WLOG_ERROR, "primitives_get(): nullptr");
		return -1;
	}

	switch (av1->profile)
	{
		case 0:
			return primitives->RGBToYUV420_8u_P3AC4R(pSrcData, SrcFormat, nSrcStep, av1->yuvdata,
			                                         av1->yuvStride, &roi);
		case 1:
			return primitives->RGBToI444_8u(pSrcData, SrcFormat, nSrcStep, av1->yuvdata,
			                                av1->yuvStride, &roi);
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unsupported AV1 profile %" PRIu32, av1->profile);
			return -1;
	}
#endif
}

WINPR_ATTR_NODISCARD
static pstatus_t av1_convert_yuv_to_rgb(FREERDP_AV1_CONTEXT* av1, UINT32 profile,
                                        const BYTE* planes[3], const UINT32 strides[3],
                                        BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep,
                                        UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(planes);
	WINPR_ASSERT(strides);
	WINPR_ASSERT(pDstData);

#if defined(WITH_LIBYUV)
	WINPR_UNUSED(DstFormat);
	switch (profile)
	{
		case 0:
			return J420ToARGB(planes[0], strides[0], planes[1], strides[1], planes[2], strides[2],
			                  pDstData, nDstStep, width, height);
		case 1:
			return J444ToARGB(planes[0], strides[0], planes[1], strides[1], planes[2], strides[2],
			                  pDstData, nDstStep, width, height);
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unsupported AV1 profile %" PRIu32, profile);
			return -1;
	}
#else
	primitives_t* primitives = primitives_get();
	if (!primitives)
	{
		WLog_Print(av1->log, WLOG_ERROR, "primitives_get(): nullptr");
		return -1;
	}

	const prim_size_t roi = { .width = width, .height = height };

	switch (profile)
	{
		case 0:
			return primitives->YUV420ToRGB_8u_P3AC4R(planes, strides, pDstData, nDstStep, DstFormat,
			                                         &roi);
		case 1:
			return primitives->YUV444ToRGB_8u_P3AC4R(planes, strides, pDstData, nDstStep, DstFormat,
			                                         &roi);
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unsupported AV1 profile %" PRIu32, profile);
			return -1;
	}
#endif
}

WINPR_ATTR_NODISCARD
static FREERDP_AV1_BACKEND av1_select_backend(const FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);

	if (av1->encoder)
	{
		switch (av1->profile)
		{
			case 0:
#if defined(WITH_LIBSVTAV1)
				return FREERDP_AV1_BACKEND_SVTAV1;
#elif defined(WITH_LIBAOM)
				return FREERDP_AV1_BACKEND_AOM;
#else
				return FREERDP_AV1_BACKEND_NONE;
#endif
			case 1:
#if defined(WITH_LIBAOM)
				return FREERDP_AV1_BACKEND_AOM;
#else
				return FREERDP_AV1_BACKEND_NONE;
#endif
			default:
				return FREERDP_AV1_BACKEND_NONE;
		}
	}

#if defined(WITH_LIBDAV1D)
	return FREERDP_AV1_BACKEND_DAV1D;
#elif defined(WITH_LIBAOM)
	return FREERDP_AV1_BACKEND_AOM;
#else
	return FREERDP_AV1_BACKEND_NONE;
#endif
}

WINPR_ATTR_NODISCARD
static FREERDP_AV1_BACKEND av1_fallback_backend(const FREERDP_AV1_CONTEXT* av1,
                                                FREERDP_AV1_BACKEND failed)
{
	WINPR_ASSERT(av1);

	if (av1->encoder)
	{
		if ((failed == FREERDP_AV1_BACKEND_SVTAV1) && (av1->profile == 0))
		{
#if defined(WITH_LIBAOM)
			return FREERDP_AV1_BACKEND_AOM;
#endif
		}
		return FREERDP_AV1_BACKEND_NONE;
	}

	if (failed == FREERDP_AV1_BACKEND_DAV1D)
	{
#if defined(WITH_LIBAOM)
		return FREERDP_AV1_BACKEND_AOM;
#endif
	}
	return FREERDP_AV1_BACKEND_NONE;
}

#if defined(WITH_LIBAOM)
WINPR_ATTR_NODISCARD
static BOOL av1_aom_init(FREERDP_AV1_CONTEXT* av1, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);

	aom_codec_iface_t* iface = av1->encoder ? aom_codec_av1_cx() : aom_codec_av1_dx();
	if (!iface)
	{
		WLog_Print(av1->log, WLOG_ERROR, "aom_codec_av1_%s() nullptr", av1->encoder ? "cx" : "dx");
		return FALSE;
	}

	if (av1->encoder)
	{
		aom_codec_err_t rc = aom_codec_enc_config_default(iface, &av1->aomEcfg, av1->usagetype);
		if (rc != AOM_CODEC_OK)
		{
			WLog_Print(av1->log, WLOG_ERROR, "aom_codec_enc_config_default() %s",
			           aom_codec_err_to_string(rc));
			return FALSE;
		}

		av1->aomEcfg.g_w = width;
		av1->aomEcfg.g_h = height;
		av1->aomEcfg.g_profile = av1->profile;
		av1->aomEcfg.rc_end_usage = av1->ratecontrol;
		av1->aomEcfg.rc_target_bitrate = av1->bitrate;

		rc = aom_codec_enc_init(&av1->aom, iface, &av1->aomEcfg, av1->aomFlags);
		if (rc != AOM_CODEC_OK)
		{
			WLog_Print(av1->log, WLOG_WARN, "aom_codec_enc_init: %s", aom_codec_err_to_string(rc));
			return FALSE;
		}

		av1->aomEflags = 0;
	}
	else
	{
		av1->aomDcfg.w = width;
		av1->aomDcfg.h = height;
		av1->aomDcfg.allow_lowbitdepth = 1;

		const aom_codec_err_t rc =
		    aom_codec_dec_init(&av1->aom, iface, &av1->aomDcfg, av1->aomFlags);
		if (rc != AOM_CODEC_OK)
		{
			WLog_Print(av1->log, WLOG_WARN, "aom_codec_dec_init: %s", aom_codec_err_to_string(rc));
			return FALSE;
		}
	}

	return TRUE;
}

static void av1_aom_uninit(FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);

	const aom_codec_err_t rc = aom_codec_destroy(&av1->aom);
	if (rc != AOM_CODEC_OK)
		WLog_Print(av1->log, WLOG_WARN, "aom_codec_destroy: %s", aom_codec_err_to_string(rc));
}
#endif

#if defined(WITH_LIBSVTAV1)
WINPR_ATTR_NODISCARD
static uint8_t av1_svt_ratecontrol(UINT32 ratecontrol)
{
	switch (ratecontrol)
	{
		case FREERDP_AV1_VBR:
		case FREERDP_AV1_CBR:
			/* SVT-AV1 3.1.2 does not support VBR with low-delay prediction. */
			return SVT_AV1_RC_MODE_CBR;
		case FREERDP_AV1_CQ:
		case FREERDP_AV1_Q:
		default:
			return SVT_AV1_RC_MODE_CQP_OR_CRF;
	}
}

WINPR_ATTR_NODISCARD
static BOOL av1_svt_init(FREERDP_AV1_CONTEXT* av1, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(av1->encoder);

	EbErrorType err = svt_av1_enc_init_handle(&av1->svt, &av1->svtCfg);
	if (err != EB_ErrorNone)
	{
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_init_handle: 0x%08" PRIx32, (UINT32)err);
		return FALSE;
	}

	av1->svtCfg.source_width = width;
	av1->svtCfg.source_height = height;
	av1->svtCfg.forced_max_frame_width = width;
	av1->svtCfg.forced_max_frame_height = height;
	av1->svtCfg.frame_rate_numerator = 60;
	av1->svtCfg.frame_rate_denominator = 1;
	av1->svtCfg.encoder_bit_depth = 8;
	av1->svtCfg.encoder_color_format = EB_YUV420;
	av1->svtCfg.profile = MAIN_PROFILE;
	av1->svtCfg.enc_mode = 13;
#if defined(SVT_AV1_PRED_LOW_DELAY_B)
	av1->svtCfg.pred_structure = SVT_AV1_PRED_LOW_DELAY_B;
#else
	av1->svtCfg.pred_structure = LOW_DELAY;
#endif
	av1->svtCfg.hierarchical_levels = 2;
	av1->svtCfg.intra_period_length = 63;
	av1->svtCfg.rate_control_mode = av1_svt_ratecontrol(av1->ratecontrol);
	av1->svtCfg.qp = 32;
	av1->svtCfg.target_bit_rate =
	    (av1->bitrate > (UINT32_MAX / 1000u)) ? UINT32_MAX : av1->bitrate * 1000u;
	av1->svtCfg.max_bit_rate = 0;
	av1->svtCfg.look_ahead_distance = 0;
#if !defined(SVT_AV1_CHECK_VERSION) || !SVT_AV1_CHECK_VERSION(4, 0, 0)
	av1->svtCfg.enable_tpl_la = 0;
#endif
	av1->svtCfg.enable_tf = 0;
	av1->svtCfg.enable_dg = 0;
	av1->svtCfg.startup_mg_size = 0;
	av1->svtCfg.screen_content_mode = 1;
	av1->svtCfg.tune = 1;

	err = svt_av1_enc_set_parameter(av1->svt, &av1->svtCfg);
	if (err != EB_ErrorNone)
	{
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_set_parameter: 0x%08" PRIx32, (UINT32)err);
		svt_av1_enc_deinit_handle(av1->svt);
		av1->svt = nullptr;
		return FALSE;
	}

	err = svt_av1_enc_init(av1->svt);
	if (err != EB_ErrorNone)
	{
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_init: 0x%08" PRIx32, (UINT32)err);
		svt_av1_enc_deinit_handle(av1->svt);
		av1->svt = nullptr;
		return FALSE;
	}

	av1->svtInitialized = true;
	return TRUE;
}

static void av1_svt_uninit(FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);

	if (!av1->svt)
		return;

	if (av1->svtInitialized)
	{
		const EbErrorType err = svt_av1_enc_deinit(av1->svt);
		if (err != EB_ErrorNone)
			WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_deinit: 0x%08" PRIx32, (UINT32)err);
		av1->svtInitialized = false;
	}

	const EbErrorType err = svt_av1_enc_deinit_handle(av1->svt);
	if (err != EB_ErrorNone)
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_deinit_handle: 0x%08" PRIx32, (UINT32)err);
	av1->svt = nullptr;
}
#endif

#if defined(WITH_LIBDAV1D)
WINPR_ATTR_NODISCARD
static BOOL av1_dav1d_init(FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(!av1->encoder);

	Dav1dSettings settings = WINPR_C_ARRAY_INIT;
	dav1d_default_settings(&settings);
	settings.n_threads = 1;
	settings.max_frame_delay = 1;
	settings.apply_grain = 0;

	const int rc = dav1d_open(&av1->dav1d, &settings);
	if (rc < 0)
	{
		WLog_Print(av1->log, WLOG_WARN, "dav1d_open: %d", rc);
		return FALSE;
	}
	return TRUE;
}

static void av1_dav1d_uninit(FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);

	if (av1->dav1d)
		dav1d_close(&av1->dav1d);
}
#endif

static void av1_backend_uninit(FREERDP_AV1_CONTEXT* av1)
{
	WINPR_ASSERT(av1);

	if (!av1->initialized)
		return;

	switch (av1->backend)
	{
#if defined(WITH_LIBAOM)
		case FREERDP_AV1_BACKEND_AOM:
			av1_aom_uninit(av1);
			break;
#endif
#if defined(WITH_LIBSVTAV1)
		case FREERDP_AV1_BACKEND_SVTAV1:
			av1_svt_uninit(av1);
			break;
#endif
#if defined(WITH_LIBDAV1D)
		case FREERDP_AV1_BACKEND_DAV1D:
			av1_dav1d_uninit(av1);
			break;
#endif
		case FREERDP_AV1_BACKEND_NONE:
		default:
			break;
	}

	av1->backend = FREERDP_AV1_BACKEND_NONE;
	av1->initialized = false;
}

WINPR_ATTR_NODISCARD
static BOOL av1_backend_init(FREERDP_AV1_CONTEXT* av1, FREERDP_AV1_BACKEND backend, UINT32 width,
                             UINT32 height)
{
	WINPR_ASSERT(av1);

	BOOL rc = FALSE;
	switch (backend)
	{
#if defined(WITH_LIBAOM)
		case FREERDP_AV1_BACKEND_AOM:
			rc = av1_aom_init(av1, width, height);
			break;
#endif
#if defined(WITH_LIBSVTAV1)
		case FREERDP_AV1_BACKEND_SVTAV1:
			rc = av1_svt_init(av1, width, height);
			break;
#endif
#if defined(WITH_LIBDAV1D)
		case FREERDP_AV1_BACKEND_DAV1D:
			rc = av1_dav1d_init(av1);
			break;
#endif
		case FREERDP_AV1_BACKEND_NONE:
		default:
			break;
	}

	if (rc)
	{
		av1->backend = backend;
		av1->initialized = true;
	}
	return rc;
}

WINPR_ATTR_NODISCARD
static BOOL av1_reinit_backend(FREERDP_AV1_CONTEXT* av1, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);

	av1_backend_uninit(av1);

	FREERDP_AV1_BACKEND backend = av1_select_backend(av1);
	if (backend == FREERDP_AV1_BACKEND_NONE)
	{
		WLog_Print(av1->log, WLOG_WARN, "No AV1 %s backend available for profile %" PRIu32,
		           av1->encoder ? "encoder" : "decoder", av1->profile);
		return FALSE;
	}

	if (av1_backend_init(av1, backend, width, height))
		return TRUE;

	const FREERDP_AV1_BACKEND fallback = av1_fallback_backend(av1, backend);
	if (fallback == FREERDP_AV1_BACKEND_NONE)
		return FALSE;

	WLog_Print(av1->log, WLOG_WARN, "Falling back from AV1 %s backend to %s",
	           av1_backend_name(backend), av1_backend_name(fallback));
	av1_backend_uninit(av1);
	return av1_backend_init(av1, fallback, width, height);
}

#if defined(WITH_LIBAOM)
WINPR_ATTR_NODISCARD
static INT32 av1_aom_compress(FREERDP_AV1_CONTEXT* av1, BYTE** ppDstData, UINT32* pDstSize)
{
	WINPR_ASSERT(av1);

	aom_image_t img = WINPR_C_ARRAY_INIT;
	img.fmt = (av1->profile == 0) ? AOM_IMG_FMT_I420 : AOM_IMG_FMT_I444;
	img.bit_depth = 8;
	img.d_w = img.r_w = img.w = av1->width;
	img.d_h = img.r_h = img.h = av1->height;
	if (av1->profile == 0)
	{
		img.x_chroma_shift = 1;
		img.y_chroma_shift = 1;
	}
	img.stride[0] = WINPR_ASSERTING_INT_CAST(int, av1->yuvStride[0]);
	img.stride[1] = WINPR_ASSERTING_INT_CAST(int, av1->yuvStride[1]);
	img.stride[2] = WINPR_ASSERTING_INT_CAST(int, av1->yuvStride[2]);
	img.planes[0] = av1->yuvdata[0];
	img.planes[1] = av1->yuvdata[1];
	img.planes[2] = av1->yuvdata[2];

	const aom_codec_err_t rc =
	    aom_codec_encode(&av1->aom, &img, ++av1->framecount, 1, av1->aomEflags);
	if (rc != AOM_CODEC_OK)
	{
		WLog_Print(av1->log, WLOG_WARN, "aom_codec_encode: %s", aom_codec_err_to_string(rc));
		return -1;
	}

	aom_codec_iter_t iter = nullptr;
	const aom_codec_cx_pkt_t* pkt = nullptr;
	while ((pkt = aom_codec_get_cx_data(&av1->aom, &iter)) != nullptr)
	{
		if (pkt->kind != AOM_CODEC_CX_FRAME_PKT)
			continue;
		if (!av1_bitstream_append(av1, pkt->data.frame.buf, pkt->data.frame.sz))
			return -1;
	}

	if (av1->bitstreamSize == 0)
		return 0;

	*ppDstData = av1->bitstream;
	*pDstSize = av1->bitstreamSize;
	return 1;
}

WINPR_ATTR_NODISCARD
static INT32 av1_aom_decompress(FREERDP_AV1_CONTEXT* av1, const BYTE* pSrcData, UINT32 SrcSize,
                                BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nDstWidth,
                                UINT32 nDstHeight)
{
	WINPR_ASSERT(av1);

	const aom_codec_err_t rc = aom_codec_decode(&av1->aom, pSrcData, SrcSize, nullptr);
	if (rc != AOM_CODEC_OK)
	{
		WLog_Print(av1->log, WLOG_WARN, "aom_codec_decode: %s", aom_codec_err_to_string(rc));
		return -1;
	}

	INT32 status = 0;
	aom_image_t* img = nullptr;
	aom_codec_iter_t iter = nullptr;
	while ((img = aom_codec_get_frame(&av1->aom, &iter)) != nullptr)
	{
		UINT32 profile = UINT32_MAX;
		switch (img->fmt)
		{
			case AOM_IMG_FMT_I420:
				profile = 0;
				break;
			case AOM_IMG_FMT_I444:
				profile = 1;
				break;
			default:
				WLog_Print(av1->log, WLOG_ERROR, "img->fmt %d not supported", img->fmt);
				return -1;
		}

		const BYTE* pSrc[] = { img->planes[0], img->planes[1], img->planes[2] };
		const UINT32 strides[] = { WINPR_ASSERTING_INT_CAST(UINT32, img->stride[0]),
			                       WINPR_ASSERTING_INT_CAST(UINT32, img->stride[1]),
			                       WINPR_ASSERTING_INT_CAST(UINT32, img->stride[2]) };

		const pstatus_t rec = av1_convert_yuv_to_rgb(av1, profile, pSrc, strides, pDstData,
		                                             DstFormat, nDstStep, nDstWidth, nDstHeight);
		if (rec != 0)
		{
			WLog_Print(av1->log, WLOG_ERROR, "AV1 YUV to RGB conversion failed: %d", rec);
			return -1;
		}
		status = 1;
	}

	return status;
}
#endif

#if defined(WITH_LIBSVTAV1)
WINPR_ATTR_NODISCARD
static INT32 av1_svt_compress(FREERDP_AV1_CONTEXT* av1, BYTE** ppDstData, UINT32* pDstSize)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(av1->profile == 0);

	av1->svtPicture.luma = av1->yuvdata[0];
	av1->svtPicture.cb = av1->yuvdata[1];
	av1->svtPicture.cr = av1->yuvdata[2];
	av1->svtPicture.y_stride = av1->yuvStride[0];
	av1->svtPicture.cb_stride = av1->yuvStride[1];
	av1->svtPicture.cr_stride = av1->yuvStride[2];

	av1->svtInput = (EbBufferHeaderType)WINPR_C_ARRAY_INIT;
	av1->svtInput.size = sizeof(av1->svtInput);
	av1->svtInput.p_buffer = (uint8_t*)&av1->svtPicture;
	av1->svtInput.n_filled_len =
	    av1->width * av1->height + 2 * (((av1->width + 1) / 2) * ((av1->height + 1) / 2));
	av1->svtInput.n_alloc_len = av1->svtInput.n_filled_len;
	av1->svtInput.pts = WINPR_ASSERTING_INT_CAST(int64_t, ++av1->framecount);
	av1->svtInput.flags = 0;

	EbErrorType err = svt_av1_enc_send_picture(av1->svt, &av1->svtInput);
	if (err != EB_ErrorNone)
	{
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_send_picture: 0x%08" PRIx32, (UINT32)err);
		goto fail;
	}

	EbBufferHeaderType eos = WINPR_C_ARRAY_INIT;
	eos.size = sizeof(eos);
	eos.flags = EB_BUFFERFLAG_EOS;
	err = svt_av1_enc_send_picture(av1->svt, &eos);
	if (err != EB_ErrorNone)
	{
		WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_send_picture(EOS): 0x%08" PRIx32, (UINT32)err);
		goto fail;
	}

	while (true)
	{
		EbBufferHeaderType* pkt = nullptr;
		err = svt_av1_enc_get_packet(av1->svt, &pkt, 1);
		if ((err == EB_NoErrorEmptyQueue) || (err == EB_NoErrorFifoShutdown))
			break;
		if (err != EB_ErrorNone)
		{
			WLog_Print(av1->log, WLOG_WARN, "svt_av1_enc_get_packet: 0x%08" PRIx32, (UINT32)err);
			goto fail;
		}

		if (pkt)
		{
			const BOOL eos = (pkt->flags & EB_BUFFERFLAG_EOS) != 0;
			const BOOL ok = av1_bitstream_append(av1, pkt->p_buffer, pkt->n_filled_len);
			svt_av1_enc_release_out_buffer(&pkt);
			if (!ok)
				goto fail;
			if (eos)
				break;
		}
	}

	av1_backend_uninit(av1);

	if (av1->bitstreamSize == 0)
		return 0;

	*ppDstData = av1->bitstream;
	*pDstSize = av1->bitstreamSize;
	return 1;

fail:
	av1_backend_uninit(av1);
	return -1;
}
#endif

#if defined(WITH_LIBDAV1D)
WINPR_ATTR_NODISCARD
static INT32 av1_dav1d_output_picture(FREERDP_AV1_CONTEXT* av1, const Dav1dPicture* picture,
                                      BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep,
                                      UINT32 nDstWidth, UINT32 nDstHeight)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(picture);

	if ((picture->p.bpc != 8) || (picture->p.w != WINPR_ASSERTING_INT_CAST(int, nDstWidth)) ||
	    (picture->p.h != WINPR_ASSERTING_INT_CAST(int, nDstHeight)))
	{
		WLog_Print(av1->log, WLOG_ERROR,
		           "dav1d picture format mismatch: %dx%d %dbpc, expected %" PRIu32 "x%" PRIu32
		           " 8bpc",
		           picture->p.w, picture->p.h, picture->p.bpc, nDstWidth, nDstHeight);
		return -1;
	}

	UINT32 profile = UINT32_MAX;
	switch (picture->p.layout)
	{
		case DAV1D_PIXEL_LAYOUT_I420:
			profile = 0;
			break;
		case DAV1D_PIXEL_LAYOUT_I444:
			profile = 1;
			break;
		default:
			WLog_Print(av1->log, WLOG_ERROR, "dav1d layout %d not supported", picture->p.layout);
			return -1;
	}

	if ((picture->stride[0] < 0) || (picture->stride[1] < 0) || (picture->stride[0] > UINT32_MAX) ||
	    (picture->stride[1] > UINT32_MAX))
	{
		WLog_Print(av1->log, WLOG_ERROR, "dav1d returned invalid strides");
		return -1;
	}

	const BYTE* pSrc[] = { picture->data[0], picture->data[1], picture->data[2] };
	const UINT32 strides[] = { (UINT32)picture->stride[0], (UINT32)picture->stride[1],
		                       (UINT32)picture->stride[1] };

	const pstatus_t rec = av1_convert_yuv_to_rgb(av1, profile, pSrc, strides, pDstData, DstFormat,
	                                             nDstStep, nDstWidth, nDstHeight);
	if (rec != 0)
	{
		WLog_Print(av1->log, WLOG_ERROR, "AV1 YUV to RGB conversion failed: %d", rec);
		return -1;
	}
	return 1;
}

WINPR_ATTR_NODISCARD
static INT32 av1_dav1d_decompress(FREERDP_AV1_CONTEXT* av1, const BYTE* pSrcData, UINT32 SrcSize,
                                  BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep,
                                  UINT32 nDstWidth, UINT32 nDstHeight)
{
	WINPR_ASSERT(av1);

	Dav1dData data = WINPR_C_ARRAY_INIT;
	uint8_t* buffer = dav1d_data_create(&data, SrcSize);
	if (!buffer)
	{
		WLog_Print(av1->log, WLOG_ERROR, "dav1d_data_create failed");
		return -1;
	}
	memcpy(buffer, pSrcData, SrcSize);

	INT32 status = 0;
	do
	{
		int rc = dav1d_send_data(av1->dav1d, &data);
		if ((rc < 0) && (rc != DAV1D_ERR(EAGAIN)))
		{
			WLog_Print(av1->log, WLOG_WARN, "dav1d_send_data: %d", rc);
			dav1d_data_unref(&data);
			return -1;
		}

		while (true)
		{
			Dav1dPicture picture = WINPR_C_ARRAY_INIT;
			rc = dav1d_get_picture(av1->dav1d, &picture);
			if (rc == DAV1D_ERR(EAGAIN))
				break;
			if (rc < 0)
			{
				WLog_Print(av1->log, WLOG_WARN, "dav1d_get_picture: %d", rc);
				dav1d_data_unref(&data);
				return -1;
			}

			const INT32 converted = av1_dav1d_output_picture(av1, &picture, pDstData, DstFormat,
			                                                 nDstStep, nDstWidth, nDstHeight);
			dav1d_picture_unref(&picture);
			if (converted < 0)
			{
				dav1d_data_unref(&data);
				return -1;
			}
			status = converted;
		}

		if ((rc == DAV1D_ERR(EAGAIN)) && (data.sz > 0))
			continue;
	} while (data.sz > 0);

	return status;
}
#endif

BOOL freerdp_av1_context_set_option(FREERDP_AV1_CONTEXT* av1, FREERDP_AV1_CONTEXT_OPTION option,
                                    UINT32 value)
{
	WINPR_ASSERT(av1);

	if (!av1->encoder)
		return FALSE;

	switch (option)
	{
		case FREERDP_AV1_CONTEXT_OPTION_PROFILE:
			av1->profile = value;
			break;
		case FREERDP_AV1_CONTEXT_OPTION_RATECONTROL:
			if (!av1_validate_ratecontrol(av1, value))
				return FALSE;
			av1->ratecontrol = value;
			break;
		case FREERDP_AV1_CONTEXT_OPTION_BITRATE:
			av1->bitrate = value;
			break;
		case FREERDP_AV1_CONTEXT_OPTION_USAGETYPE:
			av1->usagetype = value;
			break;
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unknown FREERDP_AV1_CONTEXT_OPTION[0x%08" PRIx32 "]",
			           option);
			return FALSE;
	}

	if ((av1->width == 0) || (av1->height == 0))
		return TRUE;

	return freerdp_av1_context_reset(av1, av1->width, av1->height);
}

UINT32 freerdp_av1_context_get_option(FREERDP_AV1_CONTEXT* av1, FREERDP_AV1_CONTEXT_OPTION option)
{
	WINPR_ASSERT(av1);

	if (!av1->encoder)
		return 0;

	switch (option)
	{
		case FREERDP_AV1_CONTEXT_OPTION_PROFILE:
			return av1->profile;
		case FREERDP_AV1_CONTEXT_OPTION_RATECONTROL:
			return av1->ratecontrol;
		case FREERDP_AV1_CONTEXT_OPTION_BITRATE:
			return av1->bitrate;
		case FREERDP_AV1_CONTEXT_OPTION_USAGETYPE:
			return av1->usagetype;
		default:
			WLog_Print(av1->log, WLOG_ERROR, "Unknown FREERDP_AV1_CONTEXT_OPTION[0x%08" PRIx32 "]",
			           option);
			return 0;
	}
}

INT32 freerdp_av1_compress(FREERDP_AV1_CONTEXT* av1, const BYTE* pSrcData, DWORD SrcFormat,
                           UINT32 nSrcStep, UINT32 nSrcWidth, UINT32 nSrcHeight,
                           const RECTANGLE_16* regionRect, BYTE** ppDstData, UINT32* pDstSize,
                           RDPGFX_H264_METABLOCK* meta)
{
	WINPR_ASSERT(av1);
	WINPR_ASSERT(ppDstData);
	WINPR_ASSERT(pDstSize);
	WINPR_ASSERT(meta);

	WINPR_UNUSED(regionRect);

	*ppDstData = nullptr;
	*pDstSize = 0;
	av1->bitstreamSize = 0;

	if (!av1->encoder)
	{
		WLog_Print(av1->log, WLOG_ERROR, "AV1 context is not an encoder");
		return -1;
	}

	if (!av1->initialized && !freerdp_av1_context_reset(av1, nSrcWidth, nSrcHeight))
		return -1;

	if ((av1->width != nSrcWidth) || (av1->height != nSrcHeight))
	{
		WLog_Print(av1->log, WLOG_ERROR,
		           "AV1 context size %" PRIu32 "x%" PRIu32 " does not match source %" PRIu32
		           "x%" PRIu32,
		           av1->width, av1->height, nSrcWidth, nSrcHeight);
		return -1;
	}

	const pstatus_t rec =
	    av1_convert_rgb_to_yuv(av1, pSrcData, SrcFormat, nSrcStep, nSrcWidth, nSrcHeight);
	if (rec != 0)
	{
		WLog_Print(av1->log, WLOG_ERROR, "AV1 RGB to YUV conversion failed: %d", rec);
		return -1;
	}

	INT32 rc = -1;
	switch (av1->backend)
	{
#if defined(WITH_LIBAOM)
		case FREERDP_AV1_BACKEND_AOM:
			rc = av1_aom_compress(av1, ppDstData, pDstSize);
			break;
#endif
#if defined(WITH_LIBSVTAV1)
		case FREERDP_AV1_BACKEND_SVTAV1:
			rc = av1_svt_compress(av1, ppDstData, pDstSize);
			break;
#endif
		case FREERDP_AV1_BACKEND_NONE:
		default:
			WLog_Print(av1->log, WLOG_ERROR, "No AV1 encoder backend initialized");
			return -1;
	}

	if (rc <= 0)
		return rc;

	RECTANGLE_16* rect = calloc(1, sizeof(RECTANGLE_16));
	if (rect)
	{
		rect->right = nSrcWidth;
		rect->bottom = nSrcHeight;
	}
	return allocate_h264_metablock(10, rect, meta, 1);
}

INT32 freerdp_av1_decompress(FREERDP_AV1_CONTEXT* av1, const BYTE* pSrcData, UINT32 SrcSize,
                             BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nDstWidth,
                             UINT32 nDstHeight, const RECTANGLE_16* regionRects,
                             UINT32 numRegionRect)
{
	WINPR_ASSERT(av1);

	WINPR_UNUSED(regionRects);
	WINPR_UNUSED(numRegionRect);

	if (av1->encoder)
	{
		WLog_Print(av1->log, WLOG_ERROR, "AV1 context is not a decoder");
		return -1;
	}

	if (!av1->initialized && !freerdp_av1_context_reset(av1, nDstWidth, nDstHeight))
		return -1;

	switch (av1->backend)
	{
#if defined(WITH_LIBAOM)
		case FREERDP_AV1_BACKEND_AOM:
			return av1_aom_decompress(av1, pSrcData, SrcSize, pDstData, DstFormat, nDstStep,
			                          nDstWidth, nDstHeight);
#endif
#if defined(WITH_LIBDAV1D)
		case FREERDP_AV1_BACKEND_DAV1D:
			return av1_dav1d_decompress(av1, pSrcData, SrcSize, pDstData, DstFormat, nDstStep,
			                            nDstWidth, nDstHeight);
#endif
		case FREERDP_AV1_BACKEND_NONE:
		default:
			WLog_Print(av1->log, WLOG_ERROR, "No AV1 decoder backend initialized");
			return -1;
	}
}

BOOL freerdp_av1_context_reset(FREERDP_AV1_CONTEXT* av1, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(av1);

	if ((width == 0) || (height == 0))
		return FALSE;

	av1->width = width;
	av1->height = height;
	av1->framecount = 0;

	if (av1->encoder && !av1_allocate_yuv(av1, width, height))
		return FALSE;

	if (!av1_reinit_backend(av1, width, height))
		return FALSE;

	return TRUE;
}

void freerdp_av1_context_free(FREERDP_AV1_CONTEXT* av1)
{
	if (!av1)
		return;

	av1_backend_uninit(av1);
	av1_free_yuv(av1);
	free(av1->bitstream);
	free(av1);
}

FREERDP_AV1_CONTEXT* freerdp_av1_context_new(BOOL Compressor)
{
	FREERDP_AV1_CONTEXT* ctx = calloc(1, sizeof(FREERDP_AV1_CONTEXT));
	if (!ctx)
		return nullptr;

	ctx->encoder = Compressor;
	ctx->backend = FREERDP_AV1_BACKEND_NONE;
	ctx->profile = 0;
	ctx->ratecontrol = FREERDP_AV1_VBR;
	ctx->bitrate = 500;
	ctx->usagetype = av1_default_usage();
	ctx->log = WLog_Get(TAG);
	if (!ctx->log)
		goto fail;

#if defined(WITH_LIBAOM)
	if (Compressor)
	{
		aom_codec_iface_t* iface = aom_codec_av1_cx();
		if (iface)
		{
			aom_codec_enc_cfg_t cfg = WINPR_C_ARRAY_INIT;
			const aom_codec_err_t rc = aom_codec_enc_config_default(iface, &cfg, ctx->usagetype);
			if (rc == AOM_CODEC_OK)
			{
				ctx->profile = cfg.g_profile;
				ctx->ratecontrol = cfg.rc_end_usage;
				ctx->bitrate = cfg.rc_target_bitrate;
			}
		}
	}
#endif

	return ctx;

fail:
	freerdp_av1_context_free(ctx);
	return nullptr;
}
