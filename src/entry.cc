/*
 * This file is part of JCoz.
 *
 * JCoz is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * JCoz is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with JCoz.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file has been modified from lightweight-java-profiler
 * (https://github.com/dcapwell/lightweight-java-profiler). See APACHE_LICENSE for
 * a copy of the license that was included with that original work.
 */

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "globals.h"
#include "profiler.h"
#include "stacktraces.h"

static Profiler *prof;
FILE *Globals::OutFile;
static bool updateEventsEnabledState(jvmtiEnv *jvmti, jvmtiEventMode enabledState);
#ifdef __APPLE__
static volatile pthread_t class_prep_lock = 0;
#else
static volatile int class_prep_lock = 0;
#endif
static bool acquireCreateLock();
static void releaseCreateLock();

jvmtiError run_profiler(JNIEnv *jni);

void JNICALL OnThreadStart(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                           jthread thread)
{
  auto logger = prof->getLogger();
  logger->debug("OnThreadStart fired");
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(thread);
  Accessors::SetCurrentJniEnv(jni_env);

  prof->addUserThread(thread);
}

void JNICALL OnThreadEnd(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread)
{
  auto logger = prof->getLogger();
  logger->debug("OnThreadEnd fired");
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);

  prof->removeUserThread(thread);
}

// This has to be here, or the VM turns off class loading events.
// And AsyncGetCallTrace needs class loading events to be turned on!
void JNICALL OnClassLoad(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread,
                         jclass klass)
{

  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);
  IMPLICITLY_USE(klass);
}

// Create a java thread -- currently used
// to run profiler thread
jthread create_thread(JNIEnv *jni_env)
{
  auto logger = prof->getLogger();
  logger->debug("Creating a thread in create_thread");
  jclass cls = jni_env->FindClass("java/lang/Thread");
  if (cls == NULL)
  {
    logger->critical("Unable to find class java/lang/Thread in jni_env - therefore cannot create profiler thread. Exiting program.\n");
    exit(1);
  }
  const char *method = "<init>";
  jmethodID methodId = jni_env->GetMethodID(cls, method, "()V");

  if (methodId == NULL)
  {
    logger->critical("Unable to find init method for class java/lang/Thread in jni_env - therefore cannot create profiler thread. Exiting program.\n");
    exit(1);
  }

  jobject ret = jni_env->NewObject(cls, methodId);

  return (jthread)ret;
}

/**
 * Either enable or disable the custom agent events. This is
 * fired when {@code startProfilingNative} or {@code endProfilingNative}
 * are called.
 */
static bool updateEventsEnabledState(jvmtiEnv *jvmti, jvmtiEventMode enabledState)
{
  auto logger = prof->getLogger();
  logger->debug("Setting CLASS_PREPARE to enabled");
  JVMTI_ERROR_1(
      (jvmti->SetEventNotificationMode(enabledState, JVMTI_EVENT_CLASS_PREPARE, NULL)),
      false);

  return true;
}

static bool acquireCreateLock()
{
  bool has_lock = class_prep_lock == pthread_self();
  if (!has_lock)
  {
    while (!__sync_bool_compare_and_swap(&class_prep_lock, 0, pthread_self()))
      ;

    std::atomic_thread_fence(std::memory_order_acquire);
  }

  return !has_lock;
}

static void releaseCreateLock()
{
  class_prep_lock = 0;
  std::atomic_thread_fence(std::memory_order_release);
}

bool is_class_fqn_prefix(const char *prefix, char *class_sig)
{
  // Assumed that class signature has format `L<name>;`,
  // and that prefix does not have additional symbols.
  // So to check that <name> itself has given prefix,
  // we must skip the first symbol of the signature.
  return strstr(class_sig, prefix) == class_sig + 1;
}

bool contains_class_fqn_prefix(std::vector<std::string> &elements, char *class_sig)
{
  // This predicate is a lambda function passed to std::find_if
  auto predicate = [&class_sig](std::string &scope)
  { 
    return is_class_fqn_prefix(scope.c_str(), class_sig); 
  };
  return std::find_if(std::begin(elements), std::end(elements), predicate) != std::end(elements);
}

// TODO faster search (trie maybe)
bool is_in_allowed_scope(char *class_sig)
{
  return !contains_class_fqn_prefix(Profiler::get_ignored_scopes(), class_sig) && contains_class_fqn_prefix(Profiler::get_search_scopes(), class_sig);
}

// Calls GetClassMethods on a given class to force the creation of
// jmethodIDs of it.
void CreateJMethodIDsForClass(jvmtiEnv *jvmti, jclass klass)
{
  if (!prof->isRunning())
  {
    return;
  }
  auto logger = prof->getLogger();
  logger->trace("In CreateJMethodIDsForClass start");
  bool releaseLock = acquireCreateLock();
  jint method_count;
  JvmtiScopedPtr<jmethodID> methods(jvmti);
  jvmtiError e = jvmti->GetClassMethods(klass, &method_count, methods.GetRef());
  logger->trace("Got class methods from the JVM");
  if (e != JVMTI_ERROR_NONE)
  {
    JvmtiScopedPtr<char> ksig(jvmti);
    JVMTI_ERROR((jvmti->GetClassSignature(klass, ksig.GetRef(), NULL)));
    logger->error("Failed to create method IDs for methods in class {} with error {}", ksig.Get(), e);
  }
  else
  {
    JvmtiScopedPtr<char> ksig(jvmti);
    jvmti->GetClassSignature(klass, ksig.GetRef(), NULL);

    logger->debug(
        "Creating JMethod IDs. [Class: {class}]",
        fmt::arg("class", ksig.Get()));
    if (is_in_allowed_scope(ksig.Get()))
    {
      Profiler::addInScopeMethods(method_count, methods.Get());
    }

    // Initial check for progress point class
    // This check matches a prefix. class name AA will match a progress point set with class A 
    // (i.e. model/DummyClass and newmodel/DummyClass would both match)
    const char *progress_class_c_str = prof->getProgressClass().c_str(); 
    std::string klass_sig = std::string(ksig.Get());
    if (strstr(klass_sig.c_str(), progress_class_c_str) == klass_sig.c_str())
    {
      logger->info("Setting progress point - initial check for correct class has passed");
      // The progress_class_c_str is in the format "LMain"
      // Whereas the klass_sig is in the format "LMain;"
      // Therefore need to strip the ";" before the second check
      // Since the initial check will occur hundreds/thousands of times - but the second check should only occur once or twice
      // Having the second check with the extra step of ";" replacement is slightly more performant than doing the replace earlier
      // and having a single check
      klass_sig.replace(klass_sig.length() - 1, 1, "\0");
      // Second check for progress point class
      if (klass_sig.compare(progress_class_c_str) == 0) {
        logger->info("Setting progress point - second check for correct class has passed");
        prof->addProgressPoint(method_count, methods.Get());
      } 
    }
  }
  if (releaseLock)
  {
    releaseCreateLock();
  }
}

void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread)
{
  IMPLICITLY_USE(jvmti);
  IMPLICITLY_USE(thread);
  IMPLICITLY_USE(jni_env);

  run_profiler(jni_env);
}

void JNICALL OnClassPrepare(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                            jthread thread, jclass klass)
{
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);
  // We need to do this to "prime the pump", as it were -- make sure
  // that all of the methodIDs have been initialized internally, for
  // AsyncGetCallTrace.  I imagine it slows down class loading a mite,
  // but honestly, how fast does class loading have to be?
  CreateJMethodIDsForClass(jvmti_env, klass);
}

void JNICALL OnVMDeath(jvmtiEnv *jvmti_env, JNIEnv *jni_env)
{
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);

  prof->getLogger()->info("On VM death. Stopping profiler...");
  prof->Stop();
  updateEventsEnabledState(prof->getJVMTI(), JVMTI_DISABLE);
  Profiler::clearProgressPoint();
}

static bool PrepareJvmti(jvmtiEnv *jvmti)
{
  // Set the list of permissions to do the various internal VM things
  // we want to do.
  jvmtiCapabilities caps;

  memset(&caps, 0, sizeof(caps));
  caps.can_generate_all_class_hook_events = 1;

  caps.can_get_source_file_name = 1;
  caps.can_get_line_numbers = 1;
  caps.can_get_bytecodes = 1;
  caps.can_get_constant_pool = 1;
  caps.can_generate_breakpoint_events = 1;

  jvmtiCapabilities all_caps;
  memset(&all_caps, 0, sizeof(all_caps));
  int error;

  if (JVMTI_ERROR_NONE ==
      (error = jvmti->GetPotentialCapabilities(&all_caps)))
  {
    // This makes sure that if we need a capability, it is one of the
    // potential capabilities.  The technique isn't wonderful, but it
    // This makes sure that if we need a capability, it is one of the
    // potential capabilities.  The technique isn't wonderful, but it
    // is compact and as likely to be compatible between versions as
    // anything else.
    char *has = reinterpret_cast<char *>(&all_caps);
    const char *should_have = reinterpret_cast<const char *>(&caps);
    for (int i = 0; i < sizeof(all_caps); i++)
    {
      if ((should_have[i] != 0) && (has[i] == 0))
      {
        return false;
      }
    }

    // This adds the capabilities.
    if ((error = jvmti->AddCapabilities(&caps)) != JVMTI_ERROR_NONE)
    {
      fprintf(stderr, "Failed to add capabilities with error %d\n", error);
      return false;
    }
  }
  return true;
}

static bool RegisterJvmti(jvmtiEnv *jvmti)
{
  // Create the list of callbacks to be called on given events.
  auto logger = prof->getLogger();
  logger->trace("Registering jvmtiEventCallbacks in RegisterJvmti");
  jvmtiEventCallbacks *callbacks = new jvmtiEventCallbacks();
  memset(callbacks, 0, sizeof(jvmtiEventCallbacks));

  callbacks->ThreadStart = &OnThreadStart;
  callbacks->ThreadEnd = &OnThreadEnd;
  callbacks->VMInit = &OnVMInit;
  callbacks->VMDeath = &OnVMDeath;

  callbacks->ClassLoad = &OnClassLoad;
  callbacks->ClassPrepare = &OnClassPrepare;
  callbacks->Breakpoint = &(Profiler::HandleBreakpoint);

  JVMTI_ERROR_1(
      (jvmti->SetEventCallbacks(callbacks, sizeof(jvmtiEventCallbacks))),
      false);

  jvmtiEvent events[] = {JVMTI_EVENT_CLASS_LOAD, JVMTI_EVENT_BREAKPOINT,
                         JVMTI_EVENT_THREAD_END, JVMTI_EVENT_THREAD_START,
                         JVMTI_EVENT_VM_DEATH, JVMTI_EVENT_VM_INIT};

  size_t num_events = sizeof(events) / sizeof(jvmtiEvent);

  // Enable the callbacks to be triggered when the events occur.
  // Events are enumerated in jvmstatagent.h
  logger->debug("Setting event notification mode to JVMTI_ENABLE in Register Jvmti");
  for (int i = 0; i < num_events; i++)
  {
    JVMTI_ERROR_1(
        (jvmti->SetEventNotificationMode(JVMTI_ENABLE, events[i], NULL)),
        false);
  }
  logger->info("JVMTI successfully registered and event notifications successfully enabled");

  return true;
}

AGENTEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options,
                                      void *reserved)
{
  IMPLICITLY_USE(reserved);
  int err;
  jvmtiEnv *jvmti;

  Accessors::Init();

  if ((err = (vm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION))) != JNI_OK)
  {
    return 1;
  }

  if (!PrepareJvmti(jvmti))
  {
    fprintf(stderr, "Failed to initialize JVMTI.  Continuing...\n");
    return 0;
  }

  if (!RegisterJvmti(jvmti))
  {
    fprintf(stderr, "Failed to enable JVMTI events.  Continuing...\n");
    // We fail hard here because we may have failed in the middle of
    // registering callbacks, which will leave the system in an
    // inconsistent state.
    return 1;
  }

  Asgct::SetAsgct(Accessors::GetJvmFunction<ASGCTType>("AsyncGetCallTrace"));

  prof = new Profiler(jvmti);
  prof->ParseOptions(options);
  prof->setJVMTI(jvmti);
  prof->getLogger()->info("Successfully loaded agent.");
  return 0;
}

AGENTEXPORT void JNICALL Agent_OnUnload(JavaVM *vm)
{
  IMPLICITLY_USE(vm);
  Accessors::Destroy();
}

jvmtiError run_profiler(JNIEnv *jni)
{
  jvmtiEnv *jvmti = prof->getJVMTI();

  jint loaded_classes_count;
  JvmtiScopedPtr<jclass> loaded_classes_ptr(jvmti);
  prof->Start();

  updateEventsEnabledState(jvmti, JVMTI_ENABLE);
  jvmti->GetLoadedClasses(&loaded_classes_count, loaded_classes_ptr.GetRef());
  jclass *loaded_classes = loaded_classes_ptr.Get();
  for (int i = 0; i < loaded_classes_count; ++i)
  {
    jclass next_loaded_class = loaded_classes[i];
    JvmtiScopedPtr<char> ksig(jvmti);
    jvmti->GetClassSignature(next_loaded_class, ksig.GetRef(), nullptr);
    prof->getLogger()->debug("Within entry.cc::run_profiler - Loading class {}", ksig.Get());
    CreateJMethodIDsForClass(jvmti, next_loaded_class);
  }

  jthread agent_thread = create_thread(jni);
  prof->getLogger()->debug("Calling jmvti->RunAgentThread ...");
  jvmtiError agent_error = jvmti->RunAgentThread(agent_thread, &Profiler::runAgentThread, nullptr, 1);
  return agent_error;
}