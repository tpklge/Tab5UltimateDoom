//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#undef bool
#undef true
#undef false
#endif

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

typedef struct
{
    wad_file_t wad;
    FILE *fstream;
} stdc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;
    FILE *fstream;

    fstream = fopen(path, "rb");

    if (fstream == NULL)
    {
        return NULL;
    }

    // Create a new stdc_wad_file_t to hold the file handle.

    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = M_FileLength(fstream);
    result->fstream = fstream;
#ifdef ESP_PLATFORM
    // SPIFFS random seeks through a large WAD are expensive. Use PSRAM as
    // the existing engine's memory-mapped backend, with streaming fallback.
    int64_t started = esp_timer_get_time();
    byte *mapped = heap_caps_malloc(result->wad.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mapped != NULL) {
        rewind(fstream);
        size_t read_bytes = 0;
        while (read_bytes < result->wad.length) {
            size_t chunk = result->wad.length - read_bytes;
            if (chunk > 65536) chunk = 65536;
            size_t got = fread(mapped + read_bytes, 1, chunk, fstream);
            if (got == 0) break;
            read_bytes += got;
            vTaskDelay(1);
        }
        if (read_bytes == result->wad.length) {
            result->wad.mapped = mapped;
            fclose(fstream);
            result->fstream = NULL;
            ESP_LOGI("WAD_RAM", "Loaded %u bytes into PSRAM in %lld ms: %s",
                     result->wad.length, (long long)((esp_timer_get_time() - started) / 1000), path);
        } else {
            free(mapped);
            rewind(fstream);
            ESP_LOGW("WAD_RAM", "Preload incomplete, using file streaming: %s", path);
        }
    } else {
        ESP_LOGW("WAD_RAM", "No contiguous PSRAM for WAD, using file streaming: %s", path);
    }
#endif

    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

    if (stdc_wad->fstream) fclose(stdc_wad->fstream);
    free(wad->mapped);
    Z_Free(stdc_wad);
}

// Read data from the specified position in the file into the 
// provided buffer.  Returns the number of bytes read.

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad;
    size_t result;

    stdc_wad = (stdc_wad_file_t *) wad;
    if (offset >= wad->length) return 0;
    if (buffer_len > wad->length - offset) buffer_len = wad->length - offset;
    if (wad->mapped != NULL) {
        memcpy(buffer, wad->mapped + offset, buffer_len);
        return buffer_len;
    }

    // Jump to the specified position in the file.

    fseek(stdc_wad->fstream, offset, SEEK_SET);

    // Read into the buffer.

    result = fread(buffer, 1, buffer_len, stdc_wad->fstream);

    return result;
}


wad_file_class_t stdc_wad_file = 
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};
