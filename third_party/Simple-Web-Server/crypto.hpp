#ifndef SIMPLE_WEB_CRYPTO_HPP
#define SIMPLE_WEB_CRYPTO_HPP

#include <cmath>
#include <iomanip>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

namespace SimpleWeb {
// TODO 2017: remove workaround for MSVS 2012
#if _MSC_VER == 1700                       // MSVS 2012 has no definition for round()
  inline double round(double x) noexcept { // Custom definition of round() for positive numbers
    return floor(x + 0.5);
  }
#endif

  class Crypto {
    const static std::size_t buffer_size = 131072;

  public:
    class Base64 {
    public:
      /// Returns Base64 encoded string from input string.
      static ::std::string encode(const ::std::string &bindata) {
        const char b64_table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char reverse_table[128] = {
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
            52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
            64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
            64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64};

        using ::std::numeric_limits;
        using ::std::string;

        if(bindata.size() > (numeric_limits<string::size_type>::max() / 4u) * 3u) {
          throw ::std::length_error("Converting too large a string to base64.");
        }

        const ::std::size_t binlen = bindata.size();
        // Use = signs so the end is properly padded.
        string retval((((binlen + 2) / 3) * 4), '=');
        ::std::size_t outpos = 0;
        int bits_collected = 0;
        unsigned int accumulator = 0;
        const string::const_iterator binend = bindata.end();

        for(string::const_iterator i = bindata.begin(); i != binend; ++i) {
          accumulator = (accumulator << 8) | (*i & 0xffu);
          bits_collected += 8;
          while(bits_collected >= 6) {
            bits_collected -= 6;
            retval[outpos++] = b64_table[(accumulator >> bits_collected) & 0x3fu];
          }
        }
        if(bits_collected > 0) { // Any trailing bits that are missing.
          assert(bits_collected < 6);
          accumulator <<= 6 - bits_collected;
          retval[outpos++] = b64_table[accumulator & 0x3fu];
        }
        assert(outpos >= (retval.size() - 2));
        assert(outpos <= retval.size());
        return retval;
      }

      /// Returns Base64 decoded string from base64 input.
      static ::std::string decode(const ::std::string &ascdata) {
        const char b64_table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char reverse_table[128] = {
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
            64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
            52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
            64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
            64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64};

        using ::std::string;
        string retval;
        const string::const_iterator last = ascdata.end();
        int bits_collected = 0;
        unsigned int accumulator = 0;

        for(string::const_iterator i = ascdata.begin(); i != last; ++i) {
          const int c = *i;
          if(::std::isspace(c) || c == '=') {
            // Skip whitespace and padding. Be liberal in what you accept.
            continue;
          }
          if((c > 127) || (c < 0) || (reverse_table[c] > 63)) {
            throw ::std::invalid_argument("This contains characters not legal in a base64 encoded string.");
          }
          accumulator = (accumulator << 6) | reverse_table[c];
          bits_collected += 6;
          if(bits_collected >= 8) {
            bits_collected -= 8;
            retval += static_cast<char>((accumulator >> bits_collected) & 0xffu);
          }
        }
        return retval;
      }
    };

    /// Returns hex string from bytes in input string.
    static std::string to_hex_string(const std::string &input) noexcept {
      std::stringstream hex_stream;
      hex_stream << std::hex << std::internal << std::setfill('0');
      for(auto &byte : input)
        hex_stream << std::setw(2) << static_cast<int>(static_cast<unsigned char>(byte));
      return hex_stream.str();
    }

    /// Returns md5 hash value from input string.
    static std::string md5(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(128 / 8);
      MD5(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns md5 hash value from input stream.
    static std::string md5(std::istream &stream, std::size_t iterations = 1) noexcept {
      MD5_CTX context;
      MD5_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        MD5_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(128 / 8);
      MD5_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha1 hash value from input string.
    static std::string sha1(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(160 / 8);
      SHA1(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha1 hash value from input stream.
    static std::string sha1(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA_CTX context;
      SHA1_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA1_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(160 / 8);
      SHA1_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha256 hash value from input string.
    static std::string sha256(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(256 / 8);
      SHA256(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha256 hash value from input stream.
    static std::string sha256(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA256_CTX context;
      SHA256_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(256 / 8);
      SHA256_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha512 hash value from input string.
    static std::string sha512(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(512 / 8);
      SHA512(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns sha512 hash value from input stream.
    static std::string sha512(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA512_CTX context;
      SHA512_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA512_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(512 / 8);
      SHA512_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// Returns PBKDF2 hash value from the given password
    /// Input parameter key_size  number of bytes of the returned key.

    /**
     * Returns PBKDF2 derived key from the given password.
     *
     * @param password   The password to derive key from.
     * @param salt       The salt to be used in the algorithm.
     * @param iterations Number of iterations to be used in the algorithm.
     * @param key_size   Number of bytes of the returned key.
     *
     * @return The PBKDF2 derived key.
     */
    static std::string pbkdf2(const std::string &password, const std::string &salt, int iterations, int key_size) noexcept {
      std::string key;
      key.resize(static_cast<std::size_t>(key_size));
      PKCS5_PBKDF2_HMAC_SHA1(password.c_str(), password.size(),
                             reinterpret_cast<const unsigned char *>(salt.c_str()), salt.size(), iterations,
                             key_size, reinterpret_cast<unsigned char *>(&key[0]));
      return key;
    }
  };
} // namespace SimpleWeb
#endif /* SIMPLE_WEB_CRYPTO_HPP */
