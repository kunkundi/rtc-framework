#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
	inline std::uint32_t RotL(std::uint32_t value, std::uint32_t bits) {
		return (value << bits) | (value >> (32U - bits));
	}
}

extern "C" unsigned char* SHA1(const unsigned char* d, std::size_t n, unsigned char* md) {
	if (md == nullptr) {
		return nullptr;
	}

	std::uint32_t h0 = 0x67452301U;
	std::uint32_t h1 = 0xEFCDAB89U;
	std::uint32_t h2 = 0x98BADCFEU;
	std::uint32_t h3 = 0x10325476U;
	std::uint32_t h4 = 0xC3D2E1F0U;

	const std::uint64_t bit_len = static_cast<std::uint64_t>(n) * 8U;
	const std::size_t padded_len = ((n + 9U + 63U) / 64U) * 64U;

	for (std::size_t offset = 0; offset < padded_len; offset += 64U) {
		std::uint32_t w[80];
		std::memset(w, 0, sizeof(w));

		for (std::size_t i = 0; i < 64U; ++i) {
			const std::size_t index = offset + i;
			std::uint8_t byte = 0;
			if (index < n) {
				byte = d[index];
			}
			else if (index == n) {
				byte = 0x80U;
			}
			else if (index >= padded_len - 8U) {
				const std::size_t shift = (padded_len - 1U - index) * 8U;
				byte = static_cast<std::uint8_t>((bit_len >> shift) & 0xFFU);
			}
			w[i / 4U] = (w[i / 4U] << 8U) | byte;
		}

		for (std::size_t i = 16U; i < 80U; ++i) {
			w[i] = RotL(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
		}

		std::uint32_t a = h0;
		std::uint32_t b = h1;
		std::uint32_t c = h2;
		std::uint32_t d0 = h3;
		std::uint32_t e = h4;

		for (std::size_t i = 0; i < 80U; ++i) {
			std::uint32_t f = 0;
			std::uint32_t k = 0;
			if (i < 20U) {
				f = (b & c) | ((~b) & d0);
				k = 0x5A827999U;
			}
			else if (i < 40U) {
				f = b ^ c ^ d0;
				k = 0x6ED9EBA1U;
			}
			else if (i < 60U) {
				f = (b & c) | (b & d0) | (c & d0);
				k = 0x8F1BBCDCU;
			}
			else {
				f = b ^ c ^ d0;
				k = 0xCA62C1D6U;
			}

			const std::uint32_t temp = RotL(a, 5U) + f + e + k + w[i];
			e = d0;
			d0 = c;
			c = RotL(b, 30U);
			b = a;
			a = temp;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d0;
		h4 += e;
	}

	const std::uint32_t digest[5] = { h0, h1, h2, h3, h4 };
	for (std::size_t i = 0; i < 5U; ++i) {
		md[i * 4U + 0U] = static_cast<unsigned char>((digest[i] >> 24U) & 0xFFU);
		md[i * 4U + 1U] = static_cast<unsigned char>((digest[i] >> 16U) & 0xFFU);
		md[i * 4U + 2U] = static_cast<unsigned char>((digest[i] >> 8U) & 0xFFU);
		md[i * 4U + 3U] = static_cast<unsigned char>(digest[i] & 0xFFU);
	}

	return md;
}
