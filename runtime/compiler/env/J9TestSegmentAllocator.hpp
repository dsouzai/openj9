#ifndef J9_TEST_SEGMENT_ALLOCATOR
#define J9_TEST_SEGMENT_ALLOCATOR

#pragma once

#include <set>
#include <deque>
#include "j9.h"
#include "infra/ReferenceWrapper.hpp"
#include "env/OMRTestSegmentAllocator.hpp"
#include "env/jittypes.h"

extern "C" {
struct J9MemorySegment;
}

namespace TestAlloc {

class J9SA : public SegmentAllocator
   {
public:
   J9SA(int32_t segmentType, J9JavaVM &javaVM, RawAllocator &rawAllocator) throw();
   ~J9SA() throw();
   virtual TR::MemorySegment &allocate(size_t size);
   virtual void  deallocate(TR::MemorySegment &segment);

   size_t pageSize() throw();
   size_t pageAlign(const size_t requestedSize) throw();

private:
   void preventAllocationOfBTLMemory(J9MemorySegment * &segment, J9JavaVM * javaVM, int32_t segmentType);

   RawAllocator &_rawAllocator;
   const int32_t _segmentType;
   J9JavaVM &_javaVM;
   };

class J9SegmentCache : public SegmentAllocator
   {
public:
   J9SegmentCache(size_t cachedSegmentSize, SegmentAllocator &backingProvider);
   J9SegmentCache(J9SegmentCache &donor);

   ~J9SegmentCache() throw();

   virtual TR::MemorySegment &allocate(size_t size);
   virtual void  deallocate(TR::MemorySegment &segment) throw();
   virtual size_t getPreferredSegmentSize() { return _cachedSegmentSize; }

   J9SegmentCache &ref() { return *this; }

private:
   size_t _cachedSegmentSize;
   SegmentAllocator &_backingProvider;
   TR::MemorySegment &_firstSegment;
   bool _firstSegmentInUse;
   bool _firstSegmentDonated;
   };

class J9SystemSegmentProvider : public SegmentAllocator
   {
public:
   J9SystemSegmentProvider(size_t defaultSegmentSize, size_t systemSegmentSize, size_t allocationLimit, SegmentAllocator &segmentAllocator, RawAllocator &rawAllocator);
   ~J9SystemSegmentProvider() throw();
   virtual TR::MemorySegment &allocate(size_t size);
   virtual void  deallocate(TR::MemorySegment &segment) throw();
   virtual size_t allocationLimit() const throw();
   virtual void setAllocationLimit(size_t allocationLimit);
   virtual size_t systemBytesAllocated() const throw();
   virtual size_t regionBytesAllocated() const throw();
   virtual size_t bytesAllocated() const throw();
   bool isLargeSegment(size_t segmentSize);

private:
   size_t round(size_t requestedSize);
   TR::MemorySegment &allocateNewSegment(size_t size, TR::reference_wrapper<TR::MemorySegment> systemSegment);
   TR::MemorySegment &createSegmentFromArea(size_t size, void * segmentArea);

   // _systemSegmentSize is only to be written once in the constructor
   size_t _systemSegmentSize;
   size_t _allocationLimit;
   size_t _systemBytesAllocated;
   size_t _regionBytesAllocated;
   size_t _defaultSegmentSize;
   SegmentAllocator &_systemSegmentAllocator;

   typedef TR::typed_allocator<
      TR::reference_wrapper<TR::MemorySegment>,
      RawAllocator
      > SystemSegmentDequeAllocator;

   std::deque<
      TR::reference_wrapper<TR::MemorySegment>,
      SystemSegmentDequeAllocator
      > _systemSegments;

   typedef TR::typed_allocator<
      TR::MemorySegment,
      RawAllocator
      > SegmentSetAllocator;

   std::set<
      TR::MemorySegment,
      std::less< TR::MemorySegment >,
      SegmentSetAllocator
      > _segments;

   typedef TR::typed_allocator<
      TR::reference_wrapper<TR::MemorySegment>,
      RawAllocator
      > FreeSegmentDequeAllocator;

   std::deque<
      TR::reference_wrapper<TR::MemorySegment>,
      FreeSegmentDequeAllocator
      > _freeSegments;

   // Current active System segment from where memory might be allocated.
   // A segment with space larger than _systemSegmentSize is used for only one request,
   // and is to be released when the release method is invoked, e.g. when a TR::Region
   // goes out of scope. Thus _currentSystemSegment is not allowed to hold such segment.
   TR::reference_wrapper<TR::MemorySegment> _currentSystemSegment;
   };

class DebugExtSegmentAllocator : public SegmentAllocator
   {
public:
   DebugExtSegmentAllocator(
         size_t segmentSize,
         void* (*dbgMalloc)(uintptrj_t size, void *originalAddress),
         void  (*dbgFree)(void *addr)
      );

   virtual TR::MemorySegment &allocate(size_t size);
   virtual void  deallocate(TR::MemorySegment &segment) throw();
   size_t bytesAllocated() const throw();

private:
   void* (*_dbgMalloc)(uintptrj_t size, void *originalAddress);
   void  (*_dbgFree)(void *addr);
   size_t _defaultSegmentSize;
   };

}


#endif // J9_TEST_SEGMENT_ALLOCATOR
