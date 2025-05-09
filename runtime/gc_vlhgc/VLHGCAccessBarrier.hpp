
/*******************************************************************************
 * Copyright IBM Corp. and others 1991
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


/**
 * @file
 * @ingroup GC_Modron_Standard
 */

#if !defined(VLHGCACCESSBARRIER_HPP_)
#define VLHGCACCESSBARRIER_HPP_


#include "j9.h"
#include "j9cfg.h"	

#include "ObjectAccessBarrier.hpp"
#include "GenerationalAccessBarrierComponent.hpp"

/**
 * Access barrier for Modron collector.
 */
 
class MM_VLHGCAccessBarrier : public MM_ObjectAccessBarrier
{
private:
	void postObjectStoreImpl(J9VMThread *vmThread, J9Object *dstObject, J9Object *srcObject);
	void postBatchObjectStoreImpl(J9VMThread *vmThread, J9Object *dstObject);

protected:
	virtual bool initialize(MM_EnvironmentBase *env);
	virtual void tearDown(MM_EnvironmentBase *env);
	
public:
	static MM_VLHGCAccessBarrier *newInstance(MM_EnvironmentBase *env);
	virtual void kill(MM_EnvironmentBase *env);

	MM_VLHGCAccessBarrier(MM_EnvironmentBase *env) :
		MM_ObjectAccessBarrier(env)
	{
		_typeId = __FUNCTION__;
	}

	virtual void postObjectStore(J9VMThread *vmThread, J9Object *destObject, fj9object_t *destAddress, J9Object *value, bool isVolatile=false);
	virtual void postObjectStore(J9VMThread *vmThread, J9Class *destClass, J9Object **destAddress, J9Object *value, bool isVolatile=false);
	virtual bool postBatchObjectStore(J9VMThread *vmThread, J9Object *destObject, bool isVolatile=false);
	virtual bool postBatchObjectStore(J9VMThread *vmThread, J9Class *destClass, bool isVolatile=false);
	virtual void recentlyAllocatedObject(J9VMThread *vmThread, J9Object *object); 
	virtual void postStoreClassToClassLoader(J9VMThread *vmThread, J9ClassLoader* destClassLoader, J9Class* srcClass);
	
	virtual bool preWeakRootSlotRead(J9VMThread *vmThread, j9object_t *srcAddress);
	virtual bool preWeakRootSlotRead(J9JavaVM *vm, j9object_t *srcAddress);

	virtual void postUnmountContinuation(J9VMThread *vmThread, j9object_t contObject);

	virtual I_32 backwardReferenceArrayCopyIndex(J9VMThread *vmThread, J9IndexableObject *srcObject, J9IndexableObject *destObject, I_32 srcIndex, I_32 destIndex, I_32 lengthInSlots);
	virtual I_32 forwardReferenceArrayCopyIndex(J9VMThread *vmThread, J9IndexableObject *srcObject, J9IndexableObject *destObject, I_32 srcIndex, I_32 destIndex, I_32 lengthInSlots);

	virtual IDATA indexableDataDisplacement(J9StackWalkState *walkState, J9IndexableObject *src, J9IndexableObject *dst);

	virtual void* jniGetPrimitiveArrayCritical(J9VMThread* vmThread, jarray array, jboolean *isCopy);
	virtual void jniReleasePrimitiveArrayCritical(J9VMThread* vmThread, jarray array, void * elems, jint mode);
	virtual const jchar* jniGetStringCritical(J9VMThread* vmThread, jstring str, jboolean *isCopy);
	virtual void jniReleaseStringCritical(J9VMThread* vmThread, jstring str, const jchar* elems);

	virtual void referenceReprocess(J9VMThread *vmThread, J9Object *refObject)
	{
		/* Equivalent to J9WriteBarrierBatchStore */
		postBatchObjectStore(vmThread, refObject);
	}
};

#endif /* VLHGCACCESSBARRIER_HPP_ */
