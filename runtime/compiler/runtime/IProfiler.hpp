/*******************************************************************************
 * Copyright IBM Corp. and others 2000
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

#ifndef IPROFILER_HPP
#define IPROFILER_HPP

/**
 * \page Iprofiling Interpreter Profiling
 *
 * The Interpreter Profiler, or IProfiler, is a mechanism created to extract
 * knowledge from the interpreter to feed into our first compilation.
 *
 * Structurally, the IProfiler consists of three main components:
 *
 * - The Interpreter / Virtual Machine which creates the data. The creation
 *   of data is not free however, and so this data collection should be
 *   possible to disable.
 * - The IProfiler component consumes the raw data from the VM, parsing it
 *   into a form consumable in the JIT compiler.
 * - The JIT compiler, which consumes the data produced by the IProfiler
 *   and may also query the IProfiler for specific data.
 *
 * Below is a rough diagram of the IProfiler system.
 *
 *
 *             Control            Queries
 *          +-----------+       +-------- --+
 *          |           |       |           |
 *          |           |       |           |
 *     +----v-+       +-+-------v-+        ++-----+
 *     |      |       |           |        |      |
 *     | VM   |       | IProfiler |        | JIT  |
 *     |      |       |           |        |      |
 *     +----+-+       +-^-------+-+        +^-----+
 *          |           |       |           |
 *          +-----------+       +-----------+
 *            Raw Data            Processed
 *                                  Data
 *
 *
 * For further details see "Experiences in Designing a Robust and Scalable
 * Interpreter Profiling Framework" by Ian Gartley, Marius Pirvu, Vijay
 * Sundaresan and Nikola Grecvski, published in *Code Generation and Optimization (CGO)*, 2013.
 */

#include "j9.h"
#include "j9cfg.h"
#include "env/jittypes.h"
#include "env/CompilerEnv.hpp"
#include "il/Node.hpp"
#include "infra/Link.hpp"
#include "runtime/ExternalProfiler.hpp"

#undef EXPERIMENTAL_IPROFILER

class TR_BlockFrequencyInfo;
namespace TR { class CompilationInfo; }
namespace TR { class Options; }
class TR_FrontEnd;
class TR_IPBCDataPointer;
class TR_IPBCDataCallGraph;
class TR_IPBCDataDirectCall;
class TR_IPBCDataFourBytes;
class TR_IPBCDataEightWords;
class TR_IPBCDataAllocation;
class TR_IPByteVector;
class TR_J9ByteCodeIterator;
class TR_ExternalValueProfileInfo;
class TR_OpaqueMethodBlock;
namespace TR { class PersistentInfo; }
namespace TR { class Monitor; }
struct J9Class;
struct J9PortLibrary;
namespace TR { class ResolvedMethodSymbol; }
class TR_ResolvedMethod;
class TR_AbstractInfo;
class TR_BitVector;
class TR_J9VMBase;
class TR_J9SharedCache;

#if defined (_MSC_VER)
extern "C" __declspec(dllimport) void __stdcall DebugBreak();
#if _MSC_VER < 1600
#error "Testarossa requires atleast MSVC10 aka Win SDK 7.1 aka Visual Studio 2010"
#endif
#endif

struct TR_IPHashedCallSite
   {
   void * operator new (size_t size) throw();
   void operator delete(void *p) throw() {}
   //
   TR_IPHashedCallSite () : _method(NULL), _offset(0) {};
   TR_IPHashedCallSite (J9Method* method, uint32_t offset) : _method(method), _offset(offset) {};

   J9Method* _method;
   uint32_t _offset;
   };


#define SWITCH_DATA_COUNT 4
#define NUM_CS_SLOTS 3
class CallSiteProfileInfo
   {
public:
   uint16_t _weight[NUM_CS_SLOTS];
   uint16_t _residueWeight:15;
   uint16_t _tooBigToBeInlined:1;

   void initialize()
         {
         for (int i = 0; i < NUM_CS_SLOTS; i++)
            {
            _clazz[i]=0;
            _weight[i]=0;
            }
         _residueWeight=0;
         _tooBigToBeInlined=0;
         }

   uintptr_t getClazz(int index) const { return _clazz[index]; }
   void setClazz(int index, uintptr_t clazzPtr) { _clazz[index] = clazzPtr; }
   uintptr_t getDominantClass(int32_t &sumW, int32_t &maxW);
   uint32_t getDominantSlot() const;

private:
   uintptr_t _clazz[NUM_CS_SLOTS]; // store them in either 64 or 32 bits
   };

#define TR_IPBCD_FOUR_BYTES  1
#define TR_IPBCD_EIGHT_WORDS 2
#define TR_IPBCD_CALL_GRAPH  3
#define TR_IPBCD_DIRECT_CALL 4


// We rely on the following structures having the same first 4 fields
typedef struct TR_IPBCDataStorageHeader
   {
   uint32_t pc;
   uint32_t left:8;
   uint32_t right:16;
   uint32_t ID:8;
   } TR_IPBCDataStorageHeader;

typedef struct TR_IPBCDataFourBytesStorage
   {
   TR_IPBCDataStorageHeader header;
   uint32_t data;
   } TR_IPBCDataFourBytesStorage;

typedef struct TR_IPBCDataEightWordsStorage
   {
   TR_IPBCDataStorageHeader header;
   uint64_t data[SWITCH_DATA_COUNT];
   } TR_IPBCDataEightWordsStorage;

typedef struct TR_IPBCDataCallGraphStorage
   {
   TR_IPBCDataStorageHeader header;
   CallSiteProfileInfo _csInfo;
   } TR_IPBCDataCallGraphStorage;

typedef struct TR_IPBCDataDirectCallStorage
   {
   TR_IPBCDataStorageHeader header;
   uint16_t _callCount;
   bool _isInvalid;
   bool _tooBigToBeInlined;
   } TR_IPBCDataDirectCallStorage;

enum TR_EntryStatusInfo
   {
   IPBC_ENTRY_CANNOT_PERSIST = 0,
   IPBC_ENTRY_CAN_PERSIST,
   IPBC_ENTRY_PERSIST_LOCK,
   IPBC_ENTRY_PERSIST_NOTINSCC,
   IPBC_ENTRY_PERSIST_UNLOADED
   };

#define TR_IPBC_PERSISTENT_ENTRY_READ  0x1 // used to check if the persistent entry has been read, if so we want to avoid the overhead introduced by calculating sample counts
#define TR_IPBC_OVERFLOW 0x1 // Set and never reset
#define TR_IPBC_COUNTERS_RESET 0x2 // Set and never reset

// Hash table for bytecodes
class TR_IPBytecodeHashTableEntry
   {
public:
   void * operator new (size_t size) throw();
   void operator delete(void *p) throw();
   void * operator new (size_t size, void * placement) {return placement;}
   void operator delete(void *p, void *) {}

   TR_IPBytecodeHashTableEntry(uintptr_t pc) : _next(NULL), _pc(pc), _lastSeenClassUnloadID(-1), _entryFlags(0), _persistFlags(IPBC_ENTRY_CAN_PERSIST_FLAG), _overflowFlags(0) {}
   virtual ~TR_IPBytecodeHashTableEntry() {}
   uintptr_t getPC() const { return _pc; }
   TR_IPBytecodeHashTableEntry * getNext() const { return _next; }
   void setNext(TR_IPBytecodeHashTableEntry *n) { _next = n; }
   int32_t getLastSeenClassUnloadID() const { return _lastSeenClassUnloadID; }
   void setLastSeenClassUnloadID(int32_t v) { _lastSeenClassUnloadID = v; }
   virtual bool hasData() = 0;
   virtual uintptr_t getData(TR::Compilation *comp = NULL) = 0;
   virtual uint32_t* getDataReference() { return NULL; }
   virtual int32_t setData(uintptr_t value, uint32_t freq = 1) = 0;
   virtual uint32_t getNumSamples() const = 0;
   virtual bool isInvalid() = 0;
   virtual void setInvalid() = 0;
   virtual TR_IPBCDataPointer        *asIPBCDataPointer()        { return NULL; }
   virtual TR_IPBCDataFourBytes      *asIPBCDataFourBytes()      { return NULL; }
   virtual TR_IPBCDataEightWords     *asIPBCDataEightWords()     { return NULL; }
   virtual TR_IPBCDataCallGraph      *asIPBCDataCallGraph()      { return NULL; }
   virtual TR_IPBCDataDirectCall     *asIPBCDataDirectCall()     { return NULL; }
   virtual TR_IPBCDataAllocation     *asIPBCDataAllocation()     { return NULL; }
   // returns the number of bytes the equivalent storage structure needs
   virtual uint32_t                   getBytesFootprint() = 0;

   virtual void setWarmCallGraphTooBig(bool set=true) {}
   virtual bool isWarmCallGraphTooBig() const { return false; }

#if defined(J9VM_OPT_JITSERVER)
   // Serialization used for JITServer
   // not sufficient for persisting to the shared cache
   virtual uint32_t canBeSerialized(TR::PersistentInfo *info) { return IPBC_ENTRY_CAN_PERSIST; }
   virtual void serialize(uintptr_t methodStartAddress, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info) = 0;
   virtual void deserialize(TR_IPBCDataStorageHeader *storage) = 0;

   virtual TR_IPBytecodeHashTableEntry *newEntry(TR::Region &region) const = 0;
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR_PersistentMemory *persistentMemory,
                                                 TR_Memory::ObjectType tag = TR_Memory::UnknownType) const = 0;
   TR_IPBytecodeHashTableEntry *clone(TR::Region &region)
      {
      auto entry = newEntry(region);
      entry->copyFromEntry(this);
      return entry;
      }

   TR_IPBytecodeHashTableEntry *clone(TR_PersistentMemory *persistentMemory,
                                      TR_Memory::ObjectType tag = TR_Memory::UnknownType)
      {
      auto entry = newEntry(persistentMemory, tag);
      if (!entry)
         throw std::bad_alloc();
      entry->copyFromEntry(this);
      return entry;
      }
#endif

   virtual uint32_t canBePersisted(TR_J9SharedCache *sharedCache, TR::PersistentInfo *info) { return IPBC_ENTRY_CAN_PERSIST; }
   virtual void createPersistentCopy(TR_J9SharedCache *sharedCache, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info) = 0;
   virtual void loadFromPersistentCopy(TR_IPBCDataStorageHeader *storage, TR::Compilation *comp) {}
   virtual void copyFromEntry(TR_IPBytecodeHashTableEntry *originalEntry) = 0;
   void setPersistentEntryRead(){ _entryFlags |= TR_IPBC_PERSISTENT_ENTRY_READ;};
   bool isPersistentEntryRead(){ return (_entryFlags & TR_IPBC_PERSISTENT_ENTRY_READ) != 0;};

   bool getCanPersistEntryFlag() const { return (_persistFlags & IPBC_ENTRY_CAN_PERSIST_FLAG) != 0; }
   void setDoNotPersist() { _persistFlags &= ~IPBC_ENTRY_CAN_PERSIST_FLAG; }
   bool isLockedEntry() const { return (_persistFlags & IPBC_ENTRY_PERSIST_LOCK_FLAG) != 0; }
   void setLockedEntry() { _persistFlags |= IPBC_ENTRY_PERSIST_LOCK_FLAG; }
   void resetLockedEntry() { _persistFlags &= ~IPBC_ENTRY_PERSIST_LOCK_FLAG; }
   void setOverflow() { _overflowFlags |= TR_IPBC_OVERFLOW; }
   bool hasOverflowed() const { return (_overflowFlags & TR_IPBC_OVERFLOW) != 0; }
   void setCountersWereReset() { _overflowFlags |= TR_IPBC_COUNTERS_RESET; }
   bool countersWereReset() const { return (_overflowFlags & TR_IPBC_COUNTERS_RESET) != 0; } // only for CallGraph entries

protected:
   TR_IPBytecodeHashTableEntry *_next;
   uintptr_t _pc;
   int32_t    _lastSeenClassUnloadID;

   enum TR_PersistenceFlags
      {
      IPBC_ENTRY_CAN_PERSIST_FLAG = 0x01,
      IPBC_ENTRY_PERSIST_LOCK_FLAG = 0x02,
      };

   uint8_t _entryFlags;
   uint8_t _persistFlags;
   uint8_t _overflowFlags; // indicates whether sample counters overflowed or were reset
   }; // class TR_IPBytecodeHashTableEntry

class TR_IPMethodData
   {
   public:
   TR_IPMethodData() : _method(0),_pcIndex(0),_weight(0) {}
   TR_OpaqueMethodBlock *getMethod() const { return _method; }
   void setMethod (TR_OpaqueMethodBlock *meth) { _method = meth; }
   uint32_t getWeight() const { return _weight; }
   void     incWeight() { ++_weight; }
   void     setWeight(uint32_t weight) { _weight = weight; }
   uint32_t getPCIndex() const { return _pcIndex; }
   void     setPCIndex(uint32_t i) { _pcIndex = i; }

   TR_IPMethodData* next;

   private:
   TR_OpaqueMethodBlock *_method;
   uint32_t _pcIndex;
   uint32_t _weight;
   };

struct TR_DummyBucket
   {
   uint32_t getWeight() const { return _weight; }
   void     incWeight() { ++_weight; }

   uint32_t _weight;
   };



struct TR_IPMethodHashTableEntry
   {
   static const int32_t MAX_IPMETHOD_CALLERS = 20;
   public:
   void * operator new (size_t size) throw();
   void operator delete(void *p) throw() {}

   TR_IPMethodHashTableEntry *_next;   // for chaining in the hashtable
   TR_OpaqueMethodBlock      *_method; // callee
   TR_IPMethodData            _caller; // link list of callers and their weights. Capped at MAX_IPMETHOD_CALLERS
   TR_DummyBucket             _otherBucket;

   void add(TR_OpaqueMethodBlock *caller, TR_OpaqueMethodBlock *callee, uint32_t pcIndex);
   };

class TR_IPBCDataFourBytes : public TR_IPBytecodeHashTableEntry
   {
public:
   TR_IPBCDataFourBytes(uintptr_t pc) : TR_IPBytecodeHashTableEntry(pc), data(0) {}

   static const uint32_t IPROFILING_INVALID = ~0;
   virtual bool hasData() { return data != 0; }
   virtual uintptr_t getData(TR::Compilation *comp = NULL) { return data; }
   virtual uint32_t* getDataReference() { static uint32_t data_copy = data; return &data_copy; }

   virtual int32_t setData(uintptr_t value, uint32_t freq = 1) { data = (uint32_t)value; return 0;}
   virtual uint32_t getNumSamples() const { return getSumBranchCount(); }
   virtual bool isInvalid() { if (data == IPROFILING_INVALID) return true; return false; }
   virtual void setInvalid() { data = IPROFILING_INVALID; }
   virtual TR_IPBCDataFourBytes  *asIPBCDataFourBytes() { return this; }
   virtual uint32_t getBytesFootprint() {return sizeof (TR_IPBCDataFourBytesStorage);}
#if defined(J9VM_OPT_JITSERVER)
   virtual void serialize(uintptr_t methodStartAddress, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void deserialize(TR_IPBCDataStorageHeader *storage);
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR::Region &region) const override
      {
      return new (region.allocate(sizeof(*this))) TR_IPBCDataFourBytes(_pc);
      }

   virtual TR_IPBytecodeHashTableEntry *newEntry(TR_PersistentMemory *persistentMemory,
                                              TR_Memory::ObjectType tag = TR_Memory::UnknownType) const override
      {
      tag = (tag != TR_Memory::UnknownType) ? tag : TR_Memory::IPBCDataFourBytes;
      return new (persistentMemory->allocatePersistentMemory(sizeof(*this), tag)) TR_IPBCDataFourBytes(_pc);
      }
#endif
   virtual void createPersistentCopy(TR_J9SharedCache *sharedCache, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void loadFromPersistentCopy(TR_IPBCDataStorageHeader *storage, TR::Compilation *comp);
   int32_t getSumBranchCount() const;
   virtual void copyFromEntry(TR_IPBytecodeHashTableEntry * originalEntry);
private:
   uint32_t data;
   };

class TR_IPBCDataAllocation : public TR_IPBytecodeHashTableEntry
   {
public:
   TR_IPBCDataAllocation(uintptr_t pc) : TR_IPBytecodeHashTableEntry(pc), clazz(0), method(0), data(0) {}
   static const uint32_t IPROFILING_INVALID = ~0;
   virtual bool hasData() { return data != 0; }
   virtual uintptr_t getData(TR::Compilation *comp = NULL) { return data; }
   virtual uint32_t* getDataReference() { return &data; }
   virtual int32_t setData(uintptr_t value, uint32_t freq = 1) { data = (uint32_t)value; return 0;}
   virtual bool isInvalid() { if (data == IPROFILING_INVALID) return true; return false; }
   virtual void setInvalid() { data = IPROFILING_INVALID; }
   virtual TR_IPBCDataAllocation  *asIPBCDataAllocation() { return this; }

   uintptr_t getClass() { return (uintptr_t)clazz; }
   void setClass(uintptr_t c) { clazz = c; }
   uintptr_t getMethod() { return (uintptr_t)method; }
   void setMethod(uintptr_t m) { method = m; }
private:
   uintptr_t clazz;
   uintptr_t method;
   uint32_t data;
   };

#define SWITCH_DATA_COUNT 4
class TR_IPBCDataEightWords : public TR_IPBytecodeHashTableEntry
   {
public:
   TR_IPBCDataEightWords(uintptr_t pc) : TR_IPBytecodeHashTableEntry(pc)
      {
      for (int i = 0; i < SWITCH_DATA_COUNT; i++)
         data[i] = 0;
      };
   static const uint64_t IPROFILING_INVALID = ~0;
   virtual bool hasData() { return getSumSwitchCount() > 1; } // Note: getSumSwitchCount() artificially adds one to the count
   virtual uintptr_t getData(TR::Compilation *comp = NULL) { /*TR_ASSERT(0, "Don't call me, I'm empty"); */return 0;}
   virtual int32_t setData(uintptr_t value, uint32_t freq = 1) { /*TR_ASSERT(0, "Don't call me, I'm empty");*/ return 0;}
   const uint64_t* getDataPointer() const { return data; }
   uint64_t* getDataPointer() { return data; }
   virtual uint32_t getNumSamples() const { return getSumSwitchCount(); }
   virtual bool isInvalid() { if (data[0] == IPROFILING_INVALID) return true; return false; }
   virtual void setInvalid() { data[0] = IPROFILING_INVALID; }
   virtual TR_IPBCDataEightWords  *asIPBCDataEightWords() { return this; }
   virtual uint32_t getBytesFootprint() {return sizeof(TR_IPBCDataEightWordsStorage);}
#if defined(J9VM_OPT_JITSERVER)
   virtual void serialize(uintptr_t methodStartAddress, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void deserialize(TR_IPBCDataStorageHeader *storage);
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR::Region &region) const override
      {
      return new (region.allocate(sizeof(*this))) TR_IPBCDataEightWords(_pc);
      }
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR_PersistentMemory *persistentMemory,
                                                 TR_Memory::ObjectType tag = TR_Memory::UnknownType) const override
      {
      tag = (tag != TR_Memory::UnknownType) ? tag : TR_Memory::IPBCDataEightWords;
      return new (persistentMemory->allocatePersistentMemory(sizeof(*this), tag)) TR_IPBCDataEightWords(_pc);
      }
#endif
   virtual void createPersistentCopy(TR_J9SharedCache *sharedCache, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void loadFromPersistentCopy(TR_IPBCDataStorageHeader *storage, TR::Compilation *comp);
   int32_t getSumSwitchCount() const;
   virtual void copyFromEntry(TR_IPBytecodeHashTableEntry *originalEntry);

private:
   uint64_t data[SWITCH_DATA_COUNT];
   };



class TR_IPBCDataCallGraph : public TR_IPBytecodeHashTableEntry
   {
public:
   TR_IPBCDataCallGraph (uintptr_t pc) : TR_IPBytecodeHashTableEntry(pc)
      {
      _csInfo.initialize();
      }
   static const uintptr_t IPROFILING_INVALID = ~0;

   virtual bool hasData() { return getData(NULL) != 0; }
   virtual uintptr_t getData(TR::Compilation *comp = NULL);
   virtual CallSiteProfileInfo* getCGData() { return &_csInfo; } // overloaded
   virtual int32_t setData(uintptr_t v, uint32_t freq = 1);
   virtual uint32_t* getDataReference() { return NULL; }
   virtual uint32_t getNumSamples() const { return getSumCount(); }
   virtual TR_IPBCDataCallGraph *asIPBCDataCallGraph() { return this; }
   int32_t getSumCount() const;
   int32_t getSumCount(TR::Compilation *comp);
   int32_t getEdgeWeight(TR_OpaqueClassBlock *clazz, TR::Compilation *comp);
   void updateEdgeWeight(TR_OpaqueClassBlock *clazz, int32_t weight);
   void printWeights(TR::Compilation *comp);

   virtual void setWarmCallGraphTooBig(bool set=true) { _csInfo._tooBigToBeInlined = (set) ? 1 : 0; }
   virtual bool isWarmCallGraphTooBig() const { return (_csInfo._tooBigToBeInlined == 1); }

   virtual bool isInvalid() { return (_csInfo.getClazz(0) == IPROFILING_INVALID); }
   virtual void setInvalid() { _csInfo.setClazz(0, IPROFILING_INVALID); }
   virtual uint32_t getBytesFootprint() {return sizeof (TR_IPBCDataCallGraphStorage);}

#if defined(J9VM_OPT_JITSERVER)
   virtual uint32_t canBeSerialized(TR::PersistentInfo *info);
   virtual void serialize(uintptr_t methodStartAddress, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void deserialize(TR_IPBCDataStorageHeader *storage);
   //TODO: add override to all functions that need it for consistency
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR::Region &region) const override
      {
      return new (region.allocate(sizeof(*this))) TR_IPBCDataCallGraph(_pc);
      }

   virtual TR_IPBytecodeHashTableEntry *newEntry(TR_PersistentMemory *persistentMemory,
                                                 TR_Memory::ObjectType tag = TR_Memory::UnknownType) const override
      {
      tag = (tag != TR_Memory::UnknownType) ? tag : TR_Memory::IPBCDataCallGraph;
      return new (persistentMemory->allocatePersistentMemory(sizeof(*this), tag)) TR_IPBCDataCallGraph(_pc);
      }
#endif

   virtual uint32_t canBePersisted(TR_J9SharedCache *sharedCache, TR::PersistentInfo *info);
   virtual void createPersistentCopy(TR_J9SharedCache *sharedCache, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void loadFromPersistentCopy(TR_IPBCDataStorageHeader *storage, TR::Compilation *comp);
   virtual void copyFromEntry(TR_IPBytecodeHashTableEntry *originalEntry);

   bool lockEntry();
   void releaseEntry();
   bool isLocked();

private:
   CallSiteProfileInfo _csInfo;
   };

class TR_IPBCDataDirectCall : public TR_IPBytecodeHashTableEntry
   {
public:
   TR_IPBCDataDirectCall (uintptr_t pc) : TR_IPBytecodeHashTableEntry(pc)
      {
      _callCount = 0;
      _isInvalid = false;
      _tooBigToBeInlined = false;
      setDoNotPersist();
      }

   virtual bool hasData() { return _callCount != 0; }
   virtual uintptr_t getData(TR::Compilation *comp = NULL) { return _callCount; }
   virtual int32_t setData(uintptr_t v, uint32_t freq = 1);
   virtual uint32_t* getDataReference() { return NULL; }
   virtual uint32_t getNumSamples() const { return _callCount; }
   virtual TR_IPBCDataDirectCall *asIPBCDataDirectCall() { return this; }

   virtual void setWarmCallGraphTooBig(bool set=true) { _tooBigToBeInlined = set; }
   virtual bool isWarmCallGraphTooBig() const { return _tooBigToBeInlined; }

   virtual bool isInvalid() { return _isInvalid; }
   virtual void setInvalid() { _isInvalid = true; }
   virtual uint32_t getBytesFootprint() {return sizeof (TR_IPBCDataDirectCallStorage);}

#if defined(J9VM_OPT_JITSERVER)
   virtual void serialize(uintptr_t methodStartAddress, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info);
   virtual void deserialize(TR_IPBCDataStorageHeader *storage);
   virtual TR_IPBytecodeHashTableEntry *newEntry(TR::Region &region) const override
      {
      return new (region.allocate(sizeof(*this))) TR_IPBCDataDirectCall(_pc);
      }

   virtual TR_IPBytecodeHashTableEntry *newEntry(TR_PersistentMemory *persistentMemory,
                                                 TR_Memory::ObjectType tag = TR_Memory::UnknownType) const override
      {
      tag = (tag != TR_Memory::UnknownType) ? tag : TR_Memory::IPBCDataDirectCall;
      return new (persistentMemory->allocatePersistentMemory(sizeof(*this), tag)) TR_IPBCDataDirectCall(_pc);
      }
#endif
   // We do not write TR_IPBCDataDirectCall entries in the SCC.
   virtual uint32_t canBePersisted(TR_J9SharedCache *sharedCache, TR::PersistentInfo *info) { return IPBC_ENTRY_CANNOT_PERSIST; }
   virtual void createPersistentCopy(TR_J9SharedCache *sharedCache, TR_IPBCDataStorageHeader *storage, TR::PersistentInfo *info) {}
   virtual void loadFromPersistentCopy(TR_IPBCDataStorageHeader *storage, TR::Compilation *comp) {}
   virtual void copyFromEntry(TR_IPBytecodeHashTableEntry *originalEntry);

private:
   uint16_t _callCount;
   bool _isInvalid;
   bool _tooBigToBeInlined;
   };


class IProfilerBuffer : public TR_Link0<IProfilerBuffer>
   {
   public:
   U_8* getBuffer() {return _buffer;}
   void setBuffer(U_8* buffer) { _buffer = buffer; }
   UDATA getSize() {return _size;}
   void setSize(UDATA size) {_size = size;}
   bool isValid() const { return !_isInvalidated; }
   void setIsInvalidated(bool b) { _isInvalidated = b; }
   private:
   U_8 *_buffer;
   UDATA _size;
   volatile bool _isInvalidated;
   };

class TR_ReadSampleRequestsStats
   {
friend class TR_ReadSampleRequestsHistory;
private:
   uint32_t _totalReadSampleRequests; // no locks used to update this
   uint32_t _failedReadSampleRequests;
   };

class TR_ReadSampleRequestsHistory
   {
   #define SAMPLE_CUTOFF 120     // this must be correlated with HISTORY_BUFFER_SIZE
                                  // the bigger the HISTORY_BUFFER_SIZE the bigger SAMPLE_CUTOFF should be
public:
   bool init(int32_t historyBufferSize);
   void incTotalReadSampleRequests()  { _history[_crtIndex]._totalReadSampleRequests++; }
   void incFailedReadSampleRequests() { _history[_crtIndex]._failedReadSampleRequests++; }
   uint32_t getTotalReadSampleRequests() const  { return _history[_crtIndex]._totalReadSampleRequests; }
   uint32_t getFailedReadSampleRequests() const { return _history[_crtIndex]._failedReadSampleRequests; }
   uint32_t getReadSampleFailureRate() const;
   void advanceEpoch(); // performed by sampling thread only
   uint32_t numSamplesInHistoryBuffer() const
      {
      return _history[_crtIndex]._totalReadSampleRequests - _history[nextIndex()]._totalReadSampleRequests;
      }
private:
   int32_t nextIndex() const { return (_crtIndex + 1) % _historyBufferSize; }
private:
   int32_t _historyBufferSize;
   int32_t _crtIndex;
   TR_ReadSampleRequestsStats *_history; // My circular buffer
   }; // class TR_ReadSampleRequestsHistory


// Supporting code for dumping IProfiler data to stderr to track possible
// performance issues due to insufficient or wrong IProfiler info.
// It implements a hashtable where each node stores information for
// various bytecodes of a single ROMMethod.
// Code is currently inactive. To actually use one must issue
// iProfiler->dumpIPBCDataCallGraph(vmThread)
// in some part of the code (typically at shutdown time)
class TR_AggregationHT
   {
public:
   class TR_IPChainedEntry
      {
      TR_IPChainedEntry *_next; // for chaining
      TR_IPBytecodeHashTableEntry *_IPentry;
   public:
      TR_IPChainedEntry(TR_IPBytecodeHashTableEntry *entry) : _next(NULL), _IPentry(entry) { }
      TR_IPChainedEntry *getNext() const { return _next; }
      void setNext(TR_IPChainedEntry *next) { _next = next; }
      TR_IPBytecodeHashTableEntry *getIPData() const { return _IPentry; }
      uintptr_t getPC() const { return _IPentry->getPC(); }
      };
   class TR_AggregationHTNode
      {
      TR_AggregationHTNode *_next; // for chaining
      J9ROMMethod *_romMethod; // this is the key
      J9ROMClass  *_romClass; // TODO: is this needed?
      TR_IPChainedEntry *_IPData;
   public:
      TR_AggregationHTNode(J9ROMMethod *romMethod, J9ROMClass *romClass, TR_IPBytecodeHashTableEntry *entry);
      ~TR_AggregationHTNode();
      TR_AggregationHTNode *getNext() const { return _next; }
      void setNext(TR_AggregationHTNode *next) { _next = next; }
      J9ROMMethod *getROMMethod() const { return _romMethod; }
      J9ROMClass *getROMClass() const { return _romClass; }
      TR_IPChainedEntry *getFirstIPEntry() const { return _IPData; }
      void setFirstCGEntry(TR_IPChainedEntry *e) { _IPData = e; }
      };
   struct SortingPair
      {
      char *_methodName;
      TR_AggregationHTNode *_IPdata;
      };

   TR_AggregationHT(size_t sz);
   ~TR_AggregationHT();
   size_t hash(J9ROMMethod *romMethod) const { return (((uintptr_t)romMethod) >> 3) % _sz; }
   size_t getSize() const { return _sz; }
   size_t numTrackedMethods() const { return _numTrackedMethods; }
   TR_AggregationHTNode* getBucket(size_t i) const { return _backbone[i]; }
   void add(J9ROMMethod *romMethod, J9ROMClass *romClass, TR_IPBytecodeHashTableEntry *cgEntry);
   void sortByNameAndPrint();
private:
   size_t _sz; // size of the backbone of the hashtable
   size_t _numTrackedMethods; // only increasing
   TR_AggregationHTNode** _backbone;
   }; // class TR_AggregationHT

class TR_IProfiler : public TR_ExternalProfiler
   {
public:

   enum TR_IprofilerThreadLifetimeStates
      {
      IPROF_THR_NOT_CREATED = 0,
      IPROF_THR_FAILED_TO_ATTACH,
      IPROF_THR_INITIALIZED,
      IPROF_THR_WAITING_FOR_WORK,
      IPROF_THR_SUSPENDING,
      IPROF_THR_SUSPENDED,
      IPROF_THR_RESUMING,
      IPROF_THR_STOPPING,
      IPROF_THR_DESTROYED,
      IPROF_THR_LAST_STATE // must be the last one
      };

   static TR_IProfiler *allocate (J9JITConfig *);
   static TR::PersistentAllocator *createPersistentAllocator(J9JITConfig *);
   static TR::PersistentAllocator *allocator() { return _allocator;}
   static void setAllocator(TR::PersistentAllocator *allocator) { _allocator = allocator; }
   static uint32_t getProfilerMemoryFootprint();

   uintptr_t getReceiverClassFromCGProfilingData(TR_ByteCodeInfo &bcInfo, TR::Compilation *comp);

   TR_IProfiler (J9JITConfig *);
   void * operator new (size_t) throw();
   void operator delete(void *p) throw() {}
   void shutdown();
   void outputStats();
   void dumpIPBCDataCallGraph(J9VMThread* currentThread);
   void startIProfilerThread(J9JavaVM *javaVM);
   void deallocateIProfilerBuffers();
   void stopIProfilerThread();
   void invalidateProfilingBuffers(); // called for class unloading
   bool isIProfilingEnabled() const { return _isIProfilingEnabled; }
   void incrementNumRequests() { _numRequests++; }
   // this is registered as the BufferFullEvent handler
   UDATA parseBuffer(J9VMThread * vmThread, const U_8* dataStart, UDATA size, bool verboseReparse=false);

   void printAllocationReport(); //Called by HookedByTheJIT

   bool isWarmCallGraphTooBig(TR_OpaqueMethodBlock *method, int32_t bcIndex, TR::Compilation *comp);
   void setWarmCallGraphTooBig(TR_OpaqueMethodBlock *method, int32_t bcIndex, TR::Compilation *comp, bool set = true);

   // This method is used to search the hash table first, then the shared cache. Added to support RI data >> public.
   TR_IPBytecodeHashTableEntry *profilingSampleRI (uintptr_t pc, uintptr_t data, bool addIt, uint32_t freq);

   /*
   leave the TR_ResolvedMethodSymbol argument for debugging purpose when called from Ilgen
   */
   virtual void persistIprofileInfo(TR::ResolvedMethodSymbol *methodSymbol, TR_ResolvedMethod *method, TR::Compilation *comp); // JITServer: mark virtual
   bool elgibleForPersistIprofileInfo(TR::Compilation *comp) const;

   void persistAllEntries(); // Persists all entries from IProfiler table into the SCC; TODO: check that JITServer does not execute this
   void traverseIProfilerTableAndCollectEntries(TR_AggregationHT *aggregationHT, J9VMThread* vmThread, bool collectOnlyCallGraphEntries = false);

   void checkMethodHashTable();

   //j9method.cpp
   void getFaninInfo(TR_OpaqueMethodBlock *calleeMethod, uint32_t *count, uint32_t *weight, uint32_t *otherBucketWeight = NULL);
   bool getCallerWeight(TR_OpaqueMethodBlock *calleeMethod, TR_OpaqueMethodBlock *callerMethod , uint32_t *weight, uint32_t pcIndex = ~0, TR::Compilation *comp = 0);

   bool    isCallGraphProfilingEnabled();

   virtual TR_AbstractInfo *createIProfilingValueInfo( TR_ByteCodeInfo &bcInfo, TR::Compilation *comp);
   virtual TR_ExternalValueProfileInfo * getValueProfileInfo(TR_ByteCodeInfo &bcInfo, TR::Compilation *comp);
   uint32_t *getAllocationProfilingDataPointer(TR_ByteCodeInfo &bcInfo, TR_OpaqueClassBlock *clazz, TR_OpaqueMethodBlock *method, TR::Compilation *comp);
   uint32_t *getGlobalAllocationDataPointer(bool isAOT);
   TR_ExternalProfiler * canProduceBlockFrequencyInfo(TR::Compilation& comp);

   TR_IPMethodHashTableEntry *findOrCreateMethodEntry(J9Method *, J9Method *, bool addIt, uint32_t pcIndex =  ~0);
   // Returns the number of entries released, and also stores the number of
   // entries that were not expected to be locked in unexpectedLockedEntries
   uint32_t releaseAllEntries(uint32_t &unexpectedLockedEntries);
   uint32_t countEntries();
   void advanceEpochForHistoryBuffer() { _readSampleRequestsHistory->advanceEpoch(); }
   uint32_t getReadSampleFailureRate() const { return _readSampleRequestsHistory->getReadSampleFailureRate(); }
   uint32_t getTotalReadSampleRequests() const { return _readSampleRequestsHistory->getTotalReadSampleRequests(); }
   uint32_t getFailedReadSampleRequests() const { return _readSampleRequestsHistory->getFailedReadSampleRequests(); }
   uint32_t numSamplesInHistoryBuffer() const { return _readSampleRequestsHistory->numSamplesInHistoryBuffer(); }



public:
   J9VMThread* getIProfilerThread() { return _iprofilerThread; }
   void setIProfilerThread(J9VMThread* thread) { _iprofilerThread = thread; }
   j9thread_t getIProfilerOSThread() { return _iprofilerOSThread; }
   TR::Monitor* getIProfilerMonitor() { return _iprofilerMonitor; }
   bool processProfilingBuffer(J9VMThread *vmThread, const U_8* dataStart, UDATA size);
   void processWorkingQueue();
   IProfilerBuffer *getCrtProfilingBuffer() const { return _crtProfilingBuffer; }
   void setCrtProfilingBuffer(IProfilerBuffer *b) { _crtProfilingBuffer = b; }
   void jitProfileParseBuffer(J9VMThread *vmThread);
   bool postIprofilingBufferToWorkingQueue(J9VMThread * vmThread, const U_8* dataStart, UDATA size);
   // this is wrapper of registered version, for the helper function, from JitRunTime

   // Data accessors, overridden for JITServer
   //

   // This method is used to search only the hash table
   virtual TR_IPBytecodeHashTableEntry *profilingSample (uintptr_t pc, uintptr_t data, bool addIt, bool isRIData = false, uint32_t freq  = 1);
   // This method is used to search the hash table first, then the shared cache
   virtual TR_IPBytecodeHashTableEntry *profilingSample (TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex,
                                                 TR::Compilation *comp, uintptr_t data = 0xDEADF00D, bool addIt = false);
   virtual uintptr_t getProfilingData(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *);
   virtual uintptr_t getProfilingData(TR::Node *node, TR::Compilation *comp);
   uintptr_t getSearchPC (TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *);
   static uintptr_t getSearchPCFromMethodAndBCIndex(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex);
   static uintptr_t getSearchPCFromMethodAndBCIndex(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation * comp);
   virtual TR_IPBytecodeHashTableEntry *searchForSample(uintptr_t pc, int32_t bucket);
   virtual TR_IPMethodHashTableEntry *searchForMethodSample(TR_OpaqueMethodBlock *omb, int32_t bucket);

   // Use _iprofilerMonitor for these two routines
   TR_IprofilerThreadLifetimeStates getIProfilerThreadLifetimeState() const { return _iprofilerThreadLifetimeState; }
   void setIProfilerThreadLifetimeState(TR_IprofilerThreadLifetimeStates s) { _iprofilerThreadLifetimeState = s; }

   void traverseIProfilerTableAndGenerateHistograms(J9JITConfig *jitConfig);

protected:
   bool isCompact(U_8 byteCode);
   bool isSwitch(U_8 byteCode);
   bool isNewOpCode(U_8 byteCode);
   virtual bool invalidateEntryIfInconsistent(TR_IPBytecodeHashTableEntry *entry);
   TR::CompilationInfo *getCompInfo() { return _compInfo; }

private:
#ifdef J9VM_INTERP_PROFILING_BYTECODES
   U_8 *getProfilingBufferCursor(J9VMThread *vmThread) const { return vmThread->profilingBufferCursor; }
   void setProfilingBufferCursor(J9VMThread *vmThread, U_8* p) { vmThread->profilingBufferCursor = p; }
   U_8 *getProfilingBufferEnd(J9VMThread *vmThread) const { return vmThread->profilingBufferEnd; }
   void setProfilingBufferEnd(J9VMThread *vmThread, U_8* p) { vmThread->profilingBufferEnd = p; }
#else
   U_8 *getProfilingBufferCursor(J9VMThread *vmThread) const { return NULL; }
   void setProfilingBufferCursor(J9VMThread *vmThread, U_8* p) {}
   U_8 *getProfilingBufferEnd(J9VMThread *vmThread) const { return NULL; }
   void setProfilingBufferEnd(J9VMThread *vmThread, U_8* p) {}
#endif
   bool needTriggerCallContextCollection(U_8 *pc, J9Method *caller, J9Method *callee);
   uintptr_t getProfilingData(TR_ByteCodeInfo &bcInfo, TR::Compilation *comp);
   virtual void setBlockAndEdgeFrequencies(TR::CFG *cfg, TR::Compilation *comp);
   virtual bool hasSameBytecodeInfo(TR_ByteCodeInfo & persistentByteCodeInfo, TR_ByteCodeInfo & currentByteCodeInfo, TR::Compilation *comp);
   virtual void getBranchCounters(TR::Node *node, TR::TreeTop *fallThroughTree, int32_t *taken, int32_t *notTaken, TR::Compilation *comp);
   virtual int32_t getSwitchCountForValue(TR::Node *node, int32_t value, TR::Compilation *comp);
   virtual int32_t getSumSwitchCount(TR::Node *node, TR::Compilation *comp);
   virtual int32_t getFlatSwitchProfileCounts (TR::Node *node, TR::Compilation *comp);
   virtual bool isSwitchProfileFlat(TR::Node *node, TR::Compilation *comp);

   TR_IPBCDataStorageHeader *getJ9SharedDataDescriptorForMethod(J9SharedDataDescriptor * descriptor, unsigned char * buffer, uint32_t length, TR_OpaqueMethodBlock * method, TR::Compilation *comp);

   static int32_t bcHash (uintptr_t);
   static int32_t allocHash (uintptr_t);

   static int32_t methodHash(uintptr_t pc);
//   static int32_t pcHash(uintptr_t pc);

   bool acquireHashTableWriteLock(bool forceFullLock);
   void releaseHashTableWriteLock();

   TR_IPBCDataStorageHeader *searchForPersistentSample(TR_IPBCDataStorageHeader  *root, uintptr_t pc);
   TR_IPBCDataAllocation *searchForAllocSample(uintptr_t pc, int32_t bucket);

   TR_IPBytecodeHashTableEntry * persistentProfilingSample (TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *comp, bool *methodProfileExistsInSCC);
   TR_IPBCDataStorageHeader * persistentProfilingSample (TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *comp, bool *methodProfileExistsInSCC, TR_IPBCDataStorageHeader *store);

   TR_IPBCDataAllocation *profilingAllocSample (uintptr_t pc, uintptr_t data, bool addIt);
   TR_IPBytecodeHashTableEntry *findOrCreateEntry (int32_t bucket, uintptr_t pc, bool addIt);
   TR_IPBCDataAllocation *findOrCreateAllocEntry (int32_t bucket, uintptr_t pc, bool addIt);
   TR_OpaqueMethodBlock * getMethodFromNode(TR::Node *node, TR::Compilation *comp);
   bool addSampleData(TR_IPBytecodeHashTableEntry *entry, uintptr_t data, bool isRIData = false, uint32_t freq = 1);
   TR_AbstractInfo *createIProfilingValueInfo( TR::Node *node, TR::Compilation *comp);


   bool branchHasSameDirection(TR::ILOpCodes nodeOpCode, TR::Node *node, TR::Compilation *comp);
   bool branchHasOppositeDirection(TR::ILOpCodes nodeOpCode, TR::Node *node, TR::Compilation *comp);
   uint8_t getBytecodeOpCode(TR::Node *node, TR::Compilation *comp);
   bool isNewOpCode(uintptr_t pc);
   bool isSwitch(uintptr_t pc);
   int32_t getOrSetSwitchData(TR_IPBCDataEightWords *entry, uint32_t data, bool isSet, bool isLookup);
   TR_IPBytecodeHashTableEntry *getProfilingEntry(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *);

   TR_IPBCDataCallGraph* getCGProfilingData(TR_ByteCodeInfo &bcInfo, TR::Compilation *comp);
   TR_IPBCDataCallGraph* getCGProfilingData(TR_OpaqueMethodBlock *method, uint32_t byteCodeIndex, TR::Compilation *comp);

   J9ROMMethod *findROMMethodFromPC(J9VMThread *vmThread, uintptr_t methodPC, J9ROMClass *&romClass);
   uintptr_t createBalancedBST(TR_IPBytecodeHashTableEntry **ipEntries, int32_t low, int32_t high, uintptr_t memChunk, TR_J9SharedCache *sharedCache);
   uintptr_t createBalancedBST(uintptr_t *pcEntries, int32_t low, int32_t high, uintptr_t memChunk, TR_J9SharedCache *sharedCache);
   uint32_t walkILTreeForEntries(uintptr_t *pcEntries, uint32_t &numEntries, TR_J9ByteCodeIterator *bcIterator, TR_OpaqueMethodBlock *method, TR::Compilation *comp,
                                 vcount_t visitCount, int32_t callerIndex, TR_BitVector *BCvisit, bool &abort);

   /**
    * @brief Moves buffers in the working queue to the free list.
    *
    * @note This method must be called with IProfiler Monitor in hand
    */
   void discardFilledIProfilerBuffers();

#if defined(J9VM_OPT_CRIU_SUPPORT)
   /**
    * @brief Suspend the IProfiler Thread
    *
    * @note This method is called by the IProfiler Thread to suspend itself.
    */
   void suspendIProfilerThreadForCheckpoint();
#endif

   // data members
   static TR::PersistentAllocator *_allocator;
   J9PortLibrary                  *_portLib;
   bool                            _isIProfilingEnabled; // set to TRUE in constructor; set to FALSE in shutdown()
   TR_J9VMBase                    *_vm;
   TR::CompilationInfo *            _compInfo;
   TR::Monitor *_hashTableMonitor;
   uint32_t                        _lightHashTableMonitor;

   // value profiling
   TR_OpaqueMethodBlock           *_valueProfileMethod;

   // bytecode hashtable
   protected:
   TR_IPBytecodeHashTableEntry   **_bcHashTable;
   private:
#if defined(EXPERIMENTAL_IPROFILER)
   // bytecode hashtable
   TR_IPBCDataAllocation         **_allocHashTable;
#endif

   // giving out profiling information for inlined calls
   bool                            _allowedToGiveInlinedInformation;
   int32_t                         _classLoadTimeStampGap;
   // call-graph profiling
   bool                            _enableCGProfiling;
   uint32_t                        _globalAllocationCount;
   int32_t                         _maxCallFrequency;
   j9thread_t                      _iprofilerOSThread;
   J9VMThread                     *_iprofilerThread;
   TR_LinkHead0<IProfilerBuffer>   _freeBufferList;
   TR_LinkHead0<IProfilerBuffer>   _workingBufferList;
   IProfilerBuffer                *_workingBufferTail;
   IProfilerBuffer                *_crtProfilingBuffer; // profiling buffer being processes by iprofiling thread
   TR::Monitor                    *_iprofilerMonitor;
   volatile int32_t                _numOutstandingBuffers;
   uint64_t                        _numRequests;
   uint64_t                        _numRequestsDropped;
   uint64_t                        _numRequestsSkipped;
   uint64_t                        _numRequestsHandedToIProfilerThread;
   uint64_t                        _iprofilerNumRecords; // info stats only

   TR_IPMethodHashTableEntry       **_methodHashTable;
   uint32_t                        _numMethodHashEntries;

   uint32_t                        _iprofilerBufferSize;
   TR_ReadSampleRequestsHistory   *_readSampleRequestsHistory;

   volatile TR_IprofilerThreadLifetimeStates _iprofilerThreadLifetimeState;

   public:
   static int32_t                  _STATS_noProfilingInfo;
   static int32_t                  _STATS_doesNotWantToGiveProfilingInfo;
   static int32_t                  _STATS_weakProfilingRatio;
   static int32_t                  _STATS_cannotGetClassInfo;
   static int32_t                  _STATS_timestampHasExpired;
   static int32_t                  _STATS_abortedPersistence;
   static int32_t                  _STATS_methodPersisted;
   static int32_t                  _STATS_methodNotPersisted_SCCfull;
   static int32_t                  _STATS_methodNotPersisted_classNotInSCC;
   static int32_t                  _STATS_methodNotPersisted_delayed;
   static int32_t                  _STATS_methodNotPersisted_other;
   static int32_t                  _STATS_methodNotPersisted_alreadyStored;
   static int32_t                  _STATS_methodNotPersisted_noEntries;
   static int32_t                  _STATS_persistError;
   static int32_t                  _STATS_methodPersistenceAttempts;
   static int32_t                  _STATS_entriesPersisted;
   static int32_t                  _STATS_entriesNotPersisted_NotInSCC;
   static int32_t                  _STATS_entriesNotPersisted_Unloaded;
   static int32_t                  _STATS_entriesNotPersisted_NoInfo;
   static int32_t                  _STATS_entriesNotPersisted_Other;

   static int32_t                  _STATS_persistedIPReadFail;
   static int32_t                  _STATS_persistedIPReadHadBadData;
   static int32_t                  _STATS_persistedIPReadSuccess;
   static int32_t                  _STATS_persistedAndCurrentIPDataDiffer;
   static int32_t                  _STATS_persistedAndCurrentIPDataMatch;

   static int32_t                  _STATS_currentIPReadFail;
   static int32_t                  _STATS_currentIPReadSuccess;
   static int32_t                  _STATS_currentIPReadHadBadData;

   static int32_t                  _STATS_IPEntryRead;
   static int32_t                  _STATS_IPEntryChoosePersistent;
   };

void printIprofilerStats(TR::Options *options, J9JITConfig * jitConfig, TR_IProfiler *iProfiler, const char *event);
void turnOffInterpreterProfiling(J9JITConfig *jitConfig);

#endif
