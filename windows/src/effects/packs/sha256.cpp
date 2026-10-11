#include "sha256.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <exception>

namespace PrismForge::Effects::Packs {

namespace {

inline uint32_t RotR(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t Sigma0(uint32_t x) {
    return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22);
}

inline uint32_t Sigma1(uint32_t x) {
    return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25);
}

inline uint32_t Gamma0(uint32_t x) {
    return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3);
}

inline uint32_t Gamma1(uint32_t x) {
    return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10);
}

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

} // namespace

Sha256::Sha256() {
    Reset();
}

void Sha256::Reset() {
    m_state[0] = 0x6a09e667;
    m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372;
    m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f;
    m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab;
    m_state[7] = 0x5be0cd19;
    m_bitLength = 0;
    m_bufferLen = 0;
}

void Sha256::Transform(const uint8_t* chunk) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
               (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(chunk[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = Gamma1(w[i - 2]) + w[i - 7] + Gamma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = m_state[0];
    uint32_t b = m_state[1];
    uint32_t c = m_state[2];
    uint32_t d = m_state[3];
    uint32_t e = m_state[4];
    uint32_t f = m_state[5];
    uint32_t g = m_state[6];
    uint32_t h = m_state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = Sigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::Update(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        m_buffer[m_bufferLen++] = data[i];
        if (m_bufferLen == 64) {
            Transform(m_buffer);
            m_bitLength += 512;
            m_bufferLen = 0;
        }
    }
}

void Sha256::Update(const std::string& str) {
    Update(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

std::string Sha256::Final() {
    uint64_t totalBits = m_bitLength + (m_bufferLen * 8);

    // Padding byte 0x80
    m_buffer[m_bufferLen++] = 0x80;

    if (m_bufferLen > 56) {
        while (m_bufferLen < 64) {
            m_buffer[m_bufferLen++] = 0x00;
        }
        Transform(m_buffer);
        m_bufferLen = 0;
    }

    while (m_bufferLen < 56) {
        m_buffer[m_bufferLen++] = 0x00;
    }

    // Append 64-bit total bit count big-endian
    for (int i = 7; i >= 0; --i) {
        m_buffer[56 + (7 - i)] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFF);
    }

    Transform(m_buffer);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) {
        ss << std::setw(8) << m_state[i];
    }

    Reset();
    return ss.str();
}

std::string Sha256::ComputeHex(const uint8_t* data, size_t length) {
    Sha256 hasher;
    hasher.Update(data, length);
    return hasher.Final();
}

std::string Sha256::ComputeFileHexBounded(const std::string& filePath,
    uint64_t expectedBytes,uint64_t maxBytes,std::string& outError,uint64_t* outBytesRead) {
    outError.clear();if(outBytesRead)*outBytesRead=0;
    if(expectedBytes>maxBytes){outError="Declared hash input exceeds explicit byte limit";return {};}
    try {
        std::ifstream file(std::filesystem::u8path(filePath),std::ios::binary);
        if(!file){outError="Cannot open bounded hash input";return {};}
        Sha256 hasher;uint8_t buffer[8192];uint64_t consumed=0;
        while(consumed<expectedBytes){
            const auto request=static_cast<std::streamsize>(std::min<uint64_t>(sizeof(buffer),expectedBytes-consumed));
            file.read(reinterpret_cast<char*>(buffer),request);const auto count=file.gcount();
            if(count>0){consumed+=uint64_t(count);if(outBytesRead)*outBytesRead=consumed;}
            if(file.bad()||count!=request){outError="Bounded hash input truncated or unreadable";return {};}
            hasher.Update(buffer,static_cast<size_t>(count));
        }
        char extra=0;file.get(extra);
        if(file.gcount()!=0){if(outBytesRead)*outBytesRead=consumed+1;outError="Bounded hash input grew or has trailing bytes";return {};}
        if(file.bad()||!file.eof()){outError="Cannot verify bounded hash input end";return {};}
        return hasher.Final();
    }catch(const std::exception& e){outError=std::string("Bounded hash failed: ")+e.what();return {};}
}

std::string Sha256::ComputeFileHex(const std::string& filePath, uint64_t* outFileSize) {
    std::ifstream file(std::filesystem::u8path(filePath), std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    Sha256 hasher;
    uint8_t buf[8192];
    uint64_t totalBytes = 0;

    while (file.good()) {
        file.read(reinterpret_cast<char*>(buf), sizeof(buf));
        std::streamsize bytesRead = file.gcount();
        if (bytesRead > 0) {
            hasher.Update(buf, static_cast<size_t>(bytesRead));
            totalBytes += static_cast<uint64_t>(bytesRead);
        }
    }

    if (outFileSize) {
        *outFileSize = totalBytes;
    }

    return hasher.Final();
}

} // namespace PrismForge::Effects::Packs
