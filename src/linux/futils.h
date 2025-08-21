#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../hints.h"

#define BUF_SIZE 1024

typedef uint32_t crc32_t;

// ----------------------------------------------------------------------------
static inline crc32_t
crc32(const uint8_t* data, size_t length) {
    crc32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (size_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1))); // cppcheck-suppress [integerOverflow]
        }
    }
    return ~crc;
}

// ----------------------------------------------------------------------------
static inline crc32_t
fhash(FILE* fp) {

    uint8_t buf[BUF_SIZE];
    size_t  bytes_read;
    crc32_t crc = 0xFFFFFFFF;

    // Save the current position
    long int current_offset = ftell(fp);

    // Move to the beginning of the file
    fseek(fp, 0, SEEK_SET);

    while ((bytes_read = fread(buf, 1, BUF_SIZE, fp)) != 0) {
        crc = crc32(buf, bytes_read);
    }

    // Restore the position
    fseek(fp, current_offset, SEEK_SET);

    return ~crc;
}

// ----------------------------------------------------------------------------
static inline long
fmtime_ns(FILE* fp) {
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        set_error(IO, "Cannot get file status");
        FAIL_INT;
    }
    return st.st_mtim.tv_nsec;
}
