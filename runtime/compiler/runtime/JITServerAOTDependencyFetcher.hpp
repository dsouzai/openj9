/*******************************************************************************
 * Copyright IBM Corp. and others 2021
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

#ifndef JITSERVER_AOT_DEPENDENCY_FETCHER_H
#define JITSERVER_AOT_DEPENDENCY_FETCHER_H

#include "control/CompilationRuntime.hpp"
#include "env/PersistentCollections.hpp"

namespace TR { class Region; }

class JITServerAOTDependencyFetcher
   {
   public:

   enum AOTDependencyFetcherLifetimeStates
      {
      AOTDF_THR_NOT_CREATED = 0,
      AOTDF_THR_FAILED_TO_ATTACH,
      AOTDF_THR_INITIALIZED,
      AOTDF_THR_WAITING_FOR_WORK,
      AOTDF_THR_SUSPENDING,
      AOTDF_THR_SUSPENDED,
      AOTDF_THR_RESUMING,
      AOTDF_THR_STOPPING,
      AOTDF_THR_DESTROYED,
      AOTDF_THR_LAST_STATE // must be the last one
      };

   JITServerAOTDependencyFetcher(J9JITConfig *jitConfig, TR::CompilationInfo *compInfo,
                                 TR_PersistentMemory *pm);

   bool valid() { return _valid; }
   void notify();

   void onClassLoad(J9Class *ramClass, const J9UTF8 *loaderName, J9VMThread *vmThread, TR::Region &region);

   J9JITConfig * jitConfig() { return _jitConfig; }

   j9thread_t osThread() { return _osThread; }
   J9VMThread * vmThread() { return _j9vmThread; }
   void setVmThread(J9VMThread * vmThread) { _j9vmThread = vmThread; }

   TR::Monitor * monitor() { return _monitor; }


   private:

   bool startAOTDependencyFetcherThread();
   static int J9THREAD_PROC threadProc(void *arg);

   void run();

   bool handleClassBatchRequests(JITServer::ClientStream &stream);

   // Use _monitor for these two routines
   AOTDependencyFetcherLifetimeStates getLifetimeState() const { return _lifetimeState; }
   void setLifetimeState(AOTDependencyFetcherLifetimeStates s) { _lifetimeState = s; }


   bool _valid;

   J9JITConfig *_jitConfig;
   J9JavaVM *_javaVM;

   j9thread_t _osThread;
   J9VMThread *_j9vmThread;


   TR::Monitor *_monitor;

   TR::CompilationInfo *_compInfo;
   TR_PersistentMemory *_pm;

   AOTDependencyFetcherLifetimeStates _lifetimeState;

   PersistentUnorderedMap<J9Class *, const J9UTF8 */*loaderName*/> _requestQueue;
   };

#endif // JITSERVER_AOT_DEPENDENCY_FETCHER_H

