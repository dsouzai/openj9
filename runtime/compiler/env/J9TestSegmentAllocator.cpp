#if defined(NEW_MEMORY)

#include <algorithm>
#include "infra/Assert.hpp"
#include "env/J9TestRawAllocator.hpp"
#include "env/J9TestSegmentAllocator.hpp"
#include "env/J9MemorySegment.hpp"
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "control/CompilationRuntime.hpp"
#include "OMR/Bytes.hpp"

// TestAlloc::J9SA

TestAlloc::J9SA::J9SA(int32_t segmentType, J9JavaVM &javaVM, TestAlloc::RawAllocator &rawAllocator) throw() :
   _rawAllocator(rawAllocator),
   _segmentType(segmentType),
   _javaVM(javaVM)
   {
   TR_ASSERT(((pageSize() & (pageSize()-1)) == 0), "Page size is not a power of 2, %llu", static_cast<unsigned long long>(pageSize()) );
   }

TestAlloc::J9SA::~J9SA() throw()
   {
   }

TR::MemorySegment &
TestAlloc::J9SA::allocate(const size_t segmentSize)
   {
   size_t const alignedSize = pageAlign(segmentSize);

   // If low on physical memory we may want to return NULL when allocating scratch segments
   if (_segmentType & MEMORY_TYPE_JIT_SCRATCH_SPACE)
      {
      bool incomplete;
      TR::CompilationInfo *compInfo = TR::CompilationInfo::get(_javaVM.jitConfig);
      uint64_t freePhysicalMemory = compInfo->computeAndCacheFreePhysicalMemory(incomplete, 20);
      if (freePhysicalMemory != OMRPORT_MEMINFO_NOT_AVAILABLE && !incomplete)
         {
         if (freePhysicalMemory < (TR::Options::getSafeReservePhysicalMemoryValue() + segmentSize))
            {
            // Set a flag that will determine one compilation thread to suspend itself
            // Even if multiple threads enter his path we still want to suspend only
            // one thread, not several. This is why we use a flag and not a counter.
            //
            // We allow a small race condition: it is possible that between the test
            // for available physical memory and setting of the flag below, another
            // compilation thread has suspended itself and reset the flag. The code
            // below is going to set the flag again, possibly resulting into two
            // compilation threads being suspended. This is still fine, because, if
            // needed, a new compilation thread will be activated when a compilation
            // request is queued
            compInfo->setSuspendThreadDueToLowPhysicalMemory(true);
            throw std::bad_alloc();
            }
         }
      }
   J9MemorySegment * newSegment =
      _javaVM.internalVMFunctions->allocateMemorySegment(
         &_javaVM,
         alignedSize,
         _segmentType,
         J9MEM_CATEGORY_JIT
      );
   TR_ASSERT(
      !newSegment || (newSegment->heapAlloc == newSegment->heapBase),
      "Segment @ %p { heapBase: %p, heapAlloc: %p, heapTop: %p } is stale",
      newSegment,
      newSegment->heapBase,
      newSegment->heapAlloc,
      newSegment->heapTop
      );
   preventAllocationOfBTLMemory(newSegment, &_javaVM, _segmentType);
   if (!newSegment)
      throw std::bad_alloc();

   // The javacore will display that the entire segment is used
   newSegment->heapAlloc = newSegment->heapTop;

   TR::MemorySegment *segment = new (_rawAllocator) TestAlloc::TRJ9MemorySegment(newSegment->heapBase, alignedSize, newSegment);
   if (!segment)
      throw std::bad_alloc();
   return *segment;
   }

void
TestAlloc::J9SA::deallocate(TR::MemorySegment &segment)
   {
   // reinterpret_cast is fine becuase we're in OpenJ9 and hence know that we're dealing with a TRJ9MemorySegment
   J9MemorySegment *j9memorySegment = reinterpret_cast<TestAlloc::TRJ9MemorySegment &>(segment).getJ9MemorySegment();
   _javaVM.internalVMFunctions->freeMemorySegment(&_javaVM, j9memorySegment, TRUE);
   _rawAllocator.deallocate(&segment);
   }

size_t
TestAlloc::J9SA::pageSize() throw()
   {
   PORT_ACCESS_FROM_JAVAVM(&_javaVM);
   static const size_t pageSize = j9vmem_supported_page_sizes()[0];
   return pageSize;
   }

size_t
TestAlloc::J9SA::pageAlign(const size_t requestedSize) throw()
   {
   size_t const pageSize = this->pageSize();
   size_t alignedSize = OMR::align(requestedSize, pageSize);
   return alignedSize;
   }

void
TestAlloc::J9SA::preventAllocationOfBTLMemory(J9MemorySegment * &segment, J9JavaVM * javaVM, int32_t segmentType)
   {
#if  defined(J9ZOS390)
   // Special code for zOS. If we allocated BTL memory (first 16MB), then we must
   // release this segment, failing the compilation and forcing to use only one compilation thread
   if (TR::Options::getCmdLineOptions()->getOption(TR_DontAllocateScratchBTL) &&
      segment && ((uintptrj_t)(segment->heapBase) < (uintptrj_t)(1 << 24)))
      {
      // If applicable, reduce the number of compilation threads to 1
      TR::CompilationInfo * compInfo = TR::CompilationInfo::get();
      if (compInfo)
         {
         if (!compInfo->getRampDownMCT())
            {
            compInfo->setRampDownMCT();
            if (TR::Options::getCmdLineOptions()->getVerboseOption(TR_VerboseCompilationThreads))
               {
               TR_VerboseLog::writeLineLocked(TR_Vlog_INFO, "t=%u setRampDownMCT because JIT allocated BTL memory", (uint32_t)compInfo->getPersistentInfo()->getElapsedTime());
               }
            }
         else
            {
            // Perhaps we should consider lowering the compilation aggressiveness
            if (!TR::Options::getAOTCmdLineOptions()->getOption(TR_NoOptServer))
               {
               TR::Options::getAOTCmdLineOptions()->setOption(TR_NoOptServer);
               }
            if (!TR::Options::getJITCmdLineOptions()->getOption(TR_NoOptServer))
               {
               TR::Options::getJITCmdLineOptions()->setOption(TR_NoOptServer);
               }
            }

         // For scratch memory refuse to return memory below the line. Free the segment and let the compilation fail
         // Compilation will be retried at lower opt level.
         if (segmentType & MEMORY_TYPE_VIRTUAL)
            {
            // We should not reject requests coming from hooks. Test if this is a comp thread
            if (compInfo->useSeparateCompilationThread())
               {
               J9VMThread *crtVMThread = javaVM->internalVMFunctions->currentVMThread(javaVM);
               if (compInfo->getCompInfoForThread(crtVMThread))
                  {
                  javaVM->internalVMFunctions->freeMemorySegment(javaVM, segment, TRUE);
                  segment = NULL;
                  }
               }
            }
         }
      }
#endif // defined(J9ZOS390)
   }


// TestAlloc::J9SegmentCache

TestAlloc::J9SegmentCache::J9SegmentCache(size_t cachedSegmentSize, SegmentAllocator &backingProvider) :
   _cachedSegmentSize(cachedSegmentSize),
   _backingProvider(backingProvider),
   _firstSegment(_backingProvider.allocate(_cachedSegmentSize)),
   _firstSegmentInUse(false),
   _firstSegmentDonated(false)
   {
   }

TestAlloc::J9SegmentCache::J9SegmentCache(J9SegmentCache &donor) :
   _cachedSegmentSize(donor._cachedSegmentSize),
   _backingProvider(donor._backingProvider),
   _firstSegment(donor._firstSegment),
   _firstSegmentInUse(false),
   _firstSegmentDonated(false)
   {
   TR_ASSERT_FATAL(donor._firstSegmentInUse == false, "Unsafe hand off between SegmentCaches");
   TR_ASSERT_FATAL(donor._firstSegmentDonated == false, "Unsafe hand off between SegmentCaches");
   donor._firstSegmentDonated = true;
   }

TestAlloc::J9SegmentCache::~J9SegmentCache()
   {
   if (!_firstSegmentDonated)
      _backingProvider.deallocate(_firstSegment);
   }

TR::MemorySegment &
TestAlloc::J9SegmentCache::allocate(size_t requiredSize)
   {
   if (_firstSegmentInUse || requiredSize > _cachedSegmentSize)
      {
      return _backingProvider.allocate(requiredSize);
      }
   _firstSegmentInUse = true;
   return _firstSegment;
   }

void
TestAlloc::J9SegmentCache::deallocate(TR::MemorySegment &segment) throw()
   {
   if ( segment == _firstSegment )
      {
      _firstSegmentInUse = false;
      }
   else
      {
      _backingProvider.deallocate(segment);
      }
   }

// TestAlloc::J9SystemSegmentProvider

TestAlloc::J9SystemSegmentProvider::J9SystemSegmentProvider(size_t defaultSegmentSize,
                                                            size_t systemSegmentSize,
                                                            size_t allocationLimit,
                                                            TestAlloc::SegmentAllocator &segmentAllocator,
                                                            TestAlloc::RawAllocator &rawAllocator) :
   _allocationLimit(allocationLimit),
   _systemBytesAllocated(0),
   _regionBytesAllocated(0),
   _defaultSegmentSize(defaultSegmentSize),
   _systemSegmentAllocator(segmentAllocator),
   _systemSegments( SystemSegmentDequeAllocator(rawAllocator) ),
   _segments(std::less< TR::MemorySegment >(), SegmentSetAllocator(rawAllocator)),
   _freeSegments( FreeSegmentDequeAllocator(rawAllocator) ),
   _currentSystemSegment( TR::ref(_systemSegmentAllocator.allocate(systemSegmentSize) ) )
   {
   TR_ASSERT(defaultSegmentSize <= systemSegmentSize, "defaultSegmentSize should be smaller than or equal to systemSegmentSize");

   // We cannot simply assign systemSegmentSize to _systemSegmentSize because:
   // We want to make sure that _currentSystemSegment is always a small system segment, i.e. its size <= _systemSegmentSize. When
   // size alignment happens in _systemSegmentAllocator.allocate, this will be violated, make it _currentSystemSegment a large
   // system segment capable of allocating large segments and small segments. A large segment and its containing system segment
   // is allocated/deallocated differently, we don't want to have a system segment whose memory is used by a mix of small segments
   // and large segments.
   //
   _systemSegmentSize = _currentSystemSegment.get().size();

   try
      {
      _systemSegments.push_back(TR::ref(_currentSystemSegment));
      }
   catch (...)
      {
      _systemSegmentAllocator.deallocate(_currentSystemSegment);
      throw;
      }
   _systemBytesAllocated += _systemSegmentSize;
   }

TestAlloc::J9SystemSegmentProvider::~J9SystemSegmentProvider() throw()
   {
   while (!_systemSegments.empty())
      {
      TR::MemorySegment &topSegment = _systemSegments.back().get();
      _systemSegments.pop_back();
      _systemSegmentAllocator.deallocate(topSegment);
      }
   }

bool
TestAlloc::J9SystemSegmentProvider::isLargeSegment(size_t segmentSize)
   {
   return segmentSize > _systemSegmentSize;
   }

TR::MemorySegment &
TestAlloc::J9SystemSegmentProvider::allocate(size_t requiredSize)
   {
   size_t const roundedSize = round(requiredSize);
   if (!_freeSegments.empty() && !(_defaultSegmentSize < roundedSize))
      {
      TR::MemorySegment &recycledSegment = _freeSegments.back().get();
      _freeSegments.pop_back();
      recycledSegment.reset();
      return recycledSegment;
      }

   if (_currentSystemSegment.get().remaining() >= roundedSize)
      {
      // Only allocate small segments from _currentSystemSegment
      TR_ASSERT(!isLargeSegment(_currentSystemSegment.get().remaining()), "_currentSystemSegment must be a small segment");
      return allocateNewSegment(roundedSize, _currentSystemSegment);
      }

   size_t systemSegmentSize = std::max(roundedSize, _systemSegmentSize);
   if (_systemBytesAllocated + systemSegmentSize > _allocationLimit )
      {
      throw std::bad_alloc();
      }

   TR::MemorySegment &newSegment = _systemSegmentAllocator.allocate(systemSegmentSize);

   TR::reference_wrapper<TR::MemorySegment> newSegmentRef = TR::ref(newSegment);

   try
      {
      _systemSegments.push_back(newSegmentRef);
      }
   catch (...)
      {
      _systemSegmentAllocator.deallocate(newSegment);
      throw;
      }
   _systemBytesAllocated += systemSegmentSize;

   // Rounded size determines when the segment is deallocated, see J9::SystemSegmentProvider::deallocate
   if (!isLargeSegment(roundedSize))
      {
      // We want to use the remaining space of _currentSystemSegment after updating it.
      // Carve its remaining space into small segments and add them to the free segment list so that we can use it later
      //
      while (_currentSystemSegment.get().remaining() >= _defaultSegmentSize)
         {
         _freeSegments.push_back( TR::ref(allocateNewSegment(_defaultSegmentSize, _currentSystemSegment) ) );
         }

      _currentSystemSegment = newSegmentRef;
      }
   else
      {
      // _currentSystemSegment should not point to any segment with space larger than _systemSegmentSize because
      // such segment cannot be reused for other requests and is to be deallocated when the deallocate method is invoked,
      // e.g. when a TR::Region goes out of scope
      //
      }

   return allocateNewSegment(roundedSize, newSegmentRef);
   }

void
TestAlloc::J9SystemSegmentProvider::deallocate(TR::MemorySegment & segment) throw()
   {
   size_t const segmentSize = segment.size();

   if (segmentSize == _defaultSegmentSize)
      {
      try
         {
         _freeSegments.push_back(TR::ref(segment));
         }
      catch (...)
         {
         /* not much we can do here except leak */
         }
      }
   else if (isLargeSegment(segmentSize))
      {
      // System segments larger than _systemSegmentSize is used to create only one segment,
      // deallocate the corresponding system segment when releasing `segment`
      for (auto i = _systemSegments.begin(); i != _systemSegments.end(); ++i)
         {
         if (i->get().base() == segment.base())
            {
            _regionBytesAllocated -= segmentSize;
            _systemBytesAllocated -= segmentSize;

            /* Removing segment from _segments */
            auto it = _segments.find(segment);
            _segments.erase(it);

            TR::MemorySegment &customSystemSegment = i->get();
            _systemSegments.erase(i);
            _systemSegmentAllocator.deallocate(customSystemSegment);
            break;
            }
         }
      }
   else
      {
      size_t const oldSegmentSize = segmentSize;
      void * const oldSegmentArea = segment.base();

      /* Removing segment from _segments */
      auto it = _segments.find(segment);
      _segments.erase(it);

      TR_ASSERT(oldSegmentSize % _defaultSegmentSize == 0, "misaligned segment");
      size_t const chunks = oldSegmentSize / _defaultSegmentSize;
      for (size_t i = 0; i < chunks; ++i)
         {
         try
            {
            createSegmentFromArea(_defaultSegmentSize, static_cast<uint8_t *>(oldSegmentArea) + (_defaultSegmentSize * i));
            }
         catch (...)
            {
            /* not much we can do here except leak */
            }
         }
      }
   }

size_t
TestAlloc::J9SystemSegmentProvider::systemBytesAllocated() const throw()
   {
   return _systemBytesAllocated;
   }

size_t
TestAlloc::J9SystemSegmentProvider::regionBytesAllocated() const throw()
   {
   return _regionBytesAllocated;
   }

size_t
TestAlloc::J9SystemSegmentProvider::bytesAllocated() const throw()
   {
   return _regionBytesAllocated;
   }

TR::MemorySegment &
TestAlloc::J9SystemSegmentProvider::allocateNewSegment(size_t size, TR::reference_wrapper<TR::MemorySegment> systemSegment)
   {
   TR_ASSERT( (size % _defaultSegmentSize) == 0, "Misaligned segment");

   void *newSegmentArea = systemSegment.get().allocate(size);
   if (!newSegmentArea) throw std::bad_alloc();

   TR::MemorySegment &newSegment = createSegmentFromArea(size, newSegmentArea);
   _regionBytesAllocated += size;

   return newSegment;
   }

TR::MemorySegment &
TestAlloc::J9SystemSegmentProvider::createSegmentFromArea(size_t size, void *newSegmentArea)
   {
   auto result = _segments.insert( TR::MemorySegment(newSegmentArea, size) );

   TR_ASSERT(result.first != _segments.end(), "Bad iterator");
   TR_ASSERT(result.second, "Insertion failed");

   return const_cast<TR::MemorySegment &>(*(result.first));
   }

size_t
TestAlloc::J9SystemSegmentProvider::allocationLimit() const throw()
   {
   return _allocationLimit;
   }

void
TestAlloc::J9SystemSegmentProvider::setAllocationLimit(size_t allocationLimit)
   {
   _allocationLimit = allocationLimit;
   }

size_t
TestAlloc::J9SystemSegmentProvider::round(size_t requestedSize)
   {
   return ( ( ( requestedSize + (_defaultSegmentSize - 1) ) / _defaultSegmentSize ) * _defaultSegmentSize );
   }

TestAlloc::DebugExtSegmentAllocator::DebugExtSegmentAllocator(
      size_t segmentSize,
      void* (*dbgMalloc)(uintptrj_t size, void *originalAddress),
      void  (*dbgFree)(void *addr)
      ) :
   _dbgMalloc(dbgMalloc),
   _dbgFree(dbgFree),
   _defaultSegmentSize(segmentSize)
   {
   }

TR::MemorySegment &
TestAlloc::DebugExtSegmentAllocator::allocate(size_t requiredSize)
   {
   size_t adjustedSize = ( ( requiredSize + (_defaultSegmentSize - 1) ) / _defaultSegmentSize ) * _defaultSegmentSize;
   void *segmentArea = _dbgMalloc(adjustedSize, 0);
   if (!segmentArea) throw std::bad_alloc();
   void *segmentAllocation = _dbgMalloc(sizeof(TR::MemorySegment), 0);
   if (!segmentAllocation) throw std::bad_alloc();
   TR::MemorySegment &newSegment = *new (segmentArea) TR::MemorySegment(segmentArea, adjustedSize);
   return newSegment;
   }

void
TestAlloc::DebugExtSegmentAllocator::deallocate(TR::MemorySegment &segment) throw()
   {
   void * segmentArea = segment.base();
   _dbgFree(&segment);
   _dbgFree(segmentArea);
   }

size_t
TestAlloc::DebugExtSegmentAllocator::bytesAllocated() const throw()
   {
   return 0;
   }

#endif //
