#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include "accel_test.h"
#include "dsa.h"
#include "iaa.h"
#include "algorithms/iaa_filter.h"
#include <setjmp.h>
#include <jpeglib.h>

static int read_bmp_to_buffer(const char *filepath, void **buffer, size_t *buffer_size) {
    int width, height, bitDepth;
    unsigned char header[54];
    size_t row_padded;
    FILE *file = fopen(filepath, "rb");
    if (!file) {
		printf("No file\n");
        return -errno;
    }

    // Read the BMP header
    if (fread(header, 1, 54, file) != 54) {
        fclose(file);
        return -EINVAL; // Invalid file
    }

    // Check the BMP signature
    if (header[0] != 'B' || header[1] != 'M') {
        fclose(file);
        return -EINVAL;
    }

    // Read image dimensions
    width = *(int*)&header[18];
    height = *(int*)&header[22];
    bitDepth = *(int*)&header[28];

    if (bitDepth != 24) {
        fclose(file);
        return -EINVAL; // Not a 24-bit BMP
    }

    // Calculate the size of the image data
    row_padded = (width * 3 + 3) & (~3);
    *buffer_size = row_padded * height;

    // Allocate memory for the buffer
    *buffer = aligned_alloc(ADDR_ALIGNMENT, *buffer_size);
	if (!buffer)
		return -ENOMEM;
    if (!*buffer) {
        fclose(file);
        return -ENOMEM;
    }

    // Go to the beginning of the bitmap data
    fseek(file, *(int*)&header[10], SEEK_SET);

    // Read the bitmap data
    for (int i = 0; i < height; i++) {
        fread(*buffer + (row_padded * (height - i - 1)), 1, row_padded, file);
    }

    fclose(file);
    return ACCTEST_STATUS_OK;
}

static int read_jpeg_to_buffer(const char *filepath, void **buffer, size_t *buffer_size) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *infile;
    JSAMPARRAY bufferArray;
    int row_stride;

    // Open the JPEG file
    if ((infile = fopen(filepath, "rb")) == NULL) {
        fprintf(stderr, "can't open %s\n", filepath);
        return -errno;
    }

    // Set up the error handler
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    // Set up for decompression
    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    row_stride = cinfo.output_width * cinfo.output_components;
    *buffer_size = row_stride * cinfo.output_height;

    // Allocate the buffer
    *buffer = malloc(*buffer_size);
    if (!*buffer) {
        fclose(infile);
        jpeg_destroy_decompress(&cinfo);
        return -ENOMEM;
    }

    // Read the JPEG into the buffer
    while (cinfo.output_scanline < cinfo.output_height) {
        bufferArray = (*cinfo.mem->alloc_sarray)
            ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

        jpeg_read_scanlines(&cinfo, bufferArray, 1);
        memcpy(*buffer + (row_stride * (cinfo.output_scanline - 1)), bufferArray[0], row_stride);
    }

    // Finish decompression
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);

    return ACCTEST_STATUS_OK;
}

static void shuffle_elements(void *array, size_t size) {
    uint32_t *arr = (uint32_t *)array;
    size_t n = size / sizeof(uint32_t); // Number of elements
    if (n > 1) {
        srand((unsigned)time(NULL)); // Seed the random number generator
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rand() % (i + 1); // Random index from 0 to i
            // Swap arr[i] and arr[j]
            uint32_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
}

static void print_elements(void *array, size_t size) {
	unsigned char *bytes = (unsigned char *)array;

    // Calculate number of pixels (each pixel is 3 bytes for 24-bit images)
    size_t numPixels = size / 3;

    for (size_t i = 0; i < numPixels; i++) {
        // Each pixel is represented by 3 bytes: R, G, B
        unsigned char red = bytes[i * 3 + 2];   // Red
        unsigned char green = bytes[i * 3 + 1]; // Green
        unsigned char blue = bytes[i * 3];      // Blue

        // Print the combined RGB value in hex format
        printf("#%02X%02X%02X ", red, green, blue);

        // New line after every 8 pixels for readability
        if ((i + 1) % 8 == 0) {
            printf("\n");
        }
    }
    printf("\n");
}

#endif