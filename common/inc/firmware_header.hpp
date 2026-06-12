#pragma once
#include <cstdint>

namespace Bootloader {

// A distinct 32-bit signature indicating a valid image is present
constexpr uint32_t MAGIC_NUMBER = 0x5A5A5A5A;

// The footprint of our tracking block
struct alignas(4) FirmwareHeader {
    uint32_t magic;         // 0x5A5A5A5A
    uint32_t payload_size;  // Calculated automatically by the linker
    uint32_t expected_crc;  // The checksum
    uint32_t version;       // Application version tag
};

enum class ValidationResult : uint8_t {
    Success = 0,
    InvalidMagic,
    InvalidResetHandler,
    InvalidSize,
    CrcMismatch
};

} // namespace Bootloader
