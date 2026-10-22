#include "ui/ImageDecode.h"

#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD        // Pentium III: sin SSE2
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace ImageDecode
{
unsigned char* decode(const unsigned char* data, int length, int* width, int* height)
{
    int channels = 0;
    return stbi_load_from_memory(data, length, width, height, &channels, 4);
}

void release(unsigned char* pixels)
{
    stbi_image_free(pixels);
}
}
