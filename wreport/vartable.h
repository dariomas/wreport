#ifndef WREPORT_VARTABLE_H
#define WREPORT_VARTABLE_H

#include <wreport/varinfo.h>
#include <vector>
#include <string>

namespace wreport {

/**
 * Holds a variable information table
 *
 * It never needs to be deallocated, as all the Vartable returned by
 * wreport are pointers to memory-cached versions that are guaranteed to exist
 * for all the lifetime of the program.
 *
 * There are many B tables with slight differences used by different
 * meteorological centre or equipment.  This module allows to access
 * different vartables using Vartable::get().
 *
 * Vartable and Varinfo have special memory management: they are never
 * deallocated.  This is an explicit design choice to speed up passing and
 * copying Varinfo values, that are used very intensely as they accompany all
 * the physical values processed by wreport. This behaviour should not be a
 * cause of memory leaks, since a software would only need to access a limited
 * amount of distinct variable informations during its lifetime.
 */
struct Vartable
{
    virtual ~Vartable();

    /// Return the pathname of the file from which this table has been loaded
    virtual std::string pathname() const = 0;

    /**
     * Query the Vartable. Throws an exception if not found.
     *
     * @param code
     *   wreport::Varcode to query
     * @return
     *   the wreport::varinfo with the results of the query.
     */
    virtual Varinfo query(Varcode code) const = 0;

    /// Check if the code can be resolved to a varinfo
    virtual bool contains(Varcode code) const = 0;

    /**
     * Query an altered version of the vartable
     *
     * @param var
     *    wreport::Varcode to query
     * @param scale
     *   Scale to use instead of the default
     * @param bit_len
     *   Bit length to use instead of the default
     * @return
     *   the wreport::Varinfo with the results of the query.
     *   The resulting Varinfo is stored inside the Vartable, can be freely
     *   copied around and does not need to be deallocated.
     */
    virtual Varinfo query_altered(Varcode code, int new_scale, unsigned new_bit_len) const = 0;

    /**
     * Return a BUFR vartable, by file name.
     *
     * Once loaded, the table will be cached in memory for reuse, and
     * further calls to load_bufr() will return the cached version.
     */
    static const Vartable* load_bufr(const std::string& pathname);

    /**
     * Return a CREX vartable, by file name.
     *
     * Once loaded, the table will be cached in memory for reuse, and
     * further calls to load_crex() will return the cached version.
     */
    static const Vartable* load_crex(const std::string& pathname);

    /// Find a BUFR table
    static const Vartable* get_bufr(int master_table, int centre=0, int subcentre=0, int local_table=0);

    /// Find a CREX table
    static const Vartable* get_crex(int master_table_number, int edition, int table);

    /// Find a BUFR table, by file name (without extension)
    static const Vartable* get_bufr(const std::string& basename);

    /// Find a CREX table, by file name (without extension)
    static const Vartable* get_crex(const std::string& basename);
};

}

#endif
