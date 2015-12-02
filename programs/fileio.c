/*
  fileio.c - File i/o handler
  Copyright (C) Yann Collet 2013-2015

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - zstd source repository : https://github.com/Cyan4973/zstd
  - Public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of ZSTD compression library, it is a user program of ZSTD library.
  The license of ZSTD library is BSD.
  The license of this file is GPLv2.
*/

/* *************************************
*  Tuning options
***************************************/
#ifndef ZSTD_LEGACY_SUPPORT
/**LEGACY_SUPPORT :
*  decompressor can decode older formats (starting from Zstd 0.1+) */
#  define ZSTD_LEGACY_SUPPORT 1
#endif


/* *************************************
*  Compiler Options
***************************************/
/* Disable some Visual warning messages */
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#endif

#define _FILE_OFFSET_BITS 64   /* Large file support on 32-bits unix */
#define _POSIX_SOURCE 1        /* enable fileno() within <stdio.h> on unix */


/* *************************************
*  Includes
***************************************/
#include <stdio.h>      /* fprintf, fopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcmp, strlen */
#include <time.h>       /* clock */
#include <errno.h>      /* errno */
#include <sys/types.h>  /* stat64 */
#include <sys/stat.h>   /* stat64 */
#include "mem.h"
#include "fileio.h"
#include "zstd_static.h"   /* ZSTD_magicNumber */
#include "zstd_buffered_static.h"

#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
#  include "zstd_legacy.h"    /* legacy */
#  include "fileio_legacy.h"  /* legacy */
#endif


/* *************************************
*  OS-specific Includes
***************************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>    /* _O_BINARY */
#  include <io.h>       /* _setmode, _isatty */
#  ifdef __MINGW32__
   // int _fileno(FILE *stream);   /* seems no longer useful /* MINGW somehow forgets to include this windows declaration into <stdio.h> */
#  endif
#  define SET_BINARY_MODE(file) { int unused = _setmode(_fileno(file), _O_BINARY); (void)unused; }
#  define IS_CONSOLE(stdStream) _isatty(_fileno(stdStream))
#else
#  include <unistd.h>   /* isatty */
#  define SET_BINARY_MODE(file)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#endif

#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif


/* *************************************
*  Constants
***************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _6BITS 0x3F
#define _8BITS 0xFF

#define BIT6  0x40
#define BIT7  0x80

#define BLOCKSIZE      (128 KB)
#define ROLLBUFFERSIZE (BLOCKSIZE*8*64)

#define FIO_FRAMEHEADERSIZE 5        /* as a define, because needed to allocated table on stack */
#define FSE_CHECKSUM_SEED        0

#define CACHELINE 64


/* *************************************
*  Macros
***************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static U32 g_displayLevel = 2;   /* 0 : no display;   1: errors;   2 : + result + interaction + warnings;   3 : + progression;   4 : + information */

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((FIO_GetMilliSpan(g_time) > refreshRate) || (g_displayLevel>=4)) \
            { g_time = clock(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(stdout); } }
static const unsigned refreshRate = 150;
static clock_t g_time = 0;


/* *************************************
*  Local Parameters
***************************************/
static U32 g_overwrite = 0;

void FIO_overwriteMode(void) { g_overwrite=1; }
void FIO_setNotificationLevel(unsigned level) { g_displayLevel=level; }


/* *************************************
*  Exceptions
***************************************/
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/* *************************************
*  Functions
***************************************/
static unsigned FIO_GetMilliSpan(clock_t nPrevious)
{
    clock_t nCurrent = clock();
    unsigned nSpan = (unsigned)(((nCurrent - nPrevious) * 1000) / CLOCKS_PER_SEC);
    return nSpan;
}


static void FIO_getFileHandles(FILE** pfinput, FILE** pfoutput, const char* input_filename, const char* output_filename)
{
    if (!strcmp (input_filename, stdinmark))
    {
        DISPLAYLEVEL(4,"Using stdin for input\n");
        *pfinput = stdin;
        SET_BINARY_MODE(stdin);
    }
    else
    {
        *pfinput = fopen(input_filename, "rb");
    }

    if (!strcmp (output_filename, stdoutmark))
    {
        DISPLAYLEVEL(4,"Using stdout for output\n");
        *pfoutput = stdout;
        SET_BINARY_MODE(stdout);
    }
    else
    {
        /* Check if destination file already exists */
        *pfoutput=0;
        if (strcmp(output_filename,nulmark)) *pfoutput = fopen( output_filename, "rb" );
        if (*pfoutput!=0)
        {
            fclose(*pfoutput);
            if (!g_overwrite)
            {
                char ch;
                if (g_displayLevel <= 1)   /* No interaction possible */
                    EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
                DISPLAYLEVEL(2, "Warning : %s already exists\n", output_filename);
                DISPLAYLEVEL(2, "Overwrite ? (Y/N) : ");
                ch = (char)getchar();
                if ((ch!='Y') && (ch!='y')) EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
            }
        }
        *pfoutput = fopen( output_filename, "wb" );
    }

    if ( *pfinput==0 ) EXM_THROW(12, "Pb opening src : %s", input_filename);
    if ( *pfoutput==0) EXM_THROW(13, "Pb opening dst : %s", output_filename);
}


static U64 FIO_getFileSize(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
#endif
    if (r || !S_ISREG(statbuf.st_mode)) return 0;
    return (U64)statbuf.st_size;
}


unsigned long long FIO_compressFilename(const char* output_filename, const char* input_filename, int cLevel)
{
    U64 filesize = 0;
    U64 compressedfilesize = 0;
    BYTE* inBuff;
    BYTE* outBuff;
    size_t inBuffSize = ZBUFF_recommendedCInSize();
    size_t outBuffSize = ZBUFF_recommendedCOutSize();
    FILE* finput;
    FILE* foutput;
    size_t sizeCheck, errorCode;
    ZBUFF_CCtx* ctx;

    /* Allocate Memory */
    ctx = ZBUFF_createCCtx();
    inBuff  = (BYTE*)malloc(inBuffSize);
    outBuff = (BYTE*)malloc(outBuffSize);
    if (!inBuff || !outBuff || !ctx) EXM_THROW(21, "Allocation error : not enough memory");

    /* init */
    FIO_getFileHandles(&finput, &foutput, input_filename, output_filename);
    filesize = FIO_getFileSize(input_filename);
    errorCode = ZBUFF_compressInit_advanced(ctx, ZSTD_getParams(cLevel, filesize));
    if (ZBUFF_isError(errorCode)) EXM_THROW(22, "Error initializing compression");
    filesize = 0;

    /* Main compression loop */
    while (1)
    {
        size_t inSize;

        /* Fill input Buffer */
        inSize = fread(inBuff, (size_t)1, inBuffSize, finput);
        if (inSize==0) break;
        filesize += inSize;
        DISPLAYUPDATE(2, "\rRead : %u MB  ", (U32)(filesize>>20));

        {
            /* Compress (buffered streaming ensures appropriate formatting) */
            size_t usedInSize = inSize;
            size_t cSize = outBuffSize;
            size_t result = ZBUFF_compressContinue(ctx, outBuff, &cSize, inBuff, &usedInSize);
            if (ZBUFF_isError(result))
                EXM_THROW(23, "Compression error : %s ", ZBUFF_getErrorName(result));
            if (inSize != usedInSize)
                /* inBuff should be entirely consumed since buffer sizes are recommended ones */
                EXM_THROW(24, "Compression error : input block not fully consumed");

            /* Write cBlock */
            sizeCheck = fwrite(outBuff, 1, cSize, foutput);
            if (sizeCheck!=cSize) EXM_THROW(25, "Write error : cannot write compressed block into %s", output_filename);
            compressedfilesize += cSize;
        }

        DISPLAYUPDATE(2, "\rRead : %u MB  ==> %.2f%%   ", (U32)(filesize>>20), (double)compressedfilesize/filesize*100);
    }

    /* End of Frame */
    {
        size_t cSize = outBuffSize;
        size_t result = ZBUFF_compressEnd(ctx, outBuff, &cSize);
        if (result!=0) EXM_THROW(26, "Compression error : cannot create frame end");

        sizeCheck = fwrite(outBuff, 1, cSize, foutput);
        if (sizeCheck!=cSize) EXM_THROW(27, "Write error : cannot write frame end into %s", output_filename);
        compressedfilesize += cSize;
    }

    /* Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2,"Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);

    /* clean */
    free(inBuff);
    free(outBuff);
    ZBUFF_freeCCtx(ctx);
    fclose(finput);
    if (fclose(foutput)) EXM_THROW(28, "Write error : cannot properly close %s", output_filename);

    return compressedfilesize;
}


#ifndef ZSTDC_NO_DECOMPRESSOR
unsigned long long FIO_decompressFrame(FILE* foutput, FILE* finput,
                                       BYTE* inBuff, size_t inBuffSize, size_t alreadyLoaded,
                                       BYTE* outBuff, size_t outBuffSize,
                                       ZBUFF_DCtx* dctx)
{
    U64    frameSize = 0;
    size_t readSize=alreadyLoaded;

    /* Main decompression Loop */
    ZBUFF_decompressInit(dctx);
    while (1)
    {
        /* Decode */
        size_t sizeCheck;
        size_t inSize=readSize, decodedSize=outBuffSize;
        size_t inStart=0;
        size_t toRead = ZBUFF_decompressContinue(dctx, outBuff, &decodedSize, inBuff+inStart, &inSize);
        if (ZBUFF_isError(toRead)) EXM_THROW(36, "Decoding error : %s", ZBUFF_getErrorName(toRead));
        readSize -= inSize;
        inStart += inSize;

        /* Write block */
        sizeCheck = fwrite(outBuff, 1, decodedSize, foutput);
        if (sizeCheck != decodedSize) EXM_THROW(37, "Write error : unable to write data block to destination file");
        frameSize += decodedSize;
        DISPLAYUPDATE(2, "\rDecoded : %u MB...     ", (U32)(frameSize>>20) );

        if (toRead == 0) break;
        if (readSize) continue;   /* still some data left within inBuff */

        /* Fill input buffer */
        if (toRead > inBuffSize) EXM_THROW(34, "too large block");
        readSize = fread(inBuff, 1, toRead, finput);
        if (readSize != toRead) EXM_THROW(35, "Read error");
    }

    return frameSize;
}


unsigned long long FIO_decompressFilename(const char* output_filename, const char* input_filename)
{
    FILE* finput, *foutput;
    BYTE* inBuff=NULL;
    size_t inBuffSize = ZBUFF_recommendedDInSize();
    BYTE* outBuff=NULL;
    size_t outBuffSize = ZBUFF_recommendedDOutSize();
    U64   filesize = 0;
    size_t toRead;


    /* Init */
    ZBUFF_DCtx* dctx = ZBUFF_createDCtx();
    FIO_getFileHandles(&finput, &foutput, input_filename, output_filename);

    /* Allocate Memory (if needed) */
    inBuff  = (BYTE*)malloc(inBuffSize);
    outBuff  = (BYTE*)malloc(outBuffSize);
    if (!inBuff || !outBuff) EXM_THROW(33, "Allocation error : not enough memory");

    /* for each frame */
    for ( ; ; )
    {
        size_t sizeCheck;
        /* check magic number -> version */
        toRead = 4;
        sizeCheck = fread(inBuff, (size_t)1, toRead, finput);
        if (sizeCheck==0) break;   /* no more input */
        if (sizeCheck != toRead) EXM_THROW(31, "Read error : cannot read header");
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
        if (ZSTD_isLegacy(MEM_readLE32(inBuff)))
        {
            filesize += FIO_decompressLegacyFrame(foutput, finput, MEM_readLE32(inBuff));
            continue;
        }
#endif   /* ZSTD_LEGACY_SUPPORT */

        filesize += FIO_decompressFrame(foutput, finput, inBuff, inBuffSize, toRead, outBuff, outBuffSize, dctx);
    }

    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Decoded %llu bytes   \n", (long long unsigned)filesize);

    /* clean */
    free(inBuff);
    free(outBuff);
    ZBUFF_freeDCtx(dctx);
    fclose(finput);
    if (fclose(foutput)) EXM_THROW(38, "Write error : cannot properly close %s", output_filename);

    return filesize;
}
#endif

