#include <string.h>
#include <stdlib.h>

#include <freerdp/freerdp.h>
#include <freerdp/codec/av1.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>

#if defined(WITH_LIBAOM)
#define HAVE_AV1_I420_ENCODER 1
#endif

#if defined(WITH_LIBAOM)
#define HAVE_AV1_I444_ENCODER 1
#endif

#if defined(WITH_LIBAOM)
#define HAVE_AV1_DECODER 1
#endif

static void* allocRGB(UINT32 format, UINT32 width, UINT32 height, UINT32* pstride)
{
	WINPR_ASSERT(pstride);

	const size_t bpp = FreeRDPGetBytesPerPixel(format);
	const size_t stride = width * bpp + 16;
	BYTE* rgb = calloc(stride, height);
	if (!rgb)
		return nullptr;

	for (UINT32 y = 0; y < height; y++)
	{
		BYTE* line = &rgb[y * stride];
		for (UINT32 x = 0; x < width; x++)
		{
			const BYTE r = (BYTE)((3u * x + 5u * y) & 0xFFu);
			const BYTE g = (BYTE)((7u * x + 11u * y + 29u) & 0xFFu);
			const BYTE b = (BYTE)((13u * x + 17u * y + 53u) & 0xFFu);
			const UINT32 color = FreeRDPGetColor(format, r, g, b, 0xFF);
			if (!FreeRDPWriteColor(&line[x * bpp], format, color))
			{
				free(rgb);
				return nullptr;
			}
		}
	}

	*pstride = WINPR_ASSERTING_INT_CAST(UINT32, stride);
	return rgb;
}

static BOOL compareRGB(const BYTE* src, const BYTE* dst, UINT32 format, UINT32 width, UINT32 height,
                       UINT32 stride, UINT32 maxAverageError)
{
	const size_t bpp = FreeRDPGetBytesPerPixel(format);
	UINT64 error = 0;
	UINT64 samples = 0;

	for (UINT32 y = 0; y < height; y++)
	{
		const BYTE* srcLine = &src[y * stride];
		const BYTE* dstLine = &dst[y * stride];

		for (UINT32 x = 0; x < width; x++)
		{
			BYTE sr = 0;
			BYTE sg = 0;
			BYTE sb = 0;
			BYTE dr = 0;
			BYTE dg = 0;
			BYTE db = 0;
			const UINT32 srcColor = FreeRDPReadColor(&srcLine[x * bpp], format);
			const UINT32 dstColor = FreeRDPReadColor(&dstLine[x * bpp], format);
			FreeRDPSplitColor(srcColor, format, &sr, &sg, &sb, nullptr, nullptr);
			FreeRDPSplitColor(dstColor, format, &dr, &dg, &db, nullptr, nullptr);

			error += (UINT64)abs((int)sr - (int)dr);
			error += (UINT64)abs((int)sg - (int)dg);
			error += (UINT64)abs((int)sb - (int)db);
			samples += 3;
		}
	}

	if (samples == 0)
		return FALSE;

	if (error > samples * maxAverageError)
	{
		fprintf(stderr, "AV1 roundtrip error too high: average=%" PRIu64 ", limit=%" PRIu32 "\n",
		        error / samples, maxAverageError);
		return FALSE;
	}

	return TRUE;
}

static BOOL testContextOptions(UINT32 width, UINT32 height)
{
	BOOL rc = FALSE;

#if defined(HAVE_AV1_I420_ENCODER)
	FREERDP_AV1_CONTEXT* enc = freerdp_av1_context_new(TRUE);
	if (!enc)
		return FALSE;

	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_PROFILE, 0))
		goto fail_enc;
	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL,
	                                    FREERDP_AV1_VBR))
		goto fail_enc;
	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_BITRATE, 10000))
		goto fail_enc;
	if (freerdp_av1_context_get_option(enc, FREERDP_AV1_CONTEXT_OPTION_PROFILE) != 0)
		goto fail_enc;
	if (freerdp_av1_context_get_option(enc, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL) !=
	    FREERDP_AV1_VBR)
		goto fail_enc;
	if (freerdp_av1_context_get_option(enc, FREERDP_AV1_CONTEXT_OPTION_BITRATE) != 10000)
		goto fail_enc;
	if (!freerdp_av1_context_reset(enc, width, height))
		goto fail_enc;

	rc = TRUE;
fail_enc:
	freerdp_av1_context_free(enc);
	if (!rc)
		return FALSE;
#else
	WINPR_UNUSED(width);
	WINPR_UNUSED(height);
#endif

#if defined(HAVE_AV1_DECODER)
	FREERDP_AV1_CONTEXT* dec = freerdp_av1_context_new(FALSE);
	if (!dec)
		return FALSE;
	if (freerdp_av1_context_set_option(dec, FREERDP_AV1_CONTEXT_OPTION_PROFILE, 0))
	{
		freerdp_av1_context_free(dec);
		return FALSE;
	}
	if (!freerdp_av1_context_reset(dec, width, height))
	{
		freerdp_av1_context_free(dec);
		return FALSE;
	}
	freerdp_av1_context_free(dec);
	rc = TRUE;
#endif

	return rc;
}

static BOOL testEncode(UINT32 profile, UINT32 format, UINT32 width, UINT32 height)
{
	BOOL rc = FALSE;
	void* src = nullptr;
	void* out = nullptr;
	BYTE* encoded = nullptr;
	UINT32 encodedSize = 0;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	FREERDP_AV1_CONTEXT* enc = freerdp_av1_context_new(TRUE);
#if defined(HAVE_AV1_DECODER)
	FREERDP_AV1_CONTEXT* dec = freerdp_av1_context_new(FALSE);
#else
	FREERDP_AV1_CONTEXT* dec = nullptr;
#endif
	if (!enc)
		goto fail;
#if defined(HAVE_AV1_DECODER)
	if (!dec)
		goto fail;
#endif

	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_PROFILE, profile))
		goto fail;
	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL,
	                                    FREERDP_AV1_VBR))
		goto fail;
	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_BITRATE, 10000))
		goto fail;
	if (!freerdp_av1_context_reset(enc, width, height))
		goto fail;
#if defined(HAVE_AV1_DECODER)
	if (!freerdp_av1_context_reset(dec, width, height))
		goto fail;
#endif

	UINT32 stride = 0;
	UINT32 ostride = 0;
	src = allocRGB(format, width, height, &stride);
	out = calloc(stride, height);
	ostride = stride;
	if (!src || !out || (stride != ostride))
		goto fail;
	memset(out, 0xA5, stride * height);

	const RECTANGLE_16 rect = { .left = 0, .top = 0, .right = width, .bottom = height };
	for (UINT32 x = 0; x < 4; x++)
	{
		free_h264_metablock(&meta);
		const INT32 status = freerdp_av1_compress(enc, src, format, stride, width, height, &rect,
		                                          &encoded, &encodedSize, &meta);
		if (status < 0)
			goto fail;
		if ((status > 0) && encoded && (encodedSize > 0))
			break;
	}

	if (!encoded || (encodedSize == 0))
		goto fail;

#if defined(HAVE_AV1_DECODER)
	if (freerdp_av1_decompress(dec, encoded, encodedSize, out, format, stride, width, height, &rect,
	                           1) < 0)
		goto fail;

	if (!compareRGB(src, out, format, width, height, stride, 64))
		goto fail;
#endif

	rc = TRUE;
fail:
	freerdp_av1_context_free(enc);
	freerdp_av1_context_free(dec);
	free_h264_metablock(&meta);
	free(src);
	free(out);
	return rc;
}

int TestFreeRDPCodecAV1(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	const UINT32 width = 128;
	const UINT32 height = 96;
	const UINT32 format = PIXEL_FORMAT_BGRA32;

#if !defined(HAVE_AV1_I420_ENCODER) && !defined(HAVE_AV1_DECODER)
	(void)fprintf(stderr, "[%s] skipping, no AV1 encoder/decoder support compiled in\n", __func__);
	return 0;
#endif

	if (!testContextOptions(width, height))
		return -1;

#if defined(HAVE_AV1_I420_ENCODER)
	if (!testEncode(0, format, width, height))
		return -1;
#endif

#if defined(HAVE_AV1_I444_ENCODER)
	if (!testEncode(1, format, width, height))
		return -1;
#endif

	return 0;
}
