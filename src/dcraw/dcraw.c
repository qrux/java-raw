/*
   dcraw.c -- Dave Coffin's raw photo decoder
   Copyright 1997-2003 by Dave Coffin cybercom dot net user dcoffin

   This is a portable ANSI C program to convert raw image files from
   any digital camera into PPM format.  TIFF and CIFF parsing are
   based upon public specifications, but no such documentation is
   available for the raw sensor data, so writing this program has
   been an immense effort.

   This code is freely licensed for all uses, commercial and
   otherwise.  Comments, questions, and encouragement are welcome.

   $Revision: 1.1.1.1 $
   $Date: 2004/03/25 02:21:43 $

   The Canon EOS-1D and some Kodak cameras compress their raw data
   with lossless JPEG.  To read such images, you must also download:

	http://www.cybercom.net/~dcoffin/dcraw/ljpeg_decode.tar.gz
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define strcasecmp stricmp
typedef __int64 INT64;
#else
#define __USE_XOPEN
#include <unistd.h>
#include <netinet/in.h>
typedef long long INT64;
#endif

#ifdef LJPEG_DECODE
#include "jpeg.h"
#include "mcu.h"
#include "proto.h"
#endif

typedef unsigned char uchar;
typedef unsigned short ushort;

/* Global Variables */

FILE *ifp;
short order;
char make[64], model[64], model2[64];
int raw_height, raw_width;	/* Including black borders */
int timestamp;
int tiff_data_offset, tiff_data_compression;
int kodak_data_compression;
int nef_curve_offset;
int height, width, colors, black, rgb_max;
int is_canon, is_cmy, is_foveon, use_coeff, trim, ymag;
unsigned filters;
ushort (*image)[4];
void (*load_raw)();
float gamma_val=0.8, bright=1.0, red_scale=1.0, blue_scale=1.0;
int four_color_rgb=0, use_camera_wb=0, document_mode=0, quick_interpolate=0;
float camera_red, camera_blue;
float pre_mul[4], coeff[3][4];
int histogram[0x2000];
void write_ppm(FILE *);
void (*write_fun)(FILE *) = write_ppm;

struct decode {
  struct decode *branch[2];
  int leaf;
} first_decode[32], second_decode[512];

/*
   In order to inline this calculation, I make the risky
   assumption that all filter patterns can be described
   by a repeating pattern of eight rows and two columns

   Return values are either 0/1/2/3 = G/M/C/Y or 0/1/2/3 = R/G1/B/G2
 */
#define FC(row,col) \
	(filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3)
/*
   PowerShot 600 uses 0xe1e4e1e4:

	  0 1 2 3 4 5
	0 G M G M G M
	1 C Y C Y C Y
	2 M G M G M G
	3 C Y C Y C Y

   PowerShot A5 uses 0x1e4e1e4e:

	  0 1 2 3 4 5
	0 C Y C Y C Y
	1 G M G M G M
	2 C Y C Y C Y
	3 M G M G M G

   PowerShot A50 uses 0x1b4e4b1e:

	  0 1 2 3 4 5
	0 C Y C Y C Y
	1 M G M G M G
	2 Y C Y C Y C
	3 G M G M G M
	4 C Y C Y C Y
	5 G M G M G M
	6 Y C Y C Y C
	7 M G M G M G

   PowerShot Pro70 uses 0x1e4b4e1b:

	  0 1 2 3 4 5
	0 Y C Y C Y C
	1 M G M G M G
	2 C Y C Y C Y
	3 G M G M G M
	4 Y C Y C Y C
	5 G M G M G M
	6 C Y C Y C Y
	7 M G M G M G

   PowerShots Pro90 and G1 use 0xb4b4b4b4:

	  0 1 2 3 4 5
	0 G M G M G M
	1 Y C Y C Y C

   All RGB cameras use one of these Bayer grids:

	0x16161616:	0x61616161:	0x49494949:	0x94949494:

	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5
	0 B G B G B G	0 G R G R G R	0 G B G B G B	0 R G R G R G
	1 G R G R G R	1 B G B G B G	1 R G R G R G	1 G B G B G B
	2 B G B G B G	2 G R G R G R	2 G B G B G B	2 R G R G R G
	3 G R G R G R	3 B G B G B G	3 R G R G R G	3 G B G B G B

 */

void merror (void *ptr, char *where)
{
  if (ptr) return;
  fprintf (stderr, "Out of memory in %s\n", where);
  exit(1);
}

void ps600_load_raw()
{
  uchar  data[1120], *dp;
  ushort pixel[896], *pix;
  int irow, orow, col;

/*
   Immediately after the 26-byte header come the data rows.  First
   the even rows 0..612, then the odd rows 1..611.  Each row is 896
   pixels, ten bits per pixel, packed into 1120 bytes (8960 bits).
 */
  for (irow=orow=0; irow < height; irow++)
  {
    fread (data, 1120, 1, ifp);
    for (dp=data, pix=pixel; dp < data+1120; dp+=10, pix+=8)
    {
      pix[0] = (dp[0] << 2) + (dp[1] >> 6    );
      pix[1] = (dp[2] << 2) + (dp[1] >> 4 & 3);
      pix[2] = (dp[3] << 2) + (dp[1] >> 2 & 3);
      pix[3] = (dp[4] << 2) + (dp[1]      & 3);
      pix[4] = (dp[5] << 2) + (dp[9]      & 3);
      pix[5] = (dp[6] << 2) + (dp[9] >> 2 & 3);
      pix[6] = (dp[7] << 2) + (dp[9] >> 4 & 3);
      pix[7] = (dp[8] << 2) + (dp[9] >> 6    );
    }
/*
   Copy 854 pixels into the image[] array.  The other 42 pixels
   are black.  Left-shift by 4 for extra precision in upcoming
   calculations.
 */
    for (col=0; col < width; col++)
      image[orow*width+col][FC(orow,col)] = pixel[col] << 4;
    for (col=width; col < 896; col++)
      black += pixel[col];

    if ((orow+=2) > height)	/* Once we've read all the even rows, */
      orow = 1;			/* read the odd rows. */
  }
  black = ((INT64) black << 4) / ((896 - width) * height);
}

void a5_load_raw()
{
  uchar  data[1240], *dp;
  ushort pixel[992], *pix;
  int row, col;

/*
   Each data row is 992 ten-bit pixels, packed into 1240 bytes.
 */
  for (row=0; row < height; row++) {
    fread (data, 1240, 1, ifp);
    for (dp=data, pix=pixel; dp < data+1240; dp+=10, pix+=8)
    {
      pix[0] = (dp[1] << 2) + (dp[0] >> 6);
      pix[1] = (dp[0] << 4) + (dp[3] >> 4);
      pix[2] = (dp[3] << 6) + (dp[2] >> 2);
      pix[3] = (dp[2] << 8) + (dp[5]     );
      pix[4] = (dp[4] << 2) + (dp[7] >> 6);
      pix[5] = (dp[7] << 4) + (dp[6] >> 4);
      pix[6] = (dp[6] << 6) + (dp[9] >> 2);
      pix[7] = (dp[9] << 8) + (dp[8]     );
    }
/*
   Copy 960 pixels into the image[] array.  The other 32 pixels
   are black.  Left-shift by 4 for extra precision in upcoming
   calculations.
 */
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = (pixel[col] & 0x3ff) << 4;
    for (col=width; col < 992; col++)
      black += pixel[col] & 0x3ff;
  }
  black = ((INT64) black << 4) / ((992 - width) * height);
}

void a50_load_raw()
{
  uchar  data[1650], *dp;
  ushort pixel[1320], *pix;
  int row, col;

/*
  Each row is 1320 ten-bit pixels, packed into 1650 bytes.
 */
  for (row=0; row < height; row++) {
    fread (data, 1650, 1, ifp);
    for (dp=data, pix=pixel; dp < data+1650; dp+=10, pix+=8)
    {
      pix[0] = (dp[1] << 2) + (dp[0] >> 6);
      pix[1] = (dp[0] << 4) + (dp[3] >> 4);
      pix[2] = (dp[3] << 6) + (dp[2] >> 2);
      pix[3] = (dp[2] << 8) + (dp[5]     );
      pix[4] = (dp[4] << 2) + (dp[7] >> 6);
      pix[5] = (dp[7] << 4) + (dp[6] >> 4);
      pix[6] = (dp[6] << 6) + (dp[9] >> 2);
      pix[7] = (dp[9] << 8) + (dp[8]     );
    }
/*
   Copy 1290 pixels into the image[] array.  The other 30 pixels
   are black.  Left-shift by 4 for extra precision in upcoming
   calculations.
 */
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = (pixel[col] & 0x3ff) << 4;
    for (col=width; col < 1320; col++)
      black += pixel[col] & 0x3ff;
  }
  black = ((INT64) black << 4) / ((1320 - width) * height);
}

void pro70_load_raw()
{
  uchar  data[1940], *dp;
  ushort pixel[1552], *pix;
  int row, col;

/*
  Each row is 1552 ten-bit pixels, packed into 1940 bytes.
 */
  for (row=0; row < height; row++) {
    fread (data, 1940, 1, ifp);
    for (dp=data, pix=pixel; dp < data+1940; dp+=10, pix+=8)
    {
      pix[0] = (dp[1] << 2) + (dp[0] >> 6);	/* Same as PS A5 */
      pix[1] = (dp[0] << 4) + (dp[3] >> 4);
      pix[2] = (dp[3] << 6) + (dp[2] >> 2);
      pix[3] = (dp[2] << 8) + (dp[5]     );
      pix[4] = (dp[4] << 2) + (dp[7] >> 6);
      pix[5] = (dp[7] << 4) + (dp[6] >> 4);
      pix[6] = (dp[6] << 6) + (dp[9] >> 2);
      pix[7] = (dp[9] << 8) + (dp[8]     );
    }
/*
   Copy all pixels into the image[] array.  Left-shift by 4 for
   extra precision in upcoming calculations.  No black pixels?
 */
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = (pixel[col] & 0x3ff) << 4;
  }
}

/*
   A rough description of Canon's compression algorithm:

+  Each pixel outputs a 10-bit sample, from 0 to 1023.
+  Split the data into blocks of 64 samples each.
+  Subtract from each sample the value of the sample two positions
   to the left, which has the same color filter.  From the two
   leftmost samples in each row, subtract 512.
+  For each nonzero sample, make a token consisting of two four-bit
   numbers.  The low nibble is the number of bits required to
   represent the sample, and the high nibble is the number of
   zero samples preceding this sample.
+  Output this token as a variable-length bitstring using
   one of three tablesets.  Follow it with a fixed-length
   bitstring containing the sample.

   The "first_decode" table is used for the first sample in each
   block, and the "second_decode" table is used for the others.
 */

/*
   Construct a decode tree according the specification in *source.
   The first 16 bytes specify how many codes should be 1-bit, 2-bit
   3-bit, etc.  Bytes after that are the leaf values.

   For example, if the source is

    { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
      0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },

   then the code is

	00		0x04
	010		0x03
	011		0x05
	100		0x06
	101		0x02
	1100		0x07
	1101		0x01
	11100		0x08
	11101		0x09
	11110		0x00
	111110		0x0a
	1111110		0x0b
	1111111		0xff
 */
void make_decoder(struct decode *dest, const uchar *source, int level)
{
  static struct decode *free;	/* Next unused node */
  static int leaf;		/* number of leaves already added */
  int i, next;

  if (level==0) {
    free = dest;
    leaf = 0;
  }
  free++;
/*
   At what level should the next leaf appear?
 */
  for (i=next=0; i <= leaf && next < 16; )
    i += source[next++];

  if (level < next) {		/* Are we there yet? */
    dest->branch[0] = free;
    make_decoder(free,source,level+1);
    dest->branch[1] = free;
    make_decoder(free,source,level+1);
  } else
    dest->leaf = source[16 + leaf++];
}

void init_tables(unsigned table)
{
  static const uchar first_tree[3][29] = {
    { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
      0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },

    { 0,2,2,3,1,1,1,1,2,0,0,0,0,0,0,0,
      0x03,0x02,0x04,0x01,0x05,0x00,0x06,0x07,0x09,0x08,0x0a,0x0b,0xff  },

    { 0,0,6,3,1,1,2,0,0,0,0,0,0,0,0,0,
      0x06,0x05,0x07,0x04,0x08,0x03,0x09,0x02,0x00,0x0a,0x01,0x0b,0xff  },
  };

  static const uchar second_tree[3][180] = {
    { 0,2,2,2,1,4,2,1,2,5,1,1,0,0,0,139,
      0x03,0x04,0x02,0x05,0x01,0x06,0x07,0x08,
      0x12,0x13,0x11,0x14,0x09,0x15,0x22,0x00,0x21,0x16,0x0a,0xf0,
      0x23,0x17,0x24,0x31,0x32,0x18,0x19,0x33,0x25,0x41,0x34,0x42,
      0x35,0x51,0x36,0x37,0x38,0x29,0x79,0x26,0x1a,0x39,0x56,0x57,
      0x28,0x27,0x52,0x55,0x58,0x43,0x76,0x59,0x77,0x54,0x61,0xf9,
      0x71,0x78,0x75,0x96,0x97,0x49,0xb7,0x53,0xd7,0x74,0xb6,0x98,
      0x47,0x48,0x95,0x69,0x99,0x91,0xfa,0xb8,0x68,0xb5,0xb9,0xd6,
      0xf7,0xd8,0x67,0x46,0x45,0x94,0x89,0xf8,0x81,0xd5,0xf6,0xb4,
      0x88,0xb1,0x2a,0x44,0x72,0xd9,0x87,0x66,0xd4,0xf5,0x3a,0xa7,
      0x73,0xa9,0xa8,0x86,0x62,0xc7,0x65,0xc8,0xc9,0xa1,0xf4,0xd1,
      0xe9,0x5a,0x92,0x85,0xa6,0xe7,0x93,0xe8,0xc1,0xc6,0x7a,0x64,
      0xe1,0x4a,0x6a,0xe6,0xb3,0xf1,0xd3,0xa5,0x8a,0xb2,0x9a,0xba,
      0x84,0xa4,0x63,0xe5,0xc5,0xf3,0xd2,0xc4,0x82,0xaa,0xda,0xe4,
      0xf2,0xca,0x83,0xa3,0xa2,0xc3,0xea,0xc2,0xe2,0xe3,0xff,0xff  },

    { 0,2,2,1,4,1,4,1,3,3,1,0,0,0,0,140,
      0x02,0x03,0x01,0x04,0x05,0x12,0x11,0x06,
      0x13,0x07,0x08,0x14,0x22,0x09,0x21,0x00,0x23,0x15,0x31,0x32,
      0x0a,0x16,0xf0,0x24,0x33,0x41,0x42,0x19,0x17,0x25,0x18,0x51,
      0x34,0x43,0x52,0x29,0x35,0x61,0x39,0x71,0x62,0x36,0x53,0x26,
      0x38,0x1a,0x37,0x81,0x27,0x91,0x79,0x55,0x45,0x28,0x72,0x59,
      0xa1,0xb1,0x44,0x69,0x54,0x58,0xd1,0xfa,0x57,0xe1,0xf1,0xb9,
      0x49,0x47,0x63,0x6a,0xf9,0x56,0x46,0xa8,0x2a,0x4a,0x78,0x99,
      0x3a,0x75,0x74,0x86,0x65,0xc1,0x76,0xb6,0x96,0xd6,0x89,0x85,
      0xc9,0xf5,0x95,0xb4,0xc7,0xf7,0x8a,0x97,0xb8,0x73,0xb7,0xd8,
      0xd9,0x87,0xa7,0x7a,0x48,0x82,0x84,0xea,0xf4,0xa6,0xc5,0x5a,
      0x94,0xa4,0xc6,0x92,0xc3,0x68,0xb5,0xc8,0xe4,0xe5,0xe6,0xe9,
      0xa2,0xa3,0xe3,0xc2,0x66,0x67,0x93,0xaa,0xd4,0xd5,0xe7,0xf8,
      0x88,0x9a,0xd7,0x77,0xc4,0x64,0xe2,0x98,0xa5,0xca,0xda,0xe8,
      0xf3,0xf6,0xa9,0xb2,0xb3,0xf2,0xd2,0x83,0xba,0xd3,0xff,0xff  },

    { 0,0,6,2,1,3,3,2,5,1,2,2,8,10,0,117,
      0x04,0x05,0x03,0x06,0x02,0x07,0x01,0x08,
      0x09,0x12,0x13,0x14,0x11,0x15,0x0a,0x16,0x17,0xf0,0x00,0x22,
      0x21,0x18,0x23,0x19,0x24,0x32,0x31,0x25,0x33,0x38,0x37,0x34,
      0x35,0x36,0x39,0x79,0x57,0x58,0x59,0x28,0x56,0x78,0x27,0x41,
      0x29,0x77,0x26,0x42,0x76,0x99,0x1a,0x55,0x98,0x97,0xf9,0x48,
      0x54,0x96,0x89,0x47,0xb7,0x49,0xfa,0x75,0x68,0xb6,0x67,0x69,
      0xb9,0xb8,0xd8,0x52,0xd7,0x88,0xb5,0x74,0x51,0x46,0xd9,0xf8,
      0x3a,0xd6,0x87,0x45,0x7a,0x95,0xd5,0xf6,0x86,0xb4,0xa9,0x94,
      0x53,0x2a,0xa8,0x43,0xf5,0xf7,0xd4,0x66,0xa7,0x5a,0x44,0x8a,
      0xc9,0xe8,0xc8,0xe7,0x9a,0x6a,0x73,0x4a,0x61,0xc7,0xf4,0xc6,
      0x65,0xe9,0x72,0xe6,0x71,0x91,0x93,0xa6,0xda,0x92,0x85,0x62,
      0xf3,0xc5,0xb2,0xa4,0x84,0xba,0x64,0xa5,0xb3,0xd2,0x81,0xe5,
      0xd3,0xaa,0xc4,0xca,0xf2,0xb1,0xe4,0xd1,0x83,0x63,0xea,0xc3,
      0xe2,0x82,0xf1,0xa3,0xc2,0xa1,0xc1,0xe3,0xa2,0xe1,0xff,0xff  }
  };

  if (table > 2) table = 2;
  memset( first_decode, 0, sizeof first_decode);
  memset(second_decode, 0, sizeof second_decode);
  make_decoder( first_decode,  first_tree[table], 0);
  make_decoder(second_decode, second_tree[table], 0);
}

/*
   getbits(-1) initializes the buffer
   getbits(n) where 0 <= n <= 25 returns an n-bit integer
 */
unsigned long getbits(int nbits)
{
  static unsigned long bitbuf=0, ret=0;
  static int vbits=0;
  unsigned char c;

  if (nbits == 0) return 0;
  if (nbits == -1)
    ret = bitbuf = vbits = 0;
  else {
    ret = bitbuf << (32 - vbits) >> (32 - nbits);
    vbits -= nbits;
  }
  while (vbits < 25) {
    c = fgetc(ifp);
    bitbuf = (bitbuf << 8) + c;
    if (c == 0xff && is_canon)	/* Canon puts an extra 0 after 0xff */
      fgetc(ifp);
    vbits += 8;
  }
  return ret;
}

/*
   Decompress "count" blocks of 64 samples each.

   Note that the width passed to this function is slightly
   larger than the global width, because it includes some
   blank pixels that (*load_raw) will strip off.
 */
void decompress(ushort *outbuf, int count)
{
  struct decode *decode, *dindex;
  int i, leaf, len, sign, diff, diffbuf[64];
  static int carry, pixel, base[2];

  if (!outbuf) {			/* Initialize */
    carry = pixel = 0;
    fseek (ifp, count, SEEK_SET);
    getbits(-1);
    return;
  }
  while (count--) {
    memset(diffbuf,0,sizeof diffbuf);
    decode = first_decode;
    for (i=0; i < 64; i++ ) {

      for (dindex=decode; dindex->branch[0]; )
	dindex = dindex->branch[getbits(1)];
      leaf = dindex->leaf;
      decode = second_decode;

      if (leaf == 0 && i) break;
      if (leaf == 0xff) continue;
      i  += leaf >> 4;
      len = leaf & 15;
      if (len == 0) continue;
      sign=(getbits(1));	/* 1 is positive, 0 is negative */
      diff=getbits(len-1);
      if (sign)
	diff += 1 << (len-1);
      else
	diff += (-1 << len) + 1;
      if (i < 64) diffbuf[i] = diff;
    }
    diffbuf[0] += carry;
    carry = diffbuf[0];
    for (i=0; i < 64; i++ ) {
      if (pixel++ % raw_width == 0)
	base[0] = base[1] = 512;
      outbuf[i] = ( base[i & 1] += diffbuf[i] );
    }
    outbuf += 64;
  }
}

/*
   Return 0 if the image starts with compressed data,
   1 if it starts with uncompressed low-order bits.

   In Canon compressed data, 0xff is always followed by 0x00.
 */
int canon_has_lowbits()
{
  uchar test[8192];
  int ret=1, i;

  fseek (ifp, 0, SEEK_SET);
  fread (test, 1, 8192, ifp);
  for (i=540; i < 8191; i++)
    if (test[i] == 0xff) {
      if (test[i+1]) return 1;
      ret=0;
    }
  return ret;
}

void canon_compressed_load_raw()
{
  ushort *pixel, *prow;
  int lowbits, shift, i, row, r, col, save;
  unsigned top=0, left=0, irow, icol;
  uchar c;

/* Set the width of the black borders */
  switch (raw_width) {
    case 2144:  top = 8;  left =  4;  break;	/* G1 */
    case 2224:  top = 6;  left = 48;  break;	/* EOS D30 */
    case 2376:  top = 6;  left = 12;  break;	/* G2 or G3 */
    case 2672:  top = 6;  left = 12;  break;	/* S50 */
    case 3152:  top =12;  left = 64;  break;	/* EOS D60 */
  }
  pixel = calloc (raw_width*8, sizeof *pixel);
  merror (pixel, "canon_compressed_load_raw()");
  lowbits = canon_has_lowbits();
  shift = 4 - lowbits*2;
  decompress(0, 540 + lowbits*raw_height*raw_width/4);
  for (row = 0; row < raw_height; row += 8) {
    decompress(pixel, raw_width/8);		/* Get eight rows */
    if (lowbits) {
      save = ftell(ifp);			/* Don't lose our place */
      fseek (ifp, 26 + row*raw_width/4, SEEK_SET);
      for (prow=pixel, i=0; i < raw_width*2; i++) {
	c = fgetc(ifp);
	for (r = 0; r < 8; r += 2)
	  *prow++ = (*prow << 2) + ((c >> r) & 3);
      }
      fseek (ifp, save, SEEK_SET);
    }
    for (r=0; r < 8; r++)
      for (col = 0; col < raw_width; col++) {
	irow = row+r-top;
	icol = col-left;
	if (irow >= height) continue;
	if (icol < width)
	  image[irow*width+icol][FC(irow,icol)] =
		pixel[r*raw_width+col] << shift;
	  else
	    black += pixel[r*raw_width+col];
      }
  }
  free(pixel);
  black = ((INT64) black << shift) / ((raw_width - width) * height);
}

#ifdef LJPEG_DECODE
/*
   Lossless JPEG code calls this function to get data.
 */
int ReadJpegData (char *buffer, int numBytes)
{
  return fread(buffer, 1, numBytes, ifp);
}

/*
   Called from DecodeImage() in huffd.c to write one row.
   Notice that one row of the JPEG data is two rows for us.
   Canon did this so that the predictors could work against
   like colors.  Quite clever!

   Kodak didn't think of that.  8-/
 */
void PmPutRow(ushort **buf, int numComp, int numCol, int row)
{
  int r, col, trick=1;

  trick = numComp * numCol / width;
  row *= trick;
  for (r = row; r < row+trick; r++)
    for (col = 0; col < width; col+=2) {
      image[r*width+col+0][FC(r,col+0)] = buf[0][0] << 2;
      image[r*width+col+1][FC(r,col+1)] = buf[0][1] << 2;
      buf++;
    }
}

void lossless_jpeg_load_raw()
{
  DecompressInfo dcInfo;

  fseek (ifp, tiff_data_offset, SEEK_SET);

  MEMSET(&dcInfo, 0, sizeof(dcInfo));
  ReadFileHeader (&dcInfo);
  ReadScanHeader (&dcInfo);
  DecoderStructInit (&dcInfo);
  HuffDecoderInit (&dcInfo);
  DecodeImage (&dcInfo);
  FreeArray2D (mcuROW1);
  FreeArray2D (mcuROW2);
}
#else
void lossless_jpeg_load_raw() { }
#endif /* LJPEG_DECODE */

ushort fget2 (FILE *f);
int    fget4 (FILE *f);

void nikon_compressed_load_raw()
{
  int left=0, right=0;
  static const uchar nikon_tree[] = {
    0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,
    5,4,3,6,2,7,1,0,8,9,11,10,12
  };
  int vpred[4], hpred[2], csize, row, col, i, len, diff;
  ushort *curve;
  struct decode *dindex;

  if (!strcmp(model,"D1X"))
    right = 4;
  if (!strcmp(model,"D2H")) {
    left  = 6;
    right = 8;
  }

  memset( first_decode, 0, sizeof first_decode);
  make_decoder( first_decode,  nikon_tree, 0);

  fseek (ifp, nef_curve_offset, SEEK_SET);
  for (i=0; i < 4; i++)
    vpred[i] = fget2(ifp);
  csize = fget2(ifp);
  curve = calloc (csize, sizeof *curve);
  merror (curve, "nikon_compressed_load_raw()");
  for (i=0; i < csize; i++)
    curve[i] = fget2(ifp);

  fseek (ifp, tiff_data_offset, SEEK_SET);
  getbits(-1);

  for (row=0; row < height; row++)
    for (col=-left; col < width+right; col++)
    {
      for (dindex=first_decode; dindex->branch[0]; )
	dindex = dindex->branch[getbits(1)];
      len = dindex->leaf;
      diff = getbits(len);
      if ((diff & (1 << (len-1))) == 0)
	diff -= (1 << len) - 1;
      if (col+left < 2) {
	i = 2*(row & 1) + (col & 1);
	vpred[i] += diff;
	hpred[col & 1] = vpred[i];
      } else
	hpred[col & 1] += diff;
      if ((unsigned) col >= width) continue;
      diff = hpred[col & 1];
      if (diff < 0) diff = 0;
      if (diff >= csize) diff = csize-1;
      image[row*width+col][FC(row,col)] = curve[diff] << 2;
    }
  free(curve);
}

/*
   Figure out if a NEF file is compressed.  These fancy heuristics
   are only needed for the D100, thanks to a bug in some cameras
   that tags all images as "compressed".
 */
int nikon_is_compressed()
{
  uchar test[256];
  int i;

  if (tiff_data_compression != 34713)
    return 0;
  if (strcmp(model,"D100"))
    return 1;
  fseek (ifp, tiff_data_offset, SEEK_SET);
  fread (test, 1, 256, ifp);
  for (i=15; i < 256; i+=16)
    if (test[i]) return 1;
  return 0;
}

void nikon_load_raw()
{
  int left=0, right=0, skip16=0;
  int irow, row, col, i;

  if (!strcmp(model,"D100"))
    width = 3034;
  if (nikon_is_compressed()) {
    nikon_compressed_load_raw();
    return;
  }
  if (!strcmp(model,"D1X"))
    right = 4;
  if (!strcmp(model,"D100") && tiff_data_compression == 34713) {
    right = 3;
    skip16 = 1;
    width = 3037;
  }
  if (!strcmp(model,"D2H")) {
    left  = 6;
    right = 8;
  }

  fseek (ifp, tiff_data_offset, SEEK_SET);
  getbits(-1);
  for (irow=0; irow < height; irow++) {
    row = irow;
    if (model[0] == 'E') {
      row = irow * 2 % height + irow / (height/2);
      if (row == 1 && atoi(model+1) < 5000) {
	fseek (ifp, 0, SEEK_END);
	fseek (ifp, ftell(ifp)/2, SEEK_SET);
	getbits(-1);
      }
    }
    for (col=-left; col < width+right; col++) {
      i = getbits(12);
      if ((unsigned) col < width)
	image[row*width+col][FC(row,col)] = i << 2;
      if (skip16 && (col % 10) == 9)
	getbits(8);
    }
  }
}

void nikon_e950_load_raw()
{
  int irow, row, col;

  fseek (ifp, 0, SEEK_SET);
  getbits(-1);
  for (irow=0; irow < height; irow++) {
    row = irow * 2 % height;
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = getbits(10) << 4;
    for (col=28; col--; )
      getbits(8);
  }
}

/*
   The Fuji Super CCD is just a Bayer grid rotated 45 degrees.
 */
void fuji_s2_load_raw()
{
  ushort pixel[2944];
  int row, col, r, c;

  fseek (ifp, tiff_data_offset + (2944*24+32)*2, SEEK_SET);
  for (row=0; row < 2144; row++) {
    fread (pixel, 2, 2944, ifp);
    for (col=0; col < 2880; col++) {
      r = row + ((col+1) >> 1);
      c = 2143 - row + (col >> 1);
      image[r*width+c][FC(r,c)] = ntohs(pixel[col]) << 2;
    }
  }
}

void fuji_s5000_load_raw()
{
  ushort pixel[1472];
  int row, col, r, c;

  fseek (ifp, tiff_data_offset + (1472*4+24)*2, SEEK_SET);
  for (row=0; row < 2152; row++) {
    fread (pixel, 2, 1472, ifp);
    if (ntohs(0xaa55) == 0xaa55)	/* data is little-endian */
      swab (pixel, pixel, 1472*2);
    for (col=0; col < 1424; col++) {
      r = 1423 - col + (row >> 1);
      c = col + ((row+1) >> 1);
      image[r*width+c][FC(r,c)] = pixel[col];
    }
  }
}

/*
   The Fuji Super CCD SR has two photodiodes for each pixel.
   The secondary has about 1/16 the sensitivity of the primary,
   but this ratio may vary.
 */
void fuji_f700_load_raw()
{
  ushort pixel[2944];
  int row, col, r, c, val;

  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (row=0; row < 2168; row++) {
    fread (pixel, 2, 2944, ifp);
    if (ntohs(0xaa55) == 0xaa55)	/* data is little-endian */
      swab (pixel, pixel, 2944*2);
    for (col=0; col < 1440; col++) {
      r = 1439 - col + (row >> 1);
      c = col + ((row+1) >> 1);
      val = pixel[col+16];
      if (val == 0x3fff)		/* If the primary is maxed, */
	val = pixel[col+1488] << 4;	/* use the secondary.       */
      if (val > 0xffff)
	val = 0xffff;
      image[r*width+c][FC(r,c)] = val;
    }
  }
}

void rollei_load_raw()
{
  uchar pixel[10];
  unsigned left=0, top=0, iten=0, isix, i, buffer=0, row, col, todo[16];

  switch (raw_width) {
    case 1316: left = 6; top = 1; width = 1300; height = 1030;  break;
    case 2568: left = 8; top = 2; width = 2560; height = 1960;  break;
  }
  isix = raw_width * raw_height * 5 / 8;
  fseek (ifp, tiff_data_offset, SEEK_SET);
  while (fread (pixel, 1, 10, ifp) == 10) {
    for (i=0; i < 10; i+=2) {
      todo[i]   = iten++;
      todo[i+1] = pixel[i] << 8 | pixel[i+1];
      buffer    = pixel[i] >> 2 | buffer << 6;
    }
    for (   ; i < 16; i+=2) {
      todo[i]   = isix++;
      todo[i+1] = buffer >> (14-i)*5;
    }
    for (i=0; i < 16; i+=2) {
      row = todo[i] / raw_width - top;
      col = todo[i] % raw_width - left;
      if (row < height && col < width)
	image[row*width+col][FC(row,col)] = (todo[i+1] & 0x3ff) << 4;
    }
  }
}

void packed_12_load_raw()
{
  int row, col;

  fseek (ifp, tiff_data_offset, SEEK_SET);
  getbits(-1);
  for (row=0; row < height; row++)
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = getbits(12) << 2;
}

void unpacked_12_load_raw()
{
  ushort *pixel;
  int row, col;

  pixel = calloc (width, sizeof *pixel);
  merror (pixel, "unpacked_12_load_raw()");
  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (row=0; row < height; row++) {
    fread (pixel, 2, width, ifp);
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = ntohs(pixel[col]) << 2;
  }
  free(pixel);
}

void olympus_load_raw()
{
  ushort *pixel;
  int row, col;

  pixel = calloc (width, sizeof *pixel);
  merror (pixel, "olympus_load_raw()");
  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (row=0; row < height; row++) {
    fread (pixel, 2, width, ifp);
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = ntohs(pixel[col]) >> 2;
  }
  free(pixel);
}

void olympus2_load_raw()
{
  int irow, row, col;

  for (irow=0; irow < height; irow++) {
    row = irow * 2 % height + irow / (height/2);
    if (row < 2) {
      fseek (ifp, 15360 + row*(width*height*3/4 + 184), SEEK_SET);
      getbits(-1);
    }
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = getbits(12) << 2;
  }
}

void kyocera_load_raw()
{
  int row, col;

  fseek (ifp, tiff_data_offset, SEEK_SET);
  getbits(-1);
  for (row=0; row < height; row++)
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = getbits(12) << 2;
}

void casio_easy_load_raw()
{
  uchar *pixel;
  int row, col;

  pixel = calloc (raw_width, sizeof *pixel);
  merror (pixel, "casio_easy_load_raw()");
  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (row=0; row < height; row++) {
    fread (pixel, 1, raw_width, ifp);
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = pixel[col] << 6;
  }
  free (pixel);
}

void casio_qv5700_load_raw()
{
  uchar  data[3232],  *dp;
  ushort pixel[2576], *pix;
  int row, col;

  fseek (ifp, 0, SEEK_SET);
  for (row=0; row < height; row++) {
    fread (data, 1, 3232, ifp);
    for (dp=data, pix=pixel; dp < data+3220; dp+=5, pix+=4) {
      pix[0] = (dp[0] << 2) + (dp[1] >> 6);
      pix[1] = (dp[1] << 4) + (dp[2] >> 4);
      pix[2] = (dp[2] << 6) + (dp[3] >> 2);
      pix[3] = (dp[3] << 8) + (dp[4]     );
    }
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = (pixel[col] & 0x3ff) << 4;
  }
}

void nucore_load_raw()
{
  uchar *data, *dp;
  int irow, row, col;

  data = calloc (width, 2);
  merror (data, "nucore_load_raw()");
  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (irow=0; irow < height; irow++) {
    fread (data, 2, width, ifp);
    if (model[0] == 'B' && width == 2598)
      row = height - 1 - irow/2 - height/2 * (irow & 1);
    else
      row = irow;
    for (dp=data, col=0; col < width; col++, dp+=2)
      image[row*width+col][FC(row,col)] = (dp[0] << 2) + (dp[1] << 10);
  }
  free(data);
}

void kodak_easy_load_raw()
{
  uchar *pixel;
  int row, col, margin;

  if ((margin = (raw_width - width)/2))
    black = 0;
  pixel = calloc (raw_width, sizeof *pixel);
  merror (pixel, "kodak_easy_load_raw()");
  fseek (ifp, tiff_data_offset, SEEK_SET);
  for (row=0; row < height; row++) {
    fread (pixel, 1, raw_width, ifp);
    for (col=0; col < width; col++)
      image[row*width+col][FC(row,col)] = (ushort) pixel[col+margin] << 6;
    if (margin == 2)
      black += pixel[0] + pixel[1] + pixel[raw_width-2] + pixel[raw_width-1];
  }
  if (margin)
    black = ((INT64) black << 6) / (4 * height);
  free(pixel);
}

void kodak_compressed_load_raw()
{
  uchar c, blen[256];
  unsigned row, col, len, i, bits=0, pred[2];
  INT64 bitbuf=0;
  int diff;

  fseek (ifp, tiff_data_offset, SEEK_SET);

  for (row=0; row < height; row++)
    for (col=0; col < width; col++)
    {
      if ((col & 255) == 0) {		/* Get the bit-lengths of the */
	len = width - col;		/* next 256 pixel values      */
	if (len > 256) len = 256;
	for (i=0; i < len; ) {
	  c = fgetc(ifp);
	  blen[i++] = c & 15;
	  blen[i++] = c >> 4;
	}
	bitbuf = bits = pred[0] = pred[1] = 0;
	if (len % 8 == 4) {
	  bitbuf  = fgetc(ifp) << 8;
	  bitbuf += fgetc(ifp);
	  bits = 16;
	}
      }
      len = blen[col & 255];		/* Number of bits for this pixel */
      if (bits < len) {			/* Got enough bits in the buffer? */
	for (i=0; i < 32; i+=8)
	  bitbuf += (INT64) fgetc(ifp) << (bits+(i^8));
	bits += 32;
      }
      diff = bitbuf & (0xffff >> (16-len));  /* Pull bits from buffer */
      bitbuf >>= len;
      bits -= len;
      if ((diff & (1 << (len-1))) == 0)
	diff -= (1 << len) - 1;
      pred[col & 1] += diff;
      diff = pred[col & 1];
      image[row*width+col][FC(row,col)] = diff << 2;
    }
}

void kodak_yuv_load_raw()
{
  uchar c, blen[384];
  unsigned row, col, len, bits=0;
  INT64 bitbuf=0;
  int i, li=0, si, diff, six[6], y[4], cb=0, cr=0, rgb[3];
  ushort *ip;

  fseek (ifp, tiff_data_offset, SEEK_SET);

  for (row=0; row < height; row+=2)
    for (col=0; col < width; col+=2) {
      if ((col & 127) == 0) {
	len = (width - col) * 3;
	if (len > 384) len = 384;
	for (i=0; i < len; ) {
	  c = fgetc(ifp);
	  blen[i++] = c & 15;
	  blen[i++] = c >> 4;
	}
	li = bitbuf = bits = y[1] = y[3] = cb = cr = 0;
      }
      for (si=0; si < 6; si++) {
	len = blen[li++];
	if (bits < len) {
	  for (i=0; i < 32; i+=8)
	    bitbuf += (INT64) fgetc(ifp) << (bits+(i^8));
	  bits += 32;
	}
	diff = bitbuf & (0xffff >> (16-len));
	bitbuf >>= len;
	bits -= len;
	if ((diff & (1 << (len-1))) == 0)
	  diff -= (1 << len) - 1;
	six[si] = diff << 2;
      }
      y[0] = six[0] + y[1];
      y[1] = six[1] + y[0];
      y[2] = six[2] + y[3];
      y[3] = six[3] + y[2];
      cb  += six[4];
      cr  += six[5];
      for (i=0; i < 4; i++) {
	ip = image[(row+(i >> 1))*width + col+(i & 1)];
	rgb[0] = y[i] + 1.40200/2 * cr;
	rgb[1] = y[i] - 0.34414/2 * cb - 0.71414/2 * cr;
	rgb[2] = y[i] + 1.77200/2 * cb;
	for (c=0; c < 3; c++)
	  if (rgb[c] > 0) ip[c] = rgb[c];
      }
    }
}

void foveon_decoder(struct decode *dest, unsigned huff[1024], unsigned code)
{
  static struct decode *free;
  int i, len;

  free++;
  if (code) {
    for (i=0; i < 1024; i++)
      if (huff[i] == code) {
	dest->leaf = i;
	return;
      }
  } else
    free = dest + 1;

  if ((len = code >> 27) > 26) return;
  code = (len+1) << 27 | (code & 0x3ffffff) << 1;

  dest->branch[0] = free;
  foveon_decoder (free, huff, code);
  dest->branch[1] = free;
  foveon_decoder (free, huff, code+1);
}

void foveon_load_raw()
{
  struct decode decode[2048], *dindex;
  short diff[1024], pred[3];
  unsigned huff[1024], bitbuf=0, top=0, left=0;
  int row, col, bit=-1, c, i;

/* Set the width of the black borders */
  switch (raw_height) {
    case  763:  top = 2;  break;
    case 1531:  top = 7;  break;
  }
  switch (raw_width) {
    case 1152: left =  8;  break;
    case 2304: left = 17;  break;
  }
  fseek (ifp, 260, SEEK_SET);
  for (i=0; i < 1024; i++)
    diff[i] = fget2(ifp);
  for (i=0; i < 1024; i++)
    huff[i] = fget4(ifp);

  memset (decode, 0, sizeof decode);
  foveon_decoder (decode,  huff, 0);

  for (row=0; row < raw_height; row++) {
    memset (pred, 0, sizeof pred);
    if (!bit) fget4(ifp);
    for (col=bit=0; col < raw_width; col++) {
      for (c=0; c < 3; c++) {
	for (dindex=decode; dindex->branch[0]; ) {
	  if ((bit = (bit-1) & 31) == 31)
	    for (i=0; i < 4; i++)
	      bitbuf = (bitbuf << 8) + fgetc(ifp);
	  dindex = dindex->branch[bitbuf >> bit & 1];
	}
	pred[c] += diff[dindex->leaf];
      }
      if ((unsigned) row-top  >= height ||
	  (unsigned) col-left >= width ) continue;
      for (c=0; c < 3; c++)
	if (pred[c] > 0)
	  image[(row-top)*width+(col-left)][c] = pred[c];
    }
  }
}

int apply_curve(int i, const int *curve)
{
  if (i <= -curve[0])
    return -curve[curve[0]]-1;
  else if (i < 0)
    return -curve[1-i];
  else if (i < curve[0])
    return  curve[1+i];
  else
    return  curve[curve[0]]+1;
}

void foveon_interpolate()
{
  float mul[3] =
  { 1.0321, 1.0, 1.1124 };
  static const int weight[3][3][3] =
  { { {   4141,  37726,  11265  },
      { -30437,  16066, -41102  },
      {    326,   -413,    362  } },
    { {   1770,  -1316,   3480  },
      {  -2139,    213,  -4998  },
      {  -2381,   3496,  -2008  } },
    { {  -3838, -24025, -12968  },
      {  20144, -12195,  30272  },
      {   -631,  -2025,    822  } } },
  curve1[73] = { 72,
     0,1,2,2,3,4,5,6,6,7,8,9,9,10,11,11,12,13,13,14,14,
    15,16,16,17,17,18,18,18,19,19,20,20,20,21,21,21,22,
    22,22,23,23,23,23,23,24,24,24,24,24,25,25,25,25,25,
    25,25,25,26,26,26,26,26,26,26,26,26,26,26,26,26,26 },
  curve2[21] = { 20,
    0,1,1,2,3,3,4,4,5,5,6,6,6,7,7,7,7,7,7,7 },
  curve3[73] = { 72,
     0,1,1,2,2,3,4,4,5,5,6,6,7,7,8,8,8,9,9,10,10,10,10,
    11,11,11,12,12,12,12,12,12,13,13,13,13,13,13,13,13,
    14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
    14,14,14,14,14,14,14,14,14,14,14,14,14,14,14 },
  curve4[37] = { 36,
    0,1,1,2,3,3,4,4,5,6,6,7,7,7,8,8,9,9,9,10,10,10,
    11,11,11,11,11,12,12,12,12,12,12,12,12,12 },
  curve5[111] = { 110,
    0,1,1,2,3,3,4,5,6,6,7,7,8,9,9,10,11,11,12,12,13,13,
    14,14,15,15,16,16,17,17,18,18,18,19,19,19,20,20,20,
    21,21,21,21,22,22,22,22,22,23,23,23,23,23,24,24,24,24,
    24,24,24,24,25,25,25,25,25,25,25,25,25,25,25,25,26,26,
    26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,
    26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26 },
  *curves[3] = { curve3, curve4, curve5 },
  trans[3][3] =
  { {   7576,  -2933,   1279  },
    { -11594,  29911, -12394  },
    {   4000, -18850,  20772  } };
  ushort *pix, prev[3], (*shrink)[3];
  int row, col, c, i, j, diff, sum, ipix[3], work[3][3], total[4];
  int (*smrow[7])[3], smlast, smred, smred_p=0, hood[7], min, max;

  /* Sharpen all colors */
  for (row=0; row < height; row++) {
    pix = image[row*width];
    memcpy (prev, pix, sizeof prev);
    for (col=0; col < width; col++) {
      for (c=0; c < 3; c++) {
	diff = pix[c] - prev[c];
	prev[c] = pix[c];
	ipix[c] = pix[c] + ((diff + (diff*diff >> 14)) * 0x3333 >> 14);
      }
      for (c=0; c < 3; c++) {
	work[0][c] = ipix[c]*ipix[c] >> 14;
	work[2][c] = ipix[c]*work[0][c] >> 14;
	work[1][2-c] = ipix[(c+1) % 3] * ipix[(c+2) % 3] >> 14;
      }
      for (c=0; c < 3; c++) {
	for (sum=i=0; i < 3; i++)
	  for (  j=0; j < 3; j++)
	    sum += weight[c][i][j] * work[i][j];
	ipix[c] = (ipix[c] + (sum >> 14)) * mul[c];
	if (ipix[c] < 0)     ipix[c] = 0;
	if (ipix[c] > 32000) ipix[c] = 32000;
	pix[c] = ipix[c];
      }
      pix += 4;
    }
  }
  /* Array for 5x5 Gaussian averaging of red values */
  smrow[6] = calloc (width*5, sizeof **smrow);
  merror (smrow[6], "foveon_interpolate()");
  for (i=0; i < 5; i++)
    smrow[i] = smrow[6] + i*width;

  /* Sharpen the reds against these Gaussian averages */
  for (smlast=-1, row=2; row < height-2; row++) {
    while (smlast < row+2) {
      for (i=0; i < 6; i++)
	smrow[(i+5) % 6] = smrow[i];
      pix = image[++smlast*width+2];
      for (col=2; col < width-2; col++) {
	smrow[4][col][0] =
	  (pix[0]*6 + (pix[-4]+pix[4])*4 + pix[-8]+pix[8] + 8) >> 4;
	pix += 4;
      }
    }
    pix = image[row*width+2];
    for (col=2; col < width-2; col++) {
      smred = (smrow[2][col][0]*6 + (smrow[1][col][0]+smrow[3][col][0])*4
		+ smrow[0][col][0]+smrow[4][col][0] + 8) >> 4;
      if (col == 2)
	smred_p = smred;
      i = pix[0] + ((pix[0] - ((smred*7 + smred_p) >> 3)) >> 2);
      if (i < 0)     i = 0;
      if (i > 10000) i = 10000;
      pix[0] = i;
      smred_p = smred;
      pix += 4;
    }
  }
  /* Limit each color value to the range of its neighbors */
  hood[0] = 4;
  for (i=0; i < 3; i++) {
    hood[i+1] = (i-width-1)*4;
    hood[i+4] = (i+width-1)*4;
  }
  for (row=1; row < height-1; row++) {
    pix = image[row*width+1];
    memcpy (prev, pix-4, sizeof prev);
    for (col=1; col < width-1; col++) {
      for (c=0; c < 3; c++) {
	for (min=max=prev[c], i=0; i < 7; i++) {
	  j = pix[hood[i]];
	  if (min > j) min = j;
	  if (max < j) max = j;
	}
	prev[c] = *pix;
	if (*pix < min) *pix = min;
	if (*pix > max) *pix = max;
	pix++;
      }
      pix++;
    }
  }
/*
   Because photons that miss one detector often hit another,
   the sum R+G+B is much less noisy than the individual colors.
   So smooth the hues without smoothing the total.
 */
  for (smlast=-1, row=2; row < height-2; row++) {
    while (smlast < row+2) {
      for (i=0; i < 6; i++)
	smrow[(i+5) % 6] = smrow[i];
      pix = image[++smlast*width+2];
      for (col=2; col < width-2; col++) {
	for (c=0; c < 3; c++)
	  smrow[4][col][c] = pix[c-8]+pix[c-4]+pix[c]+pix[c+4]+pix[c+8];
	pix += 4;
      }
    }
    pix = image[row*width+2];
    for (col=2; col < width-2; col++) {
      for (total[3]=1500, sum=60, c=0; c < 3; c++) {
	for (total[c]=i=0; i < 5; i++)
	  total[c] += smrow[i][col][c];
	total[3] += total[c];
	sum += pix[c];
      }
      j = (sum << 16) / total[3];
      for (c=0; c < 3; c++) {
	i = apply_curve ((total[c] * j >> 16) - pix[c], curve1);
	i += pix[c] - 13 - (c==1);
	ipix[c] = i - apply_curve (i, curve2);
      }
      sum = (ipix[0]+ipix[1]+ipix[1]+ipix[2]) >> 2;
      for (c=0; c < 3; c++) {
	i = ipix[c] - apply_curve (ipix[c] - sum, curve2);
	if (i < 0) i = 0;
	pix[c] = i;
      }
      pix += 4;
    }
  }
  /* Translate the image to a different colorspace */
  for (pix=image[0]; pix < image[height*width]; pix+=4) {
    for (c=0; c < 3; c++) {
      for (i=j=0; j < 3; j++)
	i += trans[c][j] * pix[j];
      i = (i+0x1000) >> 13;
      if (i < 0)     i = 0;
      if (i > 24000) i = 24000;
      ipix[c] = i;
    }
    for (c=0; c < 3; c++)
      pix[c] = ipix[c];
  }
  /* Smooth the image bottom-to-top and save at 1/4 scale */
  shrink = calloc ((width/4) * (height/4), sizeof *shrink);
  merror (shrink, "foveon_interpolate()");
  for (row = height/4; row--; )
    for (col=0; col < width/4; col++) {
      ipix[0] = ipix[1] = ipix[2] = 0;
      for (i=0; i < 4; i++)
	for (j=0; j < 4; j++)
	  for (c=0; c < 3; c++)
	    ipix[c] += image[(row*4+i)*width+col*4+j][c];
      for (c=0; c < 3; c++)
	if (row+2 > height/4)
	  shrink[row*(width/4)+col][c] = ipix[c] >> 4;
	else
	  shrink[row*(width/4)+col][c] =
	    (shrink[(row+1)*(width/4)+col][c]*1840 + ipix[c]*141) >> 12;
    }

  /* From the 1/4-scale image, smooth right-to-left */
  for (row=0; row < (height & ~3); row++) {
    ipix[0] = ipix[1] = ipix[2] = 0;
    if ((row & 3) == 0)
      for (col = width & ~3 ; col--; )
	for (c=0; c < 3; c++)
	  smrow[0][col][c] = ipix[c] =
	    (shrink[(row/4)*(width/4)+col/4][c]*1485 + ipix[c]*6707) >> 13;

  /* Then smooth left-to-right */
    ipix[0] = ipix[1] = ipix[2] = 0;
    for (col=0; col < (width & ~3); col++)
      for (c=0; c < 3; c++)
	smrow[1][col][c] = ipix[c] =
	  (smrow[0][col][c]*1485 + ipix[c]*6707) >> 13;

  /* Smooth top-to-bottom */
    if (row == 0)
      memcpy (smrow[2], smrow[1], sizeof **smrow * width);
    else
      for (col=0; col < (width & ~3); col++)
	for (c=0; c < 3; c++)
	  smrow[2][col][c] =
	    (smrow[2][col][c]*6707 + smrow[1][col][c]*1485) >> 13;

  /* Adjust the chroma toward the smooth values */
    for (col=0; col < (width & ~3); col++) {
      for (i=j=60, c=0; c < 3; c++) {
	i += smrow[2][col][c];
	j += image[row*width+col][c];
      }
      j = (j << 16) / i;
      for (sum=c=0; c < 3; c++) {
	i = (smrow[2][col][c] * j >> 16) - image[row*width+col][c];
	ipix[c] = apply_curve (i, curves[c]);
	sum += ipix[c];
      }
      sum >>= 3;
      for (c=0; c < 3; c++) {
	i = image[row*width+col][c] + ipix[c] - sum;
	if (i < 0) i = 0;
	image[row*width+col][c] = i;
      }
    }
  }
  free(shrink);
  free(smrow[6]);
}

/*
   Seach from the current directory up to the root looking for
   a ".badpixels" file, and fix those pixels now.
 */
void bad_pixels()
{
  FILE *fp;
  char *fname, *cp, line[128];
  int len, time, row, col, r, c, rad, tot, n, fixed=0;

  for (len=16 ; ; len *= 2) {
    fname = malloc (len);
    if (!fname) return;
    if (getcwd (fname, len-12)) break;
    free (fname);
    if (errno != ERANGE) return;
  }
  if (*fname != '/') return;
  cp = fname + strlen(fname);
  if (cp[-1] == '/') cp--;
  while (1) {
    strcpy (cp, "/.badpixels");
    if ((fp = fopen (fname, "r"))) break;
    if (cp == fname) break;
    while (*--cp != '/');
  }
  free (fname);
  if (!fp) return;
  while (1) {
    fgets (line, 128, fp);
    if (feof(fp)) break;
    cp = strchr (line, '#');
    if (cp) *cp = 0;
    if (sscanf (line, "%d %d %d", &col, &row, &time) != 3) continue;
    if ((unsigned) col >= width || (unsigned) row >= height) continue;
    if (time > timestamp) continue;
    for (tot=n=0, rad=1; rad < 3 && n==0; rad++)
      for (r = row-rad; r <= row+rad; r++)
	for (c = col-rad; c <= col+rad; c++)
	  if ((unsigned) r < height && (unsigned) c < width &&
		(r != row || c != col) && FC(r,c) == FC(row,col)) {
	    tot += image[r*width+c][FC(r,c)];
	    n++;
	  }
    image[row*width+col][FC(row,col)] = tot/n;
    if (!fixed++)
      fprintf (stderr, "Fixed bad pixels at:");
    fprintf (stderr, " %d,%d", col, row);
  }
  if (fixed) fputc ('\n', stderr);
  fclose (fp);
}

/*
   Automatic color balance, currently used only in Document Mode.
 */
void auto_scale()
{
  int row, col, c, val;
  int min[4], max[4], count[4];
  double sum[4], maxd=0;

  for (c=0; c < 4; c++) {
    min[c] = INT_MAX;
    sum[c] = max[c] = count[c] = 0;
  }
  for (row=0; row < height; row++)
    for (col=0; col < width; col++)
      for (c=0; c < colors; c++) {
	val = image[row*width+col][c];
	if (!val) continue;
	val -= black;
	if (val < 0) val = 0;
	if (min[c] > val) min[c] = val;
	if (max[c] < val) max[c] = val;
	sum[c] += val;
	count[c]++;
      }
  for (c=0; c < colors; c++) {		/* Smallest pre_mul[] value */
    pre_mul[c] = sum[c]/count[c];	/* should be 1.0 */
    if (maxd < pre_mul[c])
        maxd = pre_mul[c];
  }
  for (c=0; c < colors; c++)
    pre_mul[c] = maxd / pre_mul[c];
}

void scale_colors()
{
  int row, col, c, val;

  rgb_max -= black;
  for (row=0; row < height; row++)
    for (col=0; col < width; col++)
      for (c=0; c < colors; c++) {
	val = image[row*width+col][c];
	if (!val) continue;
	val -= black;
	val *= pre_mul[c];
	if (val < 0) val = 0;
	if (val > rgb_max) val = rgb_max;
	image[row*width+col][c] = val;
      }
}

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://www-ise.stanford.edu/~tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
 */
void vng_interpolate()
{
  static const signed char *cp, terms[] = {
    -2,-2,+0,-1,0,0x01, -2,-2,+0,+0,1,0x01, -2,-1,-1,+0,0,0x01,
    -2,-1,+0,-1,0,0x02, -2,-1,+0,+0,0,0x03, -2,-1,+0,+1,0,0x01,
    -2,+0,+0,-1,0,0x06, -2,+0,+0,+0,1,0x02, -2,+0,+0,+1,0,0x03,
    -2,+1,-1,+0,0,0x04, -2,+1,+0,-1,0,0x04, -2,+1,+0,+0,0,0x06,
    -2,+1,+0,+1,0,0x02, -2,+2,+0,+0,1,0x04, -2,+2,+0,+1,0,0x04,
    -1,-2,-1,+0,0,0x80, -1,-2,+0,-1,0,0x01, -1,-2,+1,-1,0,0x01,
    -1,-2,+1,+0,0,0x01, -1,-1,-1,+1,0,0x88, -1,-1,+1,-2,0,0x40,
    -1,-1,+1,-1,0,0x22, -1,-1,+1,+0,0,0x33, -1,-1,+1,+1,1,0x11,
    -1,+0,-1,+2,0,0x08, -1,+0,+0,-1,0,0x44, -1,+0,+0,+1,0,0x11,
    -1,+0,+1,-2,0,0x40, -1,+0,+1,-1,0,0x66, -1,+0,+1,+0,1,0x22,
    -1,+0,+1,+1,0,0x33, -1,+0,+1,+2,0,0x10, -1,+1,+1,-1,1,0x44,
    -1,+1,+1,+0,0,0x66, -1,+1,+1,+1,0,0x22, -1,+1,+1,+2,0,0x10,
    -1,+2,+0,+1,0,0x04, -1,+2,+1,+0,0,0x04, -1,+2,+1,+1,0,0x04,
    +0,-2,+0,+0,1,0x80, +0,-1,+0,+1,1,0x88, +0,-1,+1,-2,0,0x40,
    +0,-1,+1,+0,0,0x11, +0,-1,+2,-2,0,0x40, +0,-1,+2,-1,0,0x20,
    +0,-1,+2,+0,0,0x30, +0,-1,+2,+1,0,0x10, +0,+0,+0,+2,1,0x08,
    +0,+0,+2,-2,1,0x40, +0,+0,+2,-1,0,0x60, +0,+0,+2,+0,1,0x20,
    +0,+0,+2,+1,0,0x30, +0,+0,+2,+2,1,0x10, +0,+1,+1,+0,0,0x44,
    +0,+1,+1,+2,0,0x10, +0,+1,+2,-1,0,0x40, +0,+1,+2,+0,0,0x60,
    +0,+1,+2,+1,0,0x20, +0,+1,+2,+2,0,0x10, +1,-2,+1,+0,0,0x80,
    +1,-1,+1,+1,0,0x88, +1,+0,+1,+2,0,0x08, +1,+0,+2,-1,0,0x40,
    +1,+0,+2,+1,0,0x10
  }, chood[] = { -1,-1, -1,0, -1,+1, 0,+1, +1,+1, +1,0, +1,-1, 0,-1 };
  ushort (*brow[5])[4], *pix;
  int code[8][640], *ip, gval[8], gmin, gmax, sum[4];
  int row, col, shift, x, y, x1, x2, y1, y2, t, weight, grads, color, diag;
  int g, diff, thold, num, c;

  for (row=0; row < 8; row++) {		/* Precalculate for bilinear */
    ip = code[row];
    for (col=1; col < 3; col++) {
      memset (sum, 0, sizeof sum);
      for (y=-1; y <= 1; y++)
	for (x=-1; x <= 1; x++) {
	  shift = (y==0) + (x==0);
	  if (shift == 2) continue;
	  color = FC(row+y,col+x);
	  *ip++ = (width*y + x)*4 + color;
	  *ip++ = shift;
	  *ip++ = color;
	  sum[color] += 1 << shift;
	}
      for (c=0; c < colors; c++)
	if (c != FC(row,col)) {
	  *ip++ = c;
	  *ip++ = sum[c];
	}
    }
  }
  for (row=1; row < height-1; row++) {	/* Do bilinear interpolation */
    pix = image[row*width+1];
    for (col=1; col < width-1; col++) {
      if (col & 1)
	ip = code[row & 7];
      memset (sum, 0, sizeof sum);
      for (g=8; g--; ) {
	diff = pix[*ip++];
	diff <<= *ip++;
	sum[*ip++] += diff;
      }
      for (g=colors; --g; ) {
	c = *ip++;
	pix[c] = sum[c] / *ip++;
      }
      pix += 4;
    }
  }
  if (quick_interpolate)
    return;
  for (row=0; row < 8; row++) {		/* Precalculate for VNG */
    ip = code[row];
    for (col=0; col < 2; col++) {
      for (cp=terms, t=0; t < 64; t++) {
	y1 = *cp++;  x1 = *cp++;
	y2 = *cp++;  x2 = *cp++;
	weight = *cp++;
	grads = *cp++;
	color = FC(row+y1,col+x1);
	if (FC(row+y2,col+x2) != color) continue;
	diag = (FC(row,col+1) == color && FC(row+1,col) == color) ? 2:1;
	if (abs(y1-y2) == diag && abs(x1-x2) == diag) continue;
	*ip++ = (y1*width + x1)*4 + color;
	*ip++ = (y2*width + x2)*4 + color;
	*ip++ = weight;
	for (g=0; g < 8; g++)
	  if (grads & 1<<g) *ip++ = g;
	*ip++ = -1;
      }
      *ip++ = INT_MAX;
      for (cp=chood, g=0; g < 8; g++) {
	y = *cp++;  x = *cp++;
	*ip++ = (y*width + x) * 4;
	color = FC(row,col);
	if ((g & 1) == 0 &&
	    FC(row+y,col+x) != color && FC(row+y*2,col+x*2) == color)
	  *ip++ = (y*width + x) * 8 + color;
	else
	  *ip++ = 0;
      }
    }
  }
  brow[4] = calloc (width*3, sizeof **brow);
  merror (brow[4], "vng_interpolate()");
  for (row=0; row < 3; row++)
    brow[row] = brow[4] + row*width;
  for (row=2; row < height-2; row++) {		/* Do VNG interpolation */
    pix = image[row*width+2];
    for (col=2; col < width-2; col++) {
      if ((col & 1) == 0)
	ip = code[row & 7];
      memset (gval, 0, sizeof gval);
      while ((g = *ip++) != INT_MAX) {		/* Calculate gradients */
	diff = abs(pix[g] - pix[*ip++]);
	diff <<= *ip++;
	while ((g = *ip++) != -1)
	  gval[g] += diff;
      }
      gmin = INT_MAX;				/* Choose a threshold */
      gmax = 0;
      for (g=0; g < 8; g++) {
	if (gmin > gval[g]) gmin = gval[g];
	if (gmax < gval[g]) gmax = gval[g];
      }
      thold = gmin + (gmax >> 1);
      memset (sum, 0, sizeof sum);
      color = FC(row,col);
      for (num=g=0; g < 8; g++,ip+=2) {		/* Average the neighbors */
	if (gval[g] <= thold) {
	  for (c=0; c < colors; c++)
	    if (c == color && ip[1])
	      sum[c] += (pix[c] + pix[ip[1]]) >> 1;
	    else
	      sum[c] += pix[ip[0] + c];
	  num++;
	}
      }
      for (c=0; c < colors; c++) {		/* Save to buffer */
	t = pix[color] + (sum[c] - sum[color])/num;
	brow[2][col][c] = t > 0 ? (t < 0xffff ? t : 0xffff) : 0;
      }
      pix += 4;
    }
    if (row > 3)				/* Write buffer to image */
      memcpy (image[(row-2)*width+2], brow[0]+2, (width-4)*sizeof *image);
    for (g=0; g < 4; g++)
      brow[(g-1) & 3] = brow[g];
  }
  memcpy (image[(row-2)*width+2], brow[0]+2, (width-4)*sizeof *image);
  memcpy (image[(row-1)*width+2], brow[1]+2, (width-4)*sizeof *image);
  free(brow[4]);
}

/*
   Get a 2-byte integer, making no assumptions about CPU byte order.
   Nor should we assume that the compiler evaluates left-to-right.
 */
ushort fget2 (FILE *f)
{
  uchar a, b;

  a = fgetc(f);
  b = fgetc(f);
  if (order == 0x4949)		/* "II" means little-endian */
    return a + (b << 8);
  else				/* "MM" means big-endian */
    return (a << 8) + b;
}

/*
   Same for a 4-byte integer.
 */
int fget4 (FILE *f)
{
  uchar a, b, c, d;

  a = fgetc(f);
  b = fgetc(f);
  c = fgetc(f);
  d = fgetc(f);
  if (order == 0x4949)
    return a + (b << 8) + (c << 16) + (d << 24);
  else
    return (a << 24) + (b << 16) + (c << 8) + d;
}

void tiff_parse_subifd(int base)
{
  int entries, tag, type, len, val, save;

  entries = fget2(ifp);
  while (entries--) {
    tag  = fget2(ifp);
    type = fget2(ifp);
    len  = fget4(ifp);
    if (type == 3) {		/* short int */
      val = fget2(ifp);  fget2(ifp);
    } else
      val = fget4(ifp);
    switch (tag) {
      case 0x100:		/* ImageWidth */
	raw_width = val;
	break;
      case 0x101:		/* ImageHeight */
	raw_height = val;
	break;
      case 0x102:		/* Bits per sample */
	break;
      case 0x103:		/* Compression */
	tiff_data_compression = val;
	break;
      case 0x106:		/* Kodak color format */
	kodak_data_compression = val;
	break;
      case 0x111:		/* StripOffset */
	if (len == 1)
	  tiff_data_offset = val;
	else {
	  save = ftell(ifp);
	  fseek (ifp, val+base, SEEK_SET);
	  tiff_data_offset = fget4(ifp);
	  fseek (ifp, save, SEEK_SET);
	}
	break;
      case 0x115:		/* SamplesPerRow */
	break;
      case 0x116:		/* RowsPerStrip */
	break;
      case 0x117:		/* StripByteCounts */
	break;
      case 0x828d:		/* Unknown */
      case 0x828e:		/* Unknown */
      case 0x9217:		/* Unknown */
	break;
    }
  }
}

void nef_parse_makernote()
{
  int base=0, offset=0, entries, tag, type, len, val, save;
  short sorder;
  char buf[10];

/*
   The MakerNote might have its own TIFF header (possibly with
   its own byte-order!), or it might just be a table.
 */
  sorder = order;
  fread (buf, 1, 10, ifp);
  if (!strcmp (buf,"Nikon")) {	/* starts with "Nikon\0\2\0\0\0" ? */
    base = ftell(ifp);
    order = fget2(ifp);		/* might differ from file-wide byteorder */
    val = fget2(ifp);		/* should be 42 decimal */
    offset = fget4(ifp);
    fseek (ifp, offset-8, SEEK_CUR);
  } else
    fseek (ifp, -10, SEEK_CUR);

  entries = fget2(ifp);
  while (entries--) {
    tag  = fget2(ifp);
    type = fget2(ifp);
    len  = fget4(ifp);
    val  = fget4(ifp);
    if (tag == 0xc) {
      save = ftell(ifp);
      fseek (ifp, base + val, SEEK_SET);
      camera_red  = fget4(ifp);
      camera_red /= fget4(ifp);
      camera_blue = fget4(ifp);
      camera_blue/= fget4(ifp);
      fseek (ifp, save, SEEK_SET);
    }
    if (tag == 0x8c)
      nef_curve_offset = base + val + 2112;
    if (tag == 0x96)
      nef_curve_offset = base + val + 2;
  }
  order = sorder;
}

void nef_parse_exif()
{
  int entries, tag, type, len, val, save;

  entries = fget2(ifp);
  while (entries--) {
    tag  = fget2(ifp);
    type = fget2(ifp);
    len  = fget4(ifp);
    val  = fget4(ifp);
    save = ftell(ifp);
    if (tag == 0x927c && !strncmp(make,"NIKON",5)) {
      fseek (ifp, val, SEEK_SET);
      nef_parse_makernote();
      fseek (ifp, save, SEEK_SET);
    }
  }
}

/*
   Parse a TIFF file looking for camera model and decompress offsets.
 */
void parse_tiff(int base)
{
  int doff, entries, tag, type, len, val, save;
  char software[64];

  tiff_data_offset = 0;
  tiff_data_compression = 0;
  nef_curve_offset = 0;
  fseek (ifp, base, SEEK_SET);
  order = fget2(ifp);
  val = fget2(ifp);		/* Should be 42 for standard TIFF */
  while ((doff = fget4(ifp))) {
    fseek (ifp, doff+base, SEEK_SET);
    entries = fget2(ifp);
    while (entries--) {
      tag  = fget2(ifp);
      type = fget2(ifp);
      len  = fget4(ifp);
      val  = fget4(ifp);
      save = ftell(ifp);
      fseek (ifp, val+base, SEEK_SET);
      switch (tag) {
	case 271:			/* Make tag */
	  fgets (make, 64, ifp);
	  break;
	case 272:			/* Model tag */
	  fgets (model, 64, ifp);
	  break;
	case 33405:			/* Model2 tag */
	  fgets (model2, 64, ifp);
	  break;
	case 305:			/* Software tag */
	  fgets (software, 64, ifp);
	  if (!strncmp(software,"Adobe",5))
	    model[0] = 0;
	  break;
	case 330:			/* SubIFD tag */
	  if (len > 2) len=2;
	  if (len > 1)
	    while (len--) {
	      fseek (ifp, val+base, SEEK_SET);
	      fseek (ifp, fget4(ifp)+base, SEEK_SET);
	      tiff_parse_subifd(base);
	      val += 4;
	    }
	  else
	    tiff_parse_subifd(base);
	  break;
	case 0x8769:			/* Nikon EXIF tag */
	  nef_parse_exif();
	  break;
      }
      fseek (ifp, save, SEEK_SET);
    }
  }
}

/*
   Parse the CIFF structure looking for two pieces of information:
   The camera model, and the decode table number.
 */
void parse_ciff(int offset, int length)
{
  int tboff, nrecs, i, type, len, roff, aoff, save;
  int wbi=0;

  fseek (ifp, offset+length-4, SEEK_SET);
  tboff = fget4(ifp) + offset;
  fseek (ifp, tboff, SEEK_SET);
  nrecs = fget2(ifp);
  for (i = 0; i < nrecs; i++) {
    type = fget2(ifp);
    len  = fget4(ifp);
    roff = fget4(ifp);
    aoff = offset + roff;
    save = ftell(ifp);
    if (type == 0x080a) {		/* Get the camera make and model */
      fseek (ifp, aoff, SEEK_SET);
      fread (make, 64, 1, ifp);
      fseek (ifp, aoff+strlen(make)+1, SEEK_SET);
      fread (model, 64, 1, ifp);
    }
    if (type == 0x102a) {		/* Find the White Balance index */
      fseek (ifp, aoff+14, SEEK_SET);	/* 0=auto, 1=daylight, 2=cloudy ... */
      wbi = fget2(ifp);
    }
    if (type == 0x102c) {		/* Get white balance (G2) */
      fseek (ifp, aoff+100, SEEK_SET);	/* could use 100, 108 or 116 */
      camera_red = fget2(ifp);
      camera_red = fget2(ifp) / camera_red;
      camera_blue  = fget2(ifp);
      camera_blue /= fget2(ifp);
    }
    if (type == 0x0032 && !strcmp(model,"Canon EOS D30")) {
      fseek (ifp, aoff+72, SEEK_SET);	/* Get white balance (D30) */
      camera_red   = fget2(ifp);
      camera_red   = fget2(ifp) / camera_red;
      camera_blue  = fget2(ifp);
      camera_blue /= fget2(ifp);
      if (wbi==0)			/* AWB doesn't work here */
	camera_red = camera_blue = 0;
    }
    if (type == 0x10a9) {		/* Get white balance (D60) */
      fseek (ifp, aoff+2 + wbi*8, SEEK_SET);
      camera_red  = fget2(ifp);
      camera_red /= fget2(ifp);
      camera_blue = fget2(ifp);
      camera_blue = fget2(ifp) / camera_blue;
    }
    if (type == 0x1031) {		/* Get the raw width and height */
      fseek (ifp, aoff+2, SEEK_SET);
      raw_width  = fget2(ifp);
      raw_height = fget2(ifp);
    }
    if (type == 0x180e) {		/* Get the timestamp */
      fseek (ifp, aoff, SEEK_SET);
      timestamp = fget4(ifp);
    }
    if (type == 0x1835) {		/* Get the decoder table */
      fseek (ifp, aoff, SEEK_SET);
      init_tables (fget4(ifp));
    }
    if (type >> 8 == 0x28 || type >> 8 == 0x30)	/* Get sub-tables */
      parse_ciff(aoff, len);
    fseek (ifp, save, SEEK_SET);
  }
}

void parse_rollei()
{
  char line[128], *val;
  int tx=0, ty=0;

  do {
    fgets (line, 128, ifp);
    if ((val = strchr(line,'=')))
      *val++ = 0;
    else
      val = line + strlen(line);
    if (!strcmp(line,"HDR"))
      tiff_data_offset = atoi(val);
    if (!strcmp(line,"X  "))
      raw_width = atoi(val);
    if (!strcmp(line,"Y  "))
      raw_height = atoi(val);
    if (!strcmp(line,"TX "))
      tx = atoi(val);
    if (!strcmp(line,"TY "))
      ty = atoi(val);
  } while (strncmp(line,"EOHD",4));
  tiff_data_offset += tx * ty * 2;
  strcpy (make, "Rollei");
  strcpy (model, "d530flex");
}

void parse_foveon()
{
  char *buf, *bp, *np;
  int off1, off2, len, i;

  order = 0x4949;			/* Little-endian */
  fseek (ifp, -4, SEEK_END);
  off2 = fget4(ifp);
  fseek (ifp, off2, SEEK_SET);
  while (fget4(ifp) != 0x464d4143)	/* Search for "CAMF" */
    if (feof(ifp)) return;
  off1 = fget4(ifp);
  fseek (ifp, off1+8, SEEK_SET);
  off1 += (fget4(ifp)+3) * 8;
  len = (off2 - off1)/2;
  fseek (ifp, off1, SEEK_SET);
  buf = malloc (len);
  merror (buf, "parse_foveon()");
  for (i=0; i < len; i++)		/* Convert Unicode to ASCII */
    buf[i] = fget2(ifp);
  for (bp=buf; bp < buf+len; bp=np) {
    np = bp + strlen(bp) + 1;
    if (!strcmp(bp,"CAMMANUF"))
      strcpy (make, np);
    if (!strcmp(bp,"CAMMODEL"))
      strcpy (model, np);
  }
  fseek (ifp, 248, SEEK_SET);
  raw_width  = fget4(ifp);
  raw_height = fget4(ifp);
  free(buf);
}

void foveon_coeff()
{
  static const float foveon[3][3] = {
    {  2.0343955, -0.727533, -0.3067457 },
    { -0.2287194,  1.231793, -0.0028293 },
    { -0.0086152, -0.153336,  1.1617814 }   
  }, mul[3] = { 1.179, 1.0, 0.713 };
  int i, j;

  for (i=0; i < 3; i++)
    for (j=0; j < 3; j++)
      coeff[i][j] = foveon[i][j] * mul[i];
  use_coeff = 1;
}

/*
   The grass is always greener in my PowerShot G2 when this
   function is called.  Use at your own risk!
 */
void canon_rgb_coeff()
{
  static const float my_coeff[3][3] =
  { {  1.116187, -0.107427, -0.008760 },
    { -1.551374,  4.157144, -1.605770 },
    {  0.090939, -0.399727,  1.308788 } };
  int i, j;
  float juice = 0.1;	/* weaken the above matrix */

  for (i=0; i < 3; i++)
    for (j=0; j < 3; j++)
      coeff[i][j] = my_coeff[i][j] * juice + (i==j) * (1-juice);
  use_coeff = 1;
}

void nikon_e950_coeff()
{
  int r, g;
  static const float my_coeff[3][4] =
  { { -1.936280,  1.800443, -1.448486,  2.584324 },
    {  1.405365, -0.524955, -0.289090,  0.408680 },
    { -1.204965,  1.082304,  2.941367, -1.818705 } };

  for (r=0; r < 3; r++)
    for (g=0; g < 4; g++)
      coeff[r][g] = my_coeff[r][g];
  use_coeff = 1;
}

/*
   Given a matrix that converts RGB to GMCY, create a matrix to do
   the opposite.  Only square matrices can be inverted, so I create
   four 3x3 matrices by omitting a different GMCY color in each one.
   The final coeff[][] matrix is the sum of these four.
 */
void gmcy_coeff()
{
  static const float gmcy[4][3] = {
/*    red  green  blue			   */
    { 0.11, 0.86, 0.08 },	/* green   */
    { 0.50, 0.29, 0.51 },	/* magenta */
    { 0.11, 0.92, 0.75 },	/* cyan    */
    { 0.81, 0.98, 0.08 }	/* yellow  */
  };
  double invert[3][6], num;
  int ignore, i, j, k, r, g;

  memset (coeff, 0, sizeof coeff);
  for (ignore=0; ignore < 4; ignore++) {
    for (j=0; j < 3; j++) {
      g = (j < ignore) ? j : j+1;
      for (r=0; r < 3; r++) {
	invert[j][r] = gmcy[g][r];	/* 3x3 matrix to invert */
	invert[j][r+3] = (r == j);	/* Identity matrix	*/
      }
    }
    for (j=0; j < 3; j++) {
      num = invert[j][j];		/* Normalize this row	*/
      for (i=0; i < 6; i++)
	invert[j][i] /= num;
      for (k=0; k < 3; k++) {		/* Subtract it from the other rows */
	if (k==j) continue;
	num = invert[k][j];
	for (i=0; i < 6; i++)
	  invert[k][i] -= invert[j][i] * num;
      }
    }
    for (j=0; j < 3; j++) {		/* Add the result to coeff[][] */
      g = (j < ignore) ? j : j+1;
      for (r=0; r < 3; r++)
	coeff[r][g] += invert[r][j+3];
    }
  }
  for (r=0; r < 3; r++) {		/* Normalize such that:		*/
    for (num=g=0; g < 4; g++)		/* (1,1,1,1) x coeff = (1,1,1) */
      num += coeff[r][g];
    for (g=0; g < 4; g++)
      coeff[r][g] /= num;
  }
  use_coeff = 1;
}

/*
   Identify which camera created this file, and set global variables
   accordingly.  Return nonzero if the file cannot be decoded.
 */
int identify(char *fname)
{
  char head[26], *c;
  unsigned hlen, fsize, magic, i;

  pre_mul[0] = pre_mul[1] = pre_mul[2] = pre_mul[3] = 1;
  camera_red = camera_blue = black = timestamp = 0;
  rgb_max = 0x4000;
  colors = 3;
  is_cmy = is_foveon = use_coeff = 0;
  ymag = 1;

  strcpy (make, "NIKON");		/* wild guess */
  model[0] = model2[0] = 0;
  tiff_data_offset = 0;
  order = fget2(ifp);
  hlen = fget4(ifp);
  fread (head, 1, 26, ifp);
  fseek (ifp, 0, SEEK_END);
  fsize = ftell(ifp);
  fseek (ifp, 0, SEEK_SET);
  magic = fget4(ifp);
  if (order == 0x4949 || order == 0x4d4d) {
    if (!memcmp(head,"HEAPCCDR",8)) {
      parse_ciff (hlen, fsize - hlen);
      fseek (ifp, hlen, SEEK_SET);
    } else
      parse_tiff(0);
  } else if (magic == 0x4d524d) {	/* "\0MRM" (Minolta) */
    parse_tiff(48);
    fseek (ifp, 4, SEEK_SET);
    tiff_data_offset = fget4(ifp) + 8;
    fseek (ifp, 24, SEEK_SET);
    raw_height = fget2(ifp);
    raw_width  = fget2(ifp);
  } else if (magic >> 16 == 0x424d) {	/* "BM" */
    tiff_data_offset = 0x1000;
    order = 0x4949;
    fseek (ifp, 38, SEEK_SET);
    if (fget4(ifp) == 2834 && fget4(ifp) == 2834) {
      strcpy (model,"BMQ");
      goto nucore;
    }
  } else if (magic >> 16 == 0x4252) {	/* "BR" */
    strcpy (model,"RAW");
    nucore:
    strcpy (make,"Nucore");
    order = 0x4949;
    fseek (ifp, 10, SEEK_SET);
    tiff_data_offset += fget4(ifp);
    fget4(ifp);
    raw_width = fget4(ifp);
    raw_height = fget4(ifp);
    if (model[0] == 'B' && raw_width == 2597) {
      raw_width++;
      tiff_data_offset -= 0x1000;
    }
  } else if (!memcmp(head+19,"ARECOYK",7)) {
    strcpy (make, "CONTAX");
    strcpy (model, "N DIGITAL");
  } else if (magic == 0x46554a49) {	/* "FUJI" */
    fseek (ifp, 84, SEEK_SET);
    parse_tiff (fget4(ifp)+12);
    order = 0x4d4d;
    fseek (ifp, 100, SEEK_SET);
    tiff_data_offset = fget4(ifp);
  } else if (magic == 0x4453432d)	/* "DSC-" */
    parse_rollei();
  else if (magic == 0x464f5662)		/* "FOVb" */
    parse_foveon();
  else if (fsize == 2465792)		/* Nikon "DIAG RAW" formats */
    strcpy (model,"E950");
  else if (fsize == 2940928)
    strcpy (model,"E2500");
  else if (fsize == 4771840)
    strcpy (model,"E990/995");
  else if (fsize == 5865472)
    strcpy (model,"E4500");
  else if (fsize == 5869568)
    strcpy (model,"E4300");
  else {
    strcpy (make, "Casio");		/* Casio has similar formats */
    if (fsize == 1976352)
      strcpy (model, "QV-2000UX");
    else if (fsize == 3217760)
      strcpy (model, "QV-3*00EX");
    else if (fsize == 7684000)
      strcpy (model, "QV-4000");
    else if (fsize == 6218368)
      strcpy (model, "QV-5700");
  }

  /* Remove excess wordage */
  if (!strncmp(make,"NIKON",5) || !strncmp(make,"Canon",5))
    make[5] = 0;
  if (!strncmp(make,"PENTAX",6))
    make[6] = 0;
  if (!strncmp(make,"OLYMPUS",7) || !strncmp(make,"Minolta",7))
    make[7] = 0;
  if (!strncmp(make,"KODAK",5))
    make[16] = model[16] = 0;
  i = strlen(make);
  if (!strncmp(model,make,i++))
    memmove (model, model+i, 64-i);

  /* Remove trailing spaces */
  c = make + strlen(make);
  while (*--c == ' ') *c = 0;
  c = model + strlen(model);
  while (*--c == ' ') *c = 0;
  if (model[0] == 0) {
    fprintf (stderr, "%s: unsupported file format.\n", fname);
    return 1;
  }
  is_canon = !strcmp(make,"Canon");
  if (!strcmp(model,"PowerShot 600")) {
    height = 613;
    width  = 854;
    colors = 4;
    filters = 0xe1e4e1e4;
    load_raw = ps600_load_raw;
    pre_mul[0] = 1.137;
    pre_mul[1] = 1.257;
  } else if (!strcmp(model,"PowerShot A5")) {
    height = 776;
    width  = 960;
    colors = 4;
    filters = 0x1e4e1e4e;
    load_raw = a5_load_raw;
    pre_mul[0] = 1.5842;
    pre_mul[1] = 1.2966;
    pre_mul[2] = 1.0419;
  } else if (!strcmp(model,"PowerShot A50")) {
    height =  968;
    width  = 1290;
    colors = 4;
    filters = 0x1b4e4b1e;
    load_raw = a50_load_raw;
    pre_mul[0] = 1.750;
    pre_mul[1] = 1.381;
    pre_mul[3] = 1.182;
  } else if (!strcmp(model,"PowerShot Pro70")) {
    height = 1024;
    width  = 1552;
    colors = 4;
    filters = 0x1e4b4e1b;
    load_raw = pro70_load_raw;
    pre_mul[0] = 1.389;
    pre_mul[1] = 1.343;
    pre_mul[3] = 1.034;
  } else if (!strcmp(model,"PowerShot Pro90 IS")) {
    height = 1416;
    width  = 1896;
    colors = 4;
    filters = 0xb4b4b4b4;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 1.496;
    pre_mul[1] = 1.509;
    pre_mul[3] = 1.009;
  } else if (!strcmp(model,"PowerShot G1")) {
    height = 1550;
    width  = 2088;
    colors = 4;
    filters = 0xb4b4b4b4;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 1.446;
    pre_mul[1] = 1.405;
    pre_mul[2] = 1.016;
  } else if (!strcmp(model,"PowerShot S30")) {
    height = 1550;
    width  = 2088;
    filters = 0x94949494;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 1.785;
    pre_mul[2] = 1.266;
  } else if (!strcmp(model,"PowerShot G2")  ||
	     !strcmp(model,"PowerShot G3")  ||
	     !strcmp(model,"PowerShot S40") ||
	     !strcmp(model,"PowerShot S45")) {
    height = 1720;
    width  = 2312;
    filters = 0x94949494;
    load_raw = canon_compressed_load_raw;
    if (write_fun == write_ppm)		/* Pro users may not want my matrix */
      canon_rgb_coeff();
    pre_mul[0] = 1.965;
    pre_mul[2] = 1.208;
  } else if (!strcmp(model,"PowerShot G5")  ||
	     !strcmp(model,"PowerShot S50")) {
    height = 1960;
    width  = 2616;
    filters = 0x94949494;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 1.895;
    pre_mul[2] = 1.403;
  } else if (!strcmp(model,"EOS D30")) {
    height = 1448;
    width  = 2176;
    filters = 0x94949494;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 1.592;
    pre_mul[2] = 1.261;
  } else if (!strcmp(model,"EOS D60") ||
	     !strcmp(model,"EOS 10D") ||
	     !strcmp(model,"EOS 300D DIGITAL") ||
	     !strcmp(model,"EOS DIGITAL REBEL")) {
    height = 2056;
    width  = 3088;
    filters = 0x94949494;
    load_raw = canon_compressed_load_raw;
    pre_mul[0] = 2.242;
    pre_mul[2] = 1.245;
    rgb_max = 16000;
  } else if (!strcmp(model,"EOS-1D")) {
    height = 1662;
    width  = 2496;
    filters = 0x61616161;
    load_raw = lossless_jpeg_load_raw;
    tiff_data_offset = 288912;
    pre_mul[0] = 1.976;
    pre_mul[2] = 1.282;
  } else if (!strcmp(model,"EOS-1DS")) {
    height = 2718;
    width  = 4082;
    filters = 0x61616161;
    load_raw = lossless_jpeg_load_raw;
    tiff_data_offset = 289168;
    pre_mul[0] = 1.66;
    pre_mul[2] = 1.13;
    rgb_max = 14464;
  } else if (!strcmp(model,"EOS D2000C")) {
    height = raw_height;
    width  = raw_width;
    filters = 0x61616161;
    load_raw = lossless_jpeg_load_raw;
    black = 800;
    pre_mul[2] = 1.25;
  } else if (!strcmp(model,"D1")) {
    height = 1324;
    width  = 2012;
    filters = 0x16161616;
    load_raw = nikon_load_raw;
    pre_mul[0] = 0.838;
    pre_mul[2] = 1.095;
  } else if (!strcmp(model,"D1H")) {
    height = 1324;
    width  = 2012;
    filters = 0x16161616;
    load_raw = nikon_load_raw;
    pre_mul[0] = 1.347;
    pre_mul[2] = 3.279;
  } else if (!strcmp(model,"D1X")) {
    height = 1324;
    width  = 4024;
    filters = 0x16161616;
    ymag = 2;
    load_raw = nikon_load_raw;
    pre_mul[0] = 1.910;
    pre_mul[2] = 1.220;
  } else if (!strcmp(model,"D100")) {
    height = 2024;
    width  = 3037;
    filters = 0x61616161;
    load_raw = nikon_load_raw;
    pre_mul[0] = 2.374;
    pre_mul[2] = 1.677;
    rgb_max = 15632;
  } else if (!strcmp(model,"D2H")) {
    height = 1648;
    width  = 2482;
    filters = 0x49494949;
    load_raw = nikon_load_raw;
    pre_mul[0] = 2.8;
    pre_mul[2] = 1.2;
  } else if (!strcmp(model,"E950")) {
    height = 1203;
    width  = 1616;
    filters = 0x4b4b4b4b;
    colors = 4;
    load_raw = nikon_e950_load_raw;
    nikon_e950_coeff();
    pre_mul[0] = 1.18193;
    pre_mul[2] = 1.16452;
    pre_mul[3] = 1.17250;
  } else if (!strcmp(model,"E990/995")) {
    height = 1540;
    width  = 2064;
    filters = 0xb4b4b4b4;
    colors = 4;
    load_raw = nikon_load_raw;
    nikon_e950_coeff();
    pre_mul[0] = 1.196;
    pre_mul[1] = 1.246;
    pre_mul[2] = 1.018;
  } else if (!strcmp(model,"E2500")) {
    height = 1204;
    width  = 1616;
    filters = 0x4b4b4b4b;
    goto coolpix;
  } else if (!strcmp(model,"E4300")) {
    height = 1710;
    width  = 2288;
    filters = 0x16161616;
    load_raw = nikon_load_raw;
  } else if (!strcmp(model,"E4500")) {
    height = 1708;
    width  = 2288;
    filters = 0xb4b4b4b4;
    goto coolpix;
  } else if (!strcmp(model,"E5000") || !strcmp(model,"E5700")) {
    height = 1924;
    width  = 2576;
    filters = 0xb4b4b4b4;
    coolpix:
    colors = 4;
    load_raw = nikon_load_raw;
    pre_mul[0] = 1.300;
    pre_mul[1] = 1.300;
    pre_mul[3] = 1.148;
  } else if (!strcmp(model,"FinePixS2Pro")) {
    height = 3584;
    width  = 3583;
    filters = 0x61616161;
    load_raw = fuji_s2_load_raw;
    pre_mul[0] = 1.424;
    pre_mul[2] = 1.718;
  } else if (!strcmp(model,"FinePix S5000")) {
    height = 2499;
    width  = 2500;
    filters = 0x49494949;
    load_raw = fuji_s5000_load_raw;
    pre_mul[0] = 1.639;
    pre_mul[2] = 1.438;
  } else if (!strcmp(model,"FinePix F700")) {
    height = 2523;
    width  = 2524;
    filters = 0x49494949;
    load_raw = fuji_f700_load_raw;
    pre_mul[0] = 1.639;
    pre_mul[2] = 1.438;
    rgb_max = 0xffff;
  } else if (!strcmp(make,"Minolta")) {
    height = raw_height;
    width  = raw_width;
    filters = 0x94949494;
    load_raw = unpacked_12_load_raw;
    if (!strcmp(model,"DiMAGE A1"))
      load_raw = packed_12_load_raw;
    pre_mul[0] = 1.57;
    pre_mul[2] = 1.42;
  } else if (!strcmp(model,"*ist D")) {
    height = 2024;
    width  = 3040;
    filters = 0x94949494;
    tiff_data_offset = 0x10000;
    load_raw = unpacked_12_load_raw;
    pre_mul[0] = 1.76;
    pre_mul[1] = 1.07;
  } else if (!strcmp(model,"E-10")) {
    height = 1684;
    width  = 2256;
    filters = 0x94949494;
    tiff_data_offset = 0x4000;
    load_raw = olympus_load_raw;
    pre_mul[0] = 1.43;
    pre_mul[2] = 1.77;
  } else if (!strncmp(model,"E-20",4)) {
    height = 1924;
    width  = 2576;
    filters = 0x94949494;
    tiff_data_offset = 0x4000;
    load_raw = olympus_load_raw;
    pre_mul[0] = 1.43;
    pre_mul[2] = 1.77;
  } else if (!strcmp(model,"C5050Z")) {
    height = 1926;
    width  = 2576;
    filters = 0x16161616;
    load_raw = olympus2_load_raw;
    pre_mul[0] = 1.533;
    pre_mul[2] = 1.880;
  } else if (!strcmp(model,"N DIGITAL")) {
    height = 2047;
    width  = 3072;
    filters = 0x61616161;
    tiff_data_offset = 0x1a00;
    load_raw = kyocera_load_raw;
    pre_mul[0] = 1.366;
    pre_mul[2] = 1.251;
  } else if (!strcasecmp(make,"KODAK")) {
    height = raw_height;
    width  = raw_width;
    filters = 0x61616161;
    black = 400;
    if (!strcmp(model,"DCS315C")) {
      pre_mul[0] = 0.973;
      pre_mul[2] = 0.987;
      black = 0;
    } else if (!strcmp(model,"DCS330C")) {
      pre_mul[0] = 0.996;
      pre_mul[2] = 1.279;
      black = 0;
    } else if (!strcmp(model,"DCS420")) {
      pre_mul[0] = 1.21;
      pre_mul[2] = 1.63;
      width -= 4;
    } else if (!strcmp(model,"DCS460")) {
      pre_mul[0] = 1.46;
      pre_mul[2] = 1.84;
      width -= 4;
    } else if (!strcmp(model,"DCS460A")) {
      colors = 1;
      filters = 0;
      width -= 4;
    } else if (!strcmp(model,"EOSDCS3B")) {
      pre_mul[0] = 1.43;
      pre_mul[2] = 2.16;
      width -= 4;
    } else if (!strcmp(model,"EOSDCS1")) {
      pre_mul[0] = 1.28;
      pre_mul[2] = 2.00;
      width -= 4;
    } else if (!strcmp(model,"DCS520C")) {
      pre_mul[0] = 1.00;
      pre_mul[2] = 1.20;
    } else if (!strcmp(model,"DCS560C")) {
      pre_mul[0] = 0.985;
      pre_mul[2] = 1.15;
    } else if (!strcmp(model,"DCS620C")) {
      pre_mul[0] = 1.00;
      pre_mul[2] = 1.20;
    } else if (!strcmp(model,"DCS620X")) {
      pre_mul[0] = 1.12;
      pre_mul[2] = 1.07;
      is_cmy = 1;
    } else if (!strcmp(model,"DCS660C")) {
      pre_mul[0] = 1.05;
      pre_mul[2] = 1.17;
    } else if (!strcmp(model,"DCS660M")) {
      colors = 1;
      filters = 0;
    } else if (!strcmp(model,"DCS720X")) {
      pre_mul[0] = 1.35;
      pre_mul[2] = 1.18;
      is_cmy = 1;
    } else if (!strcmp(model,"DCS760C")) {
      pre_mul[0] = 1.06;
      pre_mul[2] = 1.72;
    } else if (!strcmp(model,"DCS760M")) {
      colors = 1;
      filters = 0;
    } else if (!strcmp(model,"ProBack")) {
      pre_mul[0] = 1.06;
      pre_mul[2] = 1.385;
    } else if (!strncmp(model2,"PB645C",6)) {
      pre_mul[0] = 1.0497;
      pre_mul[2] = 1.3306;
    } else if (!strncmp(model2,"PB645H",6)) {
      pre_mul[0] = 1.2010;
      pre_mul[2] = 1.5061;
    } else if (!strncmp(model2,"PB645M",6)) {
      pre_mul[0] = 1.01755;
      pre_mul[2] = 1.5424;
    } else if (!strcasecmp(model,"DCS Pro 14n")) {
      pre_mul[1] = 1.0191;
      pre_mul[2] = 1.1567;
    }
    switch (tiff_data_compression) {
      case 0:				/* No compression */
      case 1:
	rgb_max = 0x3fc0;
	load_raw = kodak_easy_load_raw;  break;
      case 7:				/* Lossless JPEG */
	load_raw = lossless_jpeg_load_raw;  break;
      case 65000:			/* Kodak DCR compression */
	black = 0;
	if (kodak_data_compression == 32803)
	  load_raw = kodak_compressed_load_raw;
	else {
	  load_raw = kodak_yuv_load_raw;
	  filters = 0;
	}
	break;
      default:
	fprintf (stderr, "%s: %s %s uses unsupported compression method %d.\n",
		fname, make, model, tiff_data_compression);
	return 1;
    }
  } else if (!strcmp(make,"Rollei")) {
    height = raw_height;
    width = raw_width;
    filters = 0x16161616;
    load_raw = rollei_load_raw;
    pre_mul[0] = 1.8;
    pre_mul[2] = 1.3;
  } else if (!strcmp(model,"SD9")) {
    switch (height = raw_height) {
      case  763: height =  756;  break;
      case 1531: height = 1514;  break;
    }
    switch (width = raw_width) {
      case 1152:  width = 1136;  break;
      case 2304:  width = 2271;  break;
    }
    if (height*2 < width) ymag = 2;
    filters = 0;
    load_raw = foveon_load_raw;
    is_foveon = 1;
    foveon_coeff();
    rgb_max = 5600;
  } else if (!strcmp(model,"QV-2000UX")) {
    height = 1208;
    raw_width = width = 1632;
    filters = 0x94949494;
    tiff_data_offset = width * 2;
    load_raw = casio_easy_load_raw;
  } else if (!strcmp(model,"QV-3*00EX")) {
    height = 1546;
    width  = 2070;
    raw_width = 2080;
    filters = 0x94949494;
    load_raw = casio_easy_load_raw;
  } else if (!strcmp(model,"QV-4000")) {
    height = 1700;
    width  = 2260;
    filters = 0x94949494;
    load_raw = olympus_load_raw;
  } else if (!strcmp(model,"QV-5700")) {
    height = 1924;
    width  = 2576;
    filters = 0x94949494;
    load_raw = casio_qv5700_load_raw;
  } else if (!strcmp(make,"Nucore")) {
    height = raw_height;
    width = raw_width;
    filters = 0x61616161;
    load_raw = nucore_load_raw;
  } else {
    fprintf (stderr, "%s: %s %s is not yet supported.\n",fname, make, model);
    return 1;
  }
#ifndef LJPEG_DECODE
  if (load_raw == lossless_jpeg_load_raw) {
    fprintf (stderr, "%s: %s %s requires lossless JPEG decoder.\n",
	fname, make, model);
    return 1;
  }
#endif
  if (use_camera_wb) {
    if (camera_red && camera_blue && colors == 3) {
      pre_mul[0] = camera_red;
      pre_mul[2] = camera_blue;
    } else
      fprintf (stderr, "%s: Cannot use camera white balance.\n",fname);
  }
  if (colors == 4 && !use_coeff)
    gmcy_coeff();
  if (use_coeff)		 /* Apply user-selected color balance */
    for (i=0; i < colors; i++) {
      coeff[0][i] *= red_scale;
      coeff[2][i] *= blue_scale;
    }
  else {
    pre_mul[0] *= red_scale;
    pre_mul[2] *= blue_scale;
  }
  if (four_color_rgb && filters && colors == 3) {
    for (i=0; i < 32; i+=4) {
      if ((filters >> i & 15) == 9)
	filters |= 2 << i;
      if ((filters >> i & 15) == 6)
	filters |= 8 << i;
    }
    colors++;
    if (use_coeff)
      for (i=0; i < 3; i++)
	coeff[i][3] = coeff[i][1] /= 2;
  }
  return 0;
}

/*
   Convert the entire image to RGB colorspace and build a histogram.
 */
void convert_to_rgb()
{
  int row, col, r, g, c=0;
  ushort *img;
  float rgb[4];

  if (document_mode)
    colors = 1;
  memset (histogram, 0, sizeof histogram);
  for (row = trim; row < height-trim; row++)
    for (col = trim; col < width-trim; col++) {
      img = image[row*width+col];
      if (document_mode)
	c = FC(row,col);
      if (colors == 4 && !use_coeff)	/* Recombine the greens */
	img[1] = (img[1] + img[3]) >> 1;
      if (colors == 1)			/* RGB from grayscale */
	for (r=0; r < 3; r++)
	  rgb[r] = img[c];
      else if (use_coeff) {		/* RGB from GMCY or Foveon */
	for (r=0; r < 3; r++)
	  for (rgb[r]=g=0; g < colors; g++)
	    rgb[r] += img[g] * coeff[r][g];
      } else if (is_cmy) {		/* RGB from CMY */
	rgb[0] = img[0] + img[1] - img[2];
	rgb[1] = img[1] + img[2] - img[0];
	rgb[2] = img[2] + img[0] - img[1];
      } else				/* RGB from RGB (easy) */
	for (r=0; r < 3; r++)
	  rgb[r] = img[r];
      for (rgb[3]=r=0; r < 3; r++) {	/* Compute the magnitude */
	if (rgb[r] < 0) rgb[r] = 0;
	if (rgb[r] > rgb_max) rgb[r] = rgb_max;
	rgb[3] += rgb[r]*rgb[r];
      }
      rgb[3] = sqrt(rgb[3])/2;
      if (rgb[3] > 0xffff) rgb[3] = 0xffff;
      for (r=0; r < 4; r++)
	img[r] = rgb[r];
      histogram[img[3] >> 3]++;		/* bin width is 8 */
    }
}

/*
   Write the image to a 24-bit PPM file.
 */
void write_ppm(FILE *ofp)
{
  int row, col, i, c, val, total;
  float max, mul, scale;
  ushort *rgb;
  uchar (*ppm)[3];

/*
   Set the white point to the 99th percentile
 */
  for (val=0x2000, total=0; --val; )
    if ((total+=histogram[val]) > (int)(width*height*0.01)) break;
  max = val << 4;

  fprintf (ofp, "P6\n%d %d\n255\n",
	width-trim*2, ymag*(height-trim*2));

  ppm = calloc (width-trim*2, 3);
  merror (ppm, "write_ppm()");
  mul = bright * 442 / max;

  for (row=trim; row < height-trim; row++) {
    for (col=trim; col < width-trim; col++) {
      rgb = image[row*width+col];
/* In some math libraries, pow(0,expo) doesn't return zero */
      scale = rgb[3] ? mul * pow (rgb[3]*2/max, gamma_val-1) : 0;
      for (c=0; c < 3; c++) {
	val = rgb[c] * scale;
	if (val > 255) val=255;
	ppm[col-trim][c] = val;
      }
    }
    for (i=0; i < ymag; i++)
      fwrite (ppm, width-trim*2, 3, ofp);
  }
  free(ppm);
}

/*
   Write the image to a 48-bit Photoshop file.
 */
void write_psd(FILE *ofp)
{
  char head[] = {
    '8','B','P','S',		/* signature */
    0,1,0,0,0,0,0,0,		/* version and reserved */
    0,3,			/* number of channels */
    0,0,0,0,			/* height, big-endian */
    0,0,0,0,			/* width, big-endian */
    0,16,			/* 16-bit color */
    0,3,			/* mode (1=grey, 3=rgb) */
    0,0,0,0,			/* color mode data */
    0,0,0,0,			/* image resources */
    0,0,0,0,			/* layer/mask info */
    0,0				/* no compression */
  };
  int hw[2], psize, row, col, c, val;
  ushort *buffer, *pred, *rgb;

  hw[0] = htonl(height-trim*2);	/* write the header */
  hw[1] = htonl(width-trim*2);
  memcpy (head+14, hw, sizeof hw);
  fwrite (head, 40, 1, ofp);

  psize = (height-trim*2) * (width-trim*2);
  buffer = calloc (6, psize);
  merror (buffer, "write_psd()");
  pred = buffer;

  for (row = trim; row < height-trim; row++) {
    for (col = trim; col < width-trim; col++) {
      rgb = image[row*width+col];
      for (c=0; c < 3; c++) {
	val = rgb[c] * bright;
	if (val > 0xffff) val=0xffff;
	pred[c*psize] = htons(val);
      }
      pred++;
    }
  }
  fwrite(buffer, psize, 6, ofp);
  free(buffer);
}

/*
   Write the image to a 48-bit PPM file.
 */
void write_ppm16(FILE *ofp)
{
  int row, col, c, val;
  ushort *rgb, (*ppm)[3];

  fprintf (ofp, "P6\n%d %d\n65535\n",
	width-trim*2, height-trim*2);

  ppm = calloc (width-trim*2, 6);
  merror (ppm, "write_ppm16()");

  for (row = trim; row < height-trim; row++) {
    for (col = trim; col < width-trim; col++) {
      rgb = image[row*width+col];
      for (c=0; c < 3; c++) {
	val = rgb[c] * bright;
	if (val > 0xffff) val=0xffff;
	ppm[col-trim][c] = htons(val);
      }
    }
    fwrite (ppm, width-trim*2, 6, ofp);
  }
  free(ppm);
}

int main(int argc, char **argv)
{
  char data[256], *cp;
  int arg, id, identify_only=0, write_to_files=1, minuso=0;
  const char *write_ext = ".ppm";
  FILE *ofp;

  if (argc == 1)
  {
    fprintf (stderr,
    "\nRaw Photo Decoder v5.04"
#ifdef LJPEG_DECODE
    " with Lossless JPEG support"
#endif
    "\nby Dave Coffin at cybercom dot net user dcoffin"
    "\n\nUsage:  %s [options] file1 file2 ...\n"
    "\nValid options:"
    "\n-i        Identify files but don't decode them"
    "\n-c        Write to standard output"
    "\n-o file   Write output to this file"
    "\n-f        Interpolate RGBG as four colors"
    "\n-d        Document Mode (no color, no interpolation)"
    "\n-q        Quick, low-quality color interpolation"
    "\n-g <num>  Set gamma      (0.8 by default, only for 24-bit output)"
    "\n-b <num>  Set brightness (1.0 by default)"
    "\n-w        Use camera white balance settings if possible"
    "\n-r <num>  Set red  multiplier (daylight = 1.0)"
    "\n-l <num>  Set blue multiplier (daylight = 1.0)"
    "\n-2        Write 24-bit PPM (default)"
    "\n-3        Write 48-bit PSD (Adobe Photoshop)"
    "\n-4        Write 48-bit PPM"
    "\n\n", argv[0]);
    exit(1);
  }

/* Parse out the options */

  for (arg=1; argv[arg][0] == '-'; arg++)
    switch (argv[arg][1])
    {
      case 'i':
	identify_only = 1;  break;
      case 'c':
	write_to_files = 0;  break;
      case 'o':
	minuso = ++arg;  break;
      case 'f':
	four_color_rgb = 1;  break;
      case 'd':
	document_mode = 1;  break;
      case 'q':
	quick_interpolate = 1;  break;
      case 'g':
	gamma_val = atof(argv[++arg]);  break;
      case 'b':
	bright = atof(argv[++arg]);  break;
      case 'w':
	use_camera_wb = 1;  break;
      case 'r':
	red_scale = atof(argv[++arg]);  break;
      case 'l':
	blue_scale = atof(argv[++arg]);  break;
      case '2':
	write_fun = write_ppm;
	write_ext = ".ppm";
	break;
      case '3':
	write_fun = write_psd;
	write_ext = ".psd";
	break;
      case '4':
	write_fun = write_ppm16;
	write_ext = ".ppm";
	break;
      default:
	fprintf (stderr, "Unknown option \"%s\"\n", argv[arg]);
	exit(1);
    }

/* Process the named files  */

  for ( ; arg < argc; arg++)
  {
    ifp = fopen(argv[arg],"rb");
    if (!ifp) perror(argv[arg]);
    if (identify_only) {
      if (!(id = !ifp || identify(argv[arg])))
	fprintf (stderr, "%s is a %s %s image.\n", argv[arg], make, model);
      if (ifp) fclose(ifp);
      if (arg+1 < argc) continue;
      exit(id);
    }
    if (!ifp) continue;
    if (identify(argv[arg])) {
      fclose(ifp);
      continue;
    }
    image = calloc (height * width, sizeof *image);
    merror (image, "main()");
    fprintf (stderr, "Loading %s %s image from %s...\n",
	make, model, argv[arg]);
    (*load_raw)();
    fclose(ifp);
    if (is_foveon) {
      fprintf (stderr, "Foveon interpolation...\n");
      foveon_interpolate();
    } else {
      bad_pixels();
      fprintf (stderr, "Scaling raw data (black=%d)...\n", black);
      if (document_mode)
	auto_scale();
      scale_colors();
    }
    trim = 0;
    if (filters && !document_mode) {
      trim = 1;
      fprintf (stderr, "%s interpolation...\n",
	quick_interpolate ? "Bilinear":"VNG");
      vng_interpolate();
    }
    fprintf (stderr, "Converting to RGB colorspace...\n");
    convert_to_rgb();
    ofp = stdout;
    strcpy (data, "standard output");
    if (write_to_files) {
      strcpy (data, argv[arg]);
      if ((cp = strrchr (data, '.'))) *cp = 0;
      strcat (data, write_ext);
      if (minuso)
	strcpy (data, argv[minuso]);
      ofp = fopen (data, "wb");
      if (!ofp) {
	perror(data);
	continue;
      }
    }
    fprintf (stderr, "Writing data to %s...\n", data);
    (*write_fun)(ofp);
    if (write_to_files)
      fclose(ofp);

    free(image);
  }
  return 0;
}
