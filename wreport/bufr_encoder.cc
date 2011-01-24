/*
 * wreport/bulletin - BUFR encoder
 *
 * Copyright (C) 2005--2011  ARPA-SIM <urpsim@smr.arpa.emr.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author: Enrico Zini <enrico@enricozini.com>
 */

#include "config.h"

#include "opcode.h"
#include "bulletin.h"
#include "conv.h"

#include <stdio.h>
#include <netinet/in.h>

#include <stdlib.h>	/* malloc */
#include <ctype.h>	/* isspace */
#include <string.h>	/* memcpy */
#include <stdarg.h>	/* va_start, va_end */
#include <math.h>	/* NAN */
#include <time.h>
#include <errno.h>

#include <assert.h>

//#define DEFAULT_TABLE_ID "B000000000980601"
/*
For encoding our generics:
#define DEFAULT_ORIGIN 255
#define DEFAULT_MASTER_TABLE 12
#define DEFAULT_LOCAL_TABLE 0
#define DEFAULT_TABLE_ID "B000000002551200"
*/

// #define TRACE_ENCODER

#ifdef TRACE_ENCODER
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#define IFTRACE if (1)
#else
#define TRACE(...) do { } while (0)
#define IFTRACE if (0)
#endif

using namespace std;

namespace wreport {

namespace {
/// Iterate variables in a subset, one at a time
struct Varqueue
{
	const Subset& subset;
	unsigned cur;

	Varqueue(const Subset& subset) : subset(subset), cur(0) {}

	const bool empty() const { return cur >= subset.size(); }
	const Var& peek() const { return subset[cur]; }
	const Var& pop() { return subset[cur++]; }
};

static const double e10[] = {
    1.0,
    10.0,
    100.0,
    1000.0,
    10000.0,
    100000.0,
    1000000.0,
    10000000.0,
    100000000.0,
    1000000000.0,
    10000000000.0,
    100000000000.0,
    1000000000000.0,
    10000000000000.0,
    100000000000000.0,
    1000000000000000.0,
    10000000000000000.0,
};

static unsigned encode_double(double dval, int ref, int scale)
{
    int res;
    if (scale >= 0)
        res = (int)round(dval * e10[scale]) - ref;
    else
        res = (int)round(dval / e10[-scale]) - ref;
    if (res < 0)
        error_consistency::throwf("value %f is negative (%d) after conversion to int", dval, res);
    return res;
}


struct Outbuf
{
    /* Output decoded variables */
    std::string& out;

    /* Support for binary append */
    unsigned char pbyte;
    int pbyte_len;

    Outbuf(std::string& out)
        : out(out), pbyte(0), pbyte_len(0)
    {
    }

    /**
     * Append n bits from 'val'.  n must be <= 32.
     */
    void add_bits(uint32_t val, int n)
    {
        /* Mask for reading data out of val */
        uint32_t mask = 1 << (n - 1);
        int i;

        for (i = 0; i < n; i++) 
        {
            pbyte <<= 1;
            pbyte |= ((val & mask) != 0) ? 1 : 0;
            val <<= 1;
            pbyte_len++;

            if (pbyte_len == 8) 
                flush();
        }
#if 0
        IFTRACE {
            /* Prewrite it when tracing, to allow to dump the buffer as it's
             * written */
            while (e->out->len + 1 > e->out->alloclen)
                DBA_RUN_OR_RETURN(dba_rawmsg_expand_buffer(e->out));
            e->out->buf[e->out->len] = e->pbyte << (8 - e->pbyte_len);
        }
#endif
    }

    void raw_append(const char* str, int len)
    {
        out.append(str, len);
    }

    void append_short(unsigned short val)
    {
        add_bits(val, 16);
    }

    void append_byte(unsigned char val)
    {
        add_bits(val, 8);
    }

    void append_missing(unsigned len_bits)
    {
        add_bits(0xffffffff, len_bits);
    }

    void append_string(const Var& var, unsigned len_bits)
    {
        const char* val = var.value();
        unsigned i, bi;
        unsigned smax = strlen(val);
        for (i = 0, bi = 0; bi < len_bits; ++i)
        {
            TRACE("append_string:len: %d, smax: %d, i: %d, bi: %d\n", len_bits, smax, i, bi);
            /* Strings are space-padded in BUFR */
            char todo = (i < smax) ? val[i] : ' ';
            if (len_bits - bi >= 8)
            {
                append_byte(todo);
                bi += 8;
            }
            else
            {
                /* Pad with zeros if writing strings with a number of bits
                 * which is not multiple of 8.  It's not worth to implement
                 * writing partial bytes at the moment and it's better to fail
                 * gracefully, as my understanding is that this case should
                 * never happen anyway. */
                add_bits(0, len_bits - bi);
                bi = len_bits;
            }
        }
    }

    void append_double(double val, int bit_ref, int bufr_scale, unsigned bit_len)
    {
        unsigned ival = encode_double(val, bit_ref, bufr_scale);
        TRACE("append_double:converted to int (ref %d, scale %d): %d\n", bit_ref, bufr_scale, ival);
        TRACE("append_double:writing %u with size %d\n", ival, bit_len);
        /* In case of overflow, store 'missing value' */
        if ((unsigned)ival >= (1u<<bit_len))
        {
            error_consistency::throwf("value %f does not fit in %d bits", val, bit_len);
            /*
            TRACE("append_double:overflow: %x %u %d >= (1<<%u) = %x %u %d\n",
                    ival, ival, ival, len,
                    1<<len, 1<<len, 1<<len);
            ival = 0xffffffff;
            */
        }
        TRACE("append_double:about to encode: %x %u %d\n", ival, ival, ival);
        add_bits(ival, bit_len);
    }

    /* Write all bits left to the buffer, padding with zeros */
    void flush()
    {
        if (pbyte_len == 0) return;

        while (pbyte_len < 8)
        {
            pbyte <<= 1;
            pbyte_len++;
        }

        out.append((const char*)&pbyte, 1);
        pbyte_len = 0;
        pbyte = 0;
    }
};

#if 0
/* Dump 'count' bits of 'buf', starting at the 'ofs-th' bit */
static dba_err dump_bits(void* buf, int ofs, int count, FILE* out)
{
	bitvec vec;
	int i, j;
	DBA_RUN_OR_RETURN(bitvec_create(&vec, "mem", 0, buf, (count + ofs) / 8 + 2));
	for (i = 0, j = 0; i < ofs; i++, j++)
	{
		uint32_t val;
		DBA_RUN_OR_RETURN(bitvec_get_bits(vec, 1, &val));
		if (j != 0 && (j % 8) == 0)
			putc(' ', out);
		putc(val ? ',' : '.', out);
	}
	for (i = 0; i < count; i++, j++)
	{
		uint32_t val;
		DBA_RUN_OR_RETURN(bitvec_get_bits(vec, 1, &val));
		if (j != 0 && (j % 8) == 0)
			putc(' ', out);
		putc(val ? '1' : '0', out);
	}
	bitvec_delete(vec);
	return dba_error_ok();
}
#endif



struct Encoder
{
	/* Input message data */
	const BufrBulletin& in;
    /// Output buffer
    Outbuf out;

	/* We have to memorise offsets rather than pointers, because e->out->buf
	 * can get reallocated during the encoding */

	/* Offset of the start of BUFR section 1 */
	int sec1_start;
	/* Offset of the start of BUFR section 2 */
	int sec2_start;
	/* Offset of the start of BUFR section 3 */
	int sec3_start;
	/* Offset of the start of BUFR section 4 */
	int sec4_start;
	/* Offset of the start of BUFR section 4 */
	int sec5_start;

	/* Current value of scale change from C modifier */
	int c_scale_change;
	/* Current value of width change from C modifier */
	int c_width_change;
    /** Current value of string length override from C08 modifiers (0 for no
     * override)
     */
    int c_string_len_override;

	/* Subset we are encoding */
	const Subset* subset;

	/* Set these to non-null if we are encoding a data present bitmap */
	const Var* bitmap_to_encode;
	int bitmap_use_cur;
	int bitmap_subset_cur;

	Encoder(const BufrBulletin& in, std::string& out)
		: in(in), out(out),
		  sec1_start(0), sec2_start(0), sec3_start(0), sec4_start(0), sec5_start(0),
		  c_scale_change(0), c_width_change(0), c_string_len_override(0),
		  bitmap_to_encode(0), bitmap_use_cur(0), bitmap_subset_cur(0)
	{
	}

	void bitmap_next();

	void encode_sec1ed3();
	void encode_sec1ed4();
	void encode_sec2();
	void encode_sec3();
	void encode_sec4();

	void encode_data_section(const Opcodes& ops, Varqueue& vars);
	unsigned encode_b_data(const Opcodes& ops, Varqueue& vars);
	void encode_b_data(const Varinfo& info, const Var& var);
	unsigned encode_r_data(const Opcodes& ops, Varqueue& vars, const Var* bitmap = NULL);
	unsigned encode_c_data(const Opcodes& ops, Varqueue& vars);
	unsigned encode_bitmap(const Opcodes& ops, Varqueue& vars);

	// Run the encoding, copying data from in to out
	void run();
};

void Encoder::encode_sec1ed3()
{
    /* Encode bufr section 1 (Identification section) */
    /* Length of section */
    out.add_bits(18, 24);
    /* Master table number */
    out.append_byte(0);
    /* Originating/generating sub-centre (defined by Originating/generating centre) */
    out.append_byte(in.subcentre);
    /* Originating/generating centre (Common Code tableC-1) */
    /*DBA_RUN_OR_RETURN(bufr_message_append_byte(e, 0xff));*/
    out.append_byte(in.centre);
    /* Update sequence number (zero for original BUFR messages; incremented for updates) */
    out.append_byte(in.update_sequence_number);
    /* Bit 1= 0 No optional section = 1 Optional section included Bits 2 ­ 8 set to zero (reserved) */
    out.append_byte(in.optional_section_length ? 0x80 : 0);

    /* Data category (BUFR Table A) */
    /* Data sub-category (defined by local ADP centres) */
    out.append_byte(in.type);
    out.append_byte(in.localsubtype);
    /* Version number of master tables used (currently 9 for WMO FM 94 BUFR tables) */
    out.append_byte(in.master_table);
    /* Version number of local tables used to augment the master table in use */
    out.append_byte(in.local_table);

    /* Year of century */
    out.append_byte(in.rep_year == 2000 ? 100 : (in.rep_year % 100));
    /* Month */
    out.append_byte(in.rep_month);
    /* Day */
    out.append_byte(in.rep_day);
    /* Hour */
    out.append_byte(in.rep_hour);
    /* Minute */
    out.append_byte(in.rep_minute);
    /* Century */
    out.append_byte(in.rep_year / 100);
}

void Encoder::encode_sec1ed4()
{
    /* Encode bufr section 1 (Identification section) */
    /* Length of section */
    out.add_bits(22, 24);
    /* Master table number */
    out.append_byte(0);
    /* Originating/generating centre (Common Code tableC-1) */
    out.append_short(in.centre);
    /* Originating/generating sub-centre (defined by Originating/generating centre) */
    out.append_short(in.subcentre);
    /* Update sequence number (zero for original BUFR messages; incremented for updates) */
    out.append_byte(in.update_sequence_number);
    /* Bit 1= 0 No optional section = 1 Optional section included Bits 2 ­ 8 set to zero (reserved) */
    out.append_byte(in.optional_section_length ? 0x80 : 0);

    /* Data category (BUFR Table A) */
    out.append_byte(in.type);
    /* International data sub-category */
    out.append_byte(in.subtype);
    /* Local subcategory (defined by local ADP centres) */
    out.append_byte(in.localsubtype);
    /* Version number of master tables used (currently 9 for WMO FM 94 BUFR tables) */
    out.append_byte(in.master_table);
    /* Version number of local tables used to augment the master table in use */
    out.append_byte(in.local_table);

    /* Year of century */
    out.append_short(in.rep_year);
    /* Month */
    out.append_byte(in.rep_month);
    /* Day */
    out.append_byte(in.rep_day);
    /* Hour */
    out.append_byte(in.rep_hour);
    /* Minute */
    out.append_byte(in.rep_minute);
    /* Second */
    out.append_byte(in.rep_second);
}

void Encoder::encode_sec2()
{
    /* Encode BUFR section 2 (Optional section) */
    /* Nothing to do */
    if (in.optional_section_length)
    {
        int pad;
        /* Length of section */
        if ((pad = (in.optional_section_length % 2 == 1)))
            out.add_bits(4 + in.optional_section_length + 1, 24);
        else
            out.add_bits(4 + in.optional_section_length, 24);
        /* Set to 0 (reserved) */
        out.append_byte(0);

        out.raw_append(in.optional_section, in.optional_section_length);
        // Padd to even number of bytes
        if (pad) out.append_byte(0);
    }
}

void Encoder::encode_sec3()
{
	/* Encode BUFR section 3 (Data description section) */

	if (in.subsets.empty())
		throw error_consistency("message to encode has no data subsets");

	if (in.datadesc.empty())
		throw error_consistency("message to encode has no data descriptors");
#if 0
	if (in.datadesc.empty())
	{
		TRACE("Regenerating datadesc\n");
		/* If the data descriptor list is not already present, try to generate it
		 * from the varcodes of the variables in the first subgroup to encode */
		DBA_RUN_OR_GOTO(fail, bufrex_msg_generate_datadesc(e->in));

		/* Reread the descriptors */
		DBA_RUN_OR_GOTO(fail, bufrex_msg_get_datadesc(e->in, &ops));
	} else {
		TRACE("Reusing datadesc\n");
	}
#endif
    /* Length of section */
    out.add_bits(8 + 2*in.datadesc.size(), 24);
    /* Set to 0 (reserved) */
    out.append_byte(0);
    /* Number of data subsets */
    out.append_short(in.subsets.size());
    /* Bit 0 = observed data; bit 1 = use compression */
    out.append_byte(128);

    /* Data descriptors */
    for (unsigned i = 0; i < in.datadesc.size(); ++i)
        out.append_short(in.datadesc[i]);

    /* One padding byte to make the section even */
    out.append_byte(0);
}

void Encoder::encode_sec4()
{
    /* Encode BUFR section 4 (Data section) */

    /* Length of section (currently set to 0, will be filled in later) */
    out.add_bits(0, 24);
    out.append_byte(0);

	/* Encode all the subsets, uncompressed */
	for (unsigned i = 0; i < in.subsets.size(); ++i)
	{
		/* Encode the data of this subset */
		subset = &in.subset(i);
		Varqueue varqueue(*subset);
		encode_data_section(Opcodes(in.datadesc), varqueue);
	}

    /* Write all the bits and pad the data section to reach an even length */
    out.flush();
    if ((out.out.size() % 2) == 1)
        out.append_byte(0);
    out.flush();

    /* Write the length of the section in its header */
    {
        uint32_t val = htonl(out.out.size() - sec4_start);
        memcpy((char*)out.out.data() + sec4_start, ((char*)&val) + 1, 3);

        TRACE("sec4 size %zd\n", out.out.size() - sec4_start);
    }
}

void Encoder::bitmap_next()
{
	if (bitmap_to_encode == NULL)
		throw error_consistency("applying a data present bitmap with no current bitmap");
	TRACE("bitmap_next pre %d %d %d\n", bitmap_use_cur, bitmap_subset_cur, bitmap_to_encode->info()->len);
	++bitmap_use_cur;
	++bitmap_subset_cur;
	while (bitmap_use_cur < 0 || (
		(unsigned)bitmap_use_cur < bitmap_to_encode->info()->len &&
		bitmap_to_encode->value()[bitmap_use_cur] == '-'))
	{
		TRACE("INCR\n");
		++bitmap_use_cur;
		++bitmap_subset_cur;

		while ((unsigned)bitmap_subset_cur < subset->size() &&
			WR_VAR_F((*subset)[bitmap_subset_cur].code()) != 0)
			++bitmap_subset_cur;
	}
	if ((unsigned)bitmap_use_cur > bitmap_to_encode->info()->len)
		throw error_consistency("moved past end of data present bitmap");
	if ((unsigned)bitmap_subset_cur == subset->size())
		throw error_consistency("end of data reached when applying attributes");
	TRACE("bitmap_next post %d %d\n", bitmap_use_cur, bitmap_subset_cur);
}

unsigned Encoder::encode_b_data(const Opcodes& ops, Varqueue& vars)
{
	unsigned used;
#if 0
	unsigned int len;
	dba_var var;
	dba_var tmpvar = NULL;
#endif

	IFTRACE{
		TRACE("bufr_message_encode_b_data: items: ");
		ops.print(stderr);
		TRACE("\n");
	}

	Varinfo info = in.btable->query(ops.head());
	const Var* var = NULL;
	
	// Choose which value we should encode
	if (WR_VAR_F(ops.head()) == 0 && WR_VAR_X(ops.head()) == 33
		   && bitmap_to_encode != NULL && (unsigned)bitmap_use_cur < bitmap_to_encode->info()->len)
	{
		// Attribute of the variable pointed by the bitmap
		TRACE("Encode attribute %01d%02d%03d %d/%d subset %d/%zd\n",
			WR_VAR_F(ops.head()), WR_VAR_X(ops.head()), WR_VAR_Y(ops.head()),
			bitmap_use_cur, bitmap_to_encode->info()->len,
			bitmap_subset_cur, subset->size());
		var = (*subset)[bitmap_subset_cur].enqa(ops.head());
		bitmap_next();
		used = 1;
	} else {
		// Proper variable
		if (vars.empty()) error_consistency("no more data to encode");
		var = &vars.pop();
		used = 1;
	}

	IFTRACE{
		TRACE("Encoding @.d+%d [bl %d+%d sc %d+%d ref %d]: ", /*e->in->len - (e->sec4_start + 4),*/
                out.pbyte_len,
				info->bit_len, c_width_change,
				info->scale, c_scale_change,
				info->bit_ref);
		if (var)
			var->print(stderr);
	}

	if (var && var->code() != ops.head())
		error_consistency::throwf("input variable %d%02d%03d differs from expected variable %d%02d%03d",
				WR_VAR_F(var->code()), WR_VAR_X(var->code()), WR_VAR_Y(var->code()),
				WR_VAR_F(ops.head()), WR_VAR_X(ops.head()), WR_VAR_Y(ops.head()));

    unsigned len = info->bit_len;
    if (info->is_string() && c_string_len_override != 0)
    {
        TRACE("encode_b_data:string len override to %d bytes\n", c_string_len_override);
        len = c_string_len_override * 8;
    } else if (c_width_change != 0) {
        TRACE("encode_b_data:width change: %d (len %d->%d)\n", c_width_change, len, len + c_width_change);
        len += c_width_change;
    }

    if (var == NULL || var->value() == NULL)
    {
        out.append_missing(len);
    } else if (info->is_string()) {
        out.append_string(*var, len);
    } else {
        double dval = var->enqd();
        TRACE("encode_b_data:starting point %f %s (%s)\n", dval, info->unit, info->desc);
        dval = convert_units(info->unit, info->bufr_unit, dval);
        TRACE("encode_b_data:unit conversion gives: %f %s\n", dval, info->bufr_unit);

        /* Apply scale change if required */
        int scale = info->bufr_scale;
        if (c_scale_change)
        {
            TRACE("encode_b_data:scale change: %d\n", c_scale_change);
            scale += c_scale_change;
        }

        try {
            out.append_double(dval, info->bit_ref, scale, len);
        } catch (error_consistency& e) {
            e.msg += " encoding " + varcode_format(info->var);
            throw e;
        }
    }

	IFTRACE {
		/*
#ifndef TRACE_ENCODER
		int startofs, startbofs;
#endif
		*/
		/*
		TRACE("Encoded as: ");
		DBA_RUN_OR_RETURN(dump_bits(e->out->buf + startofs, startbofs, 32, stderr));
		TRACE("\n");
		*/

		TRACE("bufr_message_encode_b_data (items:");
		ops.print(stderr);
		TRACE(")\n");
	}

	return used;
}

void Encoder::encode_b_data(const Varinfo& orig_info, const Var& var)
{
    Varinfo info = in.btable->query(var.code());

    IFTRACE{
        TRACE("Encoding @.d+%d [bl %d sc %d ref %d]: ", /*e->in->len - (e->sec4_start + 4),*/
                out.pbyte_len,
                info->bit_len, info->bufr_scale, info->bit_ref);
        var.print(stderr);
    }

    if (var.value() == NULL)
    {
        out.append_missing(info->bit_len);
    } else if (info->is_string()) {
        out.append_string(var, info->bit_len);
    } else {
        double dval = var.enqd();
        TRACE("encode_b_data:starting point %f %s (%s)\n", dval, info->unit, info->desc);
        dval = convert_units(orig_info->unit, info->bufr_unit, dval);
        TRACE("encode_b_data:unit conversion gives: %f %s\n", dval, info->bufr_unit);

        try {
            out.append_double(dval, info->bit_ref, info->bufr_scale, info->bit_len);
        } catch (error_consistency& e) {
            e.msg += varcode_format(info->var);
            throw e;
        }
    }
}

/* If using delayed replication and count is not -1, use count for the delayed
 * replication factor; else, look for a delayed replication factor among the
 * input variables */
unsigned Encoder::encode_r_data(const Opcodes& ops, Varqueue& vars, const Var* bitmap)
{
	unsigned used = 1;
	int group = WR_VAR_X(ops.head());
	int count = WR_VAR_Y(ops.head());
	
	IFTRACE{
		TRACE("bufr_message_encode_r_data %01d%02d%03d %d %d: items: ",
			WR_VAR_F(ops.head()), WR_VAR_X(ops.head()), WR_VAR_Y(ops.head()), group, count);
		ops.print(stderr);
		TRACE("\n");
	}

	if (count == 0)
	{
		/* Delayed replication */

		if (bitmap == NULL)
		{
			/* Look for a delayed replication factor in the input vars */
			if (vars.empty())
				throw error_consistency("checking for availability of data to encode");

			/* Get the repetition count */
			count = vars.pop().enqi();

			TRACE("delayed replicator count read as %d\n", count);
		} else {
			count = bitmap->info()->len;
			TRACE("delayed replicator count passed by caller as %d\n", count);
		}

		/* Get encoding informations for this repetition count */
		Varinfo info = in.btable->query(ops[used]);

        /* Encode the repetition count */
        out.add_bits(count, info->bit_len);

		/* Move past the node with the repetition count */
		++used;

		TRACE("encode_r_data %d items %d times (delayed)\n", group, count);
	} else
		TRACE("encode_r_data %d items %d times\n", group, count);

	if (bitmap)
	{
		// Encode the bitmap here directly
		if (ops[used] != WR_VAR(0, 31, 31))
			error_consistency::throwf("bitmap data descriptor is %d%02d%03d instead of B31031",
					WR_VAR_F(ops[used]), WR_VAR_X(ops[used]), WR_VAR_Y(ops[used]));

        for (unsigned i = 0; i < bitmap->info()->len; ++i)
            // One bit from the current bitmap
            out.add_bits(bitmap->value()[i] == '+' ? 0 : 1, 1);
        TRACE("Encoded %d bitmap entries\n", bitmap->info()->len);
	} else {
		// Extract the first `group' nodes, to handle here
		Opcodes group_ops = ops.sub(used, group);

		// encode_data_section on it `count' times
		for (int i = 0; i < count; ++i)
			encode_data_section(group_ops, vars);
	}

	return used + group;
}

unsigned Encoder::encode_bitmap(const Opcodes& ops, Varqueue& vars)
{
	unsigned used = 0;

	Varcode code = vars.peek().code();
	if (WR_VAR_F(code) != 2)
		error_consistency::throwf("request to encode a bitmap but the input variable is %01d%02d%03d",
				WR_VAR_F(code), WR_VAR_X(code), WR_VAR_Y(code));

	bitmap_to_encode = &vars.pop();
	++used;
	bitmap_use_cur = -1;
	bitmap_subset_cur = -1;

	IFTRACE{
		TRACE("Encoding data present bitmap:");
		bitmap_to_encode->print(stderr);
	}

	/* Encode the bitmap */
	used += encode_r_data(ops.sub(used), vars, bitmap_to_encode);

	/* Point to the first attribute to encode */
	bitmap_next();

/*	
	TRACE("Decoded bitmap count %d: ", bitmap_count);
	for (size_t i = 0; i < dba_var_info(bitmap)->len; ++i)
		TRACE("%c", dba_var_value(bitmap)[i]);
	TRACE("\n");
*/
	return used;
}

unsigned Encoder::encode_c_data(const Opcodes& ops, Varqueue& vars)
{
	Varcode code = ops.head();

	TRACE("C DATA %01d%02d%03d\n", WR_VAR_F(code), WR_VAR_X(code), WR_VAR_Y(code));

	switch (WR_VAR_X(code))
	{
		case 1: 
			c_width_change = WR_VAR_Y(code) ? WR_VAR_Y(code) - 128 : 0;
			TRACE("Set width change to %d\n", c_width_change);
			return 1;
		case 2: 
			c_scale_change = WR_VAR_Y(code) ? WR_VAR_Y(code) - 128 : 0;
			TRACE("Set scale change to %d\n", c_scale_change);
			return 1;
        case 8: {
            int cdatalen = WR_VAR_Y(code);
            IFTRACE {
                if (cdatalen)
                    TRACE("decode_c_data:character size overridden to %d chars for all fields\n", cdatalen);
                else
                    TRACE("decode_c_data:character size overridde end\n");
            }
            c_string_len_override = cdatalen;
            return 1;
        }
		case 22:
			if (WR_VAR_Y(code) == 0)
			{
				return encode_bitmap(ops, vars);
			} else
				error_consistency::throwf("C modifier %d%02d%03d not yet supported",
							WR_VAR_F(code),
							WR_VAR_X(code),
							WR_VAR_Y(code));
        case 23:
            if (WR_VAR_Y(code) == 0)
            {
                return encode_bitmap(ops, vars);
            } else if (WR_VAR_Y(code) == 255) {
                if (bitmap_to_encode == NULL)
                    error_consistency::throwf("found C23255 with no active bitmap");
                if ((unsigned)bitmap_use_cur >= bitmap_to_encode->info()->len)
                    error_consistency::throwf("found C23255 while at the end of active bitmap");

                /*
                // Attribute of the variable pointed by the bitmap
                TRACE("Encode attribute %01d%02d%03d %d/%d subset %d/%zd\n",
                        WR_VAR_F(ops.head()), WR_VAR_X(ops.head()), WR_VAR_Y(ops.head()),
                        bitmap_use_cur, bitmap_to_encode->info()->len,
                        bitmap_subset_cur, subset->size());
                */
                // Get the variable referenced by the current bitmap
                const Var& rel_var = (*subset)[bitmap_subset_cur];
                // Get the substitution attribute
                const Var* attr = rel_var.enqa(rel_var.code());
                if (!attr)
                    error_consistency::throwf("no substitute value found for C23255");
                bitmap_next();

                // Use the details of the corrisponding variable for decoding
                Varinfo info = in.subsets[0][bitmap_subset_cur].info();
                // Encode the value
                encode_b_data(info, *attr);
                return 1;
            } else
                error_consistency::throwf("C modifier %d%02d%03d not yet supported",
                        WR_VAR_F(code),
                        WR_VAR_X(code),
                        WR_VAR_Y(code));
		case 24:
			if (WR_VAR_Y(code) == 0)
			{
				return 1 + encode_r_data(ops.sub(1), vars);
			} else
				error_consistency::throwf("C modifier %d%02d%03d not yet supported",
							WR_VAR_F(code),
							WR_VAR_X(code),
							WR_VAR_Y(code));
		default:
			error_unimplemented::throwf("C modifier %d%02d%03d is not yet supported",
						WR_VAR_F(code),
						WR_VAR_X(code),
						WR_VAR_Y(code));
	}
}


void Encoder::encode_data_section(const Opcodes& ops, Varqueue& vars)
{
	TRACE("bufr_message_encode_data_section: START\n");

	for (unsigned i = 0; i < ops.size(); )
	{
		IFTRACE{
			TRACE("bufr_message_encode_data_section TODO: ");
			ops.sub(i).print(stderr);
			TRACE("\n");
			TRACE("bufr_message_encode_data_section NEXTVAR: ");
			if (vars.empty())
				TRACE("(none)\n");
			else
				vars.peek().print(stderr);
		}

		switch (WR_VAR_F(ops[i]))
		{
			case 0: i += encode_b_data(ops.sub(i), vars); break;
			case 1: i += encode_r_data(ops.sub(i), vars); break;
			case 2: i += encode_c_data(ops.sub(i), vars); break;
			case 3:
			{
				Opcodes exp = in.dtable->query(ops[i]);
				encode_data_section(exp, vars);
				++i;
				break;
			}
			default:
				error_consistency::throwf(
						"variable %01d%02d%03d cannot be handled",
							WR_VAR_F(ops[i]),
							WR_VAR_X(ops[i]),
							WR_VAR_Y(ops[i]));
		}
	}
}

void Encoder::run()
{
    /* Encode bufr section 0 (Indicator section) */
    out.raw_append("BUFR\0\0\0", 7);
    out.append_byte(in.edition);

    TRACE("sec0 ends at %zd\n", out.out.size());
    sec1_start = out.out.size();

	switch (in.edition)
	{
		case 2:
		case 3: encode_sec1ed3(); break;
		case 4: encode_sec1ed4(); break;
		default:
			error_unimplemented::throwf("Encoding BUFR edition %d is not implemented", in.edition);
	}


    TRACE("sec1 ends at %zd\n", out.out.size());
    sec2_start = out.out.size();
    encode_sec2();

    TRACE("sec2 ends at %zd\n", out.out.size());
    sec3_start = out.out.size();
    encode_sec3();

    TRACE("sec3 ends at %zd\n", out.out.size());
    sec4_start = out.out.size();
    encode_sec4();

    TRACE("sec4 ends at %zd\n", out.out.size());
    sec5_start = out.out.size();

    /* Encode section 5 (End section) */
    out.raw_append("7777", 4);
    TRACE("sec5 ends at %zd\n", out.out.size());

    /* Write the length of the BUFR message in its header */
    {
        uint32_t val = htonl(out.out.size());
        memcpy((char*)out.out.data() + 4, ((char*)&val) + 1, 3);

        TRACE("msg size %zd\n", out.out.size());
    }
}

} // Unnamed namespace

void BufrBulletin::encode(std::string& out) const
{
	Encoder e(*this, out);
	e.run();
	//out.encoding = BUFR;
}


} // wreport namespace

/* vim:set ts=4 sw=4: */
