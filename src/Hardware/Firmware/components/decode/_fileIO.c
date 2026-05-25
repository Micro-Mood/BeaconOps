#include "_fileIO.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

void *FileOpen(const char *fname, int32_t *pSize)
{
    FILE *f = fopen(fname, "rb");
    if (!f) return NULL;

    if (pSize) {
        struct stat st;
        if (fstat(fileno(f), &st) == 0) {
            *pSize = (int32_t)st.st_size;
        } else if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            *pSize = (sz >= 0) ? (int32_t)sz : 0;
        } else {
            *pSize = 0;
        }
        fseek(f, 0, SEEK_SET);
    }
    return (void *)f;
}

void FileClose(void *pHandle)
{
    FILE *f = (FILE *)pHandle;
    if (f) fclose(f);
}

int32_t PNGRead(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    if (!pFile || !pFile->fHandle || !pBuf || iLen <= 0) return 0;
    FILE *f = (FILE *)pFile->fHandle;
    int32_t n = (int32_t)fread(pBuf, 1, iLen, f);
    if (n > 0) pFile->iPos += n;
    return n;
}

int32_t PNGSeek(PNGFILE *pFile, int32_t iPosition)
{
    if (!pFile || !pFile->fHandle) return -1;
    FILE *f = (FILE *)pFile->fHandle;
    if (fseek(f, iPosition, SEEK_SET) != 0) return -1;
    pFile->iPos = iPosition;
    return iPosition;
}

int32_t JPGRead(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    if (!pFile || !pFile->fHandle || !pBuf || iLen <= 0) return 0;
    FILE *f = (FILE *)pFile->fHandle;
    int32_t n = (int32_t)fread(pBuf, 1, iLen, f);
    if (n > 0) pFile->iPos += n;
    return n;
}

int32_t JPGSeek(JPEGFILE *pFile, int32_t iPosition)
{
    if (!pFile || !pFile->fHandle) return -1;
    FILE *f = (FILE *)pFile->fHandle;
    if (fseek(f, iPosition, SEEK_SET) != 0) return -1;
    pFile->iPos = iPosition;
    return iPosition;
}
