/* PC-FX HuC6271/RAINBOW YUV/DCT encoder.
   This is a pragmatic encoder for the MJPEG-like RAINBOW stream decoded by
   Mednafen's PC-FX backend. It writes one 256x240 frame as 15 16-line YUV/DCT
   strips: first strip type 0xff with quant tables, following strips type 0xf8.

   Input is PNG or binary P6 PPM. PNG decoding is implemented locally and uses
   zlib inflate for concatenated IDAT chunks; no libpng/Pillow conversion step
   is required for the example image. Supported PNG input is non-interlaced,
   8-bit grayscale/RGB/RGBA/grayscale-alpha/indexed-color.

   The Huffman codewords below are reverse-derived from Mednafen's RAINBOW
   quick-LUT tables, themselves based on MagicEngine-FX documentation/data.

   Important hardware constraint: RAINBOW does not carry JPEG DHT-style custom
   Huffman tables.  The bitstream must use the fixed HuC6271 tables.  The
   encoder therefore improves compression by choosing quant tables and
   quantized coefficients against those fixed code lengths, not by changing the
   decoder/emulator.
*/

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { uint16_t code; uint8_t bits; } HuffCode;

static const HuffCode huff_dcy[32] = {
    { 0x0, 3 }, /* 0x00: 000 */
    { 0xe, 4 }, /* 0x01: 1110 */
    { 0x1, 3 }, /* 0x02: 001 */
    { 0x2, 3 }, /* 0x03: 010 */
    { 0x3, 3 }, /* 0x04: 011 */
    { 0x4, 3 }, /* 0x05: 100 */
    { 0x5, 3 }, /* 0x06: 101 */
    { 0x6, 3 }, /* 0x07: 110 */
    { 0x3c, 6 }, /* 0x08: 111100 */
    { 0x7a, 7 }, /* 0x09: 1111010 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x7b, 7 }, /* 0x0f: 1111011 */
    { 0x1f0, 9 }, /* 0x10: 111110000 */
    { 0x1f1, 9 }, /* 0x11: 111110001 */
    { 0x1f2, 9 }, /* 0x12: 111110010 */
    { 0x1f3, 9 }, /* 0x13: 111110011 */
    { 0x1f4, 9 }, /* 0x14: 111110100 */
    { 0x1f5, 9 }, /* 0x15: 111110101 */
    { 0x1f6, 9 }, /* 0x16: 111110110 */
    { 0x1f7, 9 }, /* 0x17: 111110111 */
    { 0x1f8, 9 }, /* 0x18: 111111000 */
    { 0x1f9, 9 }, /* 0x19: 111111001 */
    { 0x1fa, 9 }, /* 0x1a: 111111010 */
    { 0x1fb, 9 }, /* 0x1b: 111111011 */
    { 0x1fc, 9 }, /* 0x1c: 111111100 */
    { 0x1fd, 9 }, /* 0x1d: 111111101 */
    { 0x1fe, 9 }, /* 0x1e: 111111110 */
    { 0x1ff, 9 }, /* 0x1f: 111111111 */
};

static const HuffCode huff_dcuv[16] = {
    { 0x0, 2 }, /* 0x00: 00 */
    { 0x1, 2 }, /* 0x01: 01 */
    { 0x2, 2 }, /* 0x02: 10 */
    { 0x6, 3 }, /* 0x03: 110 */
    { 0xe, 4 }, /* 0x04: 1110 */
    { 0x1e, 5 }, /* 0x05: 11110 */
    { 0x3e, 6 }, /* 0x06: 111110 */
    { 0x7e, 7 }, /* 0x07: 1111110 */
    { 0xfe, 8 }, /* 0x08: 11111110 */
    { 0xff, 8 }, /* 0x09: 11111111 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
};

static const HuffCode huff_acy[256] = {
    { 0x1f, 5 }, /* 0x00: 11111 */
    { 0x0, 2 }, /* 0x01: 00 */
    { 0x1, 2 }, /* 0x02: 01 */
    { 0x4, 3 }, /* 0x03: 100 */
    { 0xa, 4 }, /* 0x04: 1010 */
    { 0x18, 5 }, /* 0x05: 11000 */
    { 0x36, 6 }, /* 0x06: 110110 */
    { 0x1dc, 9 }, /* 0x07: 111011100 */
    { 0xf10, 12 }, /* 0x08: 111100010000 */
    { 0x778, 11 }, /* 0x09: 11101111000 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf7f, 12 }, /* 0x10: 111101111111 */
    { 0xb, 4 }, /* 0x11: 1011 */
    { 0x19, 5 }, /* 0x12: 11001 */
    { 0x74, 7 }, /* 0x13: 1110100 */
    { 0xf11, 12 }, /* 0x14: 111100010001 */
    { 0xf12, 12 }, /* 0x15: 111100010010 */
    { 0xf13, 12 }, /* 0x16: 111100010011 */
    { 0xf14, 12 }, /* 0x17: 111100010100 */
    { 0xf15, 12 }, /* 0x18: 111100010101 */
    { 0x779, 11 }, /* 0x19: 11101111001 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x1a, 5 }, /* 0x21: 11010 */
    { 0x75, 7 }, /* 0x22: 1110101 */
    { 0xf16, 12 }, /* 0x23: 111100010110 */
    { 0xf17, 12 }, /* 0x24: 111100010111 */
    { 0xf18, 12 }, /* 0x25: 111100011000 */
    { 0xf19, 12 }, /* 0x26: 111100011001 */
    { 0xf1a, 12 }, /* 0x27: 111100011010 */
    { 0xf1b, 12 }, /* 0x28: 111100011011 */
    { 0x77a, 11 }, /* 0x29: 11101111010 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x37, 6 }, /* 0x31: 110111 */
    { 0xf1c, 12 }, /* 0x32: 111100011100 */
    { 0xf1d, 12 }, /* 0x33: 111100011101 */
    { 0xf1e, 12 }, /* 0x34: 111100011110 */
    { 0xf1f, 12 }, /* 0x35: 111100011111 */
    { 0xf20, 12 }, /* 0x36: 111100100000 */
    { 0xf21, 12 }, /* 0x37: 111100100001 */
    { 0xf22, 12 }, /* 0x38: 111100100010 */
    { 0x77b, 11 }, /* 0x39: 11101111011 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x38, 6 }, /* 0x41: 111000 */
    { 0xf23, 12 }, /* 0x42: 111100100011 */
    { 0xf24, 12 }, /* 0x43: 111100100100 */
    { 0xf25, 12 }, /* 0x44: 111100100101 */
    { 0xf26, 12 }, /* 0x45: 111100100110 */
    { 0xf27, 12 }, /* 0x46: 111100100111 */
    { 0xf28, 12 }, /* 0x47: 111100101000 */
    { 0xf29, 12 }, /* 0x48: 111100101001 */
    { 0x77c, 11 }, /* 0x49: 11101111100 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x39, 6 }, /* 0x51: 111001 */
    { 0xf2a, 12 }, /* 0x52: 111100101010 */
    { 0xf2b, 12 }, /* 0x53: 111100101011 */
    { 0xf2c, 12 }, /* 0x54: 111100101100 */
    { 0xf2d, 12 }, /* 0x55: 111100101101 */
    { 0xf2e, 12 }, /* 0x56: 111100101110 */
    { 0xf2f, 12 }, /* 0x57: 111100101111 */
    { 0xf30, 12 }, /* 0x58: 111100110000 */
    { 0x77d, 11 }, /* 0x59: 11101111101 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x76, 7 }, /* 0x61: 1110110 */
    { 0xf31, 12 }, /* 0x62: 111100110001 */
    { 0xf32, 12 }, /* 0x63: 111100110010 */
    { 0xf33, 12 }, /* 0x64: 111100110011 */
    { 0xf34, 12 }, /* 0x65: 111100110100 */
    { 0xf35, 12 }, /* 0x66: 111100110101 */
    { 0xf36, 12 }, /* 0x67: 111100110110 */
    { 0xf37, 12 }, /* 0x68: 111100110111 */
    { 0x77e, 11 }, /* 0x69: 11101111110 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x1dd, 9 }, /* 0x71: 111011101 */
    { 0xf38, 12 }, /* 0x72: 111100111000 */
    { 0xf39, 12 }, /* 0x73: 111100111001 */
    { 0xf3a, 12 }, /* 0x74: 111100111010 */
    { 0xf3b, 12 }, /* 0x75: 111100111011 */
    { 0xf3c, 12 }, /* 0x76: 111100111100 */
    { 0xf3d, 12 }, /* 0x77: 111100111101 */
    { 0xf3e, 12 }, /* 0x78: 111100111110 */
    { 0x77f, 11 }, /* 0x79: 11101111111 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf3f, 12 }, /* 0x81: 111100111111 */
    { 0xf40, 12 }, /* 0x82: 111101000000 */
    { 0xf41, 12 }, /* 0x83: 111101000001 */
    { 0xf42, 12 }, /* 0x84: 111101000010 */
    { 0xf43, 12 }, /* 0x85: 111101000011 */
    { 0xf44, 12 }, /* 0x86: 111101000100 */
    { 0xf45, 12 }, /* 0x87: 111101000101 */
    { 0xf46, 12 }, /* 0x88: 111101000110 */
    { 0x780, 11 }, /* 0x89: 11110000000 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf47, 12 }, /* 0x91: 111101000111 */
    { 0xf48, 12 }, /* 0x92: 111101001000 */
    { 0xf49, 12 }, /* 0x93: 111101001001 */
    { 0xf4a, 12 }, /* 0x94: 111101001010 */
    { 0xf4b, 12 }, /* 0x95: 111101001011 */
    { 0xf4c, 12 }, /* 0x96: 111101001100 */
    { 0xf4d, 12 }, /* 0x97: 111101001101 */
    { 0xf4e, 12 }, /* 0x98: 111101001110 */
    { 0x781, 11 }, /* 0x99: 11110000001 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf4f, 12 }, /* 0xa1: 111101001111 */
    { 0xf50, 12 }, /* 0xa2: 111101010000 */
    { 0xf51, 12 }, /* 0xa3: 111101010001 */
    { 0xf52, 12 }, /* 0xa4: 111101010010 */
    { 0xf53, 12 }, /* 0xa5: 111101010011 */
    { 0xf54, 12 }, /* 0xa6: 111101010100 */
    { 0xf55, 12 }, /* 0xa7: 111101010101 */
    { 0xf56, 12 }, /* 0xa8: 111101010110 */
    { 0x782, 11 }, /* 0xa9: 11110000010 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf57, 12 }, /* 0xb1: 111101010111 */
    { 0xf58, 12 }, /* 0xb2: 111101011000 */
    { 0xf59, 12 }, /* 0xb3: 111101011001 */
    { 0xf5a, 12 }, /* 0xb4: 111101011010 */
    { 0xf5b, 12 }, /* 0xb5: 111101011011 */
    { 0xf5c, 12 }, /* 0xb6: 111101011100 */
    { 0xf5d, 12 }, /* 0xb7: 111101011101 */
    { 0xf5e, 12 }, /* 0xb8: 111101011110 */
    { 0x783, 11 }, /* 0xb9: 11110000011 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf5f, 12 }, /* 0xc1: 111101011111 */
    { 0xf60, 12 }, /* 0xc2: 111101100000 */
    { 0xf61, 12 }, /* 0xc3: 111101100001 */
    { 0xf62, 12 }, /* 0xc4: 111101100010 */
    { 0xf63, 12 }, /* 0xc5: 111101100011 */
    { 0xf64, 12 }, /* 0xc6: 111101100100 */
    { 0xf65, 12 }, /* 0xc7: 111101100101 */
    { 0xf66, 12 }, /* 0xc8: 111101100110 */
    { 0x784, 11 }, /* 0xc9: 11110000100 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf67, 12 }, /* 0xd1: 111101100111 */
    { 0xf68, 12 }, /* 0xd2: 111101101000 */
    { 0xf69, 12 }, /* 0xd3: 111101101001 */
    { 0xf6a, 12 }, /* 0xd4: 111101101010 */
    { 0xf6b, 12 }, /* 0xd5: 111101101011 */
    { 0xf6c, 12 }, /* 0xd6: 111101101100 */
    { 0xf6d, 12 }, /* 0xd7: 111101101101 */
    { 0xf6e, 12 }, /* 0xd8: 111101101110 */
    { 0x785, 11 }, /* 0xd9: 11110000101 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf6f, 12 }, /* 0xe1: 111101101111 */
    { 0xf70, 12 }, /* 0xe2: 111101110000 */
    { 0xf71, 12 }, /* 0xe3: 111101110001 */
    { 0xf72, 12 }, /* 0xe4: 111101110010 */
    { 0xf73, 12 }, /* 0xe5: 111101110011 */
    { 0xf74, 12 }, /* 0xe6: 111101110100 */
    { 0xf75, 12 }, /* 0xe7: 111101110101 */
    { 0xf76, 12 }, /* 0xe8: 111101110110 */
    { 0x786, 11 }, /* 0xe9: 11110000110 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf77, 12 }, /* 0xf1: 111101110111 */
    { 0xf78, 12 }, /* 0xf2: 111101111000 */
    { 0xf79, 12 }, /* 0xf3: 111101111001 */
    { 0xf7a, 12 }, /* 0xf4: 111101111010 */
    { 0xf7b, 12 }, /* 0xf5: 111101111011 */
    { 0xf7c, 12 }, /* 0xf6: 111101111100 */
    { 0xf7d, 12 }, /* 0xf7: 111101111101 */
    { 0xf7e, 12 }, /* 0xf8: 111101111110 */
    { 0x787, 11 }, /* 0xf9: 11110000111 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
};

static const HuffCode huff_acuv[256] = {
    { 0x1f, 5 }, /* 0x00: 11111 */
    { 0x0, 2 }, /* 0x01: 00 */
    { 0x1, 2 }, /* 0x02: 01 */
    { 0xa, 4 }, /* 0x03: 1010 */
    { 0x18, 5 }, /* 0x04: 11000 */
    { 0x72, 7 }, /* 0x05: 1110010 */
    { 0xf10, 12 }, /* 0x06: 111100010000 */
    { 0xf11, 12 }, /* 0x07: 111100010001 */
    { 0xf12, 12 }, /* 0x08: 111100010010 */
    { 0x778, 11 }, /* 0x09: 11101111000 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf7f, 12 }, /* 0x10: 111101111111 */
    { 0x4, 3 }, /* 0x11: 100 */
    { 0x19, 5 }, /* 0x12: 11001 */
    { 0x73, 7 }, /* 0x13: 1110011 */
    { 0xf13, 12 }, /* 0x14: 111100010011 */
    { 0xf14, 12 }, /* 0x15: 111100010100 */
    { 0xf15, 12 }, /* 0x16: 111100010101 */
    { 0xf16, 12 }, /* 0x17: 111100010110 */
    { 0xf17, 12 }, /* 0x18: 111100010111 */
    { 0x779, 11 }, /* 0x19: 11101111001 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xb, 4 }, /* 0x21: 1011 */
    { 0x74, 7 }, /* 0x22: 1110100 */
    { 0xf18, 12 }, /* 0x23: 111100011000 */
    { 0xf19, 12 }, /* 0x24: 111100011001 */
    { 0xf1a, 12 }, /* 0x25: 111100011010 */
    { 0xf1b, 12 }, /* 0x26: 111100011011 */
    { 0xf1c, 12 }, /* 0x27: 111100011100 */
    { 0xf1d, 12 }, /* 0x28: 111100011101 */
    { 0x77a, 11 }, /* 0x29: 11101111010 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x1a, 5 }, /* 0x31: 11010 */
    { 0x1dc, 9 }, /* 0x32: 111011100 */
    { 0xf1e, 12 }, /* 0x33: 111100011110 */
    { 0xf1f, 12 }, /* 0x34: 111100011111 */
    { 0xf20, 12 }, /* 0x35: 111100100000 */
    { 0xf21, 12 }, /* 0x36: 111100100001 */
    { 0xf22, 12 }, /* 0x37: 111100100010 */
    { 0xf23, 12 }, /* 0x38: 111100100011 */
    { 0x77b, 11 }, /* 0x39: 11101111011 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x1b, 5 }, /* 0x41: 11011 */
    { 0xf24, 12 }, /* 0x42: 111100100100 */
    { 0xf25, 12 }, /* 0x43: 111100100101 */
    { 0xf26, 12 }, /* 0x44: 111100100110 */
    { 0xf27, 12 }, /* 0x45: 111100100111 */
    { 0xf28, 12 }, /* 0x46: 111100101000 */
    { 0xf29, 12 }, /* 0x47: 111100101001 */
    { 0xf2a, 12 }, /* 0x48: 111100101010 */
    { 0x77c, 11 }, /* 0x49: 11101111100 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x38, 6 }, /* 0x51: 111000 */
    { 0xf2b, 12 }, /* 0x52: 111100101011 */
    { 0xf2c, 12 }, /* 0x53: 111100101100 */
    { 0xf2d, 12 }, /* 0x54: 111100101101 */
    { 0xf2e, 12 }, /* 0x55: 111100101110 */
    { 0xf2f, 12 }, /* 0x56: 111100101111 */
    { 0xf30, 12 }, /* 0x57: 111100110000 */
    { 0xf31, 12 }, /* 0x58: 111100110001 */
    { 0x77d, 11 }, /* 0x59: 11101111101 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x75, 7 }, /* 0x61: 1110101 */
    { 0xf32, 12 }, /* 0x62: 111100110010 */
    { 0xf33, 12 }, /* 0x63: 111100110011 */
    { 0xf34, 12 }, /* 0x64: 111100110100 */
    { 0xf35, 12 }, /* 0x65: 111100110101 */
    { 0xf36, 12 }, /* 0x66: 111100110110 */
    { 0xf37, 12 }, /* 0x67: 111100110111 */
    { 0xf38, 12 }, /* 0x68: 111100111000 */
    { 0x77e, 11 }, /* 0x69: 11101111110 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x76, 7 }, /* 0x71: 1110110 */
    { 0xf39, 12 }, /* 0x72: 111100111001 */
    { 0xf3a, 12 }, /* 0x73: 111100111010 */
    { 0xf3b, 12 }, /* 0x74: 111100111011 */
    { 0xf3c, 12 }, /* 0x75: 111100111100 */
    { 0xf3d, 12 }, /* 0x76: 111100111101 */
    { 0xf3e, 12 }, /* 0x77: 111100111110 */
    { 0xf3f, 12 }, /* 0x78: 111100111111 */
    { 0x77f, 11 }, /* 0x79: 11101111111 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x1dd, 9 }, /* 0x81: 111011101 */
    { 0xf40, 12 }, /* 0x82: 111101000000 */
    { 0xf41, 12 }, /* 0x83: 111101000001 */
    { 0xf42, 12 }, /* 0x84: 111101000010 */
    { 0xf43, 12 }, /* 0x85: 111101000011 */
    { 0xf44, 12 }, /* 0x86: 111101000100 */
    { 0xf45, 12 }, /* 0x87: 111101000101 */
    { 0xf46, 12 }, /* 0x88: 111101000110 */
    { 0x780, 11 }, /* 0x89: 11110000000 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf47, 12 }, /* 0x91: 111101000111 */
    { 0xf48, 12 }, /* 0x92: 111101001000 */
    { 0xf49, 12 }, /* 0x93: 111101001001 */
    { 0xf4a, 12 }, /* 0x94: 111101001010 */
    { 0xf4b, 12 }, /* 0x95: 111101001011 */
    { 0xf4c, 12 }, /* 0x96: 111101001100 */
    { 0xf4d, 12 }, /* 0x97: 111101001101 */
    { 0xf4e, 12 }, /* 0x98: 111101001110 */
    { 0x781, 11 }, /* 0x99: 11110000001 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf4f, 12 }, /* 0xa1: 111101001111 */
    { 0xf50, 12 }, /* 0xa2: 111101010000 */
    { 0xf51, 12 }, /* 0xa3: 111101010001 */
    { 0xf52, 12 }, /* 0xa4: 111101010010 */
    { 0xf53, 12 }, /* 0xa5: 111101010011 */
    { 0xf54, 12 }, /* 0xa6: 111101010100 */
    { 0xf55, 12 }, /* 0xa7: 111101010101 */
    { 0xf56, 12 }, /* 0xa8: 111101010110 */
    { 0x782, 11 }, /* 0xa9: 11110000010 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf57, 12 }, /* 0xb1: 111101010111 */
    { 0xf58, 12 }, /* 0xb2: 111101011000 */
    { 0xf59, 12 }, /* 0xb3: 111101011001 */
    { 0xf5a, 12 }, /* 0xb4: 111101011010 */
    { 0xf5b, 12 }, /* 0xb5: 111101011011 */
    { 0xf5c, 12 }, /* 0xb6: 111101011100 */
    { 0xf5d, 12 }, /* 0xb7: 111101011101 */
    { 0xf5e, 12 }, /* 0xb8: 111101011110 */
    { 0x783, 11 }, /* 0xb9: 11110000011 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf5f, 12 }, /* 0xc1: 111101011111 */
    { 0xf60, 12 }, /* 0xc2: 111101100000 */
    { 0xf61, 12 }, /* 0xc3: 111101100001 */
    { 0xf62, 12 }, /* 0xc4: 111101100010 */
    { 0xf63, 12 }, /* 0xc5: 111101100011 */
    { 0xf64, 12 }, /* 0xc6: 111101100100 */
    { 0xf65, 12 }, /* 0xc7: 111101100101 */
    { 0xf66, 12 }, /* 0xc8: 111101100110 */
    { 0x784, 11 }, /* 0xc9: 11110000100 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf67, 12 }, /* 0xd1: 111101100111 */
    { 0xf68, 12 }, /* 0xd2: 111101101000 */
    { 0xf69, 12 }, /* 0xd3: 111101101001 */
    { 0xf6a, 12 }, /* 0xd4: 111101101010 */
    { 0xf6b, 12 }, /* 0xd5: 111101101011 */
    { 0xf6c, 12 }, /* 0xd6: 111101101100 */
    { 0xf6d, 12 }, /* 0xd7: 111101101101 */
    { 0xf6e, 12 }, /* 0xd8: 111101101110 */
    { 0x785, 11 }, /* 0xd9: 11110000101 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf6f, 12 }, /* 0xe1: 111101101111 */
    { 0xf70, 12 }, /* 0xe2: 111101110000 */
    { 0xf71, 12 }, /* 0xe3: 111101110001 */
    { 0xf72, 12 }, /* 0xe4: 111101110010 */
    { 0xf73, 12 }, /* 0xe5: 111101110011 */
    { 0xf74, 12 }, /* 0xe6: 111101110100 */
    { 0xf75, 12 }, /* 0xe7: 111101110101 */
    { 0xf76, 12 }, /* 0xe8: 111101110110 */
    { 0x786, 11 }, /* 0xe9: 11110000110 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0xf77, 12 }, /* 0xf1: 111101110111 */
    { 0xf78, 12 }, /* 0xf2: 111101111000 */
    { 0xf79, 12 }, /* 0xf3: 111101111001 */
    { 0xf7a, 12 }, /* 0xf4: 111101111010 */
    { 0xf7b, 12 }, /* 0xf5: 111101111011 */
    { 0xf7c, 12 }, /* 0xf6: 111101111100 */
    { 0xf7d, 12 }, /* 0xf7: 111101111101 */
    { 0xf7e, 12 }, /* 0xf8: 111101111110 */
    { 0x787, 11 }, /* 0xf9: 11110000111 */
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
    { 0x0, 0 },
};

static const uint8_t zigzag[63] = {
    0x01,0x08,0x10,0x09,0x02,0x03,0x0A,0x11,
    0x18,0x20,0x19,0x12,0x0B,0x04,0x05,0x0C,
    0x13,0x1A,0x21,0x28,0x30,0x29,0x22,0x1B,
    0x14,0x0D,0x06,0x07,0x0E,0x15,0x1C,0x23,
    0x2A,0x31,0x38,0x39,0x32,0x2B,0x24,0x1D,
    0x16,0x0F,0x17,0x1E,0x25,0x2C,0x33,0x3A,
    0x3B,0x34,0x2D,0x26,0x1F,0x27,0x2E,0x35,
    0x3C,0x3D,0x36,0x2F,0x37,0x3E,0x3F
};

/* Quant tables are generated from the canonical JPEG base tables.  The default
   quality=75 exactly matches the earlier hand-tuned tables closely enough to
   keep the previous visual/compression baseline, while allowing controlled
   quality/bit-rate experiments.  Values are in natural 8x8 order, not zig-zag. */
static const uint8_t jpeg_base_y[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};

static const uint8_t jpeg_base_uv[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

typedef struct {
    int quality;          /* 1..100, default 75. */
    double rdo_lambda;    /* 0 disables entropy-aware coefficient search. */
    int all_ff;           /* Diagnostic mode: emit quant tables on every strip. */
    int verbose;
} EncoderOptions;

static void make_quant_tables(int quality, uint8_t q_y[64], uint8_t q_uv[64]) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    if (quality == 100) scale = 1;
    for (int i = 0; i < 64; ++i) {
        int y = (jpeg_base_y[i] * scale + 50) / 100;
        int uv = (jpeg_base_uv[i] * scale + 50) / 100;
        if (y < 1) y = 1; else if (y > 255) y = 255;
        if (uv < 1) uv = 1; else if (uv > 255) uv = 255;
        q_y[i] = (uint8_t)y;
        q_uv[i] = (uint8_t)uv;
    }
}

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
} Vec;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void vec_reserve(Vec *v, size_t need) {
    if (need <= v->cap) return;
    size_t nc = v->cap ? v->cap * 2 : 4096;
    while (nc < need) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(v->p, nc);
    if (!np) die("out of memory");
    v->p = np;
    v->cap = nc;
}

static void vec_push(Vec *v, uint8_t b) {
    vec_reserve(v, v->len + 1);
    v->p[v->len++] = b;
}

static void vec_write(Vec *v, const void *data, size_t n) {
    vec_reserve(v, v->len + n);
    memcpy(v->p + v->len, data, n);
    v->len += n;
}

typedef struct {
    Vec *out;
    uint32_t acc;
    unsigned bits;
    size_t logical_bytes;
} BitWriter;

static void bw_emit_byte(BitWriter *bw, uint8_t b) {
    vec_push(bw->out, b);
    bw->logical_bytes++;
    if (b == 0xff) vec_push(bw->out, 0x00); /* decoder consumes stuffed byte without counting it */
}

static void bw_put(BitWriter *bw, uint32_t code, unsigned nbits) {
    if (!nbits) return;
    bw->acc = (bw->acc << nbits) | (code & ((1u << nbits) - 1u));
    bw->bits += nbits;
    while (bw->bits >= 8) {
        unsigned shift = bw->bits - 8;
        uint8_t b = (uint8_t)((bw->acc >> shift) & 0xffu);
        bw_emit_byte(bw, b);
        bw->bits -= 8;
        bw->acc &= (bw->bits ? ((1u << bw->bits) - 1u) : 0u);
    }
}

static void bw_flush(BitWriter *bw) {
    if (bw->bits) {
        uint8_t b = (uint8_t)((bw->acc << (8 - bw->bits)) & 0xffu);
        bw_emit_byte(bw, b);
        bw->acc = 0;
        bw->bits = 0;
    }
}

static void emit_huff(BitWriter *bw, const HuffCode *tab, unsigned val, const char *which) {
    HuffCode h = tab[val];
    if (!h.bits) {
        fprintf(stderr, "missing %s Huffman code for value 0x%02x\n", which, val);
        exit(1);
    }
    bw_put(bw, h.code, h.bits);
}

static unsigned coeff_category(int v) {
    unsigned a = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    unsigned n = 0;
    while (a) { n++; a >>= 1; }
    return n;
}

static unsigned coeff_payload(int v, unsigned n) {
    if (!n) return 0;
    if (v >= 0) return (unsigned)v;
    return (unsigned)(v + ((1 << n) - 1));
}

static int clamp_coeff(int v) {
    if (v > 511) return 511;
    if (v < -511) return -511;
    return v;
}

static void emit_dc_y(BitWriter *bw, int diff) {
    diff = clamp_coeff(diff);
    unsigned n = coeff_category(diff);
    if (n > 9) die("Y DC category exceeds 9 bits");
    emit_huff(bw, huff_dcy, n, "Y DC");
    bw_put(bw, coeff_payload(diff, n), n);
}

static void emit_dc_uv(BitWriter *bw, int diff) {
    diff = clamp_coeff(diff);
    unsigned n = coeff_category(diff);
    if (n > 9) die("UV DC category exceeds 9 bits");
    emit_huff(bw, huff_dcuv, n, "UV DC");
    bw_put(bw, coeff_payload(diff, n), n);
}

static void emit_ac(BitWriter *bw, const HuffCode *tab, const int q[64], const char *which) {
    unsigned run = 0;
    for (unsigned i = 0; i < 63; ++i) {
        int c = clamp_coeff(q[zigzag[i]]);
        if (!c) { run++; continue; }
        while (run > 15) {
            emit_huff(bw, tab, 0x10, which); /* RAINBOW decoder treats this as 15 zero coefficients. */
            run -= 15;
        }
        unsigned n = coeff_category(c);
        if (n > 9) die("AC category exceeds 9 bits");
        emit_huff(bw, tab, (run << 4) | n, which);
        bw_put(bw, coeff_payload(c, n), n);
        run = 0;
    }
    if (run) emit_huff(bw, tab, 0x00, which); /* EOB */
}


static unsigned huff_symbol_bits(const HuffCode *tab, unsigned val, const char *which) {
    HuffCode h = tab[val];
    if (!h.bits) {
        fprintf(stderr, "missing %s Huffman code for value 0x%02x\n", which, val);
        exit(1);
    }
    return h.bits;
}

static unsigned dc_bits_y(int diff) {
    diff = clamp_coeff(diff);
    unsigned n = coeff_category(diff);
    if (n > 9) die("Y DC category exceeds 9 bits");
    return huff_symbol_bits(huff_dcy, n, "Y DC") + n;
}

static unsigned dc_bits_uv(int diff) {
    diff = clamp_coeff(diff);
    unsigned n = coeff_category(diff);
    if (n > 9) die("UV DC category exceeds 9 bits");
    return huff_symbol_bits(huff_dcuv, n, "UV DC") + n;
}

static unsigned ac_bits_for_block(const HuffCode *tab, const int q[64], const char *which) {
    unsigned bits = 0;
    unsigned run = 0;
    for (unsigned i = 0; i < 63; ++i) {
        int c = clamp_coeff(q[zigzag[i]]);
        if (!c) { run++; continue; }
        while (run > 15) {
            bits += huff_symbol_bits(tab, 0x10, which);  /* 15-zero special. */
            run -= 15;
        }
        unsigned n = coeff_category(c);
        if (n > 9) die("AC category exceeds 9 bits");
        bits += huff_symbol_bits(tab, (run << 4) | n, which) + n;
        run = 0;
    }
    if (run) bits += huff_symbol_bits(tab, 0x00, which); /* EOB */
    return bits;
}

static double coeff_sse(const double raw[64], const uint8_t qtab[64], const int q[64]) {
    double s = 0.0;
    for (int i = 0; i < 64; ++i) {
        double e = raw[i] - (double)q[i] * (double)qtab[i];
        /* Slight low-frequency bias: these coefficients dominate visible banding/block edges. */
        double w = (i == 0) ? 1.60 : ((i < 16) ? 1.15 : 1.0);
        s += w * e * e;
    }
    return s;
}

static unsigned block_rate_bits(const int q[64], int prev_dc, int is_uv, const HuffCode *ac_tab) {
    return (is_uv ? dc_bits_uv(q[0] - prev_dc) : dc_bits_y(q[0] - prev_dc)) +
           ac_bits_for_block(ac_tab, q, is_uv ? "UV AC" : "Y AC");
}

static void add_candidate(int *cands, int *n, int v) {
    if (v > 511) v = 511;
    if (v < -511) v = -511;
    for (int i = 0; i < *n; ++i) if (cands[i] == v) return;
    cands[(*n)++] = v;
}

static void optimize_quantized_block(const double raw[64], const uint8_t qtab[64],
                                     int q[64], int prev_dc, int is_uv,
                                     const HuffCode *ac_tab, double lambda) {
    if (lambda <= 0.0) return;
    const int passes = 2;
    for (int pass = 0; pass < passes; ++pass) {
        for (int k = 0; k < 64; ++k) {
            int cur = q[k];
            int cands[16];
            int n = 0;
            add_candidate(cands, &n, cur);
            add_candidate(cands, &n, 0);
            add_candidate(cands, &n, cur + 1);
            add_candidate(cands, &n, cur - 1);
            if (cur > 0) {
                add_candidate(cands, &n, cur - 2);
                add_candidate(cands, &n, cur + 2);
            } else if (cur < 0) {
                add_candidate(cands, &n, cur + 2);
                add_candidate(cands, &n, cur - 2);
            }
            unsigned cat = coeff_category(cur);
            if (cat > 1) {
                int sign = (cur < 0) ? -1 : 1;
                add_candidate(cands, &n, sign * ((1 << (cat - 1)) - 1)); /* one smaller category */
                add_candidate(cands, &n, sign * (1 << (cat - 1)));
            }

            double best_score = 1e300;
            int best = cur;
            for (int ci = 0; ci < n; ++ci) {
                q[k] = cands[ci];
                double d = coeff_sse(raw, qtab, q);
                unsigned r = block_rate_bits(q, prev_dc, is_uv, ac_tab);
                double score = d + lambda * (double)r;
                if (score < best_score) {
                    best_score = score;
                    best = cands[ci];
                }
            }
            q[k] = best;
        }
    }
}

static int next_token(FILE *f, char *buf, size_t buflen) {
    int c;
    do {
        c = fgetc(f);
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); }
    } while (isspace(c));
    if (c == EOF) return 0;
    size_t n = 0;
    do {
        if (n + 1 < buflen) buf[n++] = (char)c;
        c = fgetc(f);
    } while (c != EOF && !isspace(c));
    buf[n] = 0;
    return 1;
}

static uint8_t *read_ppm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    char tok[128];
    if (!next_token(f, tok, sizeof(tok)) || strcmp(tok, "P6")) die("input is not binary P6 PPM");
    if (!next_token(f, tok, sizeof(tok))) die("bad PPM width");
    *w = atoi(tok);
    if (!next_token(f, tok, sizeof(tok))) die("bad PPM height");
    *h = atoi(tok);
    if (!next_token(f, tok, sizeof(tok))) die("bad PPM maxval");
    int maxv = atoi(tok);
    if (maxv != 255) die("PPM maxval must be 255");
    if (*w <= 0 || *h <= 0) die("bad PPM dimensions");
    size_t n = (size_t)(*w) * (size_t)(*h) * 3;
    uint8_t *rgb = (uint8_t *)xmalloc(n);
    if (fread(rgb, 1, n, f) != n) die("short PPM file");
    fclose(f);
    return rgb;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int png_channels(int color_type) {
    switch (color_type) {
        case 0: return 1; /* grayscale */
        case 2: return 3; /* RGB */
        case 3: return 1; /* indexed */
        case 4: return 2; /* grayscale + alpha */
        case 6: return 4; /* RGBA */
        default: return 0;
    }
}

static int paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint8_t *read_png(const char *path, int *w, int *h) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, sig, 8) != 0) die("input is not PNG");

    int saw_ihdr = 0, saw_iend = 0;
    int bit_depth = 0, color_type = 0, compression = 0, filter = 0, interlace = 0;
    uint8_t palette[256 * 3];
    size_t palette_entries = 0;
    Vec idat = {0};

    for (;;) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, f) != 8) die("truncated PNG chunk header");
        uint32_t len = be32(chdr);
        const char *type = (const char *)&chdr[4];
        uint8_t *data = NULL;
        if (len) {
            data = (uint8_t *)xmalloc(len);
            if (fread(data, 1, len, f) != len) die("truncated PNG chunk data");
        }
        uint8_t crcbuf[4];
        if (fread(crcbuf, 1, 4, f) != 4) die("truncated PNG chunk CRC");

        if (!memcmp(type, "IHDR", 4)) {
            if (len != 13) die("bad PNG IHDR length");
            *w = (int)be32(data + 0);
            *h = (int)be32(data + 4);
            bit_depth = data[8];
            color_type = data[9];
            compression = data[10];
            filter = data[11];
            interlace = data[12];
            saw_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            if ((len % 3) != 0 || len > sizeof(palette)) die("bad PNG PLTE length");
            memcpy(palette, data, len);
            palette_entries = len / 3;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!saw_ihdr) die("PNG IDAT before IHDR");
            if (len) vec_write(&idat, data, len);
        } else if (!memcmp(type, "IEND", 4)) {
            saw_iend = 1;
            free(data);
            break;
        }
        free(data);
    }
    fclose(f);

    if (!saw_ihdr || !saw_iend) die("bad PNG: missing IHDR/IEND");
    if (*w <= 0 || *h <= 0) die("bad PNG dimensions");
    if (compression != 0 || filter != 0) die("unsupported PNG compression/filter method");
    if (interlace != 0) die("Adam7 interlaced PNG is not supported");
    int channels = png_channels(color_type);
    if (!channels) die("unsupported PNG color type");
    if (bit_depth != 8) die("only 8-bit PNG input is supported");
    if (color_type == 3 && palette_entries == 0) die("indexed PNG lacks PLTE");

    if (idat.len == 0) die("PNG lacks IDAT data");

    size_t rowbytes = (size_t)(*w) * (size_t)channels;
    size_t inflated_len = (rowbytes + 1) * (size_t)(*h);
    uint8_t *inflated = (uint8_t *)xmalloc(inflated_len);

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = idat.p;
    zs.avail_in = (uInt)idat.len;
    zs.next_out = inflated;
    zs.avail_out = (uInt)inflated_len;
    if ((size_t)zs.avail_in != idat.len || (size_t)zs.avail_out != inflated_len)
        die("PNG too large for zlib uInt on this host");
    int zr = inflateInit(&zs);
    if (zr != Z_OK) die("zlib inflateInit failed");
    zr = inflate(&zs, Z_FINISH);
    if (zr != Z_STREAM_END) {
        inflateEnd(&zs);
        die("zlib inflate failed for PNG IDAT");
    }
    if ((size_t)zs.total_out != inflated_len) {
        inflateEnd(&zs);
        die("PNG IDAT inflated to unexpected size");
    }
    inflateEnd(&zs);
    free(idat.p);

    uint8_t *rgb = (uint8_t *)xmalloc((size_t)(*w) * (size_t)(*h) * 3);
    uint8_t *prev = (uint8_t *)calloc(1, rowbytes ? rowbytes : 1);
    uint8_t *cur = (uint8_t *)xmalloc(rowbytes ? rowbytes : 1);
    if (!prev) die("out of memory");

    for (int y = 0; y < *h; ++y) {
        const uint8_t *scan = inflated + (size_t)y * (rowbytes + 1);
        int ftype = scan[0];
        const uint8_t *raw = scan + 1;
        if (ftype < 0 || ftype > 4) die("unsupported PNG row filter");
        for (size_t x = 0; x < rowbytes; ++x) {
            int v = raw[x];
            switch (ftype) {
                case 0:
                    break;
                case 1: {
                    int left = (x >= (size_t)channels) ? cur[x - (size_t)channels] : 0;
                    v += left;
                    break;
                }
                case 2:
                    v += prev[x];
                    break;
                case 3: {
                    int left = (x >= (size_t)channels) ? cur[x - (size_t)channels] : 0;
                    int up = prev[x];
                    v += (left + up) >> 1;
                    break;
                }
                case 4: {
                    int left = (x >= (size_t)channels) ? cur[x - (size_t)channels] : 0;
                    int up = prev[x];
                    int upleft = (x >= (size_t)channels) ? prev[x - (size_t)channels] : 0;
                    v += paeth_predictor(left, up, upleft);
                    break;
                }
            }
            cur[x] = (uint8_t)v;
        }

        for (int x = 0; x < *w; ++x) {
            uint8_t r, g, b;
            const uint8_t *p = cur + (size_t)x * (size_t)channels;
            switch (color_type) {
                case 0:
                    r = g = b = p[0];
                    break;
                case 2:
                    r = p[0]; g = p[1]; b = p[2];
                    break;
                case 3:
                    if (p[0] >= palette_entries) die("PNG palette index out of range");
                    r = palette[(size_t)p[0] * 3 + 0];
                    g = palette[(size_t)p[0] * 3 + 1];
                    b = palette[(size_t)p[0] * 3 + 2];
                    break;
                case 4:
                    r = g = b = p[0];
                    break;
                case 6:
                    r = p[0]; g = p[1]; b = p[2];
                    break;
                default:
                    die("internal PNG color-type error");
            }
            uint8_t *q = rgb + ((size_t)y * (size_t)(*w) + (size_t)x) * 3;
            q[0] = r; q[1] = g; q[2] = b;
        }

        uint8_t *tmp = prev;
        prev = cur;
        cur = tmp;
    }

    free(cur);
    free(prev);
    free(inflated);
    return rgb;
}

static uint8_t *read_image(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint8_t magic[8] = {0};
    size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (got >= 8 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
        return read_png(path, w, h);
    if (got >= 2 && magic[0] == 'P' && magic[1] == '6')
        return read_ppm(path, w, h);
    die("input must be PNG or binary P6 PPM");
    return NULL;
}

static void write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (fwrite(data, 1, len, f) != len) die("write failed");
    fclose(f);
}

static void write_header(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "#ifndef RAINBOW_FRAME_H\n#define RAINBOW_FRAME_H\n\n");
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "#define RAINBOW_FRAME_LEN %zuu\n", len);
    fprintf(f, "#define RAINBOW_FRAME_WORD_ADDR 0x0800u\n");
    fprintf(f, "#define RAINBOW_TRANSFER_START 6u\n");
    fprintf(f, "#define RAINBOW_BLOCK_COUNT 15u\n\n");
    fprintf(f, "static const uint8_t rainbow_frame[RAINBOW_FRAME_LEN] = {\n");
    for (size_t i = 0; i < len; ++i) {
        if ((i % 12) == 0) fprintf(f, "    ");
        fprintf(f, "0x%02x", data[i]);
        if (i + 1 != len) fprintf(f, ",");
        if ((i % 12) == 11 || i + 1 == len) fprintf(f, "\n"); else fprintf(f, " ");
    }
    fprintf(f, "};\n\n#endif\n");
    fclose(f);
}

static int iround_to_int(double x) {
    return (int)((x >= 0.0) ? (x + 0.5) : (x - 0.5));
}

static void fdct_raw(const double src[64], double out[64]) {
    static int init = 0;
    static double ctab[8][8];
    static double sc[8];
    if (!init) {
        for (int u = 0; u < 8; ++u) {
            sc[u] = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
            for (int x = 0; x < 8; ++x)
                ctab[u][x] = cos(((2.0 * x + 1.0) * u * M_PI) / 16.0);
        }
        init = 1;
    }

    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            double sum = 0.0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    sum += (src[y * 8 + x] - 128.0) * ctab[u][x] * ctab[v][y];
            /* Standard JPEG forward DCT divided by four. Mednafen's RAINBOW IDCT maps
               a DC coefficient of 2*N back to a constant level N. */
            out[v * 8 + u] = (0.25 * sc[u] * sc[v] * sum) * 0.25;
        }
    }
}

static void quantize_from_raw(const double raw[64], const uint8_t qtab[64], int out[64]) {
    for (int i = 0; i < 64; ++i)
        out[i] = clamp_coeff(iround_to_int(raw[i] / (double)qtab[i]));
}

static void fdct_quant_rdo(const double src[64], const uint8_t qtab[64], int out[64],
                           int prev_dc, int is_uv, const HuffCode *ac_tab, double lambda) {
    double raw[64];
    fdct_raw(src, raw);
    quantize_from_raw(raw, qtab, out);
    optimize_quantized_block(raw, qtab, out, prev_dc, is_uv, ac_tab, lambda);
}

static void extract_y_block(const double *Y, int stride, int x0, int y0, double block[64]) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            block[y * 8 + x] = Y[(y0 + y) * stride + (x0 + x)];
}

static void extract_uv_420_block(const double *C, int stride, int x0, int y0, double block[64]) {
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int sx = x0 + x * 2;
            int sy = y0 + y * 2;
            block[y * 8 + x] = (C[sy * stride + sx] + C[sy * stride + sx + 1] +
                                C[(sy + 1) * stride + sx] + C[(sy + 1) * stride + sx + 1]) * 0.25;
        }
    }
}

static Vec encode_frame(const uint8_t *rgb, int w, int h, const EncoderOptions *opt) {
    if (w != 256 || h != 240) die("RAINBOW encoder currently requires exactly 256x240 input");
    uint8_t q_y[64], q_uv[64];
    make_quant_tables(opt ? opt->quality : 75, q_y, q_uv);

    double *Y = (double *)xmalloc((size_t)w * h * sizeof(double));
    double *U = (double *)xmalloc((size_t)w * h * sizeof(double));
    double *V = (double *)xmalloc((size_t)w * h * sizeof(double));

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = &rgb[(y * w + x) * 3];
            double r = p[0], g = p[1], b = p[2];
            double yy = 0.2990000000 * r + 0.5870000000 * g + 0.1140000000 * b;
            double uu = (b - yy) / 2.031999684343434 + 128.0;
            double vv = (r - yy) / 1.139827967171717 + 128.0;
            if (yy < 0) yy = 0; else if (yy > 255) yy = 255;
            if (uu < 0) uu = 0; else if (uu > 255) uu = 255;
            if (vv < 0) vv = 0; else if (vv > 255) vv = 255;
            Y[y * w + x] = yy;
            U[y * w + x] = uu;
            V[y * w + x] = vv;
        }
    }

    Vec out = {0};
    double blk[64];
    int q[64];

    for (int strip = 0; strip < 15; ++strip) {
        uint8_t block_type = (strip == 0 || (opt && opt->all_ff)) ? 0xff : 0xf8;
        vec_push(&out, 0xff);
        vec_push(&out, block_type);
        size_t size_pos = out.len;
        vec_push(&out, 0x00);
        vec_push(&out, 0x00);

        if (block_type == 0xff) {
            vec_write(&out, q_y, 64);
            vec_write(&out, q_uv, 64);
        }

        Vec payload = {0};
        BitWriter bw = { &payload, 0, 0, 0 };
        int dc_y = 0, dc_u = 0, dc_v = 0;
        int y0 = strip * 16;

        for (int col = 0; col < 16; ++col) {
            int x0 = col * 16;

            extract_y_block(Y, w, x0,     y0,     blk); fdct_quant_rdo(blk, q_y, q, dc_y, 0, huff_acy, opt ? opt->rdo_lambda : 0.0);
            emit_dc_y(&bw, q[0] - dc_y); dc_y = q[0]; emit_ac(&bw, huff_acy, q, "Y AC");

            extract_y_block(Y, w, x0,     y0 + 8, blk); fdct_quant_rdo(blk, q_y, q, dc_y, 0, huff_acy, opt ? opt->rdo_lambda : 0.0);
            emit_dc_y(&bw, q[0] - dc_y); dc_y = q[0]; emit_ac(&bw, huff_acy, q, "Y AC");

            extract_y_block(Y, w, x0 + 8, y0,     blk); fdct_quant_rdo(blk, q_y, q, dc_y, 0, huff_acy, opt ? opt->rdo_lambda : 0.0);
            emit_dc_y(&bw, q[0] - dc_y); dc_y = q[0]; emit_ac(&bw, huff_acy, q, "Y AC");

            extract_y_block(Y, w, x0 + 8, y0 + 8, blk); fdct_quant_rdo(blk, q_y, q, dc_y, 0, huff_acy, opt ? opt->rdo_lambda : 0.0);
            emit_dc_y(&bw, q[0] - dc_y); dc_y = q[0]; emit_ac(&bw, huff_acy, q, "Y AC");

            extract_uv_420_block(U, w, x0, y0, blk); fdct_quant_rdo(blk, q_uv, q, dc_u, 1, huff_acuv, opt ? opt->rdo_lambda : 0.0);
            emit_dc_uv(&bw, q[0] - dc_u); dc_u = q[0]; emit_ac(&bw, huff_acuv, q, "U AC");

            extract_uv_420_block(V, w, x0, y0, blk); fdct_quant_rdo(blk, q_uv, q, dc_v, 1, huff_acuv, opt ? opt->rdo_lambda : 0.0);
            emit_dc_uv(&bw, q[0] - dc_v); dc_v = q[0]; emit_ac(&bw, huff_acuv, q, "V AC");
        }
        bw_flush(&bw);

        size_t logical_payload = bw.logical_bytes;
        size_t size = 2 + logical_payload + ((block_type == 0xff) ? 128 : 0);
        if (size > 0x7fff) die("RAINBOW block too large for signed length field");
        out.p[size_pos + 0] = (uint8_t)(size >> 8);
        out.p[size_pos + 1] = (uint8_t)(size & 0xff);
        vec_write(&out, payload.p, payload.len);
        free(payload.p);
    }

    free(Y); free(U); free(V);
    return out;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] input_256x240.png|ppm output_stream.bin [output_header.h]\n"
        "options:\n"
        "  --quality N     Quantizer quality 1..100; default 75. Higher = better/larger.\n"
        "  --rdo L         Entropy-aware coefficient optimization lambda; default 0.\n"
        "                  Typical useful range: 0.05..8.0. Larger = smaller/rougher.\n"
        "  --all-ff        Diagnostic: emit 0xff + quant tables for every 16-line strip.\n"
        "  --verbose       Print selected encoder mode.\n",
        argv0);
}

int main(int argc, char **argv) {
    EncoderOptions opt;
    opt.quality = 75;
    opt.rdo_lambda = 0.0;
    opt.all_ff = 0;
    opt.verbose = 0;

    const char *pos[3] = {0, 0, 0};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--quality") || !strcmp(argv[i], "-q")) {
            if (++i >= argc) die("--quality requires a value");
            opt.quality = atoi(argv[i]);
            if (opt.quality < 1 || opt.quality > 100) die("--quality must be 1..100");
        } else if (!strcmp(argv[i], "--rdo")) {
            if (++i >= argc) die("--rdo requires a value");
            opt.rdo_lambda = atof(argv[i]);
            if (opt.rdo_lambda < 0.0) die("--rdo must be >= 0");
        } else if (!strcmp(argv[i], "--all-ff")) {
            opt.all_ff = 1;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            opt.verbose = 1;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            if (npos >= 3) {
                usage(argv[0]);
                return 2;
            }
            pos[npos++] = argv[i];
        }
    }
    if (npos < 2 || npos > 3) {
        usage(argv[0]);
        return 2;
    }

    int w = 0, h = 0;
    uint8_t *rgb = read_image(pos[0], &w, &h);
    Vec out = encode_frame(rgb, w, h, &opt);
    write_file(pos[1], out.p, out.len);
    if (npos == 3) write_header(pos[2], out.p, out.len);
    uint32_t crc = crc32_update(0, out.p, out.len);
    fprintf(stderr,
            "encoded %dx%d RAINBOW YUV/DCT: %zu bytes, crc32=%08x, mode=%s, quality=%d, rdo=%.4g\n",
            w, h, out.len, crc, opt.all_ff ? "all-ff" : "ff+f8", opt.quality, opt.rdo_lambda);
    free(rgb);
    free(out.p);
    return 0;
}
