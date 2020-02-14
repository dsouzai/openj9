#ifndef J9_TEST_RAW_ALLOCATOR
#define J9_TEST_RAW_ALLOCATOR

#include "j9.h"
#include "env/OMRTestRawAllocator.hpp"
#undef min
#undef max

extern "C" {
struct J9JavaVM;
}

namespace TestAlloc {

class J9RA : public RawAllocator
   {
public:
   J9RA(J9JavaVM *javaVM) :
      _javaVM(javaVM)
      {
      }

   J9RA(const RawAllocator &other) :
      // we have to assume that we're getting passed in another J9RA...
      _javaVM(reinterpret_cast<const J9RA &>(other)._javaVM)
      {
      }

   virtual void *allocate(size_t size, const std::nothrow_t tag, void * hint = 0) throw()
      {
      PORT_ACCESS_FROM_JAVAVM(_javaVM);
      return j9mem_allocate_memory(size, J9MEM_CATEGORY_JIT);
      }

   virtual void * allocate(size_t size, void * hint = 0)
      {
      void * allocated = allocate(size, std::nothrow, hint);
      if (!allocated) throw std::bad_alloc();
      return allocated;
      }

   virtual void deallocate(void * p) throw()
      {
      PORT_ACCESS_FROM_JAVAVM(_javaVM);
      j9mem_free_memory(p);
      }

   virtual void deallocate(void * p, const size_t size) throw()
      {
      PORT_ACCESS_FROM_JAVAVM(_javaVM);
      j9mem_free_memory(p);
      }

   private:



      J9JavaVM * _javaVM;
   };

}

#endif // J9_TEST_RAW_ALLOCATOR
