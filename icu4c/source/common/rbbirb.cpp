// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
//
//  file:  rbbirb.cpp
//
//  Copyright (C) 2002-2011, International Business Machines Corporation and others.
//  All Rights Reserved.
//
//  This file contains the RBBIRuleBuilder class implementation.  This is the main class for
//    building (compiling) break rules into the tables required by the runtime
//    RBBI engine.
//

#include "unicode/utypes.h"

#if !UCONFIG_NO_BREAK_ITERATION

#include "unicode/brkiter.h"
#include "unicode/rbbi.h"
#include "unicode/ubrk.h"
#include "unicode/unistr.h"
#include "unicode/uniset.h"
#include "unicode/uchar.h"
#include "unicode/uchriter.h"
#include "unicode/parsepos.h"
#include "unicode/parseerr.h"

#include "cmemory.h"
#include "cstring.h"
#include "rbbirb.h"
#include "rbbinode.h"
#include "rbbiscan.h"
#include "rbbisetb.h"
#include "rbbitblb.h"
#include "rbbidata.h"
#include "uassert.h"


U_NAMESPACE_BEGIN


//----------------------------------------------------------------------------------------
//
//  Constructor.
//
//----------------------------------------------------------------------------------------
RBBIRuleBuilder::RBBIRuleBuilder(const UnicodeString   &rules,
                                       UParseError     *parseErr,
                                       UErrorCode      &status)
 : fRules(rules), fStrippedRules(rules)
{
    fStatus = &status; // status is checked below
    fParseError = parseErr;
    fDebugEnv   = NULL;
#ifdef RBBI_DEBUG
    fDebugEnv   = getenv("U_RBBIDEBUG");
#endif


    fForwardTree        = NULL;
    fReverseTree        = NULL;
    fSafeFwdTree        = NULL;
    fSafeRevTree        = NULL;
    fDefaultTree        = &fForwardTree;
    fForwardTable       = NULL;
    fRuleStatusVals     = NULL;
    fChainRules         = FALSE;
    fLBCMNoChain        = FALSE;
    fLookAheadHardBreak = FALSE;
    fUSetNodes          = NULL;
    fScanner            = NULL;
    fSetBuilder         = NULL;
    if (parseErr) {
        uprv_memset(parseErr, 0, sizeof(UParseError));
    }

    if (U_FAILURE(status)) {
        return;
    }

    fUSetNodes          = new UVector(status); // bcos status gets overwritten here
    fRuleStatusVals     = new UVector(status);
    fScanner            = new RBBIRuleScanner(this);
    fSetBuilder         = new RBBISetBuilder(this);
    if (U_FAILURE(status)) {
        return;
    }
    if(fSetBuilder == 0 || fScanner == 0 || fUSetNodes == 0 || fRuleStatusVals == 0) {
        status = U_MEMORY_ALLOCATION_ERROR;
    }
}



//----------------------------------------------------------------------------------------
//
//  Destructor
//
//----------------------------------------------------------------------------------------
RBBIRuleBuilder::~RBBIRuleBuilder() {

    int        i;
    for (i=0; ; i++) {
        RBBINode *n = (RBBINode *)fUSetNodes->elementAt(i);
        if (n==NULL) {
            break;
        }
        delete n;
    }

    delete fUSetNodes;
    delete fSetBuilder;
    delete fForwardTable;
    delete fForwardTree;
    delete fReverseTree;
    delete fSafeFwdTree;
    delete fSafeRevTree;
    delete fScanner;
    delete fRuleStatusVals;
}





//----------------------------------------------------------------------------------------
//
//   flattenData() -  Collect up the compiled RBBI rule data and put it into
//                    the format for saving in ICU data files,
//                    which is also the format needed by the RBBI runtime engine.
//
//----------------------------------------------------------------------------------------
static int32_t align8(int32_t i) {return (i+7) & 0xfffffff8;}

RBBIDataHeader *RBBIRuleBuilder::flattenData() {
    int32_t    i;

    if (U_FAILURE(*fStatus)) {
        return NULL;
    }

    // Remove whitespace from the rules to make it smaller.
    // The rule parser has already removed comments.
    fStrippedRules = fScanner->stripRules(fStrippedRules);

    // Calculate the size of each section in the data.
    //   Sizes here are padded up to a multiple of 8 for better memory alignment.
    //   Sections sizes actually stored in the header are for the actual data
    //     without the padding.
    //
    int32_t headerSize        = align8(sizeof(RBBIDataHeader));
    int32_t forwardTableSize  = align8(fForwardTable->getTableSize());
    int32_t reverseTableSize  = align8(fForwardTable->getSafeTableSize());
    int32_t trieSize          = align8(fSetBuilder->getTrieSize());
    int32_t statusTableSize   = align8(fRuleStatusVals->size() * sizeof(int32_t));
    int32_t rulesSize         = align8((fStrippedRules.length()+1) * sizeof(UChar));

    int32_t         totalSize = headerSize
                                + forwardTableSize
                                + reverseTableSize
                                + statusTableSize + trieSize + rulesSize;

    RBBIDataHeader  *data     = (RBBIDataHeader *)uprv_malloc(totalSize);
    if (data == NULL) {
        *fStatus = U_MEMORY_ALLOCATION_ERROR;
        return NULL;
    }
    uprv_memset(data, 0, totalSize);


    data->fMagic            = 0xb1a0;
    data->fFormatVersion[0] = RBBI_DATA_FORMAT_VERSION[0];
    data->fFormatVersion[1] = RBBI_DATA_FORMAT_VERSION[1];
    data->fFormatVersion[2] = RBBI_DATA_FORMAT_VERSION[2];
    data->fFormatVersion[3] = RBBI_DATA_FORMAT_VERSION[3];
    data->fLength           = totalSize;
    data->fCatCount         = fSetBuilder->getNumCharCategories();

    data->fFTable        = headerSize;
    data->fFTableLen     = forwardTableSize;

    data->fRTable        = data->fFTable  + data->fFTableLen;
    data->fRTableLen     = reverseTableSize;

    data->fTrie          = data->fRTable + data->fRTableLen;
    data->fTrieLen       = fSetBuilder->getTrieSize();
    data->fStatusTable   = data->fTrie    + trieSize;
    data->fStatusTableLen= statusTableSize;
    data->fRuleSource    = data->fStatusTable + statusTableSize;
    data->fRuleSourceLen = fStrippedRules.length() * sizeof(UChar);

    uprv_memset(data->fReserved, 0, sizeof(data->fReserved));

    fForwardTable->exportTable((uint8_t *)data + data->fFTable);
    fForwardTable->exportSafeTable((uint8_t *)data + data->fRTable);
    fSetBuilder->serializeTrie ((uint8_t *)data + data->fTrie);

    int32_t *ruleStatusTable = (int32_t *)((uint8_t *)data + data->fStatusTable);
    for (i=0; i<fRuleStatusVals->size(); i++) {
        ruleStatusTable[i] = fRuleStatusVals->elementAti(i);
    }

    fStrippedRules.extract((UChar *)((uint8_t *)data+data->fRuleSource), rulesSize/2+1, *fStatus);

    return data;
}


//----------------------------------------------------------------------------------------
//
//  createRuleBasedBreakIterator    construct from source rules that are passed in
//                                  in a UnicodeString
//
//----------------------------------------------------------------------------------------
BreakIterator *
RBBIRuleBuilder::createRuleBasedBreakIterator( const UnicodeString    &rules,
                                    UParseError      *parseError,
                                    UErrorCode       &status)
{
    //
    // Read the input rules, generate a parse tree, symbol table,
    // and list of all Unicode Sets referenced by the rules.
    //
    RBBIRuleBuilder  builder(rules, parseError, status);
    if (U_FAILURE(status)) { // status checked here bcos build below doesn't
        return NULL;
    }

    RBBIDataHeader *data = builder.build(status);

    if (U_FAILURE(status)) {
        return nullptr;
    }

    //
    //  Create a break iterator from the compiled rules.
    //     (Identical to creation from stored pre-compiled rules)
    //
    // status is checked after init in construction.
    RuleBasedBreakIterator *This = new RuleBasedBreakIterator(data, status);
    if (U_FAILURE(status)) {
        delete This;
        This = NULL;
    } 
    else if(This == NULL) { // test for NULL
        status = U_MEMORY_ALLOCATION_ERROR;
    }
    return This;
}

RBBIDataHeader *RBBIRuleBuilder::build(UErrorCode &status) {
    if (U_FAILURE(status)) {
        return nullptr;
    }

    fScanner->parse();
    if (U_FAILURE(status)) {
        return nullptr;
    }

    //
    // UnicodeSet processing.
    //    Munge the Unicode Sets to create a set of character categories.
    //    Generate the mapping tables (TRIE) from input code points to
    //    the character categories.
    //
    fSetBuilder->buildRanges();

    //
    //   Generate the DFA state transition table.
    //
    fForwardTable = new RBBITableBuilder(this, &fForwardTree, status);
    if (fForwardTable == nullptr) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return nullptr;
    }

    fForwardTable->buildForwardTable();
    optimizeTables();
    fForwardTable->buildSafeReverseTable(status);


#ifdef RBBI_DEBUG
    if (fDebugEnv && uprv_strstr(fDebugEnv, "states")) {
        fForwardTable->printStates();
        fForwardTable->printRuleStatusTable();
        fForwardTable->printReverseTable();
    }
#endif

    fSetBuilder->buildTrie();

    //
    //   Package up the compiled data into a memory image
    //      in the run-time format.
    //
    RBBIDataHeader *data = flattenData(); // returns NULL if error
    if (U_FAILURE(status)) {
        return nullptr;
    }
    return data;
}

void RBBIRuleBuilder::optimizeTables() {
    bool didSomething;
    do {
        didSomething = false;

        // Begin looking for duplicates with char class 3.
        // Classes 0, 1 and 2 are special; they are unused, {bof} and {eof} respectively,
        // and should not have other categories merged into them.
        IntPair duplPair = {3, 0};
        while (fForwardTable->findDuplCharClassFrom(&duplPair)) {
            fSetBuilder->mergeCategories(duplPair);
            fForwardTable->removeColumn(duplPair.second);
            didSomething = true;
        }

        while (fForwardTable->removeDuplicateStates() > 0) {
            didSomething = true;
        }
    } while (didSomething);
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_BREAK_ITERATION */
