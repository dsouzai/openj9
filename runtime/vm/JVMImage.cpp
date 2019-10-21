/*******************************************************************************
 * Copyright (c) 2001, 2019 IBM Corp. and others
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
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "JVMImage.hpp"
#include "segment.h"
#include <errno.h>
#include <string.h>

#include <sys/mman.h> /* TODO: Change to OMRPortLibrary MMAP functionality. Currently does not allow MAP_FIXED as it is not supported in all architectures */

/* TODO: reallocation will fail so initial heap size is large (Should be PAGE_SIZE aligned) */
/* TODO: initial image size restriction will be removed once MMAP MAP_FIXED removed. see @ref JVMImage::readImageFromFile */
const UDATA JVMImage::INITIAL_IMAGE_SIZE = (1024 + 256) * 1024 * 1024;
const UDATA JVMImage::CLASS_LOADER_REMOVE_COUNT = 8;

JVMImage::JVMImage(J9PortLibrary *portLibrary, const char* ramCache) :
	_vm(NULL),
	_portLibrary(portLibrary),
	_jvmImageHeader(NULL),
	_heap(NULL),
	_invalidITable(NULL),
	_ramCache(ramCache),
	_jvmImageMonitor(NULL),
	_jvmImagePortLibrary(NULL)
{
}

void
JVMImage::setJ9JavaVM(J9JavaVM *vm)
{
	_vm = vm;
	_jvmImageHeader->vm = vm;
}

void
JVMImage::setImagePortLib(JVMImagePortLibrary *jvmImagePortLibrary)
{
	_jvmImagePortLibrary = jvmImagePortLibrary;
}

J9JavaVM *
JVMImage::getJ9JavaVM()
{
	return _vm;
}

JVMImage::~JVMImage()
{
	//PORT_ACCESS_FROM_JAVAVM(_vm);
	destroyMonitor();
	if (IS_COLD_RUN(_vm)) {
		//j9mem_free_memory((void*)_jvmImageHeader->imageAddress);
		//UDATA pageSize = j9vmem_supported_page_sizes()[0];
		munmap((void*)_jvmImageHeader, _jvmImageHeader->imageSize);
	} else {
		munmap((void*)_jvmImageHeader, _jvmImageHeader->imageAlignedAddress);
	}
}

bool
JVMImage::initializeMonitor(void)
{
	/* TODO need to free monitor */
	if (omrthread_monitor_init_with_name(&_jvmImageMonitor, 0, "JVM Image Heap Monitor") != 0) {
		return false;
	}

	return true;
}

void
JVMImage::storeInitialMethods(J9Method* cInitialStaticMethod, J9Method* cInitialSpecialMethod, J9Method* cInitialVirtualMethod)
{
	WSRP_SET(_jvmImageHeader->cInitialStaticMethod, cInitialStaticMethod);
	WSRP_SET(_jvmImageHeader->cInitialSpecialMethod, cInitialSpecialMethod);
	WSRP_SET(_jvmImageHeader->cInitialVirtualMethod, cInitialVirtualMethod);
}

void
JVMImage::setInitialMethods(J9Method** cInitialStaticMethod, J9Method** cInitialSpecialMethod, J9Method** cInitialVirtualMethod)
{
	*cInitialStaticMethod = WSRP_GET(_jvmImageHeader->cInitialStaticMethod, J9Method*);
	*cInitialSpecialMethod = WSRP_GET(_jvmImageHeader->cInitialSpecialMethod, J9Method*);
	*cInitialVirtualMethod = WSRP_GET(_jvmImageHeader->cInitialVirtualMethod, J9Method*);
}

bool
JVMImage::initializeInvalidITable(void)
{
	_invalidITable = (J9ITable *) subAllocateMemory(sizeof(J9ITable));
	if (NULL == _invalidITable) {
		return false;
	}

	_invalidITable->interfaceClass = (J9Class *) (UDATA) 0xDEADBEEF;
	_invalidITable->depth = 0;
	_invalidITable->next = (J9ITable *) NULL;

	return true;
}

void
JVMImage::destroyMonitor(void)
{
	omrthread_monitor_destroy(_jvmImageMonitor);
}

JVMImage *
JVMImage::createInstance(J9PortLibrary *portLibrary, const char* ramCache)
{
	PORT_ACCESS_FROM_PORT(portLibrary);
	JVMImage *jvmInstance = (JVMImage *)j9mem_allocate_memory(sizeof(JVMImage), J9MEM_CATEGORY_CLASSES);
	if (NULL != jvmInstance) {
		new(jvmInstance) JVMImage(portLibrary, ramCache);
	}

	return jvmInstance;
}

ImageRC
JVMImage::setupWarmRun(void)
{
	if (!readImageFromFile()) {
		return IMAGE_ERROR;
	}

	if (!initializeMonitor()) {
		return IMAGE_ERROR;
	}

	if (!restoreJ9JavaVMStructures()) {
		return IMAGE_ERROR;
	}

	return IMAGE_OK;
}

ImageRC
JVMImage::setupColdRun(void)
{
	if (NULL == allocateImageMemory(INITIAL_IMAGE_SIZE)) {
		return IMAGE_ERROR;
	}

	if (NULL == initializeHeap()) {
		return IMAGE_ERROR;
	}

	if (!initializeMonitor()) {
		return IMAGE_ERROR;
	}

	if (!allocateImageTableHeaders()) {
		return IMAGE_ERROR;
	}

	if ((NULL == allocateTable(getClassLoaderTable(), INITIAL_CLASSLOADER_TABLE_SIZE))
		|| (NULL == allocateTable(getClassTable(), INITIAL_CLASS_TABLE_SIZE))
		|| (NULL == allocateTable(getClassPathEntryTable(), INITIAL_CLASSPATH_TABLE_SIZE))) {
		return IMAGE_ERROR;
	}

	if (!initializeInvalidITable()) {
		return IMAGE_ERROR;
	}

	return IMAGE_OK;
}

void *
JVMImage::allocateImageMemory(UDATA size)
{
	PORT_ACCESS_FROM_PORT(_portLibrary);
	UDATA pageSize = j9vmem_supported_page_sizes()[0];
	//void *imageAddress = j9mem_allocate_memory(size + pageSize, J9MEM_CATEGORY_CLASSES);
        void *imageAddress = mmap(
		0,
		size + pageSize,
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	printf ("\n\n\nD_QWERTY: imageAddress = %p, errno=%s\n\n\n", imageAddress, strerror(errno));

	if (NULL == imageAddress || (void *)-1 == imageAddress) {
		return NULL;
	}

	//_jvmImageHeader = (JVMImageHeader *) ROUND_UP_TO_POWEROF2((UDATA)imageAddress, pageSize);
	_jvmImageHeader = (JVMImageHeader *)imageAddress;
	_jvmImageHeader->imageAddress = (uintptr_t)imageAddress;
	_jvmImageHeader->imageAlignedAddress = (uintptr_t)_jvmImageHeader;
	_jvmImageHeader->imageSize = size + pageSize;

	return _jvmImageHeader;
}

/* TODO: Currently reallocating image memory is broken since all references to memory inside of heap will fail (i.e. vm->classLoadingPool) */
/* TODO: Reallocation will be done once random memory loading feature is completed */
void *
JVMImage::reallocateImageMemory(UDATA size)
{
	return NULL;
}

void * 
JVMImage::initializeHeap(void)
{
	PORT_ACCESS_FROM_PORT(_portLibrary);
	UDATA pageSize = j9vmem_supported_page_sizes()[0];
	
	_heap = j9heap_create((J9Heap *)(_jvmImageHeader + 1), ROUND_DOWN_TO_POWEROF2(JVMImage::INITIAL_IMAGE_SIZE - sizeof(_jvmImageHeader), pageSize), 0);
	if (NULL == _heap) {
		return NULL;
	}

	return _heap;
}

bool
JVMImage::allocateImageTableHeaders(void)
{
	WSRP_SET(_jvmImageHeader->classLoaderTable, subAllocateMemory(sizeof(ImageTableHeader)));
	WSRP_SET(_jvmImageHeader->classTable, subAllocateMemory(sizeof(ImageTableHeader)));
	WSRP_SET(_jvmImageHeader->classPathEntryTable, subAllocateMemory(sizeof(ImageTableHeader)));

	if ((0 == _jvmImageHeader->classLoaderTable)
		|| (0 == _jvmImageHeader->classTable)
		|| (0 == _jvmImageHeader->classPathEntryTable)) {
		return false;
	}

	return true;
}

void * 
JVMImage::allocateTable(ImageTableHeader *table, uintptr_t tableSize)
{
	void *firstSlot = subAllocateMemory(tableSize * sizeof(UDATA));
	if (NULL == firstSlot) {
		return NULL;
	}

	table->tableSize = tableSize;
	table->currentSize = 0;
	WSRP_SET(table->tableHead, firstSlot);
	WSRP_SET(table->tableTail, firstSlot);
	
	return firstSlot;
}

void *
JVMImage::reallocateTable(ImageTableHeader *table, uintptr_t tableSize)
{
	UDATA *firstSlot = WSRP_GET(table->tableHead, UDATA*);
	firstSlot = (UDATA *)reallocateMemory((void *)firstSlot, tableSize * sizeof(UDATA));
	if (NULL == firstSlot) {
		return NULL;
	}

	table->tableSize = tableSize;
	WSRP_SET(table->tableHead, firstSlot);
	WSRP_SET(table->tableTail, firstSlot + table->currentSize - 1); /* Reallocation will never be called if current size = 0 */

	return firstSlot;
}

void *
JVMImage::subAllocateMemory(uintptr_t byteAmount)
{
	Trc_VM_SubAllocateImageMemory_Entry(_jvmImageHeader, byteAmount);

	omrthread_monitor_enter(_jvmImageMonitor);

	OMRPortLibrary *portLibrary = IMAGE_OMRPORT_FROM_JVMIMAGEPORT(_jvmImagePortLibrary);
	void *memStart = portLibrary->heap_allocate(portLibrary, _heap, byteAmount);	
	/* image memory is not large enough and needs to be reallocated */
	if (NULL == memStart) {
		reallocateImageMemory(_jvmImageHeader->imageSize * 2 + byteAmount); /* guarantees size of heap will accomodate byteamount */
		memStart = portLibrary->heap_allocate(portLibrary, _heap, byteAmount);
	}

	omrthread_monitor_exit(_jvmImageMonitor);

	Trc_VM_SubAllocateImageMemory_Exit(memStart);

	return memStart;
}

void*
JVMImage::reallocateMemory(void *address, uintptr_t byteAmount)
{
	omrthread_monitor_enter(_jvmImageMonitor);

	OMRPortLibrary *portLibrary = IMAGE_OMRPORT_FROM_JVMIMAGEPORT(_jvmImagePortLibrary);
	void *memStart = portLibrary->heap_reallocate(portLibrary, _heap, address, byteAmount);
	/* image memory is not large enough and needs to be reallocated */
	if (NULL == memStart) {
		reallocateImageMemory(_jvmImageHeader->imageSize * 2 + byteAmount); /* guarantees size of heap will accomodate byteamount */
		memStart = portLibrary->heap_reallocate(portLibrary, _heap, address, byteAmount);
	}

	omrthread_monitor_exit(_jvmImageMonitor);

	return memStart;
}

void
JVMImage::freeSubAllocatedMemory(void *address)
{
	omrthread_monitor_enter(_jvmImageMonitor);

	OMRPortLibrary *portLibrary = IMAGE_OMRPORT_FROM_JVMIMAGEPORT(_jvmImagePortLibrary);
	portLibrary->heap_free(portLibrary, _heap, address);

	omrthread_monitor_exit(_jvmImageMonitor);
}

void
JVMImage::registerEntryInTable(ImageTableHeader *table, UDATA entry)
{
	Trc_VM_RegisterInTable_Entry(table, table->currentSize, *(WSRP_GET(table->tableTail, UDATA*)), entry);

	/* table is not large enough and needs to be reallocated */
	if (table->currentSize >= table->tableSize) {
		reallocateTable(table, table->tableSize * 2); /*TODO: Introduce error handling for table reallocation */
	}

	UDATA *tail = WSRP_GET(table->tableTail, UDATA*);

	/* initial state of every table has currentSize set to 0 and tail/heap pointer at same location */
	if (0 != table->currentSize) {
		tail += 1;
		WSRP_SET(table->tableTail, tail);
	}

	*(tail) = entry;
	table->currentSize += 1;

	Trc_VM_RegisterInTable_Exit(table, table->currentSize, *(WSRP_GET(table->tableTail, UDATA*)));
}

void
JVMImage::deregisterEntryInTable(ImageTableHeader *table, UDATA entry)
{
	Trc_VM_DeregisterInTable_Entry(table, table->currentSize, *(WSRP_GET(table->tableTail, UDATA*)), entry);

	UDATA *tail = WSRP_GET(table->tableTail, UDATA*);
	UDATA *temp = findEntryLocationInTable(table, entry);

	if (NULL != temp) {
		while (temp != tail) {
			*temp = *(temp + 1);
			temp += 1;
		}
		*temp = 0;

		/*tail should point to first element when empty*/
		if (table->currentSize != 1) {
			tail -= 1;
		}
		table->currentSize -= 1;
	}
	
	WSRP_SET(table->tableTail, tail);

	Trc_VM_DeregisterInTable_Exit(table, table->currentSize, *(WSRP_GET(table->tableTail, UDATA*)));
}

void
JVMImage::setClassLoader(J9ClassLoader *classLoader, uint32_t classLoaderCategory)
{
	/* TODO: Function will change when hash table is created and there would be no need for setting specific class loaders */
	if (IS_SYSTEM_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		WSRP_SET(_jvmImageHeader->systemClassLoader, classLoader);
	}

	if (IS_APP_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		WSRP_SET(_jvmImageHeader->appClassLoader, classLoader);
	}

	if (IS_EXTENSION_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		WSRP_SET(_jvmImageHeader->extensionClassLoader, classLoader);
	}
}

void
JVMImage::setClassLoaderBlocks(J9JavaVM *vm)
{
	_jvmImageHeader->savedJavaVMStructs.classLoaderBlocks = vm->classLoaderBlocks;
}

J9Pool*
JVMImage::getClassLoaderBlocks()
{
	return _jvmImageHeader->savedJavaVMStructs.classLoaderBlocks;
}

void
JVMImage::setClassMemorySegments(J9JavaVM *vm)
{
	_jvmImageHeader->savedJavaVMStructs.classMemorySegments = vm->classMemorySegments;
}

J9MemorySegmentList*
JVMImage::getClassMemorySegments()
{
	return _jvmImageHeader->savedJavaVMStructs.classMemorySegments;
}

void
JVMImage::setMemorySegments(J9JavaVM *vm)
{
	J9MemorySegmentList *oldMemorySegmentList = _vm->memorySegments;
	J9MemorySegment *currentSegment = oldMemorySegmentList->nextSegment;
	J9MemorySegmentList *newMemorySegmentList = _vm->internalVMFunctions->allocateMemorySegmentList(vm, 10, OMRMEM_CATEGORY_VM);

	if (NULL == newMemorySegmentList) {
		_vm->internalVMFunctions->setNativeOutOfMemoryError(currentVMThread(_vm), 0 ,0);
		goto done;
	}

	/* copy all the persisted memory segments into the new list */
	while (NULL != currentSegment) {
		J9MemorySegment *nextSegment = currentSegment->nextSegment;
		if (IS_SEGMENT_PERSISTED(currentSegment)) {
			J9MemorySegment *newSegment = allocateMemorySegmentListEntry(newMemorySegmentList);

			newSegment->type = currentSegment->type;
			newSegment->size = currentSegment->size;
			newSegment->baseAddress = currentSegment->baseAddress;
			newSegment->heapBase = currentSegment->heapBase;
			newSegment->heapTop = currentSegment->heapTop;
			newSegment->heapAlloc = currentSegment->heapAlloc;

			newMemorySegmentList->totalSegmentSize += newSegment->size;
			if (0 != (newMemorySegmentList->flags & MEMORY_SEGMENT_LIST_FLAG_SORT)) {
				avl_insert(&newMemorySegmentList->avlTreeData, (J9AVLTreeNode *) newSegment);
			}

			J9_MEMORY_SEGMENT_LINEAR_LINKED_LIST_REMOVE(oldMemorySegmentList->nextSegment, currentSegment);
			pool_removeElement(oldMemorySegmentList->segmentPool, currentSegment);


		}
		currentSegment = nextSegment;
	}

	_jvmImageHeader->savedJavaVMStructs.memorySegments = newMemorySegmentList;
done:
	return;
}

J9MemorySegmentList*
JVMImage::getMemorySegments()
{
	return _jvmImageHeader->savedJavaVMStructs.memorySegments;
}

void
JVMImage::setPrimitiveAndArrayClasses(J9JavaVM* javaVM)
{
	J9Class **saveClasses = &this->_jvmImageHeader->savedJavaVMStructs.voidReflectClass;
	J9Class **liveClasses = &javaVM->voidReflectClass;

	for (int i = 0; i < 17; i++) {
		saveClasses[i] = liveClasses[i];
	}
}

bool
JVMImage::restorePrimitiveAndArrayClasses(void)
{
	J9Class **saveClasses = &this->_jvmImageHeader->savedJavaVMStructs.voidReflectClass;
	J9Class **liveClasses = &_vm->voidReflectClass;
	bool rc = true;

	for (int i = 0; i < 17; i++) {
		liveClasses[i] = saveClasses[i];
	}

	return rc;
}




void
JVMImage::fixupVMStructures(void)
{
	UDATA omrRuntimeOffset = ROUND_UP_TO_POWEROF2(sizeof(J9JavaVM), 8);
	UDATA omrVMOffset = ROUND_UP_TO_POWEROF2(omrRuntimeOffset + sizeof(OMR_Runtime), 8);
	UDATA vmAllocationSize = omrVMOffset + sizeof(OMR_VM);
	J9JITConfig *jitConfig = _vm->jitConfig;
	memset(_vm, 0, vmAllocationSize);
	_vm->jitConfig = jitConfig;
}

void
JVMImage::fixupClassLoaders(void)
{
	J9ClassLoader *currentClassLoader = (J9ClassLoader *) imageTableStartDo(getClassLoaderTable());
	
	while (NULL != currentClassLoader) {
		currentClassLoader->sharedLibraries = NULL;
		currentClassLoader->librariesHead = NULL;
		currentClassLoader->librariesTail = NULL;
		currentClassLoader->classLoaderObject = NULL;
		currentClassLoader->unloadLink = NULL;
		currentClassLoader->gcLinkNext = NULL;
		currentClassLoader->gcLinkPrevious = NULL;
		currentClassLoader->gcFlags = 0;
		currentClassLoader->gcRememberedSet = 0;
		currentClassLoader->gcThreadNotification = NULL;
		currentClassLoader->jniIDs = NULL;
		currentClassLoader->jniRedirectionBlocks = NULL;
		currentClassLoader->gcRememberedSet = 0;
		currentClassLoader->jitMetaDataList = NULL;

		currentClassLoader = (J9ClassLoader *) imageTableNextDo(getClassLoaderTable());
	}
}

void
JVMImage::removeUnpersistedClassLoaders(void)
{
	/* wont need this once we support all classloaders */
	pool_state classLoaderWalkState = {0};
	UDATA numOfClassLoaders = pool_numElements(_vm->classLoaderBlocks);
	U_8 buf[CLASS_LOADER_REMOVE_COUNT * sizeof(J9ClassLoader*)];
	J9ClassLoader **removeLoaders = (J9ClassLoader **) buf;
	J9VMThread *vmThread = currentVMThread(_vm);
	UDATA count = 0;

	if (CLASS_LOADER_REMOVE_COUNT < numOfClassLoaders) {
		PORT_ACCESS_FROM_PORT(_portLibrary);
		removeLoaders = (J9ClassLoader **) j9mem_allocate_memory(numOfClassLoaders * sizeof(J9ClassLoader*), J9MEM_CATEGORY_CLASSES);
		if (NULL == removeLoaders) {
			/* TODO error handling for fixups */
			Assert_VM_unreachable();
		}
	}

	J9ClassLoader *classloader = (J9ClassLoader *) pool_startDo(_vm->classLoaderBlocks, &classLoaderWalkState);
	while (NULL != classloader) {
		if ((classloader != _vm->applicationClassLoader) && (classloader != _vm->extensionClassLoader) && (classloader != _vm->systemClassLoader)) {
			removeLoaders[count] = classloader;
			count++;
		}
		classloader = (J9ClassLoader *) pool_nextDo(&classLoaderWalkState);
	}

	for (UDATA i = 0; i < count; i++) {
		freeClassLoader(removeLoaders[i], _vm, vmThread, FALSE);
	}
}


void
JVMImage::fixupClasses(void)
{
	J9Class *currentClass = (J9Class *) imageTableStartDo(getClassTable());

	while (NULL != currentClass) {
		J9ROMClass *romClass = currentClass->romClass;
		
		if (J9ROMCLASS_IS_ARRAY(romClass)) {
			fixupArrayClass((J9ArrayClass *) currentClass);
		} else {
			fixupClass(currentClass);
		}

		/* Fixup the last ITable */
		currentClass->lastITable = (J9ITable *) currentClass->iTable;
		if (NULL == currentClass->lastITable) {
			currentClass->lastITable = JVMImage::getInvalidITable();
		}

		currentClass->classFlags &= ~J9ClassIsFlattened;

		currentClass = (J9Class *) imageTableNextDo(getClassTable());
	}
}

void
JVMImage::fixupClass(J9Class *clazz)
{
	clazz->classObject = NULL;
	fixupMethodRunAddresses(clazz);
	fixupConstantPool(clazz);
	clazz->initializeStatus = J9ClassInitNotInitialized;
	clazz->jniIDs = NULL;
	clazz->replacedClass = NULL;
	clazz->gcLink = NULL;
	clazz->jitMetaDataList = NULL;

	UDATA totalStaticSlots = totalStaticSlotsForClass(clazz->romClass);
	memset(clazz->ramStatics, 0, totalStaticSlots * sizeof(UDATA));

	UDATA i;
	if (NULL != clazz->staticSplitMethodTable) {
		for (i = 0; i < clazz->romClass->staticSplitMethodRefCount; i++) {
			clazz->staticSplitMethodTable[i] = (J9Method*)_vm->initialMethods.initialStaticMethod;
		}
	}
	if (NULL != clazz->specialSplitMethodTable) {
		for (i = 0; i < clazz->romClass->specialSplitMethodRefCount; i++) {
			clazz->specialSplitMethodTable[i] = (J9Method*)_vm->initialMethods.initialSpecialMethod;
		}
	}
	if (NULL != clazz->callSites) {
		memset(clazz->callSites, 0, sizeof(UDATA) * clazz->romClass->callSiteCount);
	}

	if (NULL != clazz->methodTypes) {
		memset(clazz->methodTypes, 0, sizeof(UDATA) * clazz->romClass->methodTypeCount);
	}

	if (NULL != clazz->varHandleMethodTypes) {
		memset(clazz->varHandleMethodTypes, 0, sizeof(UDATA) * clazz->romClass->varHandleMethodTypeCount);
	}
}

void
JVMImage::fixupJITVtable(J9Class *ramClass)
{
	J9VMThread *vmThread = currentVMThread(_vm);
	J9JITConfig *jitConfig = _vm->jitConfig;
	J9VTableHeader *vTableAddress = J9VTABLE_HEADER_FROM_RAM_CLASS(ramClass);
	if (jitConfig != NULL) {
		UDATA *vTableWriteCursor = JIT_VTABLE_START_ADDRESS(ramClass);
		UDATA vTableWriteIndex = vTableAddress->size;
		J9Method **vTableReadCursor;
		if (vTableWriteIndex != 0) {
			if ((jitConfig->runtimeFlags & J9JIT_TOSS_CODE) != 0) {
				vTableWriteCursor -= vTableWriteIndex;
			} else {

				vTableReadCursor = J9VTABLE_FROM_HEADER(vTableAddress);
				for (; vTableWriteIndex > 0; vTableWriteIndex--) {
					J9Method *currentMethod = *vTableReadCursor;
					fillJITVTableSlot(vmThread, vTableWriteCursor, currentMethod);

					vTableReadCursor++;
					vTableWriteCursor--;
				}
			}
		}
	}
}

void
JVMImage::fixupArrayClass(J9ArrayClass *clazz)
{
	clazz->classObject = NULL;
	fixupConstantPool((J9Class *) clazz);
	clazz->initializeStatus = J9ClassInitSucceeded;
	clazz->jniIDs = NULL;
	clazz->replacedClass = NULL;
	clazz->callSites = NULL;
	clazz->methodTypes = NULL;
	clazz->varHandleMethodTypes = NULL;
	clazz->gcLink = NULL;

	UDATA i;
	if (NULL != clazz->staticSplitMethodTable) {
		for (i = 0; i < clazz->romClass->staticSplitMethodRefCount; i++) {
			clazz->staticSplitMethodTable[i] = (J9Method*)_vm->initialMethods.initialStaticMethod;
		}
	}
	if (NULL != clazz->specialSplitMethodTable) {
		for (i = 0; i < clazz->romClass->specialSplitMethodRefCount; i++) {
			clazz->specialSplitMethodTable[i] = (J9Method*)_vm->initialMethods.initialSpecialMethod;
		}
	}
}

void
JVMImage::fixupMethodRunAddresses(J9Class *ramClass)
{
	J9VMThread *vmThread = currentVMThread(_vm);
	J9ROMClass *romClass = ramClass->romClass;

	/* for all ramMethods call initializeMethodRunAdress for special methods */
	if (romClass->romMethodCount != 0) {
		UDATA i;
		UDATA count = romClass->romMethodCount;
		J9Method *ramMethod = ramClass->ramMethods;
		for (i = 0; i < count; i++) {
			initializeMethodRunAddress(vmThread, ramMethod);
			ramMethod++;
		}
	}
}

void
JVMImage::fixupConstantPool(J9Class *ramClass)
{
	J9VMThread *vmThread = currentVMThread(_vm);
	J9ROMClass *romClass = ramClass->romClass;
	J9ConstantPool *ramCP = ((J9ConstantPool *) ramClass->ramConstantPool);
	J9ConstantPool *ramCPWithoutHeader = ramCP + 1;
	UDATA ramCPCount = romClass->ramConstantPoolCount;
	UDATA ramCPCountWithoutHeader = ramCPCount - 1;
	
	/* Zero the ramCP and initialize constant pool */
	if (ramCPCount != 0) {
		memset(ramCPWithoutHeader, 0, ramCPCountWithoutHeader * sizeof(J9RAMConstantPoolItem));
		internalRunPreInitInstructions(ramClass, vmThread);
	}
}

void
JVMImage::fixupClassPathEntries(void)
{
	J9ClassPathEntry *currentCPEntry = (J9ClassPathEntry *) imageTableStartDo(getClassPathEntryTable());

	while (NULL != currentCPEntry) {
		currentCPEntry->type = CPE_TYPE_UNKNOWN;
		currentCPEntry->status = 0;

		currentCPEntry = (J9ClassPathEntry *) imageTableNextDo(getClassPathEntryTable());
	}
}

bool
JVMImage::readImageFromFile(void)
{
	Trc_VM_ReadImageFromFile_Entry(_heap, _ramCache);

	OMRPORT_ACCESS_FROM_OMRPORT(&_portLibrary->omrPortLibrary);

	intptr_t fileDescriptor = omrfile_open(_ramCache, EsOpenRead | EsOpenWrite, 0444);
	if (-1 == fileDescriptor) {
		printf("falied to open ramCache file file=%s \n", _ramCache);
		return false;
	}

	/* Read image header size and location then mmap the rest of the image (heap) into memory */
	JVMImageHeader imageHeaderBuffer;
	if (-1 == omrfile_read(fileDescriptor, (void *)&imageHeaderBuffer, sizeof(JVMImageHeader))) {
		return false;
	}

	/* TODO: currently mmap uses sys/mman.h and MAP_FIXED. Once random memory loading is complete it will change to omr mmap without MAP_FIXED */
	_jvmImageHeader = (JVMImageHeader *)mmap(
		(void *)imageHeaderBuffer.imageAlignedAddress,
		imageHeaderBuffer.imageSize,
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fileDescriptor, 0);
	if (-1 == (IDATA)_jvmImageHeader) {
		return false;
	}
	if (imageHeaderBuffer.imageAlignedAddress != (UDATA) _jvmImageHeader) {
		return false;
	}
	_heap = (J9Heap *)(_jvmImageHeader + 1);
	_vm = _jvmImageHeader->vm;
	_vm->ramStateFilePath = (char*) _ramCache;

	omrfile_close(fileDescriptor);

	Trc_VM_ReadImageFromFile_Exit();

	return true;
}

/**
 * Will be removed once the J9JavaVM struct is fully persisted
 */
void
JVMImage::saveJ9JavaVMStructures(void)
{
	setClassLoaderBlocks(this->_vm);
	setClassMemorySegments(this->_vm);
	setMemorySegments(this->_vm);
	setPrimitiveAndArrayClasses(this->_vm);
}

/**
 * Will be removed once the J9JavaVM struct is fully persisted
 */
bool
JVMImage::restoreJ9JavaVMStructures(void)
{
	bool success = true;
	fixupVMStructures();

	J9MemorySegmentList *segmentList = getClassMemorySegments();
	if (omrthread_monitor_init_with_name(&segmentList->segmentMutex, 0, "VM class mem segment list")) {
		success = false;
	}

	segmentList = getMemorySegments();
	if (omrthread_monitor_init_with_name(&segmentList->segmentMutex, 0, "VM mem segment list")) {
		success = false;
	}

	if (!restorePrimitiveAndArrayClasses()) {
		success = false;
	}

	return success;
}


bool
JVMImage::writeImageToFile(void)
{
	Trc_VM_WriteImageToFile_Entry(_heap, _ramCache);

	OMRPortLibrary *portLibrary = (OMRPortLibrary *) _portLibrary;
	OMRPORT_ACCESS_FROM_OMRPORT(portLibrary);

	intptr_t fileDescriptor = omrfile_open(_ramCache, EsOpenCreate | EsOpenCreateAlways | EsOpenWrite, 0666);
	if (-1 == fileDescriptor) {
		return false;
	}

	if ((intptr_t)_jvmImageHeader->imageSize != omrfile_write(fileDescriptor, (void *)_jvmImageHeader, _jvmImageHeader->imageSize)) {
		return false;
	}

	omrfile_close(fileDescriptor);

	Trc_VM_WriteImageToFile_Exit();

	return true;
}

void
JVMImage::teardownImage(void)
{
	removeUnpersistedClassLoaders();
	saveJ9JavaVMStructures();
	fixupClassLoaders();
	fixupClasses();
	fixupClassPathEntries();
	writeImageToFile();
}

JVMImagePortLibrary * 
setupJVMImagePortLibrary(J9PortLibrary *portLibrary)
{
	PORT_ACCESS_FROM_PORT(portLibrary);
	JVMImagePortLibrary *jvmImagePortLibrary = (JVMImagePortLibrary*)j9mem_allocate_memory(sizeof(JVMImagePortLibrary), OMRMEM_CATEGORY_PORT_LIBRARY);

	OMRPORT_ACCESS_FROM_J9PORT(portLibrary);
	memcpy(&(jvmImagePortLibrary->portLibrary), privateOmrPortLibrary, sizeof(OMRPortLibrary));
	jvmImagePortLibrary->portLibrary.mem_allocate_memory = image_mem_allocate_memory;
	jvmImagePortLibrary->portLibrary.mem_free_memory = image_mem_free_memory;
	jvmImagePortLibrary->jvmImage = NULL;

	return jvmImagePortLibrary;
}


extern "C" JVMImagePortLibrary *
initializeJVMImage(J9PortLibrary *portLibrary, BOOLEAN isColdRun, const char* ramCache)
{
	JVMImagePortLibrary *jvmImagePortLibrary = setupJVMImagePortLibrary(portLibrary);
	JVMImage *jvmImage = JVMImage::createInstance(portLibrary, ramCache);
	if (NULL == jvmImagePortLibrary || NULL == jvmImage) {
		goto _error;
	}

	jvmImagePortLibrary->jvmImage = jvmImage;
	((JVMImage *)jvmImagePortLibrary->jvmImage)->setImagePortLib(jvmImagePortLibrary);

	if (isColdRun && (IMAGE_ERROR == jvmImage->setupColdRun())) {
		goto _error;
	}

	if(!isColdRun && (IMAGE_ERROR == jvmImage->setupWarmRun())) {
		goto _error;
	}

	return jvmImagePortLibrary;

_error:
	shutdownJVMImage(jvmImagePortLibrary);
	jvmImage->destroyMonitor();
	return NULL;
}

extern "C" void
setupJVMImage(JVMImagePortLibrary *jvmImagePortLibrary, J9JavaVM *javaVM)
{
	((JVMImage *)jvmImagePortLibrary->jvmImage)->setJ9JavaVM(javaVM);
	javaVM->jvmImagePortLibrary = jvmImagePortLibrary;
}

extern "C" J9JavaVM *
getJ9JavaVMFromJVMImage(JVMImagePortLibrary *jvmImagePortLibrary)
{
	return ((JVMImage *)jvmImagePortLibrary->jvmImage)->getJ9JavaVM();
}

extern "C" void
registerClassLoader(J9JavaVM *javaVM, J9ClassLoader *classLoader, uint32_t classLoaderCategory)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	Trc_VM_ImageClassLoaderRegister(classLoader, classLoaderCategory);

	jvmImage->registerEntryInTable(jvmImage->getClassLoaderTable(), (UDATA)classLoader);
	/* TODO: Currently only three class loaders are stored */
	jvmImage->setClassLoader(classLoader, classLoaderCategory);
}

extern "C" J9Pool*
getClassLoaderBlocks(J9JavaVM *javaVM)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	return jvmImage->getClassLoaderBlocks();
}

extern "C" J9MemorySegmentList *
getClassMemorySegmentList(J9JavaVM *javaVM)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	return jvmImage->getClassMemorySegments();
}

extern "C" J9MemorySegmentList *
getMemorySegmentList(J9JavaVM *javaVM)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	return jvmImage->getMemorySegments();
}

extern "C" void
registerClass(J9JavaVM *javaVM, J9Class *clazz)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	J9ROMClass *romClass = clazz->romClass;

	J9UTF8 *className = J9ROMCLASS_CLASSNAME(romClass);
	Trc_VM_ImageClassRegister(javaVM->mainThread, clazz, J9UTF8_DATA(className));

	jvmImage->registerEntryInTable(jvmImage->getClassTable(), (UDATA)clazz);
}

extern "C" void
registerCPEntry(J9JavaVM *javaVM, J9ClassPathEntry *cpEntry)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	Trc_VM_ImageCPEntryRegister(cpEntry, cpEntry->path);
	
	jvmImage->registerEntryInTable(jvmImage->getClassPathEntryTable(), (UDATA)cpEntry);
}

extern "C" void
deregisterClass(J9JavaVM *javaVM, J9Class *clazz)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	jvmImage->deregisterEntryInTable(jvmImage->getClassTable(), (UDATA)clazz);
}

extern "C" void
deregisterCPEntry(J9JavaVM *javaVM, J9ClassPathEntry *cpEntry)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	jvmImage->deregisterEntryInTable(jvmImage->getClassPathEntryTable(), (UDATA)cpEntry);
}

extern "C" J9ClassLoader *
findClassLoader(J9JavaVM *javaVM, uint32_t classLoaderCategory)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);
	J9ClassLoader *classLoader = NULL;
	/* TODO: Function will change when hash table is created and there would be no need for accessing specific class loaders (hardcoded) */
	if (IS_SYSTEM_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		classLoader = jvmImage->getSystemClassLoader();
	}

	if (IS_APP_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		classLoader = jvmImage->getApplicationClassLoader();
	}

	if (IS_EXTENSION_CLASSLOADER_CATEGORY(classLoaderCategory)) {
		classLoader = jvmImage->getExtensionClassLoader();
	}

	TRIGGER_J9HOOK_VM_CLASS_LOADER_CREATED(javaVM->hookInterface, javaVM, classLoader);
	Assert_VM_notNull(classLoader);

	return classLoader;
}

extern "C" void
initializeImageClassLoaderObject(J9JavaVM *javaVM, J9ClassLoader *classLoader, j9object_t classLoaderObject)
{
	J9VMThread *vmThread = currentVMThread(javaVM);

	omrthread_monitor_enter(javaVM->classLoaderBlocksMutex);

	J9CLASSLOADER_SET_CLASSLOADEROBJECT(vmThread, classLoader, classLoaderObject);

	issueWriteBarrier();
	J9VMJAVALANGCLASSLOADER_SET_VMREF(vmThread, classLoaderObject, classLoader);
	
	omrthread_monitor_exit(javaVM->classLoaderBlocksMutex);
}

extern "C" void
initializeImageJ9Class(J9JavaVM *javaVM, J9Class *clazz)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	jvmImage->fixupJITVtable(clazz);
}


extern "C" J9Class *
initializeImageClassObject(J9VMThread *vmThread, J9ClassLoader *classLoader, J9Class *clazz)
{
	J9JavaVM *javaVM = vmThread->javaVM;

	/* Allocate class object */
	J9Class *jlClass = J9VMCONSTANTPOOL_CLASSREF_AT(javaVM, J9VMCONSTANTPOOL_JAVALANGCLASS)->value;

	if (NULL == jlClass) {
		return NULL;
	}
	J9Class *lockClass = J9VMJAVALANGJ9VMINTERNALSCLASSINITIALIZATIONLOCK_OR_NULL(javaVM);

	j9object_t classObject = javaVM->memoryManagerFunctions->J9AllocateObject(vmThread, jlClass, J9_GC_ALLOCATE_OBJECT_TENURED | J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE | J9_GC_ALLOCATE_OBJECT_HASHED);
	if (NULL == classObject) {
		setHeapOutOfMemoryError(vmThread);
		return NULL;
	}

	/* Allocate lock object if lock class already allocated */
	if (NULL != lockClass) {
		j9object_t lockObject;
		UDATA allocateFlags = J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE;

		PUSH_OBJECT_IN_SPECIAL_FRAME(vmThread, (j9object_t)classObject);
		lockObject = javaVM->memoryManagerFunctions->J9AllocateObject(vmThread, lockClass, allocateFlags);
		classObject = POP_OBJECT_IN_SPECIAL_FRAME(vmThread);

		if (NULL == lockObject) {
			setHeapOutOfMemoryError(vmThread);
			return NULL;
		}
		J9VMJAVALANGJ9VMINTERNALSCLASSINITIALIZATIONLOCK_SET_THECLASS(vmThread, lockObject, classObject);
		J9VMJAVALANGCLASS_SET_INITIALIZATIONLOCK(vmThread, classObject, lockObject);
	}

	J9VMJAVALANGCLASS_SET_CLASSLOADER(vmThread, classObject, classLoader->classLoaderObject);
	J9VMJAVALANGCLASS_SET_VMREF(vmThread, classObject, clazz);
	J9STATIC_OBJECT_STORE(vmThread, clazz, (j9object_t*)&clazz->classObject, (j9object_t)classObject);

	return clazz;
}

extern "C" void
shutdownJVMImage(JVMImagePortLibrary *jvmImagePortLibrary)
{
	if (NULL != jvmImagePortLibrary->jvmImage) {
		PORT_ACCESS_FROM_PORT(((JVMImage *)jvmImagePortLibrary->jvmImage)->getJ9JavaVM()->portLibrary);
		Assert_VM_notNull(jvmImagePortLibrary);

		JVMImage *jvmImage = (JVMImage *) jvmImagePortLibrary->jvmImage;

		if (NULL != jvmImage) {
			jvmImage->~JVMImage();
			j9mem_free_memory(jvmImage);
			/* TODO */
			//j9mem_free_memory(JVMIMAGEPORT_FROM_JAVAVM(javaVM));
		}
		j9mem_free_memory(jvmImagePortLibrary);
	}
}

extern "C" void
teardownJVMImage(J9JavaVM *javaVM)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	if (IS_COLD_RUN(javaVM)) {
		jvmImage->teardownImage();
	} else {
		jvmImage->setMemorySegments(javaVM);
	}
}

extern "C" void
storeInitialVMMethods(J9JavaVM* javaVM, J9Method* cInitialStaticMethod, J9Method* cInitialSpecialMethod, J9Method* cInitialVirtualMethod)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	jvmImage->storeInitialMethods(cInitialStaticMethod, cInitialSpecialMethod, cInitialVirtualMethod);
}

extern "C" void
setInitialVMMethods(J9JavaVM* javaVM, J9Method** cInitialStaticMethod, J9Method** cInitialSpecialMethod, J9Method** cInitialVirtualMethod)
{
	IMAGE_ACCESS_FROM_JAVAVM(javaVM);
	Assert_VM_notNull(jvmImage);

	jvmImage->setInitialMethods(cInitialStaticMethod, cInitialSpecialMethod, cInitialVirtualMethod);
}

extern "C" void *
image_mem_allocate_memory(struct OMRPortLibrary *portLibrary, uintptr_t byteAmount, const char *callSite, uint32_t category)
{
	IMAGE_ACCESS_FROM_PORT(portLibrary);
	Assert_VM_notNull(jvmImage);

	void *pointer = jvmImage->subAllocateMemory(byteAmount);
	return pointer;
}

extern "C" void
image_mem_free_memory(struct OMRPortLibrary *portLibrary, void *memoryPointer)
{
	IMAGE_ACCESS_FROM_PORT(portLibrary);
	Assert_VM_notNull(jvmImage);

	jvmImage->freeSubAllocatedMemory(memoryPointer);
}
