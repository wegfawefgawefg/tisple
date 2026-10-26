#pragma once

// minimal png writer: rgb, deflate "stored" blocks, no dependencies

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// pixels are 0xRRGGBB, row by row
std::vector<uint8_t> EncodePng(const uint32_t* pixels, int width, int height);

// the screen at 560 x 384: each scanline twice, for the apple's pixel shape
bool WriteScreenPng(const std::string& path, const Framebuffer& fb, std::string& error);

// a 32-bit fnv-1a checksum of the framebuffer, for tests
uint32_t FramebufferChecksum(const Framebuffer& fb);

} // namespace tisple
