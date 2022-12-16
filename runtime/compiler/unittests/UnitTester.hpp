/*******************************************************************************
 * Copyright (c) 2022, 2022 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#ifndef UNIT_TESTTESTER_INCL
#define UNIT_TESTTESTER_INCL

#include "j9.h"
#include "j9cfg.h"
#include "env/TRMemory.hpp"
#include "infra/Monitor.hpp"

namespace TR { class CompilationInfo; }
namespace TR { class Monitor; }
class TR_J9VMBase;

class TR_UnitTester
   {
   public:
   TR_PERSISTENT_ALLOC(TR_Memory::FrontEnd);

   enum State
      {
      NATIVE_INITIALIZED = 0,
      JAVA_INITIALIZED = 1,
      RUNMETHOD = 2,
      EXIT = 3,
      };

   TR_UnitTester(J9JITConfig *);
   static void init(J9JITConfig *);
   static TR_UnitTester * getInstance() { return _instance; }

   void run();

   J9VMThread* getUnitTesterThread() { return _unitTesterThread; }
   void setUnitTesterThread(J9VMThread* thread) { _unitTesterThread = thread; }
   j9thread_t getUnitTesterOSThread() { return _unitTesterOSThread; }
   TR::Monitor* getUnitTesterMonitor() { return _unitTesterMonitor; }

   void setUnitTesterThreadExitFlag() { _unitTesterThreadExitFlag = 1; }
   uint32_t getUnitTesterThreadExitFlag() { return _unitTesterThreadExitFlag; }
   void setAttachAttempted(bool b) { _unitTesterThreadAttachAttempted = b; }
   bool getAttachAttempted() const { return _unitTesterThreadAttachAttempted; }

   void startUnitTesterThread(J9JavaVM *javaVM);
   void stopUnitTesterThread();

   J9Class *getMainClass() { return _mainClass; }
   void setMainClass(J9Class *mainClass) { _mainClass = mainClass; }

   State getState() { return _state; }
   void setState(State state) { _state = state; }

   private:
   static TR_UnitTester *_instance;

   j9thread_t _unitTesterOSThread;
   J9VMThread *_unitTesterThread;
   TR::Monitor *_unitTesterMonitor;
   volatile uint32_t _unitTesterThreadExitFlag;
   volatile bool _unitTesterThreadAttachAttempted;

   J9PortLibrary *_portLib;
   TR_J9VMBase *_vm;
   TR::CompilationInfo *_compInfo;

   J9Class *_mainClass;

   State _state;
   };

int32_t initializeUnitTester(J9JITConfig *jitConfig);

#endif //UNIT_TESTTESTER_INCL