
/*******************************************************************************
 * Copyright (c) 1991, 2019 IBM Corp. and others
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

#include "GCExtensions.hpp"

#include "hookable_api.h"
#include "j9cfg.h"
#include "j2sever.h"
#include "j9memcategories.h"
#include "j9nongenerated.h"
#include "j9port.h"
#include "util_api.h"
#if defined(J9VM_GC_ENABLE_DOUBLE_MAP)
#include "ArrayletLeafIterator.hpp"
#include "Heap.hpp"
#endif /* J9VM_GC_ENABLE_DOUBLE_MAP */

#include "EnvironmentBase.hpp"
#include "Forge.hpp"
#if defined(J9VM_GC_IDLE_HEAP_MANAGER)
 #include  "IdleGCManager.hpp"
#endif /* defined(J9VM_GC_IDLE_HEAP_MANAGER) */
#include "MemorySubSpace.hpp"
#include "ObjectModel.hpp"
#include "ReferenceChainWalkerMarkMap.hpp"
#include "SublistPool.hpp"
#include "Wildcard.hpp"

MM_GCExtensions *
MM_GCExtensions::newInstance(MM_EnvironmentBase *env)
{
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_GCExtensions *extensions;
	
	/* Avoid using MM_Forge to allocate memory for the extension, since MM_Forge itself has not been created! */
	extensions = static_cast<MM_GCExtensions*>(j9mem_allocate_memory(sizeof(MM_GCExtensions), OMRMEM_CATEGORY_MM));
	if (extensions) {
		/* Initialize all the fields to zero */
		memset((void *)extensions, 0, sizeof(*extensions));

		new(extensions) MM_GCExtensions();
		if (!extensions->initialize(env)) {
			extensions->kill(env);
			return NULL;
		}
	}
	return extensions;
}

void
MM_GCExtensions::kill(MM_EnvironmentBase *env)
{
	/* Avoid using MM_Forge to free memory for the extension, since MM_Forge was not used to allocate the memory */
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	tearDown(env);
	j9mem_free_memory(this);
}

/**
 * Initialize the global GC extensions structure.
 * Clear all values within the extensions structure and call the appropriate initialization routines
 * on all substructures.  Upon completion of this call, the extensions structure is ready for use.
 *
 * @return true if the initialization was successful, false otherwise.
 */
bool
MM_GCExtensions::initialize(MM_EnvironmentBase *env)
{
	PORT_ACCESS_FROM_ENVIRONMENT(env);
#if defined(J9VM_GC_ENABLE_DOUBLE_MAP)
	OMRPORT_ACCESS_FROM_J9PORT(privatePortLibrary);
#endif /* J9VM_GC_ENABLE_DOUBLE_MAP */

	if (!MM_GCExtensionsBase::initialize(env)) {
		goto failed;
	}

#if defined(J9VM_GC_ENABLE_DOUBLE_MAP)
	/* Initialize arraylet hash table */
	/* Create hash table. Maps arraylet heap addresses to contiguous arraylet leaf addresses */
	arrayletHashTable = hashTableNew(
                                   privateOmrPortLibrary,
                                   J9_GET_CALLSITE(),
                                   401, /* Avoids collisions and table growth given average number of objects kept alive is 250 */
                                   sizeof(ArrayletTableEntry),
                                   sizeof(UDATA),
                                   J9HASH_TABLE_ALLOW_SIZE_OPTIMIZATION,
                                   OMRMEM_CATEGORY_MM,
                                   ArrayletTableEntry::hash,
                                   ArrayletTableEntry::equal,
                                   NULL,
                                   getJavaVM());

	if (!_arrayletLock.initialize(env, &lnrlOptions, "MM_GCExtensions:ArrayletTableEntry:lock") || (arrayletHashTable == NULL)) {
		goto failed;
	}
#endif /* J9VM_GC_ENABLE_DOUBLE_MAP */

#if defined(J9VM_GC_REALTIME)
	/* only ref slots, size in bytes: 2 * minObjectSize - header size */
	minArraySizeToSetAsScanned = 2 * (1 << J9VMGC_SIZECLASSES_LOG_SMALLEST) - sizeof(J9IndexableObjectContiguous);
#endif /* J9VM_GC_REALTIME */

#if defined(J9VM_GC_JNI_ARRAY_CACHE)
	getJavaVM()->jniArrayCacheMaxSize = J9_GC_JNI_ARRAY_CACHE_SIZE;
#endif /* J9VM_GC_JNI_ARRAY_CACHE */

#if defined(J9VM_GC_THREAD_LOCAL_HEAP)
	getJavaVM()->gcInfo.tlhThreshold = J9_GC_TLH_THRESHOLD;
	getJavaVM()->gcInfo.tlhSize = J9_GC_TLH_SIZE;
#endif /* J9VM_GC_THREAD_LOCAL_HEAP */

	/* if tuned for virtualized environment, we compromise a bit of performance for lower footprint */
	if (getJavaVM()->runtimeFlags & J9_RUNTIME_TUNE_VIRTUALIZED) {
		heapFreeMinimumRatioMultiplier = 20;
	}

	padToPageSize = J9_ARE_ALL_BITS_SET(getJavaVM()->runtimeFlags, J9_RUNTIME_AGGRESSIVE);

	if (J9HookInitializeInterface(getHookInterface(), OMRPORT_FROM_J9PORT(PORTLIB), sizeof(hookInterface))) {
		goto failed;
	}
	
	initializeReferenceArrayCopyTable(&referenceArrayCopyTable);
	
	{
		J9InternalVMFunctions const * const vmFuncs = getJavaVM()->internalVMFunctions;
		_asyncCallbackKey = vmFuncs->J9RegisterAsyncEvent(getJavaVM(), memoryManagerAsyncCallbackHandler, getJavaVM());
		_TLHAsyncCallbackKey = vmFuncs->J9RegisterAsyncEvent(getJavaVM(), memoryManagerTLHAsyncCallbackHandler, getJavaVM());
		if ((_asyncCallbackKey < 0) || (_TLHAsyncCallbackKey < 0)) {
			goto failed;
		}
	}

#if defined(J9VM_GC_IDLE_HEAP_MANAGER)
	/* absorbs GC specific idle tuning flags */
	if (J9_IDLE_TUNING_GC_ON_IDLE == (getJavaVM()->vmRuntimeStateListener.idleTuningFlags & J9_IDLE_TUNING_GC_ON_IDLE)) {
		gcOnIdle = true;
	}
	if (J9_IDLE_TUNING_COMPACT_ON_IDLE == (getJavaVM()->vmRuntimeStateListener.idleTuningFlags & J9_IDLE_TUNING_COMPACT_ON_IDLE)) {
		compactOnIdle = true;
	}
	idleMinimumFree = getJavaVM()->vmRuntimeStateListener.idleMinFreeHeap;
#endif /* if defined(J9VM_GC_IDLE_HEAP_MANAGER) */

	return true;

failed:
	tearDown(env);
	return false;
}

/**
 * Tear down the global GC extensions structure and all sub structures.
 */
void
MM_GCExtensions::tearDown(MM_EnvironmentBase *env)
{
	J9InternalVMFunctions const * const vmFuncs = getJavaVM()->internalVMFunctions;
	vmFuncs->J9UnregisterAsyncEvent(getJavaVM(), _TLHAsyncCallbackKey);
	_TLHAsyncCallbackKey = -1;
	vmFuncs->J9UnregisterAsyncEvent(getJavaVM(), _asyncCallbackKey);
	_asyncCallbackKey = -1;

#if defined(J9VM_GC_MODRON_TRACE) && !defined(J9VM_GC_REALTIME)
	tgcTearDownExtensions(getJavaVM());
#endif /* J9VM_GC_MODRON_TRACE && !defined(J9VM_GC_REALTIME) */

	MM_Wildcard *wildcard = numaCommonThreadClassNamePatterns;
	while (NULL != wildcard) {
		MM_Wildcard *nextWildcard = wildcard->_next;
		wildcard->kill(this);
		wildcard = nextWildcard;
	}
	numaCommonThreadClassNamePatterns = NULL;
	
	J9HookInterface** tmpHookInterface = getHookInterface();
	if((NULL != tmpHookInterface) && (NULL != *tmpHookInterface)){
		(*tmpHookInterface)->J9HookShutdownInterface(tmpHookInterface);
		*tmpHookInterface = NULL; /* avoid issues with double teardowns */
	}

#if defined(J9VM_GC_IDLE_HEAP_MANAGER)
	if (NULL != idleGCManager) {
		idleGCManager->kill(env);
		idleGCManager = NULL;
	}
#endif /* defined(J9VM_GC_IDLE_HEAP_MANAGER) */

	MM_GCExtensionsBase::tearDown(env);
}

void
MM_GCExtensions::identityHashDataAddRange(MM_EnvironmentBase *env, MM_MemorySubSpace* subspace, UDATA size, void* lowAddress, void* highAddress)
{
	J9IdentityHashData* hashData = getJavaVM()->identityHashData;
	if (J9_IDENTITY_HASH_SALT_POLICY_STANDARD == hashData->hashSaltPolicy) {
		if (MEMORY_TYPE_NEW == (subspace->getTypeFlags() & MEMORY_TYPE_NEW)) {
			if (hashData->hashData1 == (UDATA)highAddress) {
				/* Expanding low bound */
				hashData->hashData1 = (UDATA)lowAddress;
			} else if (hashData->hashData2 == (UDATA)lowAddress) {
				/* Expanding high bound */
				hashData->hashData2 = (UDATA)highAddress;
			} else {
				/* First expand */
				Assert_MM_true(UDATA_MAX == hashData->hashData1);
				Assert_MM_true(0 == hashData->hashData2);
				hashData->hashData1 = (UDATA)lowAddress;
				hashData->hashData2 = (UDATA)highAddress;
			}
		}
	}
}

void
MM_GCExtensions::identityHashDataRemoveRange(MM_EnvironmentBase *env, MM_MemorySubSpace* subspace, UDATA size, void* lowAddress, void* highAddress)
{
	J9IdentityHashData* hashData = getJavaVM()->identityHashData;
	if (J9_IDENTITY_HASH_SALT_POLICY_STANDARD == hashData->hashSaltPolicy) {
		if (MEMORY_TYPE_NEW == (subspace->getTypeFlags() & MEMORY_TYPE_NEW)) {
			if (hashData->hashData1 == (UDATA)lowAddress) {
				/* Contracting low bound */
				Assert_MM_true(hashData->hashData1 <= (UDATA)highAddress);
				Assert_MM_true((UDATA)highAddress <= hashData->hashData2);
				hashData->hashData1 = (UDATA)highAddress;
			} else if (hashData->hashData2 == (UDATA)highAddress) {
				/* Contracting high bound */
				Assert_MM_true(hashData->hashData1 <= (UDATA)lowAddress);
				Assert_MM_true((UDATA)lowAddress <= hashData->hashData2);
				hashData->hashData2 = (UDATA)lowAddress;
			} else {
				Assert_MM_unreachable();
			}
		}
	}
}

void
MM_GCExtensions::updateIdentityHashDataForSaltIndex(UDATA index)
{
	getJavaVM()->identityHashData->hashSaltTable[index] = (U_32)convertValueToHash(getJavaVM(), getJavaVM()->identityHashData->hashSaltTable[index]);
}

void
MM_GCExtensions::computeDefaultMaxHeap(MM_EnvironmentBase *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	MM_GCExtensionsBase::computeDefaultMaxHeap(env);

	if (OMR_CGROUP_SUBSYSTEM_MEMORY == omrsysinfo_cgroup_are_subsystems_enabled(OMR_CGROUP_SUBSYSTEM_MEMORY)) {
		if (omrsysinfo_cgroup_is_memlimit_set()) {
			/* If running in a cgroup with memory limit > 1G, reserve at-least 512M for JVM's internal requirements
			 * like JIT compilation etc, and extend default max heap memory to at-most 75% of cgroup limit.
			 * The value reserved for JVM's internal requirements excludes heap. This value is a conservative
			 * estimate of the JVM's internal requirements, given that one compilation thread can use up to 256M.
			 */
#define OPENJ9_IN_CGROUP_NATIVE_FOOTPRINT_EXCLUDING_HEAP ((U_64)512 * 1024 * 1024)
			memoryMax = (uintptr_t)OMR_MAX((int64_t)(usablePhysicalMemory / 2), (int64_t)(usablePhysicalMemory - OPENJ9_IN_CGROUP_NATIVE_FOOTPRINT_EXCLUDING_HEAP));
			memoryMax = (uintptr_t)OMR_MIN(memoryMax, (usablePhysicalMemory / 4) * 3);
#undef OPENJ9_IN_CGROUP_NATIVE_FOOTPRINT_EXCLUDING_HEAP
		}
	}

#if defined(OMR_ENV_DATA64)
	if (J2SE_VERSION((J9JavaVM *)getOmrVM()->_language_vm) >= J2SE_V11) {
		/* extend java default max memory to 25% of usable RAM */
		memoryMax = OMR_MAX(memoryMax, usablePhysicalMemory / 4);
	}

	/* limit maxheapsize up to MAXIMUM_HEAP_SIZE_RECOMMENDED_FOR_3BIT_SHIFT_COMPRESSEDREFS, then can set 3bit compressedrefs as the default */
	memoryMax = OMR_MIN(memoryMax, MAXIMUM_HEAP_SIZE_RECOMMENDED_FOR_3BIT_SHIFT_COMPRESSEDREFS);
#endif /* OMR_ENV_DATA64 */

	memoryMax = MM_Math::roundToFloor(heapAlignment, memoryMax);
}

#if defined(J9VM_GC_ENABLE_DOUBLE_MAP)
#if defined(LINUX) || defined(WIN32) || defined(WINDOWS) || defined(OSX)
/* Double map is only supported on LINUX and WINDOWS for now */
#else
#error "Platform not supported by Double Map API"
#endif /* defined(LINUX) || defined(WIN32) || defined(WINDOWS) || defined(OSX) */
void* 
MM_GCExtensions::doubleMapArraylets(MM_EnvironmentBase* env, J9Object *objectPtr) 
{
	J9JavaVM *javaVM = getJavaVM();
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	// OMRPORT_ACCESS_FROM_J9PORT(privatePortLibrary);

	GC_ArrayletLeafIterator arrayletLeafIterator(javaVM, (J9IndexableObject*)objectPtr);
	UDATA arrayletLeafCount = arrayletLeafIterator.getNumLeafs();
	MM_Heap *heap = getHeap();
	void* result = NULL;
	uintptr_t *arrayletLeaveAddrs = (uintptr_t *)malloc(arrayletLeafCount * sizeof(uintptr_t));
	if(arrayletLeaveAddrs == NULL) {
		return NULL;
	}
	UDATA elementsSize = indexableObjectModel.getDataSizeInBytes((J9IndexableObject*)objectPtr);

	GC_SlotObject *slotObject = NULL;
	size_t count = 0;

	while (NULL != (slotObject = arrayletLeafIterator.nextLeafPointer())) {
		void *currentLeaf = slotObject->readReferenceFromSlot();
		arrayletLeaveAddrs[count] = (uintptr_t)currentLeaf;
		count++;
	}

	ArrayletTableEntry addrEntry;
	UDATA arrayletLeafSize = javaVM->arrayletLeafSize;
	UDATA pageSize = j9mmap_get_region_granularity(NULL); /* get pagesize  or j9vmem_supported_page_sizes()[0]? */

	// Get heap and from there call an OMR API that will doble map everything
	result = heap->doubleMapArraylet(env, arrayletLeaveAddrs, count, arrayletLeafSize, elementsSize, 
				&addrEntry.identifier,
				pageSize);

	if (result == NULL) { /* Double map failed */
		return NULL;
	}

	addrEntry.heapAddr = (void*)objectPtr;
	addrEntry.contiguousAddr = result;
	addrEntry.dataSize = addrEntry.identifier.size; /* When arraylet storage is updated this should be larger than actualSize */
	addrEntry.actualSize = elementsSize;
	addrEntry.arrayletLeaveAddrs = arrayletLeaveAddrs;

	void *findObject = hashTableFind(arrayletHashTable, &addrEntry);

	if (findObject == NULL) {
		_arrayletLock.acquire();
		ArrayletTableEntry *entry = (ArrayletTableEntry *)hashTableAdd(arrayletHashTable, &addrEntry);
		_arrayletLock.release();

		if (entry == NULL) {
	        	result = NULL;
        	}
	} else {
		result = NULL;
	}

	return result;
}

bool 
MM_GCExtensions::freeDoubleMap(MM_EnvironmentBase* env, void* contiguousAddr, UDATA dataSize, struct J9PortVmemIdentifier *identifier, uintptr_t *arrayletLeaveAddrs) 
{
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	void *contiguousMem = contiguousAddr;

	int result = j9vmem_free_memory(contiguousMem, dataSize, identifier);

	free((void *)arrayletLeaveAddrs);

	assert(result == 0);

	return result == 0;
}
#endif /* J9VM_GC_ENABLE_DOUBLE_MAP */
