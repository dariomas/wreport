#ifndef WREPORT_BUFFERS_BUFR_H
#define WREPORT_BUFFERS_BUFR_H

#include <wreport/error.h>
#include <wreport/var.h>
#include <string>
#include <functional>
#include <cstdint>

namespace wreport {
struct Var;

namespace buffers {

/**
 * Binary buffer with bit-level read operations
 */
class BufrInput
{
protected:
    /**
     * Scan length of section \a sec_no, filling in the start of the next
     * section in sec[sec_no + 1]
     */
    void scan_section_length(unsigned sec_no);

public:
    /// Input buffer
    const uint8_t* data;

    /// Input buffer size
    size_t data_len;

    /**
     * Input file name (optional).
     *
     * If available, it will be used to generate better error messages.
     *
     * If not available, it is NULL.
     */
    const char* fname = nullptr;

    /**
     * File offset of the start of the message.
     *
     * If available, it will be used to generate better error messages.
     *
     * If not available, it is 0.
     */
    size_t start_offset = 0;

    /// Offset of the byte we are currently decoding
    unsigned s4_cursor = 0;

    /// Byte we are currently decoding
    uint8_t pbyte = 0;

    /// Bits left in pbyte to decode
    int pbyte_len = 0;

    /// Offsets of the start of BUFR sections
    unsigned sec[6];


    /**
     * Wrap a string iinto a BufrInput
     *
     * @param in
     *   String with the data to read
     */
    BufrInput(const std::string& in);

    /**
     * Scan the message filling in the sec[] array of start offsets of sections
     * 0 and 1.
     *
     * We cannot scan past section 1 until we decode it, because we need to
     * know if section 2 is present or not
     */
    void scan_lead_sections();

    /**
     * Scan the message filling in the sec[] array of section start offsets of
     * all sections from 2 on.
     *
     * It also initialises points s4_cursor to the begin of the data in section
     * 4.
     *
     * @param has_optional
     *   True if the optional section is present, false if it should be
     *   skipped.
     */
    void scan_other_sections(bool has_optional);

    /// Return the current decoding byte offset
    unsigned offset() const { return s4_cursor; }

    /// Return the number of bits left in the message to be decoded
    unsigned bits_left() const { return (data_len - s4_cursor) * 8 + pbyte_len; }

    /// Read a byte value at offset \a pos
    inline unsigned read_byte(unsigned pos) const
    {
        return (unsigned)data[pos];
    }

    /// Read a byte value at offset \a pos inside section \a section
    inline unsigned read_byte(unsigned section, unsigned pos) const
    {
        return (unsigned)data[sec[section] + pos];
    }

    /// Read a big endian integer value \a byte_len bytes long, at offset \a pos
    unsigned read_number(unsigned pos, unsigned byte_len) const
    {
        unsigned res = 0;
        for (unsigned i = 0; i < byte_len; ++i)
        {
            res <<= 8;
            res |= data[pos + i];
        }
        return res;
    }

    /**
     * Read a big endian integer value \a byte_len bytes long, at offset \a pos
     * inside section \a section
     */
    inline unsigned read_number(unsigned section, unsigned pos, unsigned byte_len) const
    {
        return read_number(sec[section] + pos, byte_len);
    }

    /**
     * Get the integer value of the next 'n' bits from the decode input
     * n must be <= 32.
     */
    uint32_t get_bits(unsigned n)
    {
        uint32_t result = 0;

        if (s4_cursor == data_len)
            parse_error("end of buffer while looking for %d bits of bit-packed data", n);

        // TODO: review and benchmark and possibly simplify
        // (a possible alternative approach is to keep a current bitmask that
        // starts at 0x80 and is shifted right by 1 at each read until it
        // reaches 0, and get rid of pbyte_len)
        for (unsigned i = 0; i < n; i++)
        {
            if (pbyte_len == 0)
            {
                pbyte_len = 8;
                pbyte = data[s4_cursor++];
            }
            result <<= 1;
            if (pbyte & 0x80)
                result |= 1;
            pbyte <<= 1;
            pbyte_len--;
        }

        return result;
    }

    /// Dump to stderr 'count' bits of 'buf', starting at the 'ofs-th' bit
    void debug_dump_next_bits(const char* desc, int count) const;

    /// Throw an error_parse at the current decoding location
    void parse_error(const char* fmt, ...) const WREPORT_THROWF_ATTRS(2, 3);

    /// Throw an error_parse at the given decoding location
    void parse_error(unsigned pos, const char* fmt, ...) const WREPORT_THROWF_ATTRS(3, 4);

    /// Throw an error_parse at the given decoding location inside the given section
    void parse_error(unsigned section, unsigned pos, const char* fmt, ...) const WREPORT_THROWF_ATTRS(4, 5);

    /**
     * Check that the input buffer contains at least \a datalen characters
     * after offset \a pos; throw error_parse otherwise.
     *
     * @param pos
     *   Starting offset of the required data
     * @param datalen
     *   Required amount of data expected starting from \a pos
     * @param expected
     *    name of what we are about to decode, used for generating nice error
     *    messages
     */
    void check_available_data(unsigned pos, size_t datalen, const char* expected);

    /**
     * Check that the input buffer contains at least \a datalen characters
     * after offset \a pos in section \a section; throw error_parse otherwise.
     *
     * @param section
     *   Number of the section to check
     * @param pos
     *   Starting offset inside the section of the required data
     * @param datalen
     *   Required amount of data expected starting from \a pos
     * @param expected
     *   Name of what we are about to decode, used for generating nice error
     *   messages
     */
    void check_available_data(unsigned section, unsigned pos, size_t datalen, const char* expected);

    /**
     * Decode a compressed number as described by dest.info(), ad set it as
     * value for \a dest.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     * @param base
     *   The base value for the compressed number
     * @param diffbits
     *   The number of bits used to encode the difference from \a base
     */
    void decode_compressed_number(Var& dest, uint32_t base, unsigned diffbits);

    /**
     * Decode a number as described by dest.info(), and set it as value for \a
     * dest.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     */
    void decode_number(Var& dest);

    bool decode_compressed_base(Varinfo info, uint32_t& base, uint32_t& diffbits);

    /**
     * Decode a number as described by \a info from a compressed bufr with
     * \a subsets subsets, and send the resulting variables to \a dest
     */
    void decode_compressed_number(Varinfo info, unsigned subsets, std::function<void(unsigned, Var&&)> dest)
    {
        // Data field base value
        uint32_t base;

        // Number of bits used for each difference value
        uint32_t diffbits;

        bool missing = decode_compressed_base(info, base, diffbits);
        if (missing)
        {
            for (unsigned i = 0; i < subsets; ++i)
                dest(i, Var(info));
        }
        else if (!diffbits)
        {
            Var var(info, info->decode_binary(base));
            for (unsigned i = 0; i < subsets; ++i)
                dest(i, Var(var));
        }
        else
        {
            Var var(info);
            for (unsigned i = 0; i < subsets; ++i)
            {
                decode_compressed_number(var, base, diffbits);
                dest(i, std::move(var));
            }
        }
    }

    template<typename Adder>
    void decode_string(Varinfo info, unsigned subsets, Adder& dest)
    {
        // Decode the base value
        char str[info->bit_len / 8 + 2];
        size_t len;
        bool missing = !decode_string(info->bit_len, str, len);

        // Decode the number of bits (encoded in 6 bits) for each difference
        // value
        uint32_t diffbits = get_bits(6);

        if (missing && diffbits == 0)
            dest.add_missing(info);
        else if (diffbits == 0)
        {
            // Add the same string to all the subsets
            dest.add_same(Var(info, str));
        } else {
            /* For compressed strings, the reference value must be all zeros */
            for (size_t i = 0; i < len; ++i)
                if (str[i] != 0)
                    error_unimplemented::throwf("compressed strings with %d bit deltas have non-zero reference value", diffbits);

            /* Let's also check that the number of
             * difference characters is the same length as
             * the reference string */
            if (diffbits > len)
                error_unimplemented::throwf("compressed strings with %zd characters have %d bit deltas (deltas should not be longer than field)", len, diffbits);

            for (unsigned i = 0; i < subsets; ++i)
            {
                // Set the variable value
                if (decode_string(diffbits * 8, str, len))
                {
                    // Compute the value for this subset
                    dest.add_var(i, Var(info, str));
                } else {
                    // Missing value
                    dest.add_var(i, Var(info));
                }
            }
        }
    }

    template<typename Adder>
    void decode_compressed_number(Varinfo info, unsigned subsets, Adder& dest)
    {
        // Data field base value
        uint32_t base;

        // Number of bits used for each difference value
        uint32_t diffbits;

        bool missing = decode_compressed_base(info, base, diffbits);
        if (missing)
            dest.add_missing(info);
        else if (!diffbits)
            dest.add_same(Var(info, info->decode_binary(base)));
        else
        {
            Var var(info);
            for (unsigned i = 0; i < subsets; ++i)
            {
                decode_compressed_number(var, base, diffbits);
                dest.add_var(i, std::move(var));
            }
        }
    }

    /**
     * Decode a number as described by \a info from a compressed bufr with
     * \a subsets subsets, and send the resulting variables to \a dest
     */
    void decode_compressed_number(Varinfo info, unsigned associated_field_bits, unsigned subsets, std::function<void(unsigned, Var&&, uint32_t)> dest);

    /**
     * Decode a number as described by dest.info(), and set it as value for \a
     * dest. The number is decoded for \a subsets compressed datasets, and an
     * exception is thrown if the values differ.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     * @param subsets
     *   Number of subsets in the compressed data section
     */
    void decode_compressed_semantic_number(Var& dest, unsigned subsets);

    /**
     * Read a string from the data section
     *
     * @param bit_len
     *   Number of bits (not bytes) to read. It is normally a multiple of 8,
     *   and when it is not, the last character will contain the partial byte
     *   read.
     * @retval str
     *   Buffer where the string is written. Must be big enough to contain the
     *   longest string described by info, plus 2 bytes
     * @retval len
     *   The string length
     * @return
     *   true if we decoded a real string, false if we decoded a missing string
     *   value
     */
    bool decode_string(unsigned bit_len, char* str, size_t& len);

    /**
     * Decode a string as described by dest.info(), ad set it as value for \a
     * dest.
     *
     * It is assumed that \a dest is not set, therefore in case we decode a
     * missing value, \a dest will not be touched.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     */
    void decode_string(Var& dest);

    /**
     * Decode a string as described by dest.info(), and set it as value for \a
     * dest. The string is decoded for \a subsets compressed datasets, and an
     * exception is thrown if the values differ.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     * @param subsets
     *   Number of subsets in the compressed data section
     */
    void decode_string(Var& dest, unsigned subsets);

    /**
     * Decode a string as described by \a info from a compressed bufr with \a
     * subsets subsets, and send the resulting variables to \a dest
     */
    void decode_string(Varinfo info, unsigned subsets, std::function<void(unsigned, Var&&)> dest);

    /**
     * Decode a generic binary value as-is, as described by dest.info(), ad set
     * it as value for \a dest.
     *
     * It is assumed that \a dest is not set, therefore in case we decode a
     * missing value, \a dest will not be touched.
     *
     * @param dest
     *   Variable which holds the decoding information and that will hold the
     *   decoded value
     */
    void decode_binary(Var& dest);

    /**
     * Decode an uncompressed bitmap of \a size bits.
     *
     * The result will be a string \a size bytes long, with a '+' where the
     * bitmap reports that data is present, and a '-' where the bitmap reports
     * that data is not present.
     */
    std::string decode_uncompressed_bitmap(unsigned size)
    {
        std::string buf;
        buf.resize(size);
        for (unsigned i = 0; i < size; ++i)
        {
            uint32_t val = get_bits(1);
            buf[i] = (val == 0) ? '+' : '-';
        }
        return buf;
    }

    /**
     * Decode a "compressed" bitmap of \a size bits.
     *
     * The result will be a string \a size bytes long, with a '+' where the
     * bitmap reports that data is present, and a '-' where the bitmap reports
     * that data is not present.
     *
     * It would be more correct to say that it decodes a bitmap from a
     * compressed BUFR message, because bitmaps in compressed messages are
     * actually encoded with 7 bits per bit instead of one, because after each
     * bit they need to send 6 bits saying that it will be followed by 0 bits
     * of difference values.
     */
    std::string decode_compressed_bitmap(unsigned size)
    {
        std::string buf;
        buf.resize(size);
        for (unsigned i = 0; i < size; ++i)
        {
            uint32_t val = get_bits(1);
            buf[i] = (val == 0) ? '+' : '-';
            // Decode the number of bits (encoded in 6 bits) of difference
            // values. It's odd to repeat this for each bit in the bitmap, but
            // that's how things are transmitted and it's somewhat consistent
            // with how data compression is specified
            val = get_bits(6);
            // If compressed, ensure that the difference bits are 0 and they are
            // not trying to transmit odd things like delta bitmaps
            if (val != 0)
                parse_error("bitmap entry %u declares %u difference bits, but we only support 0", i, val);
        }
        return buf;
    }
};

/**
 * Binary buffer with bit-level append operations
 */
struct BufrOutput
{
    /// Output buffer to which we append encoded data
    std::string& out;

    /// Byte to which we are appending bits to encode
    uint8_t pbyte;

    /// Number of bits already encoded in pbyte
    int pbyte_len;

    /**
     * Wrap a string into a BufrOutput
     *
     * @param out
     *   String to append data to
     */
    BufrOutput(std::string& out);

    /**
     * Append n bits from 'val'.  n must be <= 32.
     */
    void add_bits(uint32_t val, int n);

    /**
     * Append a string \a len bits long to the output buffer as it is,
     * ignoring partially encoded bits
     */
    void raw_append(const char* str, int len)
    {
        out.append(str, len);
    }

    /// Append a 16 bits integer
    void append_short(unsigned short val)
    {
        add_bits(val, 16);
    }

    /// Append an 8 bits integer
    void append_byte(unsigned char val)
    {
        add_bits(val, 8);
    }

    /// Append a missing value \a len_bits long
    void append_missing(unsigned len_bits)
    {
        add_bits(0xffffffff, len_bits);
    }

    /// Append a string variable
    void append_string(const Var& var, unsigned len_bits);

    /// Append a string \a len_bits bits long
    void append_string(const char* val, unsigned len_bits);

    /// Append a binary value \a len_bits bits long
    void append_binary(const unsigned char* val, unsigned len_bits);

    /// Append \a var encoded according to \a info
    void append_var(Varinfo info, const Var& var);

    /// Append a missing value according to \a info
    void append_missing(Varinfo info);

    /**
     * Write all bits left to the buffer, padding the last partial byte with
     * zeros if needed to make it even
     */
    void flush();
};


}
}

#endif
