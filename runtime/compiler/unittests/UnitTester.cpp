/*******************************************************************************
 * Copyright (c) 2022, 2022 IBM Corp. and others
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
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "j9.h"
#include "j9cfg.h"
#include "control/Options.hpp"
#include "env/TRMemory.hpp"
#include "env/VMJ9.h"
#include "unittests/UnitTester.hpp"

TR_UnitTester * TR_UnitTester::_instance = NULL;

static int32_t J9THREAD_PROC testerThreadProc(void * entryarg)
   {
   J9JITConfig * jitConfig = (J9JITConfig *) entryarg;
   J9JavaVM * vm           = jitConfig->javaVM;
   TR_J9VMBase *fe = TR_J9VM::get(jitConfig, 0);
   TR_UnitTester *unitTester = TR_UnitTester::getInstance();
   J9VMThread *unitTesterThread = NULL;
   PORT_ACCESS_FROM_JITCONFIG(jitConfig);

   int rc = vm->internalVMFunctions->internalAttachCurrentThread(vm, &unitTesterThread, NULL,
                                  J9_PRIVATE_FLAGS_DAEMON_THREAD | J9_PRIVATE_FLAGS_NO_OBJECT |
                                  J9_PRIVATE_FLAGS_SYSTEM_THREAD | J9_PRIVATE_FLAGS_ATTACHED_THREAD,
                                  unitTester->getUnitTesterOSThread());
   unitTester->getUnitTesterMonitor()->enter();
   unitTester->setAttachAttempted(true);
   if (rc == JNI_OK)
      unitTester->setUnitTesterThread(unitTesterThread);
   unitTester->getUnitTesterMonitor()->notifyAll();
   unitTester->getUnitTesterMonitor()->exit();
   if (rc != JNI_OK)
      return JNI_ERR; // attaching the Unit Tester thread failed

   j9thread_set_name(j9thread_self(), "JIT Unit Tester");

   // Run Tests Here

   vm->internalVMFunctions->DetachCurrentThread((JavaVM *) vm);
   unitTester->setUnitTesterThread(NULL);
   unitTester->getUnitTesterMonitor()->enter();
   unitTester->setUnitTesterThreadExitFlag();
   unitTester->getUnitTesterMonitor()->notifyAll();
   j9thread_exit((J9ThreadMonitor*)unitTester->getUnitTesterMonitor()->getVMMonitor());

   return 0;
   }

int32_t initializeUnitTester(J9JITConfig *jitConfig)
   {
   TR_UnitTester::init(jitConfig);

   TR_UnitTester *unitTester = TR_UnitTester::getInstance();
   if (NULL == unitTester)
      return -1;

   unitTester->startUnitTesterThread(jitConfig->javaVM);

   return 0;
   }

TR_UnitTester::TR_UnitTester(J9JITConfig *jitConfig)
   : _unitTesterMonitor(NULL),
     _unitTesterThreadExitFlag(0),
     _unitTesterThreadAttachAttempted(false)
   {
   PORT_ACCESS_FROM_JITCONFIG(jitConfig);

   _portLib = jitConfig->javaVM->portLibrary;
   _vm = TR_J9VMBase::get(jitConfig, 0);
   _compInfo = TR::CompilationInfo::get(jitConfig);
   }

void
TR_UnitTester::init(J9JITConfig *jitConfig)
   {
   _instance = new (PERSISTENT_NEW) TR_UnitTester(jitConfig);
   }

void
TR_UnitTester::startUnitTesterThread(J9JavaVM *javaVM)
   {
   PORT_ACCESS_FROM_PORT(_portLib);

   _unitTesterMonitor = TR::Monitor::create("JIT-unitTesterMonitor");
   if (_unitTesterMonitor)
      {
      // create the thread for unit testing
      if(javaVM->internalVMFunctions->createThreadWithCategory(&_unitTesterOSThread,
                                      TR::Options::_stackSize << 10,
                                      J9THREAD_PRIORITY_NORMAL,
                                      0,
                                      &testerThreadProc,
                                      javaVM->jitConfig,
                                      J9THREAD_CATEGORY_SYSTEM_JIT_THREAD))
         {
         j9tty_printf(PORTLIB, "Error: Unable to create unit tester thread\n");
         _unitTesterMonitor = NULL;
         }
      else // Must wait here until the thread gets created; otherwise an early shutdown
         { // does not know whether or not to destroy the thread
         _unitTesterMonitor->enter();
         while (!getAttachAttempted())
            _unitTesterMonitor->wait();
         _unitTesterMonitor->exit();
         }
      }
   else
      {
      j9tty_printf(PORTLIB, "Error: Unable to create JIT-unitTesterMonitor\n");
      }
   }