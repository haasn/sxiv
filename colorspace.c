/* Copyright 2014 Niklas Haas
 *
 * This file is part of sxiv.
 *
 * sxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * sxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "types.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "iccjpeg/iccjpeg.h"
#include <jpeglib.h>
#include <lcms2.h>
#include <libexif/exif-data.h>
#include <zlib.h>

static char *png_inflate(const char *input, int inlen, int *outlen) {
	z_stream strm;
	strm.next_in = (void *)input;
	strm.avail_in = inlen;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;

	if (inflateInit(&strm) != Z_OK)
		return NULL;

	int len = 16*1024; // 16 kB buffer by default
	char *out = malloc(len);
	if (out == NULL)
		return NULL;

	strm.next_out = (void *)out;
	strm.avail_out = len;

	for (;;) {
		switch (inflate(&strm, Z_FINISH)) {
			case Z_STREAM_END:
				*outlen = strm.total_out;
				inflateEnd(&strm);
				return out;

			case Z_BUF_ERROR:
				// Expand buffer and continue at new pos
				out = realloc(out, len *= 2);
				if (out == NULL)
					return NULL;

				strm.next_out = (void *)(out + strm.total_out);
				strm.avail_out = len - strm.total_out;
				break;

			default:
				// Some kind of error
				inflateEnd(&strm);
				free(out);
				return NULL;
		}
	}
}

void handle_jpeg_error (j_common_ptr cinfo)
{
	// Just jump back to where we left off and clean up
	longjmp(((struct { struct jpeg_error_mgr mgr; jmp_buf cleanup; } *) cinfo->err)->cleanup, 1);
}

cmsHPROFILE img_get_colorspace(const fileinfo_t *file) {
	cmsHPROFILE res = NULL;
	/* We do things in the following order:
	 * 1. Try opening with libexif, on success check for EXIF profile
	 * 2. Try interpreting as PNG file manually, on success check for
	 *    iCCP or fall back to embedded colorspace (or default)
	 * 3. Try interpreting as JPEG file with libjpeg, on success check
	 *    for embeded profile or fall back to colorspace/default.
	 * 4. Assume sRGB as a last resort
	 */

	int fd, len;
	struct stat sb;
	const char *data;
	fd = open(file->path, O_RDONLY);

	if ((fd = open(file->path, O_RDONLY)) == -1 ||
	    fstat(fd, &sb) == -1 ||
	    (data = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		// Opening the file failed, clean up and fall back
		if (fd != -1)
			close(fd);
		return cmsCreate_sRGBProfile();
	}
	len = sb.st_size;

	// Check libexif first
	ExifData *ed;
	ExifEntry *entry;
	if ((ed = exif_data_new_from_data((const unsigned char *) data, len)) != NULL) {
		entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_INTER_COLOR_PROFILE);
		if (entry) {
			res = cmsOpenProfileFromMem(entry->data, entry->size);
			exif_entry_unref(entry);
		}
		exif_data_unref(ed);

		if (res)
			goto end;
	}

	// Try manually parsing PNG structures next
	if (!strncmp(data, "\211PNG\r\n\032\n", 8)) {
		uint32_t chunklen, pos = 8;

		// For custom primaries, sane defaults given
		int custom = FALSE;
		cmsCIExyY white = (cmsCIExyY) {0.3127, 0.3290, 1.0}; // D65
		cmsCIExyYTRIPLE prim = (cmsCIExyYTRIPLE) { // sRGB
			.Red   = {0.64, 0.33},
			.Green = {0.30, 0.60},
			.Blue  = {0.15, 0.06},
		};
		double gamma = 2.2; // Sane default

		// Iterate through all chunks until we find iCCP or IEND
		while (pos < len) {
			chunklen = ntohl( *((uint32_t *)&data[pos]) );

#define CHUNK(str) (!strncmp(&data[pos+4], (str), 4))

			// End of relevant information reached
			if (CHUNK("IEND") || CHUNK("IDAT") || CHUNK("PLTE"))
				break;

			if (CHUNK("iCCP")) {
				// Skip comment
				int off;
				for (off = 0; data[pos+8+off] != '\0'; off++);
				// Inflate
				void *buf;
				int outlen;
				if ((buf = png_inflate(&data[pos+8+off+2], chunklen - off - 2, &outlen)) != NULL) {
					res = cmsOpenProfileFromMem(buf, outlen);
					free(buf);
				}
				break;
			}

			if (CHUNK("sRGB")) {
				res = cmsCreate_sRGBProfile();
				break;
			}

#define FLOAT(off) ( ntohl( ((uint32_t *)&data[pos+8])[(off)] )/100000.0 )

			// Collect custom primaries if present
			if (CHUNK("cHRM")) {
				white = (cmsCIExyY) {FLOAT(0), FLOAT(1), 1.0};
				prim = (cmsCIExyYTRIPLE) {
					.Red   = {FLOAT(2), FLOAT(3), 1.0},
					.Green = {FLOAT(4), FLOAT(5), 1.0},
					.Blue  = {FLOAT(6), FLOAT(7), 1.0},
				};
				custom = TRUE;
			}

			if (CHUNK("gAMA")) {
				gamma = 1.0 / FLOAT(0);
				custom = TRUE;
			}

			// Advance past header, type field, chunk data and CRC
			pos += chunklen + 12;
		}

		// Use custom primaries if available
		if (custom) {
			cmsToneCurve *tc = cmsBuildGamma(NULL, gamma);
			res = cmsCreateRGBProfile(&white, &prim, (cmsToneCurve*[3]) {tc, tc, tc});
			cmsFreeToneCurve(tc);
		}

		goto end;
	}

	// Try getting profile info with libjpeg
	struct jpeg_decompress_struct cinfo;
	struct { struct jpeg_error_mgr mgr; jmp_buf cleanup; } jerr;
	cinfo.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = handle_jpeg_error; // This jumps back to here

	if (setjmp(jerr.cleanup)) {
		// If we get here, it's due to a fatal error in libjpeg
		jpeg_destroy_decompress(&cinfo);
		goto end;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (unsigned char *) data, len);
	setup_read_icc_profile(&cinfo);
	jpeg_read_header(&cinfo, FALSE);

	unsigned char *buf;
	unsigned int outlen;

	if (read_icc_profile(&cinfo, &buf, &outlen)) {
		// ICC profile found, use it
		res = cmsOpenProfileFromMem(buf, outlen);
		free(buf);
	}

	jpeg_destroy_decompress(&cinfo);

end:
	munmap((void *) data, len);
	close(fd);

	// Fall back to sRGB as a last resort if all else fails
	if (!res)
		res = cmsCreate_sRGBProfile();

	return res;
}
