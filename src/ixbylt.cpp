// $Id$
// Author: John Wu <John.Wu at ACM.org>
// Copyright (c) 2007-2022 the Regents of the University of California
//
// This file contains the implementation of the class ibis::bylt, a
// unbinned version of range-equality encoded index.
//
// The word 'bylt' is a Danish translation of English word pack.  The class
// ibis::pack implements the binned version of range-equality encoding.
//
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)   // some identifiers longer than 256 characters
#endif
#include "irelic.h"
#include "part.h"

#include <sstream> // std::ostringstream

////////////////////////////////////////////////////////////////////////
ibis::bylt::bylt(const ibis::column *c, const char *f)
    : ibis::relic(c, f) {
    if (c == 0) return; // nothing to do
    if (cbits.empty() || cbits.size()+1 != cbounds.size()) {
        if (fname != 0)
            readCoarse(f);
        else
            coarsen();
    }
    if (ibis::gVerbose > 2) {
        ibis::util::logger lg;
        lg() << "bylt[" << col->partition()->name() << '.' << col->name()
             << "]::ctor -- intialized a range-equality index with "
             << cbits.size() << " coarse bitmap" << (cbits.size()>1?"s":"")
             << " and " << bits.size() << " fine bitmap"
             << (bits.size()>1?"s":"") << " for " << nrows << " row"
             << (nrows>1?"s":"");
        if (ibis::gVerbose > 6) {
            lg() << "\n";
            print(lg());
        }
    }
} // ibis::bylt::bylt

/// Reconstruct an index from content of a storage objecta.
/**
   The leading portion of the index file is the same as ibis::relic, which
   allows the constructor of the base class to work properly.  The content
   following the last bitvector in ibis::relic is as follows:
@code
   nc      (uint32_t)       -- number of coarse bins.
   cbounds (unsigned[nc+1]) -- boundaries of the coarse bins.
   coffsets(int32_t[nc+1])  -- starting position of the coarse level bitmaps.
   cbits   (bitvector[nc])  -- bitvector laid out one after another.
@endcode
@sa ibis::bylt::writeCoarse
*/
ibis::bylt::bylt(const ibis::column* c, ibis::fileManager::storage* st,
                 size_t start) : ibis::relic(c, st, start) {
    if (offset64.size() > bits.size()) {
        start = offset64.back();
    }
    else if (offset32.size() > bits.size()) {
        start = offset32.back();
    }
    else {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << '.'
            << col->name() << "]::ctor can not proceed further without "
            "bitmap size information";
        clear();
        return;
    }

    if (st->size() <= start+12)
        return; // no coarse bin

    size_t end;
    const char offsetsize = st->begin()[6];
    uint32_t nc = *(reinterpret_cast<uint32_t*>(st->begin()+start));
    if (nc == 0 ||
        st->size() <= start + (sizeof(int32_t)+offsetsize)*(nc+1))
        return;

    start += sizeof(uint32_t);
    end = start + sizeof(uint32_t) * (nc+1);
    {
        array_t<uint32_t> tmp(st, start, end);
        cbounds.swap(tmp);
    }
    start = end;
    end += offsetsize * (nc+1);
    if (offsetsize == 8) {
        array_t<int64_t> tmp(st, start, end);
        coffset64.swap(tmp);
        coffset32.clear();
        if (coffset64.back() <= coffset64.front()) {
            coffset64.swap(tmp);
            array_t<uint32_t> tmp2;
            cbounds.swap(tmp2);
            return;
        }
    }
    else if (offsetsize == 4) {
        array_t<int32_t> tmp(st, start, end);
        coffset32.swap(tmp);
        coffset64.clear();
        if (coffset32.back() <= coffset32.front()) {
            coffset32.swap(tmp);
            array_t<uint32_t> tmp2;
            cbounds.swap(tmp2);
            return;
        }
    }
    else {
        array_t<uint32_t> tmp2;
        cbounds.swap(tmp2);
        return;
    }

    cbits.resize(nc);
    for (unsigned i = 0; i < nc; ++ i)
        cbits[i] = 0;

    if (ibis::gVerbose > 2) {
        ibis::util::logger lg;
        lg() << "bylt[" << col->partition()->name() << '.' << col->name()
             << "]::ctor -- initialized a range-equality index with "
             << cbits.size() << " coarse bitmap" << (cbits.size()>1?"s":"")
             << " and " << bits.size() << " fine bitmap"
             << (bits.size()>1?"s":"") << " for " << nrows << " row"
             << (nrows>1?"s":"") << " from storage object at @ " << st;
        if (ibis::gVerbose > 6) {
            lg() << "\n";
            print(lg());
        }
    }
} // ibis::bylt::bylt

/// Extend the index with the data from df.
long ibis::bylt::append(const char* dt, const char* df, uint32_t nnew) {
    long ret = ibis::relic::append(dt, df, nnew);
    if (ret <= 0 || static_cast<uint32_t>(ret) != nnew)
        return ret;

    if (nrows == col->partition()->nRows())
        coarsen();
    return ret;
}

/// Build the coarse bins.  It fills the array offset64, partition the
/// bitmaps (in bits) into equal size (bytes) groups.
void ibis::bylt::coarsen() {
    if (vals.size() < 32) return; // don't construct the coarse level
    if (cbits.size() > 0 && (cbits.size()+1 == coffset64.size() ||
                             cbits.size()+1 == coffset32.size())) return;

    const uint32_t nbits = bits.size();
    if (offset64.size() != nbits+1) {
        offset64.resize(nbits+1);
        if (offset32.size() == nbits+1) {
            for (unsigned i = 0; i <= nbits; ++ i)
                offset64[i] = offset32[i];
        }
        else {
            offset64[0] = 0;
            for (unsigned i = 0; i < nbits; ++ i)
                offset64[i+1] = offset64[i] + (bits[i] ? bits[i]->bytes() : 0U);
        }
    }

    unsigned ncoarse = 0;
    if (col != 0) {
        const char* spec = col->indexSpec();
        if (spec != 0 && *spec != 0 && strstr(spec, "ncoarse=") != 0) {
            // number of coarse bins specified explicitly
            const char* tmp = 8+strstr(spec, "ncoarse=");
            unsigned j = strtol(tmp, 0, 0);
            if (j > 4)
                ncoarse = j;
        }
    }
    // default size based on the size of fine level index sf:
    // (w-1) * sqrt(sf*(sf-N/(w-1))) / (2N)
    if (ncoarse < 5U && offset64.back() > offset64[0]+(int32_t)(nrows/31U)) {
        ncoarse = sizeof(ibis::bitvector::word_t);
        const int wm1 = ncoarse*8-1;
        const long sf = (offset64.back()-offset64[0]) / ncoarse;
        ncoarse = static_cast<unsigned>
            (0.5*wm1*sqrt(sf*(sf-(double)nrows/wm1))/nrows);
        const unsigned ncmax = (unsigned) sqrt(2.0 * vals.size());
        if (ncoarse < ncmax) {
            const double obj1 = (sf + (ncoarse-1.0)*nrows/wm1)
                * (sf*0.5/ncoarse + 2.0*nrows/wm1);
            const double obj2 = (sf + (double)ncoarse*nrows/wm1)
                * (sf*0.5/(ncoarse+1) + 2.0*nrows/wm1);
            ncoarse += (obj2 < obj1);
        }
        else {
            ncoarse = ncmax;
        }
    }
    if (ncoarse < 5 || ncoarse >= vals.size()) return;

    // partition the fine level bitmaps into groups with nearly equal
    // number of bytes
    cbounds.resize(ncoarse);
    cbounds[0] = 0;
    for (unsigned i = 1; i < ncoarse; ++ i) {
        int32_t target = offset64[cbounds[i-1]] +
            (offset64.back() - offset64[cbounds[i-1]]) / (ncoarse - i + 1);
        cbounds[i] = offset64.find(target);
        if (cbounds[i] > cbounds[i-1]+1 &&
            offset64[cbounds[i]]-target > target-offset64[cbounds[i]-1])
            -- (cbounds[i]);
        else if (cbounds[i] <= cbounds[i-1])
            cbounds[i] = cbounds[i-1] + 1;
    }
    if (cbounds.back() >= nbits) {
        cbounds.back() = nbits - 1;
        for (unsigned i = ncoarse-1; i > 0 && cbounds[i] < cbounds[i-1]; -- i)
            cbounds[i-1] = cbounds[i] - 1;
    }
    if (ibis::gVerbose > 2) {
        ibis::util::logger lg;
        lg() << "bylt[" << col->partition()->name() << '.'
             << col->name() << "]::coarsen will divide " << bits.size()
             << " bitmaps into " << ncoarse << " bins\n";
        for (unsigned i = 0; i < cbounds.size(); ++ i)
            lg() << cbounds[i] << " ";
    }

    // fill cbits
    for (unsigned i = 0; i < cbits.size(); ++ i) {
        delete cbits[i];
        cbits[i] = 0;
    }
    cbits.reserve(ncoarse-1);
    for (unsigned i = 0; i < ncoarse-1; ++ i) {
        ibis::bitvector tmp;
        if (i > 0) {
            tmp.copy(*(cbits[i-1]));
            addBins(cbounds[i], cbounds[i+1], tmp);
        }
        else {
            sumBins(cbounds[i], cbounds[i+1], tmp);
        }
        cbits.push_back(new ibis::bitvector(tmp));
    }

    // fill coffsets
    coffset32.clear();
    coffset64.resize(ncoarse);
    coffset64[0] = 0;
    for (unsigned i = 0; i < ncoarse-1; ++ i) {
        cbits[i]->compress();
        coffset64[i+1] = coffset64[i] + cbits[i]->bytes();
    }
} // ibis::bylt::coarsen

void ibis::bylt::activateCoarse() const {
    std::string evt = "bylt";
    if (ibis::gVerbose > 0) {
        evt += '[';
        evt += col->partition()->name();
        evt += '.';
        evt += col->name();
        evt += ']';
    }
    evt += "::activateCoarse";

    const uint32_t nobs = cbits.size();
    bool missing = false; // any bits[i] missing (is 0)?
    ibis::column::mutexLock lock(col, evt.c_str());
    for (uint32_t i = 0; i < nobs && ! missing; ++ i)
        missing = (cbits[i] == 0);
    if (missing == false) return;

    if (coffset32.size() <= nobs && coffset64.size() <= nobs) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt
            << " cannot proceed without coffset32 or coffset64";
    }
    else if (str) { // using a ibis::fileManager::storage as back store
        LOGGER(ibis::gVerbose > 8)
            << evt << " retrieving data from "
            << "fileManager::storage(0x" << str << ")";

        if (coffset64.size() > nobs) {
            for (uint32_t i = 0; i < nobs; ++i) {
                if (cbits[i] == 0 && coffset64[i+1] > coffset64[i]) {
                    array_t<ibis::bitvector::word_t>
                        a(str, coffset64[i], coffset64[i+1]);
                    cbits[i] = new ibis::bitvector(a);
                    cbits[i]->sloppySize(nrows);
                }
            }
        }
        else {
            for (uint32_t i = 0; i < nobs; ++i) {
                if (cbits[i] == 0 && coffset32[i+1] > coffset32[i]) {
                    array_t<ibis::bitvector::word_t>
                        a(str, coffset32[i], coffset32[i+1]);
                    cbits[i] = new ibis::bitvector(a);
                    cbits[i]->sloppySize(nrows);
                }
            }
        }
    }
    else if (fname) { // using the named file directly
        int fdes = UnixOpen(fname, OPEN_READONLY);
        if (fdes < 0) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- " << evt << " failed to open file \"" << fname
                << "\"";
            return;
        }

        LOGGER(ibis::gVerbose > 8)
            << evt << " retrieving data from file \"" << fname << "\"";

#if defined(_WIN32) && defined(_MSC_VER)
        (void)_setmode(fdes, _O_BINARY);
#endif
        uint32_t i = 0;
        while (i < nobs) {
            // skip to next empty bit vector
            while (i < nobs && cbits[i] != 0)
                ++ i;
            // the last bitvector to activate. can not be larger
            // than j
            uint32_t aj = (i<nobs ? i + 1 : nobs);
            while (aj < nobs && cbits[aj] == 0)
                ++ aj;
            if (coffset64.size() > nobs && coffset64[aj] > coffset64[i]) {
                const size_t start = coffset64[i];
                ibis::fileManager::storage *a0 = new
                    ibis::fileManager::storage(fdes, start, coffset64[aj]);
                while (i < aj) {
                    if (coffset64[i+1] > coffset64[i]) {
                        array_t<ibis::bitvector::word_t>
                            a1(a0, coffset64[i]-start, coffset64[i+1]-start);
                        bits[i] = new ibis::bitvector(a1);
                        bits[i]->sloppySize(nrows);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
                        LOGGER(ibis::gVerbose >= 0)
                            << "DEBUG -- " << evt
                            << " activating bitvector " << i
                            << "by reading file " << fname
                            << "coffset64[" << i << "]= " << coffset64[i]
                            << ", coffset64[" << i+1 << "]= "
                            << coffset64[i+1];
#endif
                    }
                    ++ i;
                }
            }
            else if (coffset32.size() > nobs &&
                     coffset32[aj] > coffset32[i]) {
                const size_t start = coffset32[i];
                ibis::fileManager::storage *a0 = new
                    ibis::fileManager::storage(fdes, start, coffset32[aj]);
                while (i < aj) {
                    if (coffset32[i+1] > coffset32[i]) {
                        array_t<ibis::bitvector::word_t>
                            a1(a0, coffset32[i]-start, coffset32[i+1]-start);
                        bits[i] = new ibis::bitvector(a1);
                        bits[i]->sloppySize(nrows);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
                        LOGGER(ibis::gVerbose >= 0)
                            << "DEBUG -- " << evt
                            << " activating bitvector " << i
                            << "by reading file " << fname
                            << "coffset32[" << i << "]= " << coffset32[i]
                            << ", coffset32[" << i+1 << "]= "
                            << coffset32[i+1];
#endif
                    }
                    ++ i;
                }
            }
            i = aj; // always advance i
        }
        (void) UnixClose(fdes);
    }
    else {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << " can not regenerate "
            "bitvectors because neither str or fname is specified";
    }
} // ibis::bylt::activateCoarse

void ibis::bylt::activateCoarse(uint32_t i) const {
    if (i >= bits.size()) return;       // index out of range
    if (cbits[i] != 0) return;  // already active

    std::string evt = "bylt";
    if (ibis::gVerbose > 0) {
        evt += '[';
        evt += col->partition()->name();
        evt += '.';
        evt += col->name();
        evt += ']';
    }
    evt += "::activateCoarse";
    ibis::column::mutexLock lock(col, evt.c_str());
    if (cbits[i] != 0) return;  // already active
    if (coffset32.size() <= cbits.size() && coffset64.size() <= cbits.size()) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << '(' << i << ") no records of offsets";
        return;
    }
    if ((coffset64.size() > cbits.size() && coffset64[i+1] <= coffset64[i]) ||
        (coffset32.size() > cbits.size() && coffset32[i+1] <= coffset32[i])) {
        return;
    }

    if (str) { // using a ibis::fileManager::storage as back store
        LOGGER(ibis::gVerbose > 8)
            << evt << "(" << i << ") retrieving data from storage at " << str;

        if (coffset64.size() > cbits.size()) {
            array_t<ibis::bitvector::word_t>
                a(str, coffset64[i], coffset64[i+1]);
            cbits[i] = new ibis::bitvector(a);
            cbits[i]->sloppySize(nrows);
// #if DEBUG+0 > 0 || _DEBUG+0 > 0
//          LOGGER(ibis::gVerbose >= 0)
//              << "DEBUG -- " << evt << "(" << i
//              << ") constructed a bitvector from range ["
//              << coffset64[i] << ", " << coffset64[i+1]
//              << ") of a storage at "
//              << static_cast<const void*>(str->begin());
// #endif
        }
        else {
            array_t<ibis::bitvector::word_t>
                a(str, coffset32[i], coffset32[i+1]);
            cbits[i] = new ibis::bitvector(a);
            cbits[i]->sloppySize(nrows);
// #if DEBUG+0 > 0 || _DEBUG+0 > 0
//          LOGGER(ibis::gVerbose >= 0)
//              << "DEBUG -- " << evt << "(" << i
//              << ") constructed a bitvector from range ["
//              << coffset32[i] << ", " << coffset32[i+1]
//              << ") of a storage at "
//              << static_cast<const void*>(str->begin());
// #endif
        }
    }
    else if (fname) { // using the named file directly
        int fdes = UnixOpen(fname, OPEN_READONLY);
        if (fdes < 0) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- "  << evt << "(" << i
                << ") failed to open file \"" << fname << "\" ... "
                << (errno != 0 ? strerror(errno) : "??");
            errno = 0;
            return;
        }

        LOGGER(ibis::gVerbose > 8)
            << evt << "(" << i << ") retrieving data from file \""
            << fname << "\"";
#if defined(_WIN32) && defined(_MSC_VER)
        (void)_setmode(fdes, _O_BINARY);
#endif
        if (coffset64.size() > cbits.size()) {
            array_t<ibis::bitvector::word_t> a0(fdes, coffset64[i],
                                                coffset64[i+1]);
            cbits[i] = new ibis::bitvector(a0);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
            LOGGER(ibis::gVerbose >= 0)
                << "DEBUG -- " << evt << "(" << i
                << ") constructed a bitvector from range ["
                << coffset64[i] << ", " << coffset64[i+1]
                << ") of file " << fname;
#endif
        }
        else {
            array_t<ibis::bitvector::word_t> a0(fdes, coffset32[i],
                                                coffset32[i+1]);
            cbits[i] = new ibis::bitvector(a0);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
            LOGGER(ibis::gVerbose >= 0)
                << "DEBUG -- " << evt << "(" << i
                << ") constructed a bitvector from range ["
                << coffset32[i] << ", " << coffset32[i+1]
                << ") of file " << fname;
#endif
        }
        cbits[i]->sloppySize(nrows);
        (void) UnixClose(fdes);
    }
    else {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << "(" << i << ") can not regenerate "
            "the bitvector because neither str or fname is specified";
    }
} // ibis::bylt::activateCoarse

void ibis::bylt::activateCoarse(uint32_t i, uint32_t j) const {
    if (j > cbits.size())
        j = cbits.size();
    if (i >= j) // empty range
        return;

    std::string evt = "bylt";
    if (ibis::gVerbose > 0) {
        evt += '[';
        evt += col->partition()->name();
        evt += '.';
        evt += col->name();
        evt += ']';
    }
    evt += "::activateCoarse";
    ibis::column::mutexLock lock(col, evt.c_str());

    while (i < j && cbits[i] != 0) ++ i;
    if (i >= j) return; // all bitvectors active
    if ((coffset64.size() > cbits.size() && coffset64[j] <= coffset64[i]) ||
        (coffset32.size() > cbits.size() && coffset32[j] <= coffset32[i]))
        return;

    if (coffset64.size() <= cbits.size() && coffset32.size() <= cbits.size()) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << "(" << i << ", " << j 
            << ") can not proceed without offset64 or offset32";
    }
    else if (str) { // using an ibis::fileManager::storage as back store
        LOGGER(ibis::gVerbose > 8)
            << evt << "(" << i << ", " << j
            << ") retrieving data from ibis::fileManager::storage(0x"
            << str << ")";

        while (i < j) {
            if (cbits[i] != 0) {
            }
            else if (coffset64.size() > cbits.size() &&
                     coffset64[i+1] > coffset64[i]) {
                array_t<ibis::bitvector::word_t>
                    a(str, coffset64[i], coffset64[i+1]);
                cbits[i] = new ibis::bitvector(a);
                cbits[i]->sloppySize(nrows);
// #if DEBUG+0 > 0 || _DEBUG+0 > 0
//              LOGGER(ibis::gVerbose >= 0)
//                  << evt << " constructed bitvector " << i << " from range ["
//                  << coffset64[i] << ", " << coffset64[i+1]
//                  << ") of a storage at "
//                  << static_cast<const void*>(str->begin());
// #endif
            }
            else if (coffset32.size() > cbits.size() &&
                     coffset32[i+1] > coffset32[i]) {
                array_t<ibis::bitvector::word_t>
                    a(str, coffset32[i], coffset32[i+1]);
                cbits[i] = new ibis::bitvector(a);
                cbits[i]->sloppySize(nrows);
// #if DEBUG+0 > 0 || _DEBUG+0 > 0
//          LOGGER(ibis::gVerbose >= 0)
//              << evt << " constructed bitvector " << i << " from range ["
//              << coffset32[i] << ", " << coffset32[i+1]
//              << ") of a storage at "
//              << static_cast<const void*>(str->begin());
// #endif
            }
            ++ i;
        }
    }
    else if (fname) { // using the named file directly
        int fdes = UnixOpen(fname, OPEN_READONLY);
        if (fdes < 0) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- "<< evt << "(" << i << ", " << j
                << ") failed to open file \"" << fname << "\" ... "
                << (errno != 0 ? strerror(errno) : "??");
            return;
        }

        LOGGER(ibis::gVerbose > 8)
            << evt << "(" << i << ", " << j
            << ") retrieving data from file \"" << fname << "\"";
#if defined(_WIN32) && defined(_MSC_VER)
        (void)_setmode(fdes, _O_BINARY);
#endif
        while (i < j) {
            // skip to next empty bit vector
            while (i < j && cbits[i] != 0)
                ++ i;
            // the last bitvector to activate. can not be larger
            // than j
            uint32_t aj = (i<j ? i + 1 : j);
            while (aj < j && cbits[aj] == 0)
                ++ aj;

            if (coffset64.size() > cbits.size() &&
                coffset64[aj] > coffset64[i]) {
                const size_t start = coffset64[i];
                ibis::fileManager::storage *a0 = new
                    ibis::fileManager::storage(fdes, start,
                                               coffset64[aj]);
                while (i < aj) {
                    if (coffset64[i+1] > coffset64[i]) {
                        array_t<ibis::bitvector::word_t>
                            a1(a0, coffset64[i]-start, coffset64[i+1]-start);
                        cbits[i] = new ibis::bitvector(a1);
                        cbits[i]->sloppySize(nrows);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
                        LOGGER(ibis::gVerbose >= 0)
                            << evt << " constructed bitvector " << i
                            << " from range [" << coffset64[i] << ", "
                            << coffset64[i+1] << ") of file " << fname;
#endif
                    }
                    ++ i;
                }
            }
            else if (coffset32.size() > cbits.size() &&
                     coffset32[aj] > coffset32[i]) {
                const size_t start = coffset32[i];
                ibis::fileManager::storage *a0 = new
                    ibis::fileManager::storage(fdes, start,
                                               coffset32[aj]);
                while (i < aj) {
                    if (coffset32[i+1] > coffset32[i]) {
                        array_t<ibis::bitvector::word_t>
                            a1(a0, coffset32[i]-start, coffset32[i+1]-start);
                        cbits[i] = new ibis::bitvector(a1);
                        cbits[i]->sloppySize(nrows);
#if DEBUG+0 > 0 || _DEBUG+0 > 0
                        LOGGER(ibis::gVerbose >= 0)
                            << evt << " constructed a bitvector " << i
                            << " from range [" << coffset32[i] << ", "
                            << coffset32[i+1] << ") of file " << fname;
#endif
                    }
                    ++ i;
                }
            }
            i = aj; // always advance i

            (void) UnixClose(fdes);
        }
    }
    else {
        LOGGER(ibis::gVerbose > 0)
            << evt << "(" << i << ", " << j << ") can not regenerate "
            "bitvectors because neither str or fname is known";
    }
} // ibis::bylt::activateCoarse

uint32_t ibis::bylt::estimate(const ibis::qContinuousRange& expr) const {
    ibis::bitvector bv;
    long ierr = evaluate(expr, bv);
    return static_cast<uint32_t>(ierr > 0 ? ierr : 0);
} // ibis::bylt::estimate

/// Estimate the I/O cost needed to evaluate the given range condition.
/// This function examines all combinations of using the two levels of
/// indexes and return the minimal cost.
double ibis::bylt::estimateCost(const ibis::qContinuousRange& expr) const {
    double res = static_cast<double>(col->elementSize() * nrows);
    if (bits.empty() || (offset64.size() <= bits.size() &&
                         offset32.size() <= bits.size())) {
        return res;
    }

    // values in the range [hit0, hit1) satisfy the query
    uint32_t hit0, hit1;
    locate(expr, hit0, hit1);

    // number of coarse bit vectors
    const unsigned ncoarse = (cbits.empty() || cbounds.empty() ? 0U :
                              cbits.size() <= cbounds.size()-1 ?
                              cbits.size() : cbounds.size()-1);
    const off_t fine =
        (offset64.size() > bits.size()
         ? (offset64[hit1] - offset64[hit0] <=
            ((offset64.back() - offset64[hit1])
             + (offset64[hit0] - offset64[0]))
            ? offset64[hit1] - offset64[hit0]
            : ((offset64.back() - offset64[hit1])
               + (offset64[hit0] - offset64[0])))
         : (offset32[hit1] - offset32[hit0] <=
            ((offset32.back() - offset32[hit1])
             + (offset32[hit0] - offset32[0]))
            ? offset32[hit1] - offset32[hit0]
            : ((offset32.back() - offset32[hit1])
               + (offset32[hit0] - offset32[0]))));
    if (hit1 <= hit0) {
        res = 0.0;
        return res;
    }
    if (hit0 == 0 && hit1 >= bits.size()) {
        res = 0.0;
        return res;
    }

    if (hit0+1 == hit1) { // equality condition
        res = fine;
        return res;
    }
    if (hit0+3 >= hit1 || ncoarse == 0 ||
        (coffset64.size() <= cbits.size() && coffset32.size() < cbits.size())) {
        res = fine;
        return res;
    }

    // see whether the coarse bins could help
    const uint32_t c0 = cbounds.find(hit0);
    const uint32_t c1 = cbounds.find(hit1);
    if (c0 >= c1) { // within the same coarse bin
        off_t tmp = fine;
        if (c1 > 0 && c1 < cbounds.size()) {
            tmp = (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c1-1]])
                      + (offset64[cbounds[c1]] - offset64[hit1]))
                   : ((offset32[hit0] - offset32[cbounds[c1-1]])
                      + (offset32[cbounds[c1]] - offset32[hit1])));
            if (coffset64.size() > cbits.size()) {
                if (c1 > 1)
                    tmp += coffset64[c1] - coffset64[c1-2];
                else
                    tmp += coffset64[c1];
            }
            else if (coffset32.size() > cbits.size()) {
                if (c1 > 1)
                    tmp += coffset32[c1] - coffset32[c1-2];
                else
                    tmp += coffset32[c1];
            }
            else { // don't know the size of the coarse bins
                tmp += fine;
            }
        }
        if (fine/99 <= tmp/100)
            res = fine;
        else
            res = tmp;
    }
    else if (c1 < cbounds.size() &&
             cbounds[c1] == hit1 && cbounds[c0] == hit0) {
        // need coarse bins only
        if (c0 > 0) {
            if (c0 == c1-1) {
                if (coffset64.size() > cbits.size())
                    res = coffset64[c1+1] - coffset64[c0-1];
                else
                    res = coffset32[c1+1] - coffset32[c0-1];
            }
            else if (coffset64.size() > cbits.size()) {
                res = coffset64[c0] - coffset64[c0-1] +
                    coffset64[c1] - coffset64[c1-1];
            }
            else {
                res = coffset32[c0] - coffset32[c0-1] +
                    coffset32[c1] - coffset32[c1-1];
            }
        }
        else if (coffset64.size() > cbits.size()) {
            res = coffset64[c1] - coffset64[c1-1];
        }
        else if (coffset32.size() > cbits.size()) {
            res = coffset32[c1] - coffset32[c1-1];
        }
        else {
            res = fine;
        }
    }
    else if (hit1 >= bits.size()) { // query range open to the right
        if (c0 < ncoarse - 1) { // left edge bin is a regular bin
            off_t cost = (offset64.size() > ncoarse
                         ? offset64[cbounds[c0]] - offset64[hit0]
                         : offset32[cbounds[c0]] - offset32[hit0])
                + (c0 > 0
                   ? (coffset64.size() > ncoarse
                      ? coffset64[c0] - coffset64[c0-1]
                      : coffset32[c0] - coffset32[c0-1])
                   : 0);
            off_t tmp;
            if (c0 > 1) { // option 3: [complement | -]
                tmp = (offset64.size() > ncoarse
                       ? offset64[hit0] - offset64[cbounds[c0-1]]
                       : offset32[hit0] - offset32[cbounds[c0-1]])
                    + (coffset64.size() > ncoarse
                       ? coffset64[c0-1] - coffset64[c0-2]
                       : (coffset32.size() > ncoarse
                          ? coffset32[c0-1] - coffset32[c0-2]
                          : fine));
                if (tmp < cost) {
                    cost = tmp;
                }
            }
            // option 0/1: using fine level bitmaps only
            tmp = (offset64.size() > bits.size()
                   ? (offset64.back() - offset64[hit0] <=
                      (offset64[hit0] - offset64[0])
                      ? offset64.back() - offset64[hit0]
                      : (offset64[hit0] - offset64[0]))
                   : (offset32.back() - offset32[hit0] <=
                      (offset32[hit0] - offset32[0])
                      ? offset32.back() - offset32[hit0]
                      : (offset32[hit0] - offset32[0])));
            if (cost > static_cast<off_t>(0.99*tmp)) { // slightly prefer 0/1
                cost = tmp;
            }
            res = cost;
        }
        else { // left edge bin is the unrecorded bin
            off_t cost = (offset64.size() > bits.size()
                         ? offset64.back() - offset64[hit0]
                         : offset32.back() - offset32[hit0]);
            off_t tmp = (offset64.size() > bits.size()
                        ? offset64[hit0] - offset64[cbounds.back()]
                        : offset32[hit0] - offset32[cbounds.back()])
                + (coffset64.size() > cbits.size()
                   ? coffset64[c0] - coffset64[c0-1]
                   : (coffset32.size() > cbits.size()
                      ? coffset32[c0] - coffset32[c0-1]
                      : fine));

            if (tmp < static_cast<off_t>(0.99*cost)) {
                res = tmp;
            }
            else {
                res = cost;
            }
        }
    }
    else if (c1 >= cbounds.size()) { // right edge bin is not recorded
#if _DEBUG+0 > 1 || DEBUG+0 > 1
        if (ibis::gVerbose > 0) {
            ibis::util::logger lg;
            lg() << "DEBUG -- bylt[" << col->partition()->name() << '.'
                 << col->name() << "]::estimateCost(" << expr
                 << "): hit0 = " << hit0 << ", hit1 = " << hit1
                 << ", c0 = " << c0 << ", c1 = " << c1;
            if (c0 > 0)
                lg() << ", cbounds[" << c0-1 << "] = " << cbounds[c0-1];
            lg() << ", cbounds[" << c0 << "] = " << cbounds[c0];
            if (coffset64.size() > cbits.size()) {
                if (c0 > 0)
                    lg() << ", coffset64[" << c0-1 << "] = "
                         << coffset64[c0-1];
                lg() << ", coffset64[" << c0 << "] = " << coffset64[c0];
            }
            else {
                if (c0 > 0)
                    lg() << ", coffset32[" << c0-1 << "] = " << coffset32[c0-1];
                lg() << ", coffset32[" << c0 << "] = " << coffset32[c0];
            }
            if (offset64.size() > bits.size()) {
                lg() << ", offset64[" << cbounds[c0] << "] = "
                     << offset64[cbounds[c0]]
                     << ", offset64[" << hit0 << "] = " << offset64[hit0]
                     << ", offset64[" << hit1 << "] = " << offset64[hit1]
                     << ", offset64[" << cbounds.back()
                     << "] = " << offset64[cbounds.back()];
            }
            else {
                lg() << ", offset32[" << cbounds[c0] << "] = "
                     << offset32[cbounds[c0]]
                     << ", offset32[" << hit0 << "] = " << offset32[hit0]
                     << ", offset32[" << hit1 << "] = " << offset32[hit1]
                     << ", offset32[" << cbounds.back() << "] = "
                     << offset32[cbounds.back()];
            }
        }
#endif
        off_t tmp;
        off_t cost = (offset64.size() > bits.size()
                      ? ((offset64[cbounds[c0]] - offset64[hit0])
                         + (offset64[hit1] - offset64[cbounds.back()]))
                      : ((offset32[cbounds[c0]] - offset32[hit0])
                         + (offset32[hit1] - offset32[cbounds.back()])));
        if (c0 > 0) {
            cost += (coffset64.size() > bits.size()
                     ? ((coffset64[c0] - coffset64[c0-1])
                        + (coffset64.back() - coffset64[ncoarse-1]))
                     :(coffset32.size() > bits.size()
                       ? ((coffset32[c0] - coffset32[c0-1])
                          + (coffset32.back() - coffset32[ncoarse-1]))
                       : fine));
            // option 3: [complement | - | direct]
            tmp = (coffset64.size() > bits.size()
                   ? ((coffset64[c0-1] - (c0>1 ? coffset64[c0-2] : 0))
                      + (coffset64.back() - coffset64[ncoarse-1]))
                   : (coffset32.size() > bits.size()
                      ? ((coffset32[c0-1] - (c0>1 ? coffset32[c0-2] : 0))
                         + (coffset32.back() - coffset32[ncoarse-1]))
                      : fine))
                + (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                      + (offset64[hit1] - offset64[cbounds.back()]))
                   : ((offset32[hit0] - offset32[cbounds[c0-1]])
                      + (offset32[hit1] - offset32[cbounds.back()])));
            if (tmp < cost) {
                cost = tmp;
            }
        }
        else {
            cost += (coffset64.size() > bits.size()
                     ? (coffset64.back() - coffset64[ncoarse-1])
                     :(coffset32.size() > bits.size()
                       ? (coffset32.back() - coffset32[ncoarse-1])
                       : fine));
        }
        if (c0 > 0) {// option 4: [direct | - | complement]
            tmp = (coffset64.size() > bits.size()
                   ? coffset64[c0] - coffset64[c0-1]
                   : (coffset32.size() > bits.size()
                      ? coffset32[c0] - coffset32[c0-1] : fine))
                + (offset64.size() > bits.size()
                   ? ((offset64[cbounds[c0]] - offset64[hit0])
                      + (offset64.back() - offset64[hit1]))
                   : ((offset32[cbounds[c0]] - offset32[hit0])
                      + (offset32.back() - offset32[hit1])));
            if (tmp < cost) {
                tmp = cost;
            }
        }
        if (c0 > 1) { // option 5: [complement | - | complement]
            tmp = (coffset64.size() > bits.size()
                   ? coffset64[c0-1] - (c0>1 ? coffset64[c0-2] : 0)
                   : (coffset32.size() > bits.size()
                      ? coffset32[c0-1] - (c0>1 ? coffset32[c0-2] : 0)
                      : fine))
                + (offset64.size() > bits.size()
                   ? (offset64[hit0] - offset64[cbounds[c0-1]])
                   + (offset64.back() - offset64[hit1])
                   : (offset32[hit0] - offset32[cbounds[c0-1]])
                   + (offset32.back() - offset32[hit1]));
            if (tmp < cost) {
                cost = tmp;
            }
        }
        // option 0 and 1: fine level only
        if (cost > static_cast<off_t>(0.99*fine)) { // slightly prefer 0/1
            cost = fine;
        }
        res = cost;
    }
    else if (c0 == 0) { // left edge of query range is open
        off_t cost = (c1 > 1
                      ? (coffset64.size() > bits.size()
                         ? coffset64[c1] - coffset64[c1-1]
                         : (coffset32.size() > bits.size()
                            ? coffset32[c1] - coffset32[c1-1] : fine))
                      : 0)
            + (offset64.size() > bits.size()
               ? offset64[hit1] - offset64[cbounds[c1-1]]
               : offset32[hit1] - offset32[cbounds[c1-1]]);
        off_t tmp;
        if (c1+1 < cbounds.size()) // option 4, norm edge bin
            tmp = (coffset64.size() > bits.size()
                   ? coffset64[c1+1] - coffset64[c1]
                   : (coffset32.size() > bits.size()
                      ? coffset32[c1+1] - coffset32[c1] : fine))
                + (offset64.size() > bits.size()
                   ? offset64[cbounds[c1]] - offset64[hit1]
                   : offset32[cbounds[c1]] - offset32[hit1]);
        else // option 4, edge bin not recorded
            tmp = (offset64.size() > bits.size()
                   ? offset64.back() - offset64[hit1]
                   : offset32.back() - offset32[hit1]);
        if (tmp < cost) {
            cost = tmp;
        }
        if (cost > static_cast<off_t>(0.99*fine)) { // slightly prefer 0/1
            cost = fine;
        }
    }
    else if (c0+1 == c1) {// two edge bins right next to each other
        // option 2 [direct | - | direct], same as option 0/1
        // option 3: [complement | - | direct]
        off_t cost = (offset64.size() > bits.size()
                     ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                        + (offset64[c0] - (c0>1 ? offset64[c0-2] : 0))
                        + (offset64[hit1] - offset64[cbounds[c0]]))
                     : ((offset32[hit0] - offset32[cbounds[c0-1]])
                        + (offset32[c0] - (c0>1 ? offset32[c0-2] : 0))
                        + (offset32[hit1] - offset32[cbounds[c0]])));
        // option 4: [direct | - | complement]
        off_t tmp = (coffset64.size() > cbits.size()
                     ? coffset64[c1] - coffset64[c0-1]
                     : (coffset32.size() > cbits.size()
                        ? coffset32[c1] - coffset32[c0-1] : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
        }
        // option 5: [complement | - | complement]
        tmp = (coffset64.size() > cbits.size()
               ? ((coffset64[c0-1] - (c0>1?coffset64[c0-2]:0))
                  + (coffset64[c1] - coffset64[c0]))
               : (coffset32.size() > cbits.size()
                  ? ((coffset32[c0-1] - (c0>1?coffset32[c0-2]:0))
                     + (coffset32[c1] - coffset32[c0]))
                  : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[hit0] - offset32[cbounds[c0-1]])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
        }
        // option 0 and 1: fine level only
        if (cost > static_cast<off_t>(0.99*fine)) { // slightly prefer 0/1
            cost = fine;
        }
    }
    else {// general case: need to evaluate 6 options
        off_t cost = (coffset64.size() > cbits.size()
                      ? ((coffset64[c0] - coffset64[c0-1])
                         + (coffset64[c1-1] - coffset64[c1-2]))
                      : (coffset32.size() > cbits.size()
                         ? ((coffset32[c0] - coffset32[c0-1])
                            + (coffset32[c1-1] - coffset32[c1-2]))
                         : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[hit1] - offset64[cbounds[c1-1]]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[hit1] - offset32[cbounds[c1-1]])));
        // option 3: [complement | - | direct]
        off_t tmp = (coffset64.size() > cbits.size()
                     ? ((coffset64[c0-1] - (c0>1 ? coffset64[c0-2] : 0))
                        + (coffset64[c1-1] - coffset64[c1-2]))
                     : (coffset32.size() > cbits.size()
                        ? ((coffset32[c0-1] - (c0>1 ? coffset32[c0-2] : 0))
                           + (coffset32[c1-1] - coffset32[c1-2]))
                        : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                  + (offset64[hit1] - offset64[cbounds[c1-1]]))
               : ((offset32[hit0] - offset32[cbounds[c0-1]])
                  + (offset32[hit1] - offset32[cbounds[c1-1]])));
        if (tmp < cost) {
            cost = tmp;
        }
        // option 4: [direct | - | complement]
        tmp = (coffset64.size() > cbits.size()
               ? ((coffset64[c0] - coffset64[c0-1])
                  + (coffset64[c1] - coffset64[c1-1]))
               : (coffset32.size() > cbits.size()
                  ? ((coffset32[c0] - coffset32[c0-1])
                     + (coffset32[c1] - coffset32[c1-1]))
                  : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
        }
        // option 5: [complement | - | complement]
        tmp = (coffset64.size() > cbits.size()
               ? ((coffset64[c0] - coffset64[c0-1])
                  + (coffset64[c1] - coffset64[c1-1]))
               : (coffset32.size() > cbits.size()
                  ? ((coffset32[c0] - coffset32[c0-1])
                     + (coffset32[c1] - coffset32[c1-1]))
                  : fine))
            + (offset64.size() > bits.size()
               ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[hit0] - offset32[cbounds[c0-1]])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
        }
        // option 0 and 1: fine level only
        if (cost > static_cast<off_t>(0.99*fine)) { // slightly prefer 0/1
            cost = fine;
        }
        res = cost;
    }
    return res;
} // ibis::bylt::estimateCost

/// Compute the hits as a @c bitvector.  This function attempts to use the
/// coarse bins only if the sizes of the coarse bitmaps are known.  If the
/// coarse bitmaps are of known sizes, all combinations of coarse bins and
/// fine bins are considered.
long ibis::bylt::evaluate(const ibis::qContinuousRange& expr,
                          ibis::bitvector& lower) const {
    if (bits.empty()) { // empty index
        lower.set(0, nrows);
        return 0L;
    }

    // values in the range [hit0, hit1) satisfy the query
    uint32_t hit0, hit1;
    locate(expr, hit0, hit1);
    if (hit1 <= hit0) {
        lower.set(0, nrows);
        return 0L;
    }
    if (hit0 == 0 && hit1 >= bits.size()) {
        col->getNullMask(lower);
        return lower.cnt();
    }

    if (hit0+1 == hit1) { // equality condition
        if (bits[hit0] == 0)
            activate(hit0);
        if (bits[hit0] != 0)
            lower.copy(*(bits[hit0]));
        else
            lower.set(0, nrows);
        return lower.cnt();
    }
    // number of coarse bit vectors
    const unsigned ncoarse = (cbits.empty() || cbounds.empty() ? 0U :
                              cbits.size() <= cbounds.size()-1 ?
                              cbits.size() : cbounds.size()-1);
    if (hit0+3 >= hit1 || ncoarse == 0 ||
        (coffset64.size() <= cbits.size() &&
         coffset32.size() <= cbits.size())) {
        // no more than three bitmaps involved, or don't know the sizes
        sumBins(hit0, hit1, lower);
        return lower.cnt();
    }

    // see whether the coarse bins could help
    const uint32_t c0 = cbounds.find(hit0);
    const uint32_t c1 = cbounds.find(hit1);
    if (ibis::gVerbose > 4) {
        ibis::util::logger lg;
        lg() << "bylt::evaluate(" << expr << ") hit0=" << hit0
             << ", hit1=" << hit1;
        if (c0 < cbounds.size())
            lg() << ", cbounds[" << c0 << "]=" << cbounds[c0];
        else
            lg() << ", cbounds[" << cbounds.size()-1 << "]=" << cbounds.back();
        if (c1 < cbounds.size())
            lg() << ", cbounds[" << c1 << "]=" << cbounds[c1];
        else
            lg() << ", c1=" << c1 << ", bits.size()=" << bits.size();
    }
    const off_t finec = (offset64.size() > bits.size()
                         ? (offset64[hit1] - offset64[hit0] <=
                            ((offset64.back() - offset64[hit1])
                             + (offset64[hit0] - offset64[0]))
                            ? offset64[hit1] - offset64[hit0]
                            : ((offset64.back() - offset64[hit1])
                               + (offset64[hit0] - offset64[0])))
                         : (offset32[hit1] - offset32[hit0] <=
                            ((offset32.back() - offset32[hit1])
                             + (offset32[hit0] - offset32[0]))
                            ? offset32[hit1] - offset32[hit0]
                            : ((offset32.back() - offset32[hit1])
                               + (offset32[hit0] - offset32[0]))));
    if (c0 >= c1) { // within the same coarse bin
        off_t tmp = finec;
        if (c1 > 0 && c1 < cbounds.size()) {
            tmp = (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c1-1]])
                      + (offset64[cbounds[c1]] - offset64[hit1]))
                   : ((offset32[hit0] - offset32[cbounds[c1-1]])
                      + (offset32[cbounds[c1]] - offset32[hit1])));
            if (coffset64.size() > cbits.size()) {
                if (c1 > 1)
                    tmp += coffset64[c1] - coffset64[c1-2];
                else
                    tmp += coffset64[c1];
            }
            else if (coffset32.size() > cbits.size()) {
                if (c1 > 1)
                    tmp += coffset32[c1] - coffset32[c1-2];
                else
                    tmp += coffset32[c1];
            }
            else { // don't know the size of the coarse bins
                tmp += finec;
            }
        }
        if (finec/99 <= tmp/100) { // slightly prefer using the fine level
            sumBins(hit0, hit1, lower);
        }
        else { // need coarse bitmaps
            activateCoarse(c1>1?c1-2:0, c1);
            if (c1-1 < cbits.size() && cbits[c1-1] != 0)
                lower.copy(*(cbits[c1-1]));
            else
                col->getNullMask(lower);
            if (c1 > 1 && cbits[c1-2] != 0)
                lower -= *(cbits[c1-2]);
            if (hit0 > cbounds[c1-1]) {
                ibis::bitvector bv;
                sumBins(cbounds[c1-1], hit0, bv);
                lower -= bv;
            }
            if (c1 < cbits.size() && cbounds[c1] > hit1) {
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
            else if (c1 >= cbits.size() && bits.size() > hit1) {
                ibis::bitvector bv;
                sumBins(hit1, bits.size(), bv);
                lower -= bv;
            }
        }
    }
    else if (c1 < cbounds.size() &&
             cbounds[c1] == hit1 && cbounds[c0] == hit0) {
        // need coarse bins only
        if (c0 > 0) {
            if (c0 == c1-1) {
                activateCoarse(c0-1, c1);
            }
            else {
                activateCoarse(c0-1);
                activateCoarse(c1-1);
            }
            if (c1-1 < cbits.size() && cbits[c1-1] != 0)
                lower.copy(*(cbits[c1-1]));
            else
                col->getNullMask(lower);
            if (cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
        }
        else {
            activateCoarse(c1-1);
            if (c1-1 < cbits.size() && cbits[c1-1] != 0)
                lower.copy(*(cbits[c1-1]));
            else
                col->getNullMask(lower);
        }
    }
    else if (hit1 >= bits.size()) { // query range open to the right
        if (c0 < cbounds.size()) { // left edge bin is a regular bin
            unsigned option = 2; // option 2 [ direct | - ]
            off_t cost = (offset64.size() > bits.size()
                         ? offset64[cbounds[c0]] - offset64[hit0]
                         : offset32[cbounds[c0]] - offset32[hit0])
                + (c0 > 0
                   ? (coffset64.size() > cbits.size()
                      ? coffset64[c0] - coffset64[c0-1]
                      : coffset32[c0] - coffset32[c0-1])
                   : 0);
            off_t tmp;
            if (c0 > 1) { // option 3: [complement | -]
                tmp = (offset64.size() > bits.size()
                       ? offset64[hit0] - offset64[cbounds[c0-1]]
                       : offset32[hit0] - offset32[cbounds[c0-1]])
                    + (coffset64.size() > cbits.size()
                       ? coffset64[c0-1] - coffset64[c0-2]
                       : coffset32[c0-1] - coffset32[c0-2]);
                if (tmp < cost) {
                    cost = tmp;
                    option = 3;
                }
            }
            // option 0/1: using fine level bitmaps only
            if (cost >= static_cast<off_t>(0.99*finec)) { // slightly prefer 0/1
                cost = finec;
                option = 1;
            }
            switch (option) {
            default:
            case 1:
                sumBins(hit0, hit1, lower);
                break;
            case 2:
                col->getNullMask(lower);
                if (c0 > 0) {
                    activateCoarse(c0-1);
                    if (cbits[c0-1] != 0)
                        lower -= *(cbits[c0-1]);
                }
                if (hit0 < cbounds[c0])
                    addBins(hit0, cbounds[c0], lower);
                break;
            case 3:
                col->getNullMask(lower);
                if (c0 > 0) {
                    if (c0 > 1) {
                        activateCoarse(c0-2);
                        if (cbits[c0-2] != 0)
                            lower -= *(cbits[c0-2]);
                    }
                    ibis::bitvector bv;
                    addBins(cbounds[c0-1], hit0, bv);
                    lower -= bv;
                }
                else if (hit0 > 0) {
                    ibis::bitvector bv;
                    sumBins(0, hit0, bv);
                    lower -= bv;
                }
                break;
            }
        }
        else { // left edge bin is the unrecorded bin
            off_t cost = (offset64.size() > bits.size()
                         ? offset64.back() - offset64[hit0]
                         : offset32.back() - offset32[hit0]);
            off_t tmp = (offset64.size() > bits.size()
                        ? offset64[hit0] - offset64[cbounds.back()]
                        : offset32[hit0] - offset32[cbounds.back()])
                + (coffset64.size() > cbits.size()
                   ? coffset64[c0] - coffset64[c0-1]
                   : coffset32[c0] - coffset32[c0-1]);

            ibis::bitvector bv;
            if (tmp < static_cast<off_t>(0.99*cost)) {
                activateCoarse(c0-1);
                if (cbits[c0-1])
                    bv.copy(*cbits[c0-1]);
                else
                    bv.set(0, nrows);
                if (cbounds.back() < hit0)
                    addBins(cbounds.back(), hit0, lower);
            }
            else {
                sumBins(hit0, hit1, bv);
            }
            col->getNullMask(lower);
            lower -= bv;
        }
    }
    else if (c1 >= cbounds.size()) { // right edge bin is not recorded
        off_t tmp;
        unsigned option = 2; // [ direct | - | direct]
        off_t cost = (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[hit1] - offset64[cbounds.back()]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[hit1] - offset32[cbounds.back()])));
        if (c0 > 0) {
            cost += (coffset64.size() > cbounds.size()
                     ? ((coffset64[c0] - coffset64[c0-1])
                        + (coffset64.back() - coffset64[ncoarse-1]))
                     : ((coffset32[c0] - coffset32[c0-1])
                        + (coffset32.back() - coffset32[ncoarse-1])));
            // option 3: [complement | - | direct]
            tmp = (coffset64.size() > cbits.size()
                   ? ((coffset64[c0-1] - (c0>1 ? coffset64[c0-2] : 0))
                      + (coffset64.back() - coffset64[ncoarse-1]))
                   : ((coffset32[c0-1] - (c0>1 ? coffset32[c0-2] : 0))
                      + (coffset32.back() - coffset32[ncoarse-1])))
                + (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                      + (offset64[hit1] - offset64[cbounds.back()]))
                   : ((offset32[hit0] - offset32[cbounds[c0-1]])
                      + (offset32[hit1] - offset32[cbounds.back()])));
            if (tmp < cost) {
                cost = tmp;
                option = 3;
            }
        }
        else {
            cost += (coffset64.size() > cbits.size()
                     ? (coffset64.back() - coffset64[ncoarse-1])
                     : (coffset32.back() - coffset32[ncoarse-1]));
        }
        if (c0 > 0) {// option 4: [direct | - | complement]
            tmp = (coffset64.size() > cbits.size()
                   ? coffset64[c0] - coffset64[c0-1]
                   : coffset32[c0] - coffset32[c0-1])
                + (offset64.size() > bits.size()
                   ? ((offset64[cbounds[c0]] - offset64[hit0])
                      + (offset64.back() - offset64[hit1]))
                   : ((offset32[cbounds[c0]] - offset32[hit0])
                      + (offset32.back() - offset32[hit1])));
            if (tmp < cost) {
                tmp = cost;
                option = 4;
            }
        }
        if (c0 > 1) { // option 5: [complement | - | complement]
            tmp = (coffset64.size() > cbits.size()
                   ? coffset64[c0-1] - (c0>1 ? coffset64[c0-2] : 0)
                   : coffset32[c0-1] - (c0>1 ? coffset32[c0-2] : 0))
                + (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                      + (offset64.back() - offset64[hit1]))
                   : ((offset32[hit0] - offset32[cbounds[c0-1]])
                      + (offset32.back() - offset32[hit1])));
            if (tmp < cost) {
                cost = tmp;
                option = 5;
            }
        }
        if (cost > static_cast<off_t>(0.99*finec)) { // slightly prefer 0/1
            cost = finec;
            option = 1;
        }
        switch (option) {
        default:
        case 1: // use fine level only
            sumBins(hit0, hit1, lower);
            break;
        case 2: // [direct | - | direct]
            if (c0+2 >= cbits.size()) {
                activateCoarse(c0-1, ncoarse);
            }
            else {
                activateCoarse(c0-1);
                activateCoarse(ncoarse-1);
            }
            if (cbits[ncoarse-1] != 0)
                lower.copy(*(cbits[ncoarse-1]));
            else
                col->getNullMask(lower);
            if (c0 > 0 && cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
            if (hit0 < cbounds[c0])
                addBins(hit0, cbounds[c0], lower); // left edge bin
            if (cbounds[ncoarse] < hit1)
                addBins(cbounds[ncoarse], hit1, lower); // right edge bin
            break;
        case 3: // [complement | - | direct]
            activateCoarse(ncoarse-1);
            if (cbits[ncoarse-1] != 0)
                lower.copy(*(cbits[ncoarse-1]));
            else
                col->getNullMask(lower);
            if (c0 > 1) {
                activateCoarse(c0-2);
                if (cbits[c0-2] != 0)
                    lower -= *(cbits[c0-2]);
            }
            if (c0 > 0 && hit0 > cbounds[c0-1]) { // left edge bin
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (cbounds[ncoarse] < hit1)
                addBins(cbounds[ncoarse], hit1, lower); // right edge bin
            break;
        case 4: // [direct | - | complement]
            activateCoarse(c0-1);
            col->getNullMask(lower);
            if (c0 > 0 && cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
            if (hit0 < cbounds[c0])
                addBins(hit0, cbounds[c0], lower); // left edge bin
            if (bits.size() > hit1) { // right edge bin
                ibis::bitvector bv;
                sumBins(hit1, bits.size(), bv);
                lower -= bv;
            }
            break;
        case 5: // [complement | - | complement]
            col->getNullMask(lower);
            if (c0 > 1) {
                activateCoarse(c0-2);
                if (cbits[c0-2] != 0)
                    lower -= *(cbits[c0-2]);
            }
            if (c0 > 0 && hit0 > cbounds[c0-1]) {
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (bits.size() > hit1) {
                ibis::bitvector bv;
                sumBins(hit1, bits.size(), bv);
                lower -= bv;
            }
        }
    }
    else if (c0 == 0) { // left edge of query range is open
        unsigned option = 2; // [ - | direct ]
        off_t cost = (c1 > 1
                      ? (coffset64.size() > cbits.size()
                         ? coffset64[c1-1] - coffset64[c1-2]
                         : coffset32[c1-1] - coffset32[c1-2])
                      : 0)
            + (offset64.size() > bits.size()
               ? offset64[hit1] - offset64[cbounds[c1-1]]
               : offset32[hit1] - offset32[cbounds[c1-1]]);
        off_t tmp;
        if (c1+1 <= cbits.size()) // option 4, norm edge bin
            tmp = (coffset64.size() > cbits.size()
                   ? (coffset64[c1] - coffset64[c1-1])
                   : (coffset32[c1] - coffset32[c1-1]))
                + (offset64.size() > bits.size()
                   ? offset64[cbounds[c1]] - offset64[hit1]
                   : offset32[cbounds[c1]] - offset32[hit1]);
        else // option 4, edge bin not recorded
            tmp = (offset64.size() > bits.size()
                   ? offset64.back() - offset64[hit1]
                   : offset32.back() - offset32[hit1]);
        if (tmp < cost) {
            option = 4;
            cost = tmp;
        }
        if (cost >= static_cast<off_t>(0.99*finec)) { // slightly prefer 0/1
            cost = finec;
            option = 1;
        }
        switch (option) {
        default:
        case 1: // use fine level only
            sumBins(hit0, hit1, lower);
            break;
        case 2: // [- | direct]
            if (c1 > 1) {
                activateCoarse(c1-2);
                if (cbits[c1-2])
                    lower.copy(*(cbits[c1-2]));
                else
                    col->getNullMask(lower);
                if (cbounds[c1-1] < hit1)
                    addBins(cbounds[c1-1], hit1, lower);
            }
            else {
                sumBins(hit0, hit1, lower);
            }
            break;
        case 4: // [- | complement];
            if (c1+1 <= cbits.size()) {
                activateCoarse(c1-1);
                if (cbits[c1-1])
                    lower.copy(*cbits[c1-1]);
                else
                    col->getNullMask(lower);
            }
            else {
                col->getNullMask(lower);
            }
            if (hit1 < cbounds[c1]) {
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
            break;
        }
    }
    else if (c0+1 == c1) {// two edge bins right next to each other
        // option 2 [direct | - | direct], same as option 0/1
        unsigned option = 3;// option 3: [complement | - | direct]
        off_t cost = (offset64.size() > bits.size()
                     ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                        + (offset64[c0] - (c0>1 ? offset64[c0-2] : 0))
                        + (offset64[hit1] - offset64[cbounds[c0]]))
                     : ((offset32[hit0] - offset32[cbounds[c0-1]])
                        + (offset32[c0] - (c0>1 ? offset32[c0-2] : 0))
                        + (offset32[hit1] - offset32[cbounds[c0]])));
        // option 4: [direct | - | complement]
        off_t tmp = (coffset64.size() > cbits.size()
                    ? coffset64[c1] - coffset64[c0-1]
                    : coffset32[c1] - coffset32[c0-1])
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
            option = 4;
        }
        // option 5: [complement | - | complement]
        if (c0 > 0) {
            tmp = (coffset64.size() > cbits.size()
                   ? ((coffset64[c0-1] - (c0>1?coffset64[c0-2]:0))
                      + (coffset64[c1] - coffset64[c0]))
                   : ((coffset32[c0-1] - (c0>1?coffset32[c0-2]:0))
                      + (coffset32[c1] - coffset32[c0])))
                + (offset64.size() > bits.size()
                   ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                      + (offset64[cbounds[c1]] - offset64[hit1]))
                   : ((offset32[hit0] - offset32[cbounds[c0-1]])
                      + (offset32[cbounds[c1]] - offset32[hit1])));
            if (tmp < cost) {
                cost = tmp;
                option = 5;
            }
        }
        if (cost > static_cast<off_t>(0.99*finec)) { // slightly prefer 0/1
            cost = finec;
            option = 1;
        }
        switch (option) {
        default:
        case 1: // use fine level only
            sumBins(hit0, hit1, lower);
            break;
        case 3: // complement | - | direct
            activateCoarse((c0>1?c0-2:0), c0);
            if (cbits[c0-1] != 0)
                lower.copy(*(cbits[c0-1]));
            else
                col->getNullMask(lower);
            if (c0 > 1 && cbits[c0-2] != 0)
                lower -= *(cbits[c0-2]);
            if (cbounds[c0-1] < hit0) { // left edge bin, complement
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (cbounds[c1-1] < hit1)
                addBins(cbounds[c1-1], hit1, lower); // right edge bin
            break;
        case 4: // direct | - | complement
            activateCoarse(c0-1, c1+1);
            if (cbits[c0] != 0)
                lower.copy(*(cbits[c0]));
            else
                col->getNullMask(lower);
            if (cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
            if (hit0 < cbounds[c0])
                addBins(hit0, cbounds[c0], lower); // left edge bin
            if (cbounds[c1] > hit1) { // right edge bin
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
            break;
        case 5: // complement | - | complement
            activateCoarse(c0);
            if (cbits[c0] != 0)
                lower.copy(*(cbits[c0]));
            else
                col->getNullMask(lower);
            if (c0 > 1) {
                activateCoarse(c0-2);
                if (cbits[c0-2] != 0)
                    lower -= *(cbits[c0-2]);
            }
            if (hit0 > cbounds[c0-1]) { // left edge bin
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (cbounds[c1] > hit1) { // right edge bin
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
        }
    }
    else {// general case: need to evaluate 6 options
        unsigned option = 2; // option 2 [direct | - | direct]
        off_t cost = (coffset64.size() > cbits.size()
                     ? ((coffset64[c0] - coffset64[c0-1])
                        + (coffset64[c1-1] - coffset64[c1-2]))
                     : ((coffset32[c0] - coffset32[c0-1])
                        + (coffset32[c1-1] - coffset32[c1-2])))
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[hit1] - offset64[cbounds[c1-1]]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[hit1] - offset32[cbounds[c1-1]])));
        // option 3: [complement | - | direct]
        off_t tmp = (coffset64.size() > cbits.size()
                    ? ((coffset64[c0-1] - (c0>1?coffset64[c0-2]:0))
                       + (coffset64[c1-1] - coffset64[c1-2]))
                    : ((coffset32[c0-1] - (c0>1?coffset32[c0-2]:0))
                       + (coffset32[c1-1] - coffset32[c1-2])))
            + (offset64.size() > bits.size()
               ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                  + (offset64[hit1] - offset64[cbounds[c1-1]]))
               : ((offset32[hit0] - offset32[cbounds[c0-1]])
                  + (offset32[hit1] - offset32[cbounds[c1-1]])));
        if (tmp < cost) {
            cost = tmp;
            option = 3;
        }
        // option 4: [direct | - | complement]
        tmp = (coffset64.size() > cbits.size()
               ? ((coffset64[c0] - coffset64[c0-1])
                  + (coffset64[c1] - coffset64[c1-1]))
               : ((coffset32[c0] - coffset32[c0-1])
                  + (coffset32[c1] - coffset32[c1-1])))
            + (offset64.size() > bits.size()
               ? ((offset64[cbounds[c0]] - offset64[hit0])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[cbounds[c0]] - offset32[hit0])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
            option = 4;
        }
        // option 5: [complement | - | complement]
        tmp = (coffset64.size() > cbits.size()
               ? ((coffset64[c0-1] - (c0>1?coffset64[c0-2]:0))
                  + (coffset64[c1] - coffset64[c1-1]))
               : ((coffset32[c0-1] - (c0>1?coffset32[c0-2]:0))
                  + (coffset32[c1] - coffset32[c1-1])))
            + (offset64.size() > bits.size()
               ? ((offset64[hit0] - offset64[cbounds[c0-1]])
                  + (offset64[cbounds[c1]] - offset64[hit1]))
               : ((offset32[hit0] - offset32[cbounds[c0-1]])
                  + (offset32[cbounds[c1]] - offset32[hit1])));
        if (tmp < cost) {
            cost = tmp;
            option = 5;
        }
        if (cost > static_cast<off_t>(0.99*finec)) { // slightly prefer 0/1
            cost = finec;
            option = 1;
        }
        switch (option) {
        default:
        case 1: // use fine level only
            sumBins(hit0, hit1, lower);
            break;
        case 2: // direct | - | direct
            if (c0 >= c1-2) {
                activateCoarse(c0-1, c1-1);
            }
            else {
                activateCoarse(c0-1);
                activateCoarse(c1-2);
            }
            if (cbits[c1-2] != 0)
                lower.copy(*(cbits[c1-2]));
            else
                col->getNullMask(lower);
            if (cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
            if (hit0 < cbounds[c0])
                addBins(hit0, cbounds[c0], lower); // left edge bin
            if (cbounds[c1-1] < hit1)
                addBins(cbounds[c1-1], hit1, lower); // right edge bin
            break;
        case 3: // complement | - | direct
            activateCoarse(c1-2);
            if (cbits[c1-2] != 0)
                lower.copy(*(cbits[c1-2]));
            else
                col->getNullMask(lower);
            if (c0 > 1) {
                activateCoarse(c0-2);
                if (cbits[c0-2] != 0)
                    lower -= *(cbits[c0-2]);
            }
            if (cbounds[c0-1] < hit0) { // left edge bin, complement
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (cbounds[c1-1] < hit1)
                addBins(cbounds[c1-1], hit1, lower); // right edge bin
            break;
        case 4: // direct | - | complement
            activateCoarse(c0-1);
            activateCoarse(c1-1);
            if (cbits[c1-1] != 0)
                lower.copy(*(cbits[c1-1]));
            else
                col->getNullMask(lower);
            if (cbits[c0-1] != 0)
                lower -= *(cbits[c0-1]);
            if (hit0 < cbounds[c0])
                addBins(hit0, cbounds[c0], lower); // left edge bin
            if (cbounds[c1] > hit1) { // right edge bin
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
            break;
        case 5: // complement | - | complement
            activateCoarse(c1-1);
            if (cbits[c1-1] != 0)
                lower.copy(*(cbits[c1-1]));
            else
                col->getNullMask(lower);
            if (c0 > 1) {
                activateCoarse(c0-2);
                if (cbits[c0-2] != 0)
                    lower -= *(cbits[c0-2]);
            }
            if (hit0 > cbounds[c0-1]) { // left edge bin
                ibis::bitvector bv;
                sumBins(cbounds[c0-1], hit0, bv);
                lower -= bv;
            }
            if (cbounds[c1] > hit1) { // right edge bin
                ibis::bitvector bv;
                sumBins(hit1, cbounds[c1], bv);
                lower -= bv;
            }
        }
    }
    return lower.cnt();
} // ibis::bylt::evaluate

/// Write the content of index to the specified location.  The argument can
/// be the name of a directory or a file.  The actual index file name is
/// determined by the function indexFileName.
int ibis::bylt::write(const char* dt) const {
    if (vals.empty()) return -1;

    std::string fnm, evt;
    evt = "bylt";
    if (col != 0 && ibis::gVerbose > 1) {
        evt += '[';
        evt += col->fullname();
        evt += ']';
    }
    evt += "::write";
    if (ibis::gVerbose > 1 && dt != 0) {
        evt += '(';
        evt += dt;
        evt += ')';
    }
    indexFileName(fnm, dt);
    if (fnm.empty()) {
        return 0;
    }
    else if (0 != str && 0 != str->filename() &&
             0 == fnm.compare(str->filename())) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << " can not overwrite the index file \""
            << fnm << "\" while it is used as a read-only file map";
        return 0;
    }
    else if (fname != 0 && *fname != 0 && 0 == fnm.compare(fname)) {
        activate(); // read everything into memory
        fname = 0; // break the link with the named file
    }
    ibis::fileManager::instance().flushFile(fnm.c_str());

    if (fname != 0 || str != 0)
        activate(); // activate all bitvectors

    int fdes = UnixOpen(fnm.c_str(), OPEN_WRITENEW, OPEN_FILEMODE);
    if (fdes < 0) {
        ibis::fileManager::instance().flushFile(fnm.c_str());
        fdes = UnixOpen(fnm.c_str(), OPEN_WRITENEW, OPEN_FILEMODE);
        if (fdes < 0) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- " << evt << " failed to open \"" << fnm
                << "\" for writing";
            return -2;
        }
    }
    IBIS_BLOCK_GUARD(UnixClose, fdes);
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif
#if defined(HAVE_FLOCK)
    ibis::util::flock flck(fdes);
    if (flck.isLocked() == false) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt << " failed to acquire an exclusive lock "
            "on file " << fnm << " for writing, another thread must be "
            "writing the index now";
        return -6;
    }
#endif

    off_t ierr = 0;
#ifdef FASTBIT_USE_LONG_OFFSETS
    const bool useoffset64 = true;
#else
    const bool useoffset64 = (8+getSerialSize() > 0x80000000UL);
#endif
    const bool haveCoarseBins = !(cbits.empty() || cbounds.empty());
    char header[] = "#IBIS\7\0\0";
    header[5] = (char)(haveCoarseBins ? ibis::index::BYLT : ibis::index::RELIC);
    header[6] = (char)(useoffset64 ? 8 : 4);
    ierr = UnixWrite(fdes, header, 8);
    if (ierr < 8) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- " << evt
            << " failed to write the 8-byte header, ierr = " << ierr;
        return -3;
    }
    if (useoffset64)
        ierr = ibis::relic::write64(fdes); // write the bulk of the index file
    else
        ierr = ibis::relic::write32(fdes); // write the bulk of the index file
    if (ierr < 0) {
        return ierr;
    }
    if (haveCoarseBins) {
        if (useoffset64)
            ierr = writeCoarse64(fdes); // write the coarse level bins
        else
            ierr = writeCoarse32(fdes); // write the coarse level bins
    }

    if (ierr >= 0) {
#if defined(FASTBIT_SYNC_WRITE)
#if _POSIX_FSYNC+0 > 0
        (void) UnixFlush(fdes); // write to disk
#elif defined(_WIN32) && defined(_MSC_VER)
        (void) _commit(fdes);
#endif
#endif
        const uint32_t nobs = vals.size();
        const uint32_t nc = (cbounds.size()-1 <= cbits.size() ?
                             cbounds.size()-1 : cbits.size());
        LOGGER(ibis::gVerbose > 5)
            << evt << " wrote " << nobs << " fine bitmap" << (nobs>1?"s":"")
            << " and " << nc << " coarse bitmap" << (nc>1?"s":"")
            << " to " << fnm;
    }
    return ierr;
} // ibis::bylt::write

/// Write the coarse bins into an open file.  This function is intended to
/// be called after calling ibis::relic::write, however, it does not check
/// for this fact!
int ibis::bylt::writeCoarse32(int fdes) const {
    if (cbounds.empty() || cbits.empty() || nrows == 0)
        return -4;

    off_t ierr;
    const uint32_t nc = (cbounds.size()-1 <= cbits.size() ?
                         cbounds.size()-1 : cbits.size());
    ierr  = UnixWrite(fdes, &nc, sizeof(nc));
    ierr += UnixWrite(fdes, cbounds.begin(), sizeof(uint32_t)*(nc+1));
    if (ierr < static_cast<off_t>(sizeof(uint32_t)*(nc+2))) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse32 failed to write "
            << sizeof(uint32_t)*(nc+2) << " bytes to file descriptor "
            << fdes << ", ierr = " << ierr;
        return -5;
    }

    coffset64.clear();
    coffset32.resize(nc+1);
    coffset32[0] = UnixSeek(fdes, sizeof(int32_t)*(nc+1), SEEK_CUR);
    for (unsigned i = 0; i < nc; ++ i) {
        if (cbits[i] != 0)
            cbits[i]->write(fdes);
        coffset32[i+1] = UnixSeek(fdes, 0, SEEK_CUR);
    }
    const off_t pos = coffset32[0] - sizeof(int32_t) * (nc+1);
    ierr = UnixSeek(fdes, pos, SEEK_SET);
    if (ierr != pos) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse32 failed to seek to "
            << pos << ", ierr = " << ierr;
        return -6;
    }
    ierr = UnixWrite(fdes, coffset32.begin(), sizeof(int32_t)*(nc+1));
    if (ierr < static_cast<off_t>(sizeof(int32_t)*(nc+1))) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse32 failed to write "
            << sizeof(int32_t)*(nc+1) << " bytes to file descriptor "
            << fdes << ", ierr = " << ierr;
        return -7;
    }
    ierr = UnixSeek(fdes, coffset32.back(), SEEK_SET);
    if (ierr != coffset32.back()) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse32 failed to seek to "
            << coffset32.back() << ", ierr = " << ierr;
        return -8;
    }
    return 0;
} // ibis::bylt::writeCoarse32

/// Write the coarse bins into an open file.  This function is intended to
/// be called after calling ibis::relic::write, however, it does not check
/// for this fact!
int ibis::bylt::writeCoarse64(int fdes) const {
    if (cbounds.empty() || cbits.empty() || nrows == 0)
        return -4;

    off_t ierr;
    const uint32_t nc = (cbounds.size()-1 <= cbits.size() ?
                         cbounds.size()-1 : cbits.size());
    ierr  = UnixWrite(fdes, &nc, sizeof(nc));
    ierr += UnixWrite(fdes, cbounds.begin(), sizeof(uint32_t)*(nc+1));
    if (ierr < static_cast<off_t>(sizeof(uint32_t)*(nc+2))) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse64 failed to write "
            << sizeof(uint32_t)*(nc+2) << " bytes to file descriptor "
            << fdes << ", ierr = " << ierr;
        return -5;
    }

    coffset32.clear();
    coffset64.resize(nc+1);
    coffset64[0] = UnixSeek(fdes, sizeof(int64_t)*(nc+1), SEEK_CUR);
    for (unsigned i = 0; i < nc; ++ i) {
        if (cbits[i] != 0)
            cbits[i]->write(fdes);
        coffset64[i+1] = UnixSeek(fdes, 0, SEEK_CUR);
    }
    const off_t pos = coffset64[0] - sizeof(int64_t) * (nc+1);
    ierr = UnixSeek(fdes, pos, SEEK_SET);
    if (ierr != pos) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse64 failed to seek to "
            << pos << ", ierr = " << ierr;
        return -6;
    }
    ierr = UnixWrite(fdes, coffset64.begin(), sizeof(int64_t)*(nc+1));
    if (ierr < static_cast<off_t>(sizeof(int64_t)*(nc+1))) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse64 failed to write "
            << sizeof(int64_t)*(nc+1) << " bytes to file descriptor "
            << fdes << ", ierr = " << ierr;
        return -7;
    }
    ierr = UnixSeek(fdes, coffset64.back(), SEEK_SET);
    if (ierr != coffset64.back()) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << "."
            << col->name() << "]::writeCoarse64 failed to seek to "
            << coffset64.back() << ", ierr = " << ierr;
        return -8;
    }
    return 0;
} // ibis::bylt::writeCoarse64

/// Read the index contained in the file f.
int ibis::bylt::read(const char* f) {
    std::string fnm;
    indexFileName(fnm, f);

    int fdes = UnixOpen(fnm.c_str(), OPEN_READONLY);
    if (fdes < 0) return -1;

    char header[8];
    IBIS_BLOCK_GUARD(UnixClose, fdes);
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif
    if (8 != UnixRead(fdes, static_cast<void*>(header), 8)) {
        return -2;
    }

    if (false == (header[0] == '#' && header[1] == 'I' &&
                  header[2] == 'B' && header[3] == 'I' &&
                  header[4] == 'S' &&
                  header[5] == static_cast<char>(BYLT) &&
                  (header[6] == 8 || header[6] == 4) &&
                  header[7] == static_cast<char>(0))) {
        if (ibis::gVerbose > 0) {
            ibis::util::logger lg;
            lg() << "Warning -- bylt[" << col->partition()->name() << '.'
                 << col->name() << "]::read the header from " << fnm << " (";
            printHeader(lg(), header);
            lg() << ") does not contain the expected values";
        }
        return -3;
    }

    uint32_t dim[3];
    size_t begin, end;
    clear(); // clear the current content
    fname = ibis::util::strnewdup(fnm.c_str());

    off_t ierr = UnixRead(fdes, static_cast<void*>(dim), 3*sizeof(uint32_t));
    if (ierr < static_cast<int>(3*sizeof(uint32_t))) {
        return -4;
    }
    nrows = dim[0];
    // read vals
    begin = 8*((3*sizeof(uint32_t) + 15) / 8);
    end = begin + dim[2] * sizeof(double);
    {
        array_t<double> dbl(fname, fdes, begin, end);
        vals.swap(dbl);
    }
    // read the offsets
    begin = end;
    end += header[6] * (dim[1] + 1);
    ierr = initOffsets(fdes, header[6], begin, dim[1]);
    ibis::fileManager::instance().recordPages(0, end);
#if DEBUG+0 > 1 || _DEBUG+0 > 1
    if (ibis::gVerbose > 5) {
        unsigned nprt = (ibis::gVerbose < 30 ? (1 << ibis::gVerbose) : dim[1]);
        if (nprt > dim[1])
            nprt = dim[1];
        ibis::util::logger lg;
        lg() << "DEBUG -- bylt[" << col->partition()->name() << '.'
             << col->name() << "]::read(" << fnm
             << ") got nobs = " << dim[1] << ", card = " << dim[2]
             << ", the offsets of the bit vectors are\n";
        if (offset64.size() > dim[1]) {
            for (unsigned i = 0; i < nprt; ++ i)
                lg() << offset64[i] << " ";
            if (nprt < dim[1])
                lg() << "... (skipping " << dim[1]-nprt << ") ... ";
            lg() << offset64[dim[1]];
        }
        else {
            for (unsigned i = 0; i < nprt; ++ i)
                lg() << offset32[i] << " ";
            if (nprt < dim[1])
                lg() << "... (skipping " << dim[1]-nprt << ") ... ";
            lg() << offset32[dim[1]];
        }
    }
#endif

    initBitmaps(fdes);

    // reading the coarse bins
    if (header[6] == 8) {
        ierr = UnixSeek(fdes, offset64.back(), SEEK_SET);
        if (ierr != offset64.back()) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- bylt[" << col->partition()->name() << '.'
                << col->name() << "]::read(" << fnm << ") failed to seek to "
                << offset64.back() << ", ierr = " << ierr;
            return -4;
        }
    }
    else {
        ierr = UnixSeek(fdes, offset32.back(), SEEK_SET);
        if (ierr != offset32.back()) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- bylt[" << col->partition()->name() << '.'
                << col->name() << "]::read(" << fnm << ") failed to seek to "
                << offset32.back() << ", ierr = " << ierr;
            return -4;
        }
    }

    uint32_t nc;
    ierr = UnixRead(fdes, &nc, sizeof(nc));
    if (ierr < static_cast<off_t>(sizeof(nc))) {
        LOGGER(ibis::gVerbose > 0)
            << "Warning -- bylt[" << col->partition()->name() << '.'
            << col->name() << "]::read(" << fnm
            << ") failed to read a 4-byte ineteger, ierr = " << ierr;
        return -5;
    }
    if (header[6] == 8) {
        begin = offset64.back() + sizeof(nc);
        end = begin + sizeof(uint32_t)*(nc+1);
        if (ierr > 0 && nc > 0) {
            array_t<uint32_t> tmp(fdes, begin, end);
            cbounds.swap(tmp);
        }
        begin = end;
        end += sizeof(int64_t) * (nc+1);
        if (cbounds.size() == nc+1) {
            array_t<int64_t> tmp(fdes, begin, end);
            coffset64.swap(tmp);
        }
        coffset32.clear();
    }
    else {
        begin = offset32.back() + sizeof(nc);
        end = begin + sizeof(uint32_t)*(nc+1);
        if (ierr > 0 && nc > 0) {
            array_t<uint32_t> tmp(fdes, begin, end);
            cbounds.swap(tmp);
        }
        begin = end;
        end += sizeof(int32_t) * (nc+1);
        if (cbounds.size() == nc+1) {
            array_t<int32_t> tmp(fdes, begin, end);
            coffset32.swap(tmp);
        }
        coffset64.clear();
    }

    for (unsigned i = 0; i < cbits.size(); ++ i)
        delete cbits[i];
    cbits.resize(nc);
    for (unsigned i = 0; i < nc; ++ i)
        cbits[i] = 0;
    str = 0;
    LOGGER(ibis::gVerbose > 7)
        << "bylt[" << col->partition()->name() << '.' << col->name()
        << "]::read(" << fnm << ") finished reading the header";
    return 0;
} // ibis::bylt::read

/// Reading information about the coarse bins.  To be used after calling
/// ibis::relic::read, which happens in the constructor.
int ibis::bylt::readCoarse(const char* fn) {
    std::string fnm;
    indexFileName(fnm, fn);

    int fdes = UnixOpen(fnm.c_str(), OPEN_READONLY);
    if (fdes < 0) return -4;
    IBIS_BLOCK_GUARD(UnixClose, fdes);
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    off_t ierr;
    if (offset64.size() > bits.size()) {
        ierr = UnixSeek(fdes, offset64.back(), SEEK_SET);
        if (ierr != offset64.back()) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- bylt[" << col->partition()->name() << '.'
                << col->name() << "]::readCoarse failed to seek to "
                << offset64.back() << ", ierr = " << ierr;
            return -1;
        }
    }
    else {
        ierr = UnixSeek(fdes, offset32.back(), SEEK_SET);
        if (ierr != offset32.back()) {
            LOGGER(ibis::gVerbose > 0)
                << "Warning -- bylt[" << col->partition()->name() << '.'
                << col->name() << "]::readCoarse failed to seek to "
                << offset32.back() << ", ierr = " << ierr;
            return -2;
        }
    }

    uint32_t nc;
    off_t begin, end;
    ierr = UnixRead(fdes, &nc, sizeof(nc));
    if (ierr < static_cast<off_t>(sizeof(nc))) {
        return -3;
    }
    if (nc == 0) {
        cbits.clear();
        coffset32.clear();
        coffset64.clear();
        return 0;
    }

    if (offset64.size() > bits.size()) {
        begin = offset64.back() + sizeof(nc);
        end = begin + sizeof(uint32_t)*(nc+1);
        {
            array_t<uint32_t> tmp(fdes, begin, end);
            cbounds.swap(tmp);
        }
        begin = end;
        end += sizeof(int64_t) * (nc+1);
        if (cbounds.size() == nc+1) {
            array_t<int64_t> tmp(fdes, begin, end);
            coffset64.swap(tmp);
        }
        coffset32.clear();
    }
    else {
        begin = offset32.back() + sizeof(nc);
        end = begin + sizeof(uint32_t)*(nc+1);
        if (ierr > 0 && nc > 0) {
            array_t<uint32_t> tmp(fdes, begin, end);
            cbounds.swap(tmp);
        }
        begin = end;
        end += sizeof(int32_t) * (nc+1);
        if (cbounds.size() == nc+1) {
            array_t<int32_t> tmp(fdes, begin, end);
            coffset32.swap(tmp);
        }
        coffset64.clear();
    }

    for (unsigned i = 0; i < cbits.size(); ++ i)
        delete cbits[i];
    cbits.resize(nc);
    for (unsigned i = 0; i < nc; ++ i)
        cbits[i] = 0;

    LOGGER(ibis::gVerbose > 6)
        << "bylt[" << col->partition()->name() << '.' << col->name()
        << "]::readCoarse(" << fnm << ") completed reading the header";
    return 0;
} // ibis::bylt::readCoarse

/// Reconstruct an index from a storage object.
int ibis::bylt::read(ibis::fileManager::storage* st) {
    if (st == 0) return -1;
    if (st->begin()[5] != static_cast<char>(BYLT)) return -3;
    clear();

    const char offsetsize = st->begin()[6];
    nrows = *(reinterpret_cast<uint32_t*>(st->begin()+8));
    size_t end;
    size_t pos = 8 + sizeof(uint32_t);
    const uint32_t nobs = *(reinterpret_cast<uint32_t*>(st->begin()+pos));
    pos += sizeof(uint32_t);
    const uint32_t card = *(reinterpret_cast<uint32_t*>(st->begin()+pos));
    pos += sizeof(uint32_t) + 7;
    pos = (pos / 8) * 8;
    end = pos + sizeof(double)*card;
    {
        array_t<double> dbl(st, pos, end);
        vals.swap(dbl);
    }
    int ierr = initOffsets(st, pos + sizeof(double)*card, nobs);
    if (ierr < 0)
        return ierr;

    initBitmaps(st);

    if (!(offsetsize == 8 &&
          str->size() > static_cast<size_t>(offset64.back())) ||
        (offsetsize == 4 &&
         str->size() > static_cast<size_t>(offset32.back()))) 
        return 0;

    const uint32_t nc =
        *(reinterpret_cast<uint32_t*>
          (str->begin() +
           (offsetsize == 8 ? (size_t) offset64.back()
            : (size_t)offset32.back())));

    if (nc == 0
        || (offsetsize == 8 &&
            str->size() < static_cast<uint32_t>(offset32.back()) +
            (sizeof(int64_t)+sizeof(uint32_t))*(nc+1))
        || (offsetsize == 4 &&
            str->size() < static_cast<uint32_t>(offset32.back()) +
            (sizeof(int32_t)+sizeof(uint32_t))*(nc+1)))
        return 0;

    size_t start;
    if (offsetsize == 8)
        start = offset64.back() + 4;
    else
        start = offset32.back() + 4;
    array_t<uint32_t> btmp(str, start, nc+1);
    cbounds.swap(btmp);

    start += sizeof(uint32_t)*(nc+1);
    end = start + offsetsize * (nc+1);
    if (offsetsize == 8) {
        array_t<int64_t> otmp(str, start, end);
        coffset64.swap(otmp);
        coffset32.clear();
    }
    else {
        array_t<int32_t> otmp(str, start, end);
        coffset32.swap(otmp);
        coffset64.clear();
    }

    for (unsigned j = 0; j < cbits.size(); ++ j)
        delete cbits[j];
    cbits.resize(nc);
    for (unsigned i = 0; i < nc; ++ i)
        cbits[i] = 0;
    return 0;
} // ibis::bylt::read

/// Clear the existing content.
void ibis::bylt::clear() {
    const unsigned nc = cbits.size();
    for (unsigned i = 0; i < nc; ++ i)
        delete cbits[i];

    cbits.clear();
    cbounds.clear();
    coffset32.clear();
    coffset64.clear();
    ibis::relic::clear();
} // ibis::bylt::clear

/// Print function.
void ibis::bylt::print(std::ostream& out) const {
    if (vals.size() != bits.size() || bits.empty())
        return;

    const uint32_t nc = cbits.size();
    out << "the range-equality encoded bitmap index for "
        << col->partition()->name() << '.'
        << col->name() << " contains " << nc+1 << " coarse bin"
        << (nc>1 ? "s" : "") << " and " << bits.size()
        << " fine bit vectors for " << nrows << " objects\n";
    uint32_t nprt = (ibis::gVerbose < 30 ? 1 << ibis::gVerbose : bits.size());
    uint32_t omitted = 0;
    uint32_t end;
    if (cbounds.size() == nc+1 && nc > 0) { // has coarse bins
        for (unsigned j = 0; j < nc; ++ j) {
            out << "Coarse bin " << j << ", [" << cbounds[j] << ", "
                << cbounds[j+1] << ")";
            if (cbits[j])
                out << "\t{ <" << cbounds[j+1] << "\t" << cbits[j]->cnt()
                    << "\t" << cbits[j]->bytes() << "}\n";
            else
                out << "\n";
            end = (cbounds[j+1] <= cbounds[j]+nprt ?
                   cbounds[j+1] : cbounds[j]+nprt);
            for (unsigned i = cbounds[j]; i < end; ++ i) {
                if (bits[i]) {
                    out << "\t" << i << ":\t";
                    out.precision(12);
                    out << vals[i] << "\t" << bits[i]->cnt()
                        << "\t" << bits[i]->bytes() << "\n";
                }
                else {
                    ++ omitted;
                }
            }
            if (cbounds[j+1] > end && nprt > 0) {
                out << "\t...\n";
                omitted += (cbounds[j+1] - end);
            }
        }
        out << "Coarse bin " << nc << ", [" << cbounds.back() << ", "
            << bits.size() << ")\n";
        end = (bits.size() <= cbounds.back()+nprt ?
               bits.size() : cbounds.back()+nprt);
        for (unsigned i = cbounds.back(); i < end; ++ i) {
            if (bits[i]) {
                out << "\t" << i << ":\t";
                out.precision(12);
                out << vals[i] << "\t" << bits[i]->cnt()
                    << "\t" << bits[i]->bytes() << "\n";
            }
            else {
                ++ omitted;
            }
        }
        if (bits.size() > end && nprt > 0) {
            out << "\t...\n";
            omitted += (bits.size() - end);
        }
        if (nprt > 0 && omitted > 0)
            out << "\tfine level bitmaps omitted: " << omitted << "\n";
    }
    else { // no coarse bins
        const uint32_t nobs = bits.size();
        uint32_t skip = 0;
        if (ibis::gVerbose <= 0) {
            skip = nobs;
        }
        else if ((nobs >> 2*ibis::gVerbose) > 2) {
            skip = static_cast<uint32_t>
                (ibis::util::compactValue
                 (static_cast<double>(nobs >> (1+2*ibis::gVerbose)),
                  static_cast<double>(nobs >> (2*ibis::gVerbose))));
            if (skip < 1)
                skip = 1;
        }
        if (skip < 1)
            skip = 1;
        if (skip > 1) {
            out << " (printing 1 out of every " << skip << ")\n";
        }

        for (uint32_t i=0; i<nobs; i += skip) {
            if (bits[i]) {
                out << i << ":\t";
                out.precision(12);
                out << vals[i] << "\t" << bits[i]->cnt()
                    << "\t" << bits[i]->bytes() << "\n";
            }
            else if (ibis::gVerbose > 7) {
                out << i << ":\t";
                out.precision(12);
                out << vals[i] << " ... \n";
            }
        }
        if ((nobs-1) % skip) {
            if (bits[nobs-1]) {
                out << nobs-1 << ":\t";
                out << vals[nobs-1] << "\t" << bits[nobs-1]->cnt()
                    << "\t" << bits[nobs-1]->bytes() << "\n";
            }
            else if (ibis::gVerbose > 7) {
                out << nobs-1 << ":\t";
                out << vals[nobs-1] << " ... \n";
            }
        }
    }
    out << "\n";
} // ibis::bylt::print

/// Estiamte the size of the index in a file.
size_t ibis::bylt::getSerialSize() const throw() {
    size_t res = 40 + 8 * (bits.size() + vals.size())
        + 12 * (cbits.size());
    for (unsigned j = 0; j < bits.size(); ++ j)
        if (bits[j] != 0)
            res += bits[j]->getSerialSize();
    for (unsigned j = 0; j < cbits.size(); ++ j)
        if (cbits[j] != 0)
            res += cbits[j]->getSerialSize();
    return res;
} // ibis::bylt::getSerialSize
