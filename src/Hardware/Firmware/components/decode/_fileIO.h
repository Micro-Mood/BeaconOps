#ifndef _FILEIO_H
#define _FILEIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include "PNGdec.h"
#include "JPEGDEC.h"

void *FileOpen(const char *fname, int32_t *pSize);
void  FileClose(void *pHandle);

int32_t PNGRead(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen);
int32_t PNGSeek(PNGFILE *pFile, int32_t iPosition);

int32_t JPGRead(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen);
int32_t JPGSeek(JPEGFILE *pFile, int32_t iPosition);

#ifdef __cplusplus
}
#endif

#endif
