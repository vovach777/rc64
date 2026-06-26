/* =========================================================================
 * rANS64 — Asymmetric Numeral System codec (64-bit state, 32-bit renorm)
 * =========================================================================
 *
 * Original user code, logic unchanged.
 * 64-bit state, lower bound of the normalization interval RANS64_L = 2^31.
 * Renorm: emit 32-bit word, state >>= 32 (encoder) / shift-in word (decoder).
 *
 * Used with the static model in model.h (TARGET_TOTAL = 2^14, scale_bits=14).
 *
 * STREAM:
 *   Encoder walks the input BACKWARDS (rANS is LIFO), writes renorm words FORWARDS.
 *   flush() returns (HIGH32, LOW32) of the final state — written at the end of the stream.
 *   Decoder reads the flush-pair from the end, Init (with first/second swapped — see below),
 *   walks the stream FORWARDS, consumes renorm words from the end of the buffer BACKWARDS.
 * ========================================================================= */

#ifndef RANS_CODEC_H
#define RANS_CODEC_H

#include <cstdint>
#include <optional>
#include <utility>
#include <cassert>

namespace rANS
{
    constexpr uint64_t RANS64_L = (1ull << 31); // lower bound of our normalization interval
    using Rans64State = uint64_t;
    struct Encoder
    {
        uint64_t state{RANS64_L};

        // Renormalize the encoder. Internal function.
        auto RansEncRenorm(uint32_t freq, uint32_t scale_bits)
        {
            std::optional<uint32_t> res{};

            // renormalize (never needs to loop)
            uint64_t x_max = ((RANS64_L >> scale_bits) << (32U)) * freq; // this turns into a shift.
            if (state >= x_max)
            {
                res = state & 0xFFFFFFFFU;
                state >>= 32;
            }
            return res;
        }
        auto Rans64EncPut(uint32_t start, uint32_t freq, uint32_t scale_bits)
        {
            auto res = RansEncRenorm(freq, scale_bits);
            // x = C(s,x)
            state = ((state / freq) << scale_bits) + (state % freq) + start;
            return res;
        }
        auto RansEncPutBits(uint32_t val, uint32_t nbits)
        {
            assert(nbits <= 16);
            assert(val < (1u << nbits));
            // nbits <= 16!
            auto res = RansEncRenorm(1 << (16 - nbits), 16);
            // x = C(s,x)
            state = (state << nbits) | val;
            return res;
        }

        auto flush()
        {
            return std::make_pair( uint32_t( ((state >> 32) & 0xFFFFFFFFU)), uint32_t((state & 0xFFFFFFFFU)) );
        }
    };

    struct Decoder
    {
        uint64_t state;
    public:
        void Init(std::pair<uint32_t,uint32_t> next)
        {
            state = next.first;
            state |= static_cast<uint64_t>(next.second) << 32U;
        }

        bool Rans64DecRenorm(uint32_t next)
        {
            bool res = (state < RANS64_L);
            // renormalize
            if (res)
            {
                state <<= 32;
                state |= next;
            }
            return res;
        }

        uint32_t Rans64DecGet(uint32_t scale_bits)
        {
            return state & ((1 << scale_bits)-1);
        }
        //perforn Rans64DecRenorm(GetNext && get_next) after
        bool Rans64Dec(uint32_t start, uint32_t freq, uint32_t scale_bits, uint32_t next)
        {
            // s, x = D(x)
            state = freq * (state >> scale_bits) + (state & ((1 << scale_bits)-1)) - start;
            return Rans64DecRenorm(next);
        }

        //perforn Rans64DecRenorm(GetNext && get_next) after
        auto get_bits(uint32_t nbits, uint32_t next)
        {
            assert(nbits != 0);
            // Get value from low bits then shift them out and
            uint32_t val = Rans64DecGet(nbits);
            state >>= nbits;
            // renormalize
            return std::make_pair(val,  Rans64DecRenorm(next));
        }
    };

}

#endif /* RANS_CODEC_H */