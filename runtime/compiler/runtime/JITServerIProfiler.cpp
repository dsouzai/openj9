/*******************************************************************************
 * Copyright IBM Corp. and others 2018
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "bcnames.h"
#include "control/CompilationRuntime.hpp"
#include "control/JITServerCompilationThread.hpp"
#include "env/j9methodServer.hpp"
#include "env/StackMemoryRegion.hpp"
#include "infra/CriticalSection.hpp" // for OMR::CriticalSection
#include "ilgen/J9ByteCode.hpp"
#include "ilgen/J9ByteCodeIterator.hpp"
#include "net/ClientStream.hpp"
#include "net/ServerStream.hpp"
#include "runtime/JITClientSession.hpp"
#include "runtime/JITServerIProfiler.hpp"

JITServerIProfiler *JITServerIProfiler::allocate(J9JITConfig *jitConfig)
{
    // The global persistent allocator will be used to allocate the singleton JITServerIProfiler object.
    // All other allocations for data structures used by JITServerIProfiler will be done using
    // the per-client persistent allocator.
    TR_IProfiler::setAllocator(&TR::Compiler->persistentAllocator());
    JITServerIProfiler *profiler = new JITServerIProfiler(jitConfig);
    return profiler;
}

JITServerIProfiler::JITServerIProfiler(J9JITConfig *jitConfig)
    : TR_IProfiler(jitConfig)
    , _statsIProfilerInfoFromCache(0)
    , _statsIProfilerInfoMsgToClient(0)
    , _statsIProfilerInfoReqNotCacheable(0)
    , _statsIProfilerInfoIsEmpty(0)
    , _statsIProfilerInfoCachingFailures(0)
{
    _useCaching = feGetEnv("TR_DisableIPCaching") ? false : true;
}

/**
 * @brief Get a summary of the fanin profile info for the indicated method.
 *        The summary is allocated with "heap" memory to be cached in a resolvedMethod.
 *        There are two possible sources for the fanin profile information:
 *        (1) Fresh information sent by the client
 *        (2) Fanin info stored in the JITServer shared profile cache
 *        The server will pick the sources with a higher "quality", where the
 *        quality is given by heuristics that look at the number of samples.
 *
 * @param method The method for which we want to obtain fanin information. This is a "callee" method.
 * @param clientFaninData Fanin information sent by the client
 * @param fe Frontend
 * @param trMemory Pointer to TR_memory onject used for "heap" allocations
 * @return Pointer to "heap" allocated TR_FaninSummaryInfo struct containing
 */
TR_FaninSummaryInfo *JITServerIProfiler::cacheFaninDataForMethod(TR_OpaqueMethodBlock *method,
    const std::string &clientFaninData, TR_FrontEnd *fe, TR_Memory *trMemory)
{
    const TR_ContiguousIPMethodHashTableEntry *serialEntry
        = !clientFaninData.empty() ? (TR_ContiguousIPMethodHashTableEntry *)&clientFaninData[0] : NULL;

    auto compInfoPT = (TR::CompilationInfoPerThreadRemote *)(((TR_J9VMBase *)fe)->_compInfoPT);
    ClientSessionData *clientSession = compInfoPT->getClientData();
    TR_ASSERT_FATAL(!serialEntry || method == serialEntry->_method,
        "method missmatch when receiving fanin info"); // TODO: make this a normal assert

    int sharedProfileQuality = 0;
    if (clientSession->useSharedProfileCache()) {
        // Retrieve the shared fanin profile info and compare it to the info we got from the client
        ProfiledMethodEntry *sharedProfile = clientSession->getSharedProfileCacheForMethod((J9Method *)method);
        FaninProfileSummary clientProfileSummary(serialEntry ? serialEntry->_totalSamples : 0);
        FaninProfileSummary sharedProfileSummary
            = sharedProfile ? sharedProfile->getFaninProfileSummary() : FaninProfileSummary(0);
        sharedProfileQuality
            = JITServerSharedProfileCache::compareFaninProfiles(sharedProfileSummary, clientProfileSummary);
        if (TR::Options::getVerboseOption(TR_VerboseJITServerSharedProfileDetails)) {
            TR_VerboseLog::writeLineLocked(TR_Vlog_JITServer,
                "Compare fanin data for method %p: ClientSamples=%" OMR_PRIu64 " ServerSamples=%" OMR_PRIu64
                " while compiling %s",
                method, clientProfileSummary._numSamples, sharedProfileSummary._numSamples,
                TR::comp() ? TR::comp()->signature() : NULL);
        }
    }
    if (sharedProfileQuality > 0) {
        // The quality of the shared fanin profile is better than the quality of the fanin info sent by client.
        // Load the fanin profile from the shared repository.
        TR_FaninSummaryInfo *result = clientSession->loadFaninDataFromSharedProfileCache(method, trMemory);
        if (!result) // failed, so use client data if available
        {
            return deserializeFaninMethodEntry(serialEntry, trMemory);
        }
    } else if (sharedProfileQuality < 0) {
        // The quality of the fanin info sent by the client is better. Use that and maybe store it in the shared repo.
        TR_ASSERT(serialEntry, "serialEntry cannot be NULL if we decided that fanin info sent by the client is better");
        TR_FaninSummaryInfo *result = deserializeFaninMethodEntry(serialEntry, trMemory);

        if (clientSession->useSharedProfileCache()) {
            clientSession->storeFaninDataInSharedProfileCache(method, serialEntry);
        }
        return result;
    } else // It's a wash between the two sources of fanin info. Use the info from the client.
    {
        return deserializeFaninMethodEntry(serialEntry, trMemory);
    }
    return NULL;
}

/**
 * @brief Deserialize the fanin profile info sent by the client and allocate a
 *        TR_FaninSummaryInfo entry using "heap" memory
 *
 * @param serialEntry The fanin info sent by the client in serialized format
 * @param trMemory TR_Memory object used for "heap" allocations
 * @return TR_FaninSummaryInfo*
 */
TR_FaninSummaryInfo *JITServerIProfiler::deserializeFaninMethodEntry(
    const TR_ContiguousIPMethodHashTableEntry *serialEntry, TR_Memory *trMemory)
{
    if (!serialEntry)
        return NULL;
    // Caching is done inside TR_ResolvedJ9JITServerMethod so we need to use heap memory.
    TR_FaninSummaryInfo *entry = (TR_FaninSummaryInfo *)trMemory->allocateHeapMemory(sizeof(TR_FaninSummaryInfo));
    if (entry) {
        entry->_totalSamples = serialEntry->_totalSamples;
        entry->_samplesOther = serialEntry->_otherBucket.getWeight();
        entry->_numCallers = serialEntry->_callerCount;
    }
    return entry;
}

/**
 * @brief Serialize the information from an entry in the fanin hashtable
 *
 * @param entry The fanin hashtable entry that needs to be serialized.
 * @param serialEntry OUTPUT. The serialized version of the fanin hashtable entry.
 */
std::string TR_ContiguousIPMethodHashTableEntry::serialize(const TR_IPMethodHashTableEntry *faninEntry)
{
    if (!faninEntry)
        return std::string();
    std::string entryStr(sizeof(TR_ContiguousIPMethodHashTableEntry), 0);
    TR_ContiguousIPMethodHashTableEntry *serialEntry
        = reinterpret_cast<TR_ContiguousIPMethodHashTableEntry *>(&entryStr[0]);

    uint64_t totalSamples = faninEntry->_otherBucket.getWeight();

    serialEntry->_method = faninEntry->_method;
    serialEntry->_otherBucket = faninEntry->_otherBucket;

    size_t callerIdx = 0;
    for (const TR_IPMethodData *caller = &faninEntry->_caller; caller; caller = caller->next) {
        if (callerIdx >= TR_IPMethodHashTableEntry::MAX_IPMETHOD_CALLERS)
            break;
        auto &serialCaller = serialEntry->_callers[callerIdx];
        serialCaller._method = caller->getMethod();
        serialCaller._pcIndex = caller->getPCIndex();
        serialCaller._weight = caller->getWeight();
        totalSamples += caller->getWeight();
        callerIdx++;
    }
    serialEntry->_callerCount = callerIdx;
    serialEntry->_totalSamples = totalSamples;
    return entryStr;
}

TR_IPBytecodeHashTableEntry *JITServerIProfiler::ipBytecodeHashTableEntryFactory(TR_IPBCDataStorageHeader *storage,
    uintptr_t pc, TR_Memory *mem, TR_AllocationKind allocKind)
{
    TR_IPBytecodeHashTableEntry *entry = NULL;
    uint32_t entryType = storage->ID;
    if (entryType == TR_IPBCD_FOUR_BYTES) {
        TR_ASSERT(storage->left == 0 || storage->left == sizeof(TR_IPBCDataFourBytesStorage),
            "Wrong size for serialized IP entry %u != %u", storage->left, sizeof(TR_IPBCDataFourBytesStorage));
        entry = (TR_IPBytecodeHashTableEntry *)mem->allocateMemory(sizeof(TR_IPBCDataFourBytes), allocKind,
            TR_Memory::IPBCDataFourBytes);
        entry = new (entry) TR_IPBCDataFourBytes(pc);
    } else if (entryType == TR_IPBCD_CALL_GRAPH) {
        TR_ASSERT(storage->left == 0 || storage->left == sizeof(TR_IPBCDataCallGraphStorage),
            "Wrong size for serialized IP entry %u != %u", storage->left, sizeof(TR_IPBCDataCallGraphStorage));
        entry = (TR_IPBytecodeHashTableEntry *)mem->allocateMemory(sizeof(TR_IPBCDataCallGraph), allocKind,
            TR_Memory::IPBCDataCallGraph);
        entry = new (entry) TR_IPBCDataCallGraph(pc);
    } else if (entryType == TR_IPBCD_DIRECT_CALL) {
        TR_ASSERT(storage->left == 0 || storage->left == sizeof(TR_IPBCDataDirectCallStorage),
            "Wrong size for serialized IP entry %u != %u", storage->left, sizeof(TR_IPBCDataDirectCallStorage));
        entry = (TR_IPBytecodeHashTableEntry *)mem->allocateMemory(sizeof(TR_IPBCDataDirectCall), allocKind,
            TR_Memory::IPBCDataDirectCall);
        entry = new (entry) TR_IPBCDataDirectCall(pc);
    } else if (entryType == TR_IPBCD_EIGHT_WORDS) {
        TR_ASSERT(storage->left == 0 || storage->left == sizeof(TR_IPBCDataEightWordsStorage),
            "Wrong size for serialized IP entry %u != %u", storage->left, sizeof(TR_IPBCDataEightWordsStorage));
        entry = (TR_IPBytecodeHashTableEntry *)mem->allocateMemory(sizeof(TR_IPBCDataEightWords), allocKind,
            TR_Memory::IPBCDataEightWords);
        entry = new (entry) TR_IPBCDataEightWords(pc);
    } else {
        TR_ASSERT(false, "Unknown entry type %u", entryType);
    }
    return entry;
}

TR_IPMethodHashTableEntry *JITServerIProfiler::searchForMethodSample(TR_OpaqueMethodBlock *omb, int32_t bucket)
{
    TR_ASSERT_FATAL(false, "Unexpected call to searchForMethodSample made by the server");
    return NULL;
}

// This method is used to search only the hash table
TR_IPBytecodeHashTableEntry *JITServerIProfiler::profilingSample(uintptr_t pc, uintptr_t data, bool addIt,
    bool isRIData, uint32_t freq)
{
    if (addIt)
        return NULL; // Server should not create any samples

    TR_ASSERT_FATAL(false, "profilingSample(pc...) should not be called on JITServer");
    return NULL;
}

/**
 * @brief Deserialize bytecode profile data sent by client into a vector of entries (stack allocated)
 *
 * @param method The j9method for which we collect profiling data
 * @param ipdata Serialized bytecode profile data
 * @param ipEntries OUPUT. Vector which will contain the deserialized profiling entries. Uses stack memory.
 * @param trMemory TR_Memory object used to allocate memory for deserialized entries
 * @param cgEntriesOnly If true, only call graph entries will be deserialized.
 */
void JITServerIProfiler::deserializeIProfilerData(J9Method *method, const std::string &ipdata,
    Vector<TR_IPBytecodeHashTableEntry *> &ipEntries, TR_Memory *trMemory, bool cgEntriesOnly)
{
    if (ipdata.empty())
        return;
    const char *bufferPtr = &ipdata[0];
    TR_IPBCDataStorageHeader *storage = NULL;
    uintptr_t methodStart
        = TR::Compiler->mtd.bytecodeStart((TR_OpaqueMethodBlock *)method); // TODO: avoid this costly function
    do {
        storage = (TR_IPBCDataStorageHeader *)bufferPtr;
        if (!cgEntriesOnly || storage->ID == TR_IPBCD_CALL_GRAPH) {
            // Allocate a new entry with stack memory
            TR_IPBytecodeHashTableEntry *entry
                = ipBytecodeHashTableEntryFactory(storage, methodStart + storage->pc, trMemory, stackAlloc);
            if (entry) {
                // Fill the new entry with data sent by client
                entry->deserialize(storage);
                ipEntries.push_back(entry);
            }
        }
        // Advance to the next entry
        bufferPtr += storage->left;
    } while (storage->left != 0);
}

/**
 * @brief Walk the serialized data sent by client and add new entries to our internal profile hashtable
 *        Entries are added one by one and for each entry we need to acquire the ROMMapMonitor
 *        TODO: consider writing the entries in a vector of pointers and then write them in bulk.
 *
 * @param method: The j9method for which we store profiling informatio
 * @param ipdata: The profiling data received from the client in serialized format
 * @param usePersistentCache: If true, we store IP data into per-client cache; else, we store into the per-compilation
 * cache
 * @param clientSessionData: The client session data
 * @param compInfoPT: The compilation info for the current thread
 * @param isCompiled: If true, the method is compiled and the profiling information is stable
 * @param comp: The compilation object
 * @return true if all the data was cached; if no data or only partial data was cached, return false
 * @note This function acquires/releases ROMMapMonitor
 */
bool JITServerIProfiler::cacheProfilingDataForMethod(TR_OpaqueMethodBlock *method, const std::string &ipdata,
    bool usePersistentCache, ClientSessionData *clientSessionData, TR::CompilationInfoPerThreadRemote *compInfoPT,
    bool isCompiled, TR::Compilation *comp)
{
    // TODO: this should also return the desired entry so that we don't have to search again
    bool cachingFailed = false;
    // Walk the data sent by the client and add new entries to our internal hashtable
    const char *bufferPtr = &ipdata[0];
    TR_IPBCDataStorageHeader *storage = NULL;
    uintptr_t methodStart = TR::Compiler->mtd.bytecodeStart(method); // TODO: avoid this costly function
    uint32_t methodSize = 0; // Will be computed later
    do {
        storage = (TR_IPBCDataStorageHeader *)bufferPtr;
        // Allocate a new entry of the specified type
        TR_IPBytecodeHashTableEntry *entry = ipBytecodeHashTableEntryFactory(storage, methodStart + storage->pc,
            comp->trMemory(), usePersistentCache ? persistentAlloc : heapAlloc);
        if (entry) {
            // Fill the new entry with data sent by client
            entry->deserialize(storage);

            // Adjust the bci for interfaces. Interfaces are weird; we may have to add 2 to the bci that is going to be
            // the key
            uint32_t bci = storage->pc;
            if (storage->ID == TR_IPBCD_CALL_GRAPH) {
                U_8 *pc = (U_8 *)entry->getPC();
                if (methodSize == 0)
                    methodSize = TR::Compiler->mtd.bytecodeSize(method);
                TR_ASSERT(bci < methodSize, "Bytecode index can't be higher than the methodSize: bci=%u methodSize=%u",
                    bci, methodSize);
                if (*pc == JBinvokeinterface2) {
                    TR_ASSERT(bci + 2 < methodSize,
                        "Bytecode index can't be higher than the methodSize: bci=%u methodSize=%u", bci, methodSize);
                    if (*(pc + 2) == JBinvokeinterface)
                        bci += 2;
                }
            }
            if (usePersistentCache) {
                if (!clientSessionData->cacheIProfilerInfo(method, bci, entry, isCompiled)) {
                    // If caching failed we must delete the entry allocated with persistent memory
                    _statsIProfilerInfoCachingFailures++;
                    comp->trMemory()->freeMemory(entry, persistentAlloc);
                    cachingFailed = true;
                    // Either we cannot allocate memory or we cannot find the 'method' cached
                    // In both cases we cannot continue
                    break;
                }
            } else // Will use per-compilation cache
            {
                if (!compInfoPT->cacheIProfilerInfo(method, bci, entry)) {
                    // If caching failed delete the entry allocated with heap memory
                    _statsIProfilerInfoCachingFailures++;
                    comp->trMemory()->freeMemory(entry, heapAlloc);
                    cachingFailed = true;
                    break;
                }
            }
        } else {
            // What should we do if we cannot allocate from persistent memory?
            cachingFailed = true;
            break; // no point in trying again. TODO: we may have cached partial data.
        }
        // Advance to the next entry
        bufferPtr += storage->left;
    } while (storage->left != 0);
    return !cachingFailed;
}

// This method is called by the optimizer at JITServer to search
// for profiling information of a particular method and bytecodeIndex.
// First, the local (per-client) cache is searched. If not found, the
// server sends a message to the client requesting the profiling information
// for the entire method. If the shared profile repository is enabled and
// it contains info about the requested method, the quality of the client data
// is compared against the quality of the data in the shared profile repository.
// We pick the best source of the two.
TR_IPBytecodeHashTableEntry *JITServerIProfiler::profilingSample(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex,
    TR::Compilation *comp, uintptr_t data, bool addIt)
{
    if (addIt)
        return NULL; // Server should not create any samples

    auto compInfoPT = (TR::CompilationInfoPerThreadRemote *)(comp->fej9()->_compInfoPT);
    ClientSessionData *clientSession = compInfoPT->getClientData();
    TR_IPBytecodeHashTableEntry *entry = NULL;

    // Check the cache first, if allowed
    //
    if (_useCaching) {
        bool entryFromPerCompilationCache = false;
        // first, check persistent per-client cache
        bool methodInfoPresent = false;
        entry = clientSession->getCachedIProfilerInfo(method, byteCodeIndex, &methodInfoPresent);
        if (!methodInfoPresent) {
            // If no entry in persistent per-client cache, check per-compilation cache
            entry = compInfoPT->getCachedIProfilerInfo(method, byteCodeIndex, &methodInfoPresent);
            entryFromPerCompilationCache = true;
        }
        if (methodInfoPresent) {
            _statsIProfilerInfoFromCache++;
#if defined(DEBUG) || defined(PROD_WITH_ASSUMES)
            // sanity check
            // Ask the client again and see if the two sources of information match
            auto stream = comp->getStream();
            stream->write(JITServer::MessageType::IProfiler_profilingSample, method, byteCodeIndex, _useCaching,
                _useCaching && clientSession->usesProfileCache());
            auto recv = stream->read<std::string, uint64_t, size_t, bool, bool, bool, std::vector<J9Class *>,
                std::vector<JITServerHelpers::ClassInfoTuple> >();
            auto &ipdata = std::get<0>(recv);
            uint64_t numClientSamples = std::get<1>(recv);
            size_t numProfiledBytecodes = std::get<2>(recv);
            bool wholeMethod = std::get<3>(recv); // indicates whether the client sent info for entire method
            bool usePersistentCache
                = std::get<4>(recv); // indicates whether info can be saved in persistent memory, or only in heap memory
            bool isCompiled = std::get<5>(recv);
            auto &uncachedRAMClasses = std::get<6>(recv);
            auto &classInfoTuples = std::get<7>(recv);
            TR_ASSERT(!wholeMethod, "Client should not have sent whole method info");
            uintptr_t methodStart = TR::Compiler->mtd.bytecodeStart(method);
            TR_IPBCDataStorageHeader *clientData = ipdata.empty() ? NULL : (TR_IPBCDataStorageHeader *)ipdata.data();
            bool isMethodBeingCompiled = (method == comp->getMethodBeingCompiled()->getPersistentIdentifier());

            if (!clientData && entry) {
                uint8_t bytecode = *(uint8_t *)(methodStart + byteCodeIndex);
                fprintf(stderr,
                    "Error cached IP data for method %p bcIndex %u bytecode=%x: ipdata is empty but we have a cached "
                    "entry=%p\n",
                    method, byteCodeIndex, bytecode, entry);
            }

            bool isCompiledWhenProfiling = false;
            if (!entryFromPerCompilationCache) {
                auto &j9methodMap = clientSession->getJ9MethodMap();
                auto it = j9methodMap.find((J9Method *)method);
                if (it != j9methodMap.end()) {
                    isCompiledWhenProfiling = it->second._isCompiledWhenProfiling;
                }
            }
            validateCachedIPEntry(entry, clientData, methodStart, isMethodBeingCompiled, method,
                entryFromPerCompilationCache, isCompiledWhenProfiling);
#endif /* defined(DEBUG) || defined(PROD_WITH_ASSUMES) */
            return entry; // could be NULL
        }
    }
    // At this point we could not find any IProfiler information cached per-client or per-compilation.
    // Will ask the client, and, if available, the shared profile cache, and then pick the best source.
    // The best source can be determined based on total number of samples or total number of
    // bytecodes with profiling information, or a combination of both. For now we will use total number
    // of samples.

    // We could inquire about the number of samples in the sharedProfileCache first and send that to the client.
    // The client may choose to send nothing back if it decides the server is not going to pick this data.
    // However, in that case we cannot compare the two sources bytecode by bytecode as a debugging feature.

    // Ask the client.
    auto stream = comp->getStream();
    stream->write(JITServer::MessageType::IProfiler_profilingSample, method, byteCodeIndex, _useCaching,
        _useCaching && clientSession->useSharedProfileCache());
    auto recv = stream->read<std::string, uint64_t, size_t, bool, bool, bool, std::vector<J9Class *>,
        std::vector<JITServerHelpers::ClassInfoTuple> >();
    auto &ipdata = std::get<0>(recv);
    uint64_t numClientSamples = std::get<1>(recv);
    size_t numProfiledBytecodes = std::get<2>(recv);
    bool wholeMethod = std::get<3>(recv); // indicates whether the client sent info for entire method
    bool usePersistentCache
        = std::get<4>(recv); // indicates whether info can be saved in persistent memory, or only in heap memory
    bool isCompiled = std::get<5>(recv);
    auto &uncachedRAMClasses = std::get<6>(recv);
    auto &classInfoTuples = std::get<7>(recv);
    _statsIProfilerInfoMsgToClient++;

    bool doCache = _useCaching && wholeMethod;
    if (!doCache)
        _statsIProfilerInfoReqNotCacheable++;

    if (doCache) {
        // Assuming the profilingInfo for a method is not deleted from the shared profile map,
        // we could keep in the methodInfo a pointer to the entry in the shared profile map.
        // However, it's not clean to have pointers inside other containers.
        // Question: can anybody delete entries from the shared profile map? Since the key it's a method record
        // we can only do that when method records are deleted which may not happen at all. To double check.

        // If the shared profile cache is enabled, we must compare the "quality" of the data in the
        // shared repository to the "quality" of the data sent by the client.
        int sharedProfileQuality = 0;
        if (clientSession->useSharedProfileCache()) {
            BytecodeProfileSummary clientProfileSummary(numClientSamples, numProfiledBytecodes, usePersistentCache);
            BytecodeProfileSummary sharedProfileSummary
                = clientSession->getSharedBytecodeProfileSummary((J9Method *)method);
            sharedProfileQuality
                = JITServerSharedProfileCache::compareBytecodeProfiles(sharedProfileSummary, clientProfileSummary);

            if (TR::Options::getVerboseOption(TR_VerboseJITServerSharedProfileDetails)) {
                TR_VerboseLog::writeLineLocked(TR_Vlog_JITServer,
                    "Sent req for profile data for j9method %p. Client: profiled bytecodes=%zu samples=%" OMR_PRIu64
                    " stable=%d; "
                    "Shared repo: profiled bytecodes=%zu samples=%" OMR_PRIu64 " stable=%d",
                    method, clientProfileSummary._numProfiledBytecodes, clientProfileSummary._numSamples,
                    clientProfileSummary._stable, sharedProfileSummary._numProfiledBytecodes,
                    sharedProfileSummary._numSamples, sharedProfileSummary._stable);
            }

            // Process the extra classInfo that the client might have sent us.
            // TODO: Consider doing this only if the server is likely to store this info into the shared profile cache.
            // On the other hand, if the server sent us something, why not cache that information.
            if (!uncachedRAMClasses.empty())
                JITServerHelpers::cacheRemoteROMClassBatch(clientSession, uncachedRAMClasses, classInfoTuples);
        }
        if (sharedProfileQuality > 0) {
            // Ignore the data sent by the client. Just use the data from the shared profile cache.
            // This may send messages to transform the classRecords into J9Class pointers valid at the client
            bool success = clientSession->loadBytecodeDataFromSharedProfileCache((J9Method *)method, usePersistentCache,
                comp, ipdata);
            if (!success) {
                // We failed to load bytecode profile data shared repository.
                // The best thing we could do is to load the data we have from the client instead.
                bool result = cacheProfilingDataForMethod(method, ipdata, usePersistentCache, clientSession, compInfoPT,
                    isCompiled, comp);
            } else {
                // The data from the shared profile repository has been loaded into the per-client profile cache.
                // If allowed, compare how well the data sent by the client matches the loaded data.
                if (usePersistentCache && numClientSamples > 0) {
                    static bool sharedCacheDebugging = feGetEnv("TR_SharedCacheDebugging") ? true : false;
                    if (sharedCacheDebugging)
                        clientSession->checkProfileDataMatching((J9Method *)method, ipdata);
                }
            }
        } else if (sharedProfileQuality < 0) // The client has better quality for the bytecode profiling data
        {
            // Walk the data sent by the client and add new entries to our internal hashtable.
            // This will acquire the getROMMapMonitor()
            bool result = cacheProfilingDataForMethod(method, ipdata, usePersistentCache, clientSession, compInfoPT,
                isCompiled, comp);
            if (clientSession->useSharedProfileCache()) {
                // Store the information from the client into the shared profile cache.
                // We will store even if the information is not stable.
                // TODO: if the data was not stable at the client, mark the entry as not stable for the shared data.
                // TODO: what if stable shared data exists and now we want to overwrite with unstable data with more
                // samples. This uses getROMMapMonitor() briefly to get to the methodInfo
                clientSession->storeBytecodeProfileInSharedRepository(method, ipdata, numClientSamples,
                    usePersistentCache, comp);
            }
        } else // The quality of the two sources is comparable
        {
            // Walk the data sent by the client and add new entries to our internal hashtable.
            // Do not update the shared profile cache, it's good enough as it is.
            // TODO: what should we do if the "stability" of data from the two sources differs?
            // clientData:stable   sharedData:unstable ==> Mark the sharedData as stable (should we replace it?)
            // clientData:unstable  sharedData:stable  ==> Do nothing
            // There is a special case when the client sent us nothing.
            if (ipdata.empty()) // client didn't send us anything
            {
                _statsIProfilerInfoIsEmpty++;
                // Cache some empty data so that we don't ask again for this method.
                if (usePersistentCache) {
                    // Use the per-client cache
                    if (!clientSession->cacheIProfilerInfo(method, byteCodeIndex, NULL, isCompiled))
                        _statsIProfilerInfoCachingFailures++;
                } else // Use the per-compilation cache
                {
                    if (!compInfoPT->cacheIProfilerInfo(method, byteCodeIndex, NULL))
                        _statsIProfilerInfoCachingFailures++;
                }
                // The shared profile cache will not store such empty profiles.
                return NULL;
            }
            cacheProfilingDataForMethod(method, ipdata, usePersistentCache, clientSession, compInfoPT, isCompiled,
                comp);
            // TODO: replace shared IP data if it is non-stable but the data received from client is stable.
        }
        // Now that all the entries are added to the cache, search the cache
        bool methodInfoPresent = false;
        if (usePersistentCache)
            entry = clientSession->getCachedIProfilerInfo(method, byteCodeIndex, &methodInfoPresent);
        else
            entry = compInfoPT->getCachedIProfilerInfo(method, byteCodeIndex, &methodInfoPresent);
    } else // No caching
    {
        // Did the client send an entire method? Such a waste
        // This could happen if, through options, we disable the caching at the server
        uintptr_t methodStart = TR::Compiler->mtd.bytecodeStart(method);
        if (wholeMethod) {
            // Find my desired pc/bci and return a heap allocated entry for it
            const char *bufferPtr = &ipdata[0];
            TR_IPBCDataStorageHeader *storage = NULL;
            do {
                storage = (TR_IPBCDataStorageHeader *)bufferPtr;
                uint32_t bci = storage->pc;
                entry = ipBytecodeHashTableEntryFactory(storage, methodStart + bci, comp->trMemory(), heapAlloc);
                if (entry)
                    entry->deserialize(storage);

                if (storage->ID == TR_IPBCD_CALL_GRAPH) {
                    U_8 *pc = (U_8 *)entry->getPC();
                    uint32_t methodSize = TR::Compiler->mtd.bytecodeSize(method);
                    TR_ASSERT(bci < methodSize,
                        "Bytecode index can't be higher than the methodSize: bci=%u methodSize=%u", bci, methodSize);
                    if (*pc == JBinvokeinterface2) {
                        TR_ASSERT(bci + 2 < methodSize,
                            "Bytecode index can't be higher than the methodSize: bci=%u methodSize=%u", bci,
                            methodSize);
                        if (*(pc + 2) == JBinvokeinterface)
                            bci += 2;
                    }
                }
                if (bci == byteCodeIndex) {
                    // Found my desired entry
                    break;
                }
                // Move to the next entry
                bufferPtr += storage->left;
            } while (storage->left != 0);
        } else // single IP entry
        {
            TR_IPBCDataStorageHeader *storage = (TR_IPBCDataStorageHeader *)&ipdata[0];
            entry = ipBytecodeHashTableEntryFactory(storage, methodStart + storage->pc, comp->trMemory(), heapAlloc);
            if (entry)
                entry->deserialize(storage);
        }
    }

    return entry;
}

void JITServerIProfiler::printStats()
{
    PORT_ACCESS_FROM_PORT(TR::Compiler->portLib);
    j9tty_printf(PORTLIB, "IProfilerInfoMsgToClient: %6u  IProfilerInfoMsgReplyIsEmpty: %6u\n",
        _statsIProfilerInfoMsgToClient, _statsIProfilerInfoIsEmpty);
    if (_useCaching) {
        j9tty_printf(PORTLIB, "IProfilerInfoNotCacheable:   %6u\n", _statsIProfilerInfoReqNotCacheable);
        j9tty_printf(PORTLIB, "IProfilerInfoCachingFailure: %6u\n", _statsIProfilerInfoCachingFailures);
        j9tty_printf(PORTLIB, "IProfilerInfoFromCache:   %6u\n", _statsIProfilerInfoFromCache);
    }
}

bool JITServerIProfiler::invalidateEntryIfInconsistent(TR_IPBytecodeHashTableEntry *entry)
{
    // Invalid entries are purged early in the compilation for JITServer, we don't need to do anything here.
    return false;
}

void JITServerIProfiler::validateCachedIPEntry(TR_IPBytecodeHashTableEntry *entry, TR_IPBCDataStorageHeader *clientData,
    uintptr_t methodStart, bool isMethodBeingCompiled, TR_OpaqueMethodBlock *method, bool fromPerCompilationCache,
    bool isCompiledWhenProfiling)
{
    if (clientData) // client sent us some data
    {
        if (!entry) {
            static int cnt = 0;
            fprintf(stderr,
                "Error for cached IP data: client sent us something but we have no cached entry. "
                "isMethodBeingCompiled=%d cnt=%d\n",
                isMethodBeingCompiled, ++cnt);
            fprintf(stderr, "\tMethod=%p methodStart=%p bci=%u ID=%u\n", method, (void *)methodStart, clientData->pc,
                clientData->ID);
            // This situation can happen for methods that we are just compiling which
            // can accumulate more samples while they get compiled
        } else // we have data from 2 sources
        {
            // Do the bytecodes match?
            uintptr_t clientPC = clientData->pc + methodStart;
            TR_ASSERT(clientPC == entry->getPC(), "Missmatch for bci: clientPC: (%u + %p)=%p   cachedPC: %p\n",
                clientData->pc, (void *)methodStart, (void *)clientPC, entry->getPC());
            // Do the type of entries match?
            switch (clientData->ID) {
                case TR_IPBCD_FOUR_BYTES: {
                    TR_IPBCDataFourBytes *concreteEntry = entry->asIPBCDataFourBytes();
                    TR_ASSERT(concreteEntry, "Cached IP entry is not of type TR_IPBCDataFourBytes");
                    uint32_t sentData = ((TR_IPBCDataFourBytesStorage *)clientData)->data;
                    uint32_t foundData = (uint32_t)concreteEntry->getData();
                    if (sentData != foundData) {
                        // find the taken/non-taken parts
                        uint16_t takenCached = foundData >> 16;
                        uint16_t notTakenCached = foundData & 0xffff;
                        uint16_t takenSent = sentData >> 16;
                        uint16_t notTakenSent = sentData & 0xffff;
                        // 'Cached' can be different than 'Sent' but only by a small amount
                        uint16_t diff1 = (takenCached > takenSent) ? takenCached - takenSent : takenSent - takenCached;
                        uint16_t diff2 = (notTakenCached > notTakenSent) ? notTakenCached - notTakenSent
                                                                         : notTakenSent - notTakenCached;
                        if (diff1 > 4 || diff2 > 4)
                            fprintf(stderr, "Missmatch for branchInfo sentData=%x, foundData=%x\n", sentData,
                                foundData);
                    }
                } break;
                case TR_IPBCD_EIGHT_WORDS: {
                    TR_IPBCDataEightWords *concreteEntry = entry->asIPBCDataEightWords();
                    TR_ASSERT(concreteEntry, "Cached IP entry is not of type TR_IPBCDataEightWords");
                } break;
                case TR_IPBCD_CALL_GRAPH: {
                    TR_IPBCDataCallGraph *concreteEntry = entry->asIPBCDataCallGraph();
                    TR_ASSERT(concreteEntry, "Cached IP entry is not of type TR_IPBCDataCallGraph");
                    // Check that the dominant target is the same in both cases
                    CallSiteProfileInfo *csInfoServer = concreteEntry->getCGData();
                    CallSiteProfileInfo *csInfoClient = &(((TR_IPBCDataCallGraphStorage *)clientData)->_csInfo);

                    int32_t sumW;
                    int32_t maxW;
                    uintptr_t domClazzClient = csInfoClient->getDominantClass(sumW, maxW);
                    uintptr_t domClazzServer = csInfoServer->getDominantClass(sumW, maxW);

                    if (!fromPerCompilationCache && isCompiledWhenProfiling)
                        TR_ASSERT(domClazzClient == domClazzServer, "Missmatch dominant class client=%p server=%p",
                            (void *)domClazzClient, (void *)domClazzServer);
                } break;
                case TR_IPBCD_DIRECT_CALL: {
                    TR_IPBCDataDirectCall *concreteEntry = entry->asIPBCDataDirectCall();
                    TR_ASSERT(concreteEntry, "Cached IP entry is not of type TR_IPBCDataDirectCall");
                    uint32_t sentCount = ((TR_IPBCDataDirectCallStorage *)clientData)->_callCount;
                    uint32_t foundCount = concreteEntry->getNumSamples();
                    if (sentCount > 0 && foundCount == 0 || sentCount == 0 && foundCount > 0)
                        fprintf(stderr,
                            "Missmatch direct call count: sentCount=%" OMR_PRIu32 " foundCount=%" OMR_PRIu32 "\n",
                            sentCount, foundCount);
                } break;
                default:
                    TR_ASSERT_FATAL(false, "Unknown type of IP info %u", clientData->ID);
            }
        }
    }
}

void JITServerIProfiler::persistIprofileInfo(TR::ResolvedMethodSymbol *methodSymbol, TR_ResolvedMethod *method,
    TR::Compilation *comp)
{
    // resolvedMethodSymbol is only used for debugging on the client, so we don't have to send it
    auto stream = comp->getStream();
    auto compInfoPT = (TR::CompilationInfoPerThreadRemote *)comp->fej9()->_compInfoPT;
    ClientSessionData *clientSessionData = compInfoPT->getClientData();

    if (clientSessionData->getOrCacheVMInfo(stream)->_elgibleForPersistIprofileInfo) {
        auto serverMethod = static_cast<TR_ResolvedJ9JITServerMethod *>(method);
        compInfoPT->cacheResolvedMirrorMethodsPersistIPInfo(serverMethod->getRemoteMirror());
    }
}

JITClientIProfiler *JITClientIProfiler::allocate(J9JITConfig *jitConfig)
{
    // Create a dedicated persistent allocator to be used by the IProfiler
    TR_IProfiler::setAllocator(TR_IProfiler::createPersistentAllocator(jitConfig));
    JITClientIProfiler *profiler = new JITClientIProfiler(jitConfig);
    return profiler;
}

JITClientIProfiler::JITClientIProfiler(J9JITConfig *jitConfig)
    : TR_IProfiler(jitConfig)
{}

/**
 * @brief Given a method, use the bytecodeIterator to walk over its bytecodes and determine
 * the bytecodePCs that have IProfiler information. Create a sorted array with such bytecodePCs.
 *
 * @param pcEntries An array that needs to be populated (in sorted fashion)
 * @param numEntries (output) Returns the number of entries in the array
 * @param bcIterator The bytecodeIterator (must be allocated by the caller)
 * @param method j9method to be scanned
 * @param BCvisit Bit vector used to avoid scanning the same bytecode twice (why?)
 * @param abort (output) set to true if something goes wrong
 *
 * @return Number of bytes needed to store all IProfiler entries of this method
 */
uint32_t JITClientIProfiler::walkILTreeForIProfilingEntries(uintptr_t *pcEntries, uint32_t &numEntries,
    TR_J9ByteCodeIterator *bcIterator, TR_OpaqueMethodBlock *method, TR_BitVector *BCvisit, bool &abort,
    TR::Compilation *comp)
{
    abort = false; // optimistic
    uint32_t bytesFootprint = 0;
    uint32_t methodSize = TR::Compiler->mtd.bytecodeSize(method);
    for (TR_J9ByteCode bc = bcIterator->first(); bc != J9BCunknown; bc = bcIterator->next()) {
        uint32_t bci = bcIterator->bcIndex();
        if (bci < methodSize && !BCvisit->isSet(bci)) {
            uintptr_t thisPC = getSearchPCFromMethodAndBCIndex(method, bci);

            TR_IPBytecodeHashTableEntry *entry = profilingSample(method, bci, comp);
            BCvisit->set(bci);
            if (entry && !invalidateEntryIfInconsistent(entry)) {
                // now check if it can be persisted, lock it, and add it to my list.
                uint32_t canPersist = entry->canBeSerialized(getCompInfo()->getPersistentInfo());
                if (canPersist == IPBC_ENTRY_CAN_PERSIST) {
                    bytesFootprint += entry->getBytesFootprint();
                    // doing insertion sort as we go.
                    // TODO: eliminate this insertion sort and just put them in the order they are found
                    int32_t i;
                    for (i = numEntries; i > 0 && pcEntries[i - 1] > thisPC; i--) {
                        pcEntries[i] = pcEntries[i - 1];
                    }
                    pcEntries[i] = thisPC;
                    numEntries++;
                } else // cannot persist
                {
                    switch (canPersist) {
                        case IPBC_ENTRY_PERSIST_LOCK:
                            // This means the entry is locked by another thread. We are going to abort the
                            // storage of iprofiler information for this method.
                            {
                                // In some corner cases of invoke interface, we may come across the same entry
                                // twice under 2 different bytecodes. In that case, the other entry has been
                                // locked by this thread and is in the list of entries, so don't abort.
                                int32_t i;
                                bool found = false;
                                int32_t a1 = 0, a2 = numEntries - 1;
                                while (a2 >= a1 && !found) {
                                    i = (a1 + a2) / 2;
                                    if (pcEntries[i] == thisPC)
                                        found = true;
                                    else if (pcEntries[i] < thisPC)
                                        a1 = i + 1;
                                    else
                                        a2 = i - 1;
                                }
                                if (!found) {
                                    abort = true;
                                    return 0;
                                }
                            }
                            break;
                        case IPBC_ENTRY_PERSIST_UNLOADED:
                            _STATS_entriesNotPersisted_Unloaded++;
                            break;
                        default:
                            _STATS_entriesNotPersisted_Other++;
                    }
                }
            }
        } else {
            TR_ASSERT(bci < methodSize, "bytecode can't be greater then method size");
        }
    }
    return bytesFootprint;
}

/**
 * @brief Scan the given IProfiler CallGraph entry for j9classes that the server may not have,
 *        and populate the `uncachedClasses` and `classInfos` vectors with desired information.
 *
 * @param cgEntry The IProfiler entry that needs to be scanned
 * @param comp Compiler object for this compilation
 * @param uncachedClasses OUTPUT. Vector of classes that server needs but does not have
 * @param classInfos OUTPUT. Vector of ClassInfos corresponding to elements in `uncachedClasses`
 */
void JITClientIProfiler::gatherUncachedClassesUsedInCGEntry(TR_IPBCDataCallGraph *cgEntry, TR::Compilation *comp,
    std::vector<J9Class *> &uncachedClasses, std::vector<JITServerHelpers::ClassInfoTuple> &classInfos)
{
    auto csInfo = cgEntry->getCGData();
    for (size_t j = 0; j < NUM_CS_SLOTS; ++j) {
        if (auto ramClass = (J9Class *)csInfo->getClazz((int)j)) {
            bool uncached = false;
            {
                OMR::CriticalSection cs(getCompInfo()->getclassesCachedAtServerMonitor());
                uncached = getCompInfo()->getclassesCachedAtServer().insert(ramClass).second;
            }
            // TODO: consider merging all those 3 small critical sections into 1.
            if (uncached) {
                uncachedClasses.push_back(ramClass);
                classInfos.push_back(
                    JITServerHelpers::packRemoteROMClassInfo(ramClass, comp->j9VMThread(), comp->trMemory(), true));
            }
        }
    }
}

/**
 * @brief Code to be executed on the JITClient to serialize IP data of a method
 *
 * @param pcEntries Sorted array with PCs that have IProfiler info
 * @param numEntries Number of entries in the above array; guaranteed > 0
 * @param memChunk Storage area where we serialize entries
 * @param methodStartAddress Start address of the bytecodes for the method
 * @param comp The compilation object
 * @param sharedProfile Boolean indicating whether to collect info about classes the server does not have
 * @param totalSamples OUTPUT. Will contain the total number of profiling samples
 * @param uncachedClasses OUTPUT. Vector of classes that server needs but does not have
 * @param classInfos OUTPUT. Vector of ClassInfos corresponding to elements in `uncachedClasses`
 * @return Total memory space used for serialization
 */
uintptr_t JITClientIProfiler::serializeIProfilerMethodEntries(const uintptr_t *pcEntries, uint32_t numEntries,
    uintptr_t memChunk, uintptr_t methodStartAddress, TR::Compilation *comp, bool sharedProfile, uint64_t &totalSamples,
    std::vector<J9Class *> &uncachedClasses, std::vector<JITServerHelpers::ClassInfoTuple> &classInfos)
{
    uintptr_t crtAddr = memChunk;
    TR_IPBCDataStorageHeader *storage = NULL;
    TR::PersistentInfo *persistentInfo = getCompInfo()->getPersistentInfo();
    totalSamples = 0;
    for (uint32_t i = 0; i < numEntries; ++i) {
        storage = (TR_IPBCDataStorageHeader *)crtAddr;
        TR_IPBytecodeHashTableEntry *entry = profilingSample(pcEntries[i], 0, false);
        totalSamples += entry->getNumSamples();
        entry->serialize(methodStartAddress, storage, persistentInfo);

        // Collect information about the classes used in these entries that are not yet available at the server.
        // These additional classInfos will be sent to the server together with the profiling information.
        if (sharedProfile && (storage->ID == TR_IPBCD_CALL_GRAPH)) {
            TR_IPBCDataCallGraph *cgEntry = entry->asIPBCDataCallGraph();
            gatherUncachedClassesUsedInCGEntry(cgEntry, comp, uncachedClasses, classInfos);
        }

        // optimistically set link to next entry
        uint32_t bytes = entry->getBytesFootprint();
        TR_ASSERT(bytes < 1 << 8,
            "Error storing iprofile information: left child too far away"); // current size of left child
        storage->left = bytes;

        crtAddr += bytes; // advance to the next position
    }
    // Unlink the last entry
    storage->left = 0;

    return crtAddr - memChunk;
}

/**
 * @brief Code to be executed by the JITClient to send all IProfiler info for a method to JITServer
 *
 * @param method J9Method in question
 * @param comp The compilation object
 * @param client Connection to JITServer
 * @param usePersistentCache Passed to the server to indicate whether or not to use the persistent cache
 * @param isCompiled Whether the method is compiled at the moment. Passed to the server
 * @param sharedProfile Boolean indicating whether to collect info about classes the server does not have
 * @return Whether the operation was successful
 */
bool JITClientIProfiler::serializeAndSendIProfileInfoForMethod(TR_OpaqueMethodBlock *method, TR::Compilation *comp,
    JITServer::ClientStream *client, bool usePersistentCache, bool isCompiled, bool sharedProfile)
{
    TR::StackMemoryRegion stackMemoryRegion(*comp->trMemory());
    uint32_t numEntries = 0;
    uint32_t bytesFootprint = 0;
    uintptr_t methodSize = (uintptr_t)TR::Compiler->mtd.bytecodeSize(method);
    uintptr_t methodStart = (uintptr_t)TR::Compiler->mtd.bytecodeStart(method);

    uintptr_t *pcEntries = NULL;
    bool abort = false;
    try {
        uint64_t totalSamples = 0; // Total number of profiling samples for this method
        TR_ResolvedJ9Method resolvedj9method = TR_ResolvedJ9Method(method, comp->fej9(), comp->trMemory());
        TR_J9ByteCodeIterator bci(NULL, &resolvedj9method, static_cast<TR_J9VMBase *>(comp->fej9()), comp);
        // Allocate memory for every possible node in this method
        TR_BitVector *BCvisit = new (comp->trStackMemory()) TR_BitVector(methodSize, comp->trMemory(), stackAlloc);
        pcEntries = (uintptr_t *)comp->trMemory()->allocateMemory(sizeof(uintptr_t) * methodSize, stackAlloc);

        // Walk all bytecodes and populate the sorted array of interesting PCs (pcEntries)
        // numEntries will indicate how many entries have been populated
        // These profiling entries have been 'locked' so we must remember to unlock them
        bytesFootprint = walkILTreeForIProfilingEntries(pcEntries, numEntries, &bci, method, BCvisit, abort, comp);

        if (!abort) {
            std::vector<J9Class *> uncachedClasses; // This will be populated and send back to server
            std::vector<JITServerHelpers::ClassInfoTuple> classInfos; // This will be populated and send back to server

            if (numEntries) {
                // Serialize the entries
                std::string buffer(bytesFootprint, '\0');
                intptr_t writtenBytes = serializeIProfilerMethodEntries(pcEntries, numEntries, (uintptr_t)&buffer[0],
                    methodStart, comp, sharedProfile, totalSamples, uncachedClasses, classInfos);
                TR_ASSERT(writtenBytes == bytesFootprint, "BST doesn't match expected footprint");
                // Send the information to the server
                client->write(JITServer::MessageType::IProfiler_profilingSample, buffer, totalSamples,
                    (size_t)numEntries,
                    /*wholeMethod=*/true, usePersistentCache, isCompiled, uncachedClasses, classInfos);
            } else // Empty IProfiler data for this method
            {
                client->write(JITServer::MessageType::IProfiler_profilingSample, std::string(), (uint64_t)0, (size_t)0,
                    /*wholeMethod=*/true, usePersistentCache, isCompiled, uncachedClasses, classInfos);
            }
        }

        // release any entry that has been locked by us
        for (uint32_t i = 0; i < numEntries; i++) {
            TR_IPBCDataCallGraph *cgEntry = profilingSample(pcEntries[i], 0, false)->asIPBCDataCallGraph();
            if (cgEntry)
                cgEntry->releaseEntry();
        }
    } catch (const std::exception &e) {
        // release any entry that has been locked by us
        for (uint32_t i = 0; i < numEntries; i++) {
            TR_IPBCDataCallGraph *cgEntry = profilingSample(pcEntries[i], 0, false)->asIPBCDataCallGraph();
            if (cgEntry)
                cgEntry->releaseEntry();
        }
        throw;
    }
    return abort;
}

/**
 * @brief Code executed by the JITClient to serialize fanin info for a method
 *
 * @param omb J9Method in question
 * @return The serialized fanin info as a string
 */
std::string JITClientIProfiler::serializeFaninMethodEntry(TR_OpaqueMethodBlock *omb)
{
    // Find entry in a IProfiler hash table, if it exists
    auto entry = findOrCreateMethodEntry(NULL, (J9Method *)omb, false);
    return TR_ContiguousIPMethodHashTableEntry::serialize(entry);
}

