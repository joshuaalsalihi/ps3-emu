#include "common/sha1.h"

#include "common/endian.h"

#include <bit>
#include <vector>

std::array<u8, 20> sha1(std::span<const u8> data) {
    // Pad to a multiple of 64 bytes: a 1 bit, zeros, then the message length in bits.
    std::vector<u8> message(data.begin(), data.end());
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    const u64 bit_length = u64{data.size()} * 8;
    message.resize(message.size() + 8);
    store_be<u64>(message.data() + message.size() - 8, bit_length);

    std::array<u32, 5> h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::array<u32, 80> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = load_be<u32>(message.data() + block + 4 * i);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const u32 temp = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<u8, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        store_be<u32>(digest.data() + 4 * i, h[i]);
    }
    return digest;
}
