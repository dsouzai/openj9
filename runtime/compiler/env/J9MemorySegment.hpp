#ifndef J9_MEMORY_SEGMENT
#define J9_MEMORY_SEGMENT

#include "j9.h"
#include "env/MemorySegment.hpp"

namespace TestAlloc {

class TRJ9MemorySegment : public TR::MemorySegment
   {
public:
   TRJ9MemorySegment(void * const segment, size_t const size, J9MemorySegment *j9memorySegment) :
      MemorySegment(segment, size),
      _j9memorySegment(j9memorySegment)
      {}
   TRJ9MemorySegment(const TRJ9MemorySegment &other) :
      MemorySegment(other),
      _j9memorySegment(other._j9memorySegment)
      {}
   J9MemorySegment *getJ9MemorySegment() { return _j9memorySegment; }
private :
   J9MemorySegment *_j9memorySegment;
   };

}

#endif // J9_MEMORY_SEGMENT
