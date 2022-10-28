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

#include "j9.h"
#include "j9cfg.h"
#include "j9protos.h"
#include "j9nonbuilder.h"

#include "compile/Compilation.hpp"
#include "compile/CompilationTypes.hpp"
#include "control/OptimizationPlan.hpp"
#include "control/Options.hpp"
#include "env/RawAllocator.hpp"
#include "env/Region.hpp"
#include "env/SegmentAllocator.hpp"
#include "env/SystemSegmentProvider.hpp"
#include "env/VMJ9.h"
#include "env/TRMemory.hpp"
#include "ilgen/IlGeneratorMethodDetails.hpp"
#include "ilgen/IlGenRequest.hpp"
#include "infra/UnitTestInfrastructure.hpp"
#include "infra/UnitTester.hpp"

void
TR_UnitTestInfrastructure::run()
   {
   auto rawAllocator = TR::RawAllocator(_jitConfig->javaVM);
   auto segmentAllocator = J9::SegmentAllocator(MEMORY_TYPE_JIT_SCRATCH_SPACE | MEMORY_TYPE_VIRTUAL, *jitConfig->javaVM);
   auto regionSegmentProvider = J9::SystemSegmentProvider(
      1 << 20,
      1 << 20,
      TR::Options::getScratchSpaceLimit(),
      segmentAllocator,
      rawAllocator
      );
   auto dispatchRegion = TR::Region(regionSegmentProvider, rawAllocator);
   auto trMemory = TR_Memory(*::trPersistentMemory, dispatchRegion);

   auto *javaVM = _jitConfig->javaVM;
   auto *vmThread = javaVM->internalVMFunctions->currentVMThread(javaVM);

   auto *vm = TR_J9VMBase::get(_jitConfig, vmThread);

   auto *method = (J9Method *)vm->getMethodFromName("java/lang/Integer", "<init>", "(I)V");
   auto details = TR::IlGeneratorMethodDetails(method);
   auto *compilee = vm->createResolvedMethod(&trMemory, (TR_OpaqueMethodBlock *)details.getMethod());

   auto *plan = TR_OptimizationPlan::alloc(TR_Hotness::cold);

   auto *options
      = new (&trMemory, heapAlloc) TR::Options(
         &trMemory,
         0,
         0,
         compilee,
         NULL,
         plan,
         false,
         0);

   auto target = TR::Compiler->target;
   auto ilgenRequest = TR::CompileIlGenRequest(details);

   auto *comp
      = new (&trMemory, heapAlloc) TR::Compilation(
         0,
         vmThread,
         vm,
         compilee,
         ilgenRequest,
         *options,
         dispatchRegion,
         &trMemory,
         plan,
         NULL,
         &target);

   auto unitTester = TR_UnitTester(comp);

   fprintf(stderr, "Starting unitTester.run()!\n");
   unitTester.run();
   unitTester.summarize();
   }