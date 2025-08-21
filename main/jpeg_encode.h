#pragma once
#include <cstdint>
#include <string>

// Minimal JPEG encoder API for ESP32 (stub, replace with real implementation)
// Returns true on success, false on failure
bool jpeg_compress_to_file(const uint8_t* rgb888_buf, int width, int height, const char* filename, int quality);
