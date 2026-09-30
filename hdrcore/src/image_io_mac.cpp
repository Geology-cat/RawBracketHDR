// PNG・JPEG の書き出し（macOS の ImageIO。C の API なので C++ から直接呼べる）。

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include "hdrcore/preview.hpp"

namespace hdr {

namespace {

CGImageRef make_image(int width, int height, int channels, const uint8_t* data) {
    CFDataRef bytes = CFDataCreate(nullptr, data, static_cast<CFIndex>(width) * height * channels);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
    CGColorSpaceRef space = channels == 3 ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : CGColorSpaceCreateDeviceGray();
    CGImageRef image = CGImageCreate(static_cast<size_t>(width), static_cast<size_t>(height), 8, 8 * channels,
                                     static_cast<size_t>(width) * channels, space, kCGImageAlphaNone | kCGBitmapByteOrderDefault,
                                     provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(bytes);
    return image;
}

bool write_image(const std::string& path, CGImageRef image, CFStringRef type) {
    if (!image) return false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    if (!url) return false;
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, type, 1, nullptr);
    CFRelease(url);
    if (!dest) return false;
    CGImageDestinationAddImage(dest, image, nullptr);
    const bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok;
}

}  // namespace

bool write_png(const std::string& path, const Rgb8Image& img) {
    if (img.width <= 0 || img.height <= 0) return false;
    CGImageRef image = make_image(img.width, img.height, 3, img.rgb.data());
    const bool ok = write_image(path, image, CFSTR("public.png"));
    if (image) CGImageRelease(image);
    return ok;
}

bool write_gray_png(const std::string& path, int width, int height, const std::vector<uint8_t>& gray) {
    if (width <= 0 || height <= 0 || gray.size() < static_cast<std::size_t>(width) * height) return false;
    CGImageRef image = make_image(width, height, 1, gray.data());
    const bool ok = write_image(path, image, CFSTR("public.png"));
    if (image) CGImageRelease(image);
    return ok;
}

bool encode_jpeg(const Rgb8Image& img, double quality, std::vector<uint8_t>& out) {
    out.clear();
    if (img.width <= 0 || img.height <= 0) return false;
    CGImageRef image = make_image(img.width, img.height, 3, img.rgb.data());
    if (!image) return false;
    CFMutableDataRef data = CFDataCreateMutable(nullptr, 0);
    CGImageDestinationRef dest = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, nullptr);
    bool ok = false;
    if (dest) {
        CFNumberRef q = CFNumberCreate(nullptr, kCFNumberDoubleType, &quality);
        const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void* values[] = {q};
        CFDictionaryRef props = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dest, image, props);
        ok = CGImageDestinationFinalize(dest);
        CFRelease(props);
        CFRelease(q);
        CFRelease(dest);
    }
    if (ok) {
        const UInt8* p = CFDataGetBytePtr(data);
        out.assign(p, p + CFDataGetLength(data));
    }
    CFRelease(data);
    CGImageRelease(image);
    return ok;
}

}  // namespace hdr
