#include "env/Region.hpp"
#include "infra/CriticalSection.hpp"
#include "net/ClientStream.hpp"
#include "runtime/JITServerAOTDependencyFetcher.hpp"

JITServerAOTDependencyFetcher::JITServerAOTDependencyFetcher(
   J9JITConfig *jitConfig, TR::CompilationInfo *compInfo, TR_PersistentMemory *pm)
   :
   _jitConfig(jitConfig),
   _javaVM(_jitConfig->javaVM),
   _osThread(NULL),
   _j9vmThread(NULL),
   _compInfo(compInfo),
   _pm(pm),
   _requestQueue(decltype(_requestQueue)::allocator_type(pm->_persistentAllocator.get()))
   {
   _valid = startAOTDependencyFetcherThread();
   }

void
JITServerAOTDependencyFetcher::notify()
   {
   OMR::CriticalSection notifyCS(_monitor);
   _monitor->notifyAll();
   }

bool
JITServerAOTDependencyFetcher::startAOTDependencyFetcherThread()
   {
   PORT_ACCESS_FROM_JAVAVM(_javaVM);

   _monitor = TR::Monitor::create("JIT-jitserverAOTDependencyFetcherMonitor");
   if (_monitor)
      {
      // create the thread for interpreter profiling
      if(_javaVM->internalVMFunctions->createThreadWithCategory(&_osThread,
                                      TR::Options::_profilerStackSize << 10,
                                      J9THREAD_PRIORITY_NORMAL,
                                      false,
                                      &threadProc,
                                      this,
                                      J9THREAD_CATEGORY_SYSTEM_JIT_THREAD))
         {
         j9tty_printf(PORTLIB, "Error: Unable to create jitserver aot dependency fetcher thread\n");
         TR::Options::getCmdLineOptions()->setOption(TR_DisableIProfilerThread);
         _monitor = NULL;
         }
      else
         {
         // Must wait here until the thread gets created; otherwise an early
         // shutdown does not know whether or not to destroy the thread
         _monitor->enter();
         while (getLifetimeState() == JITServerAOTDependencyFetcher::AOTDF_THR_NOT_CREATED)
            _monitor->wait();
         _monitor->exit();

         // At this point the fetcher thread should have attached successfully
         // and changed the state to AOTDF_THR_INITIALIZED, or failed to attach
         // and changed the state to AOTDF_THR_FAILED_TO_ATTACH
         if (getLifetimeState() == JITServerAOTDependencyFetcher::AOTDF_THR_FAILED_TO_ATTACH)
            {
            _j9vmThread = NULL;
            _monitor = NULL;
            }
         }
      }
   else
      {
      j9tty_printf(PORTLIB, "Error: Unable to create JIT-jitserverAOTDependencyFetcherMonitor\n");
      }

   return _j9vmThread != NULL;
   }

int J9THREAD_PROC
JITServerAOTDependencyFetcher::threadProc(void *arg)
   {
   JITServerAOTDependencyFetcher *fetcher = (JITServerAOTDependencyFetcher *)arg;
   J9JavaVM *vm = fetcher->jitConfig()->javaVM;
   PORT_ACCESS_FROM_JAVAVM(vm)   ;

   J9VMThread *fetcherThread = NULL;
   int rc = vm->internalVMFunctions->internalAttachCurrentThread(vm, &fetcherThread, NULL,
                                  J9_PRIVATE_FLAGS_DAEMON_THREAD | J9_PRIVATE_FLAGS_NO_OBJECT |
                                  J9_PRIVATE_FLAGS_SYSTEM_THREAD | J9_PRIVATE_FLAGS_ATTACHED_THREAD,
                                  fetcher->osThread());

   fetcher->monitor()->enter();
   if (rc == JNI_OK)
      {
      fetcher->setVmThread(fetcherThread);
      j9thread_set_name(j9thread_self(), "JIT IProfiler");
      fetcher->setLifetimeState(JITServerAOTDependencyFetcher::AOTDF_THR_INITIALIZED);
      }
   else
      {
      fetcher->setLifetimeState(JITServerAOTDependencyFetcher::AOTDF_THR_FAILED_TO_ATTACH);
      }
   fetcher->monitor()->notifyAll();
   fetcher->monitor()->exit();

   // attaching the fetcher thread failed
   if (rc != JNI_OK)
      return JNI_ERR;

   fetcher->run();

   vm->internalVMFunctions->DetachCurrentThread((JavaVM *) vm);
   fetcher->setVmThread(NULL);
   fetcher->monitor()->enter();
   fetcher->setLifetimeState(JITServerAOTDependencyFetcher::AOTDF_THR_DESTROYED);
   fetcher->monitor()->notifyAll();
   j9thread_exit((J9ThreadMonitor*)fetcher->monitor()->getVMMonitor());

   return 0;
   }

void
JITServerAOTDependencyFetcher::run()
   {
   try
      {
      /*
      TR::RawAllocator rawAllocator(_javaVM);
      J9::SegmentAllocator segmentAllocator(MEMORY_TYPE_JIT_SCRATCH_SPACE | MEMORY_TYPE_VIRTUAL, *_javaVM);
      J9::SystemSegmentProvider regionSegmentProvider(
         1 << 20,
         1 << 20,
         TR::Options::getScratchSpaceLimit(),
         segmentAllocator,
         rawAllocator
         );
      TR::Region region(regionSegmentProvider, rawAllocator);
      */

      JITServer::ClientStream stream(_compInfo->getPersistentInfo());
      handleClassBatchRequests(stream);
      }
   catch (const std::exception &e)
      {
      if (TR::Options::getVerboseOption(TR_VerboseJITServer))
         {
         TR_VerboseLog::writeLineLocked(
            TR_Vlog_JITServer, "ERROR: Exception (%s) in JITServer AOT Dependencies Fetcher Thread", e.what());
         }
      }
   }

bool
JITServerAOTDependencyFetcher::handleClassBatchRequests(JITServer::ClientStream &stream)
   {
   bool success = true;

   _monitor->enter();
   do {
      while(getLifetimeState() == JITServerAOTDependencyFetcher::AOTDF_THR_INITIALIZED
            && _requestQueue.empty())
         {
         setLifetimeState(JITServerAOTDependencyFetcher::AOTDF_THR_WAITING_FOR_WORK);

         _monitor->wait();

         if (getLifetimeState() == JITServerAOTDependencyFetcher::AOTDF_THR_WAITING_FOR_WORK)
            setLifetimeState(JITServerAOTDependencyFetcher::AOTDF_THR_INITIALIZED);
         }

      if (getLifetimeState() == JITServerAOTDependencyFetcher::AOTDF_THR_STOPPING)
         {
         _monitor->exit();
         break;
         }
      else if (!_requestQueue.empty())
         {
         // handle requests
         }
      else
         {
         TR_ASSERT_FATAL(false, "Dependency Fetcher in invalid state %d\n", getLifetimeState());
         }
      } while (true);

   return success;
   }

void
JITServerAOTDependencyFetcher::onClassLoad(J9Class *ramClass, const J9UTF8 *loaderName, J9VMThread *vmThread, TR::Region &region)
   {
   OMR::CriticalSection onClassLoadCS(_monitor);

   if (loaderName)
      {
      _requestQueue.insert({ramClass, loaderName});
      }
   else if (TR::Options::getVerboseOption(TR_VerboseJITServer))
      {
      /*
      size_t prefixLength = JITServerHelpers::getGeneratedClassNamePrefixLength(ramClass->romClass);
      const J9UTF8 *name = prefixLength ? J9ROMCLASS_CLASSNAME(ramClass->romClass)
                                        : JITServerHelpers::getFullClassName(ramClass, region);
      TR_VerboseLog::writeLineLocked(
         TR_Vlog_JITServer,
         "ERROR: Dependency Fetcher failed to get class loader identifying name for loaded class %.*s %p",
         LENGTH_AND_DATA(name), ramClass);
      */
      }

   notify();
   }

