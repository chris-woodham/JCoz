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

#include "profiler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <vector>
#include <set>
#include <chrono>
#include <time.h>
#include <unordered_set>
#include <unordered_map>
#include <jvmti.h>
#include <pthread.h>
#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <string>
#include <sstream>
#include <iterator>

#include "globals.h"
#include "bci_hits.h"
#include "args.h"

#ifdef __APPLE__
// See comment in Accessors class
pthread_key_t Accessors::key_;
#else
__thread JNIEnv *Accessors::env_;
#endif

#define SIGNAL_FREQ 1000000L

// Maximum possible bytecode index (JVMS14, 4.7.3)
#define MAX_BCI 65535

typedef std::chrono::duration<long, std::milli> milliseconds_type;
typedef std::chrono::duration<long, std::nano> nanoseconds_type;

ASGCTType Asgct::asgct_;

thread_local struct UserThread *curr_ut;

// Initialize static Profiler variables here
std::unordered_set<void *> Profiler::in_scope_ids;
volatile bool Profiler::in_experiment = false;
volatile pthread_t Profiler::in_scope_lock = 0;
volatile int Profiler::frame_lock = 0;
volatile int Profiler::user_threads_lock = 0;
std::vector<JVMPI_CallFrame> Profiler::call_frames;
struct Experiment Profiler::current_experiment;
std::unordered_set<struct UserThread *> Profiler::user_threads;
jvmtiEnv *Profiler::jvmti;
std::atomic<long> Profiler::global_delay(0);
std::atomic_ulong Profiler::points_hit(0);
std::atomic_bool Profiler::_running(false);
volatile bool Profiler::end_to_end = false;
pthread_t Profiler::agent_pthread;
std::atomic_bool Profiler::profile_done(false);
unsigned long Profiler::experiment_time = MIN_EXP_TIME;
JNIEnv *Profiler::jni_;

// How long should we wait before starting an experiment
unsigned long Profiler::warmup_time = 0;
bool Profiler::prof_ready = false;

// Progress point stuff
std::string Profiler::package;
struct ProgressPoint *Profiler::progress_point = nullptr;
std::string Profiler::progress_class;
std::vector<std::string> Profiler::search_scopes;
std::vector<std::string> Profiler::ignored_scopes;

static std::atomic<int> call_index(0);
static JVMPI_CallFrame static_call_frames[NUM_STATIC_CALL_FRAMES];

bool Profiler::fix_exp = false;

nanoseconds_type startup_time;

// Logger
std::shared_ptr<spdlog::logger> Profiler::logger = spdlog::basic_logger_mt("basic_logger", PROFILER_LOG_FILE);

void Profiler::ParseOptions(const char *options)
{
  if (options == NULL)
  {
    agent_args::report_error("Missing options");
  }
  else
  {
    logger->info("Received options: {}", options);
  }

  std::string options_str(options);
  std::stringstream ss(options_str);
  std::string item;
  std::vector<std::string> cmd_line_options;
  progress_point = new ProgressPoint();
  progress_point->lineno = -1;
  progress_point->method_id = nullptr;

  bool isLoggingLevelSet = false;
  bool isOutputFileSet = false;

  // split underscore delimited line into options
  // (we can't use semicolon because bash is dumb)
  while (std::getline(ss, item, '_'))
  {
    cmd_line_options.push_back(item);
  }

  for (auto i = cmd_line_options.begin(); i != cmd_line_options.end(); i++)
  {
    size_t equal_index = i->find('=');
    std::string option = i->substr(0, equal_index);
    std::string value = i->substr(equal_index + 1);

    switch (agent_args::from_string(option))
    {
    case _unknown:
      agent_args::report_error(fmt::format("Unknown option: {}", option).c_str());
      break;

    case _search_scopes:
    {
      std::stringstream search_scopes_stream(value);
      while (std::getline(search_scopes_stream, item, '|'))
      {
        prepare_scope(item);
        add_search_scope(item);
      }
      break;
    }

    case _ignored_scopes:
    {
      std::stringstream ignore_scopes_stream(value);
      while (std::getline(ignore_scopes_stream, item, '|'))
      {
        prepare_scope(item);
        add_ignored_scope(item);
      }
      break;
    }

    case _progress_point:
    {
      size_t colon_index = value.find(':');
      if (colon_index == std::string::npos)
        agent_args::report_error("Missing progress point");

      Profiler::progress_class = value.substr(0, colon_index);
      progress_point->lineno = std::stoi(value.substr(colon_index + 1));
      break;
    }

    case _logging_level:
    {
      logger->set_level(agent_args::parse_logging_level(value));
      isLoggingLevelSet = true;
      break;
    }

    case _output_file:
      agent_args::set_output_file(value);
      isOutputFileSet = true;
      break;

    case _end_to_end:
      end_to_end = true;
      break;

    case _warmup:
      // We expect # of milliseconds so multiply by 1000 for usleep (takes microseconds)
      warmup_time = std::stol(value) * 1000;
      break;

    case _fix_exp:
      fix_exp = true;
      break;
    }
  }

  if (!isLoggingLevelSet)
  {
    logger->info("Logging level not specified in options, default info level used");
  }

  if (!isOutputFileSet)
  {
    kOutputFile = "jcoz-output.csv";
  }

  // Set up column names for .csv data output file
  std::stringstream column_names;
  column_names << "selectedClassLineNo" << "," << "speedup" << "," << "duration" << "," << "effectiveDuration" << "," << "progressPointHits" << "\n";
  std::ofstream output_file;
  output_file.open(kOutputFile.data(), std::ios_base::app);
  output_file << column_names.rdbuf();
  output_file.close();

  const char *const delim = ", ";

  std::stringstream joint_search_scopes;
  std::copy(search_scopes.begin(), search_scopes.end(), std::ostream_iterator<std::string>(joint_search_scopes, delim));

  std::stringstream joint_ignored_scopes;
  std::copy(ignored_scopes.begin(), ignored_scopes.end(),
            std::ostream_iterator<std::string>(joint_ignored_scopes, delim));

  logger->info("Profiler arguments:\n"
               "\tprogress point: {}:{}\n"
               "\tsearch scopes: {}\n"
               "\tignored scopes: {}\n"
               "\twarmup: {}us\n"
               "\tend-to-end: {}\n"
               "\tfixed experiment duration: {}\n"
               "\tLogging level: {}",
               progress_class, progress_point->lineno, joint_search_scopes.str(), joint_ignored_scopes.str(),
               warmup_time, end_to_end, fix_exp, spdlog::level::to_string_view(logger->level()));
  if (search_scopes.empty() || (!end_to_end && (progress_class.empty() || progress_point->lineno == -1)))
  {
    agent_args::report_error("Missing package, progress class, or progress point");
  }
}

/**
 * Wrapper function for sleeping
 */
inline long jcoz_sleep(long nanoseconds)
{

  if (nanoseconds == 0L)
  {
    return 0L;
  }

  struct timespec temp_rem, temp_req;
  memset(&temp_rem, 0, sizeof(struct timespec));
  memset(&temp_req, 0, sizeof(struct timespec));
  temp_req.tv_nsec = nanoseconds;

  auto start = std::chrono::high_resolution_clock::now();

  int err = -1;
  do
  {
    err = nanosleep(&temp_req, &temp_rem);
    temp_req.tv_nsec = temp_rem.tv_nsec;

  } while (err == -1);

  auto end = std::chrono::high_resolution_clock::now();
  nanoseconds_type total_sleep = (end - start);

  return total_sleep.count();
}

void Profiler::init()
{
  progress_point = new ProgressPoint();
  progress_point->lineno = -1;
  progress_point->method_id = nullptr;
}

jvmtiEnv *Profiler::getJVMTI()
{
  return jvmti_;
}

void Profiler::setScope(std::string package)
{
  this->package = package;
}

bool Profiler::isRunning()
{
  return _running;
}

void Profiler::setProgressPoint(std::string class_name, jint line_no)
{
  this->progress_class = class_name;
  this->progress_point->lineno = line_no;
}

void Profiler::signal_user_threads()
{
  while (!__sync_bool_compare_and_swap(&user_threads_lock, 0, 1))
    ;
  std::atomic_thread_fence(std::memory_order_acquire);
  int thread_number = 0;
  for (auto i = user_threads.begin(); i != user_threads.end(); i++)
  {
    std::atomic_thread_fence(std::memory_order_acquire);
    int error_code = pthread_kill((*i)->thread, SIGPROF);
    thread_number += 1;
    std::atomic_thread_fence(std::memory_order_release);
  }
  user_threads_lock = 0;
  std::atomic_thread_fence(std::memory_order_release);
}

/**
 * Return random number from 0 to 1.0 in
 * increments of .05
 */
float Profiler::calculate_random_speedup()
{

  int randVal = rand() % 25;

  // 20% of all experiments should have 0 speedup
  // (This is implemented because the results of the other speedups need to be interpreted relative to 0 speedup)
  if (randVal < 5)
  {
    return (float)0;
  }
  else
  {
    // Each of the speedups from 0.05 to 1.0 (increments of .05) have an equal probability of being selected
    return (float) (randVal - 4)/20.f;
  }
}

void Profiler::update_experiment_length()
{
  // Fixed experiment length => no need to update experiment length
  if (fix_exp)
    return;

  if (current_experiment.points_hit <= HITS_TO_INC_EXP_TIME)
  {
    if (experiment_time * 2 > MAX_EXP_TIME)
    {
      experiment_time = MAX_EXP_TIME;
    }
    else
    {
      experiment_time *= EXP_TIME_FACTOR;
    }
  }
  else if ((experiment_time > MIN_EXP_TIME) && (current_experiment.points_hit >= HITS_TO_DEC_EXP_TIME))
  {
    experiment_time /= EXP_TIME_FACTOR;
  }
}

void Profiler::runExperiment(JNIEnv *jni_env)
{
  logger->info("Running experiment");
  in_experiment = true;
  points_hit = 0;

  current_experiment.speedup = calculate_random_speedup();
  current_experiment.delay =
      (long)(current_experiment.speedup * SIGNAL_FREQ);

  milliseconds_type duration(experiment_time);
  auto start = std::chrono::high_resolution_clock::now();
  auto end = start + duration;

  while (_running && ((end_to_end && (points_hit == 0)) || (std::chrono::high_resolution_clock::now() < end)))
  {
    jcoz_sleep(SIGNAL_FREQ);

    signal_user_threads();

  }

  jcoz_sleep(SIGNAL_FREQ);
  // memory barrier to ensure that `in_experiment` is false before the user threads are signalled again
  std::atomic_thread_fence(std::memory_order_acquire);
  in_experiment = false;
  std::atomic_thread_fence(std::memory_order_release);
  signal_user_threads();
  jcoz_sleep(SIGNAL_FREQ);

  // TODO this is to avoid calling up to a synchronized java method, resulting in a deadlock,
  //  this might still be a race condition with Stop()
  if (!_running)
  {
    delete[] current_experiment.location_ranges;
    return;
  }

  auto expEnd = std::chrono::high_resolution_clock::now();

  current_experiment.delay = global_delay;
  current_experiment.points_hit = points_hit;
  points_hit = 0;
  current_experiment.duration = (expEnd - start).count();
  global_delay = 0;

  char *sig = getClassFromMethodIDLocation(current_experiment.method_id);
  // throw out bad samples
  if (sig == NULL)
    return;
  cleanSignature(sig);

  // Maybe update the experiment length
  Profiler::update_experiment_length();

  bci_hits::add_hit(sig, current_experiment.method_id, current_experiment.lineno, current_experiment.bci);

  // Log the run experiment results
  logger->info(
      "Ran experiment: [class: {class}:{line_no}] [speedup: {speedup}] [points hit: {points_hit}] [delay: {delay}] [duration: {duration}] [new exp time: {exp_time}]",
      fmt::arg("exp_time", experiment_time), fmt::arg("speedup", current_experiment.speedup), fmt::arg("points_hit", current_experiment.points_hit),
      fmt::arg("delay", current_experiment.delay), fmt::arg("duration", current_experiment.duration), fmt::arg("class", sig),
      fmt::arg("line_no", current_experiment.lineno));
  logger->flush();
  // Append the experiment results to the output file
  std::stringstream experiment_data;
  long effectiveDuration = current_experiment.duration - current_experiment.delay;
  experiment_data << sig << ":" << current_experiment.lineno << "," << current_experiment.speedup << "," << current_experiment.duration << "," << effectiveDuration << "," << current_experiment.points_hit << "\n";
  std::ofstream output_file;
  output_file.open(kOutputFile.data(), std::ios_base::app);
  output_file << experiment_data.rdbuf();
  output_file.close();

  delete[] current_experiment.location_ranges;

  logger->debug("Finished experiment, flushed logs, and deleted current location ranges.");
}

// == and < operators for JVMPICallFrame - needed for sort and unique
bool operator<(const JVMPI_CallFrame &lhs, const JVMPI_CallFrame &rhs)
{
  if (lhs.method_id == rhs.method_id)
  {
    return lhs.lineno < rhs.lineno;
  }
  else
  {
    return lhs.method_id < rhs.method_id;
  }
}

bool operator==(const JVMPI_CallFrame &lhs, const JVMPI_CallFrame &rhs)
{
  return lhs.method_id == rhs.method_id && lhs.lineno == rhs.lineno;
}

void JNICALL
Profiler::runAgentThread(jvmtiEnv *jvmti_env, JNIEnv *jni_env, void *args)
{
  srand(time(NULL));
  global_delay = 0;
  startup_time = std::chrono::high_resolution_clock::now().time_since_epoch();
  agent_pthread = pthread_self();
  while (!__sync_bool_compare_and_swap(&user_threads_lock, 0, 1))
    ;
  std::atomic_thread_fence(std::memory_order_acquire);
  user_threads.erase(curr_ut);
  curr_ut = NULL;
  user_threads_lock = 0;
  std::atomic_thread_fence(std::memory_order_release);
  if (warmup_time != 0)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(warmup_time));
  }
  prof_ready = true;

  while (_running)
  {
    logger->debug("Starting new agent thread _running loop...");
    // 30 * SIGNAL_FREQ with randomization should give us roughly
    // the same number of iterations as doing 20 * SIGNAL_FREQ without
    // randomization.
    long total_needed_time = 30 * SIGNAL_FREQ;
    long total_accrued_time = 0;
    while (total_accrued_time < total_needed_time)
    {
      // Sleep some randomized time to avoid bias in the profiler.
      long curr_sleep = 2 * SIGNAL_FREQ - (rand() % SIGNAL_FREQ);
      jcoz_sleep(curr_sleep);
      signal_user_threads();
      total_accrued_time += curr_sleep;
      logger->trace("Slept for {sleep_time} time. {remaining_time} Remaining.",
                    fmt::arg("sleep_time", curr_sleep),
                    fmt::arg("remaining_time", total_needed_time - total_accrued_time));
    }

    // Copy shared and unsafe static_call_frames to variable
    // that is local for the profiler thread, so it's enough
    // to synchronize on copying here and clearing later.
    // Any other code between these actions does not require
    // to be locked, too, since this lock is intended
    // to protect static_call_frames only.
    while (!__sync_bool_compare_and_swap(&frame_lock, 0, 1))
      ;
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int i = 0; (i < call_index) && (i < NUM_STATIC_CALL_FRAMES); i++)
    {
      call_frames.push_back(static_call_frames[i]);
    }
    frame_lock = 0;
    std::atomic_thread_fence(std::memory_order_release);

    // Filter `call_frames` such that it only contains unique JVMPI_CallFrames
    if (call_frames.size() > 0)
    {
      call_index = 0;
      logger->trace("Profiler::runAgentThread() - Found {} call frames", call_frames.size());
      std::sort(call_frames.begin(), call_frames.end());
      auto last = std::unique(call_frames.begin(), call_frames.end());
      call_frames.erase(last, call_frames.end());
      logger->trace("Profiler::runAgentThread() - Found {} unique call frames", call_frames.size());
      std::random_shuffle(call_frames.begin(), call_frames.end());
      JVMPI_CallFrame exp_frame;
      jint num_entries;
      jvmtiLineNumberEntry *entries = NULL;
      for (int i = 0; i < call_frames.size(); i++)
      {
        exp_frame = call_frames.at(i);
        jvmtiError lineNumberError = jvmti->GetLineNumberTable(exp_frame.method_id, &num_entries, &entries);
        if (lineNumberError == JVMTI_ERROR_NONE)
        {
          break;
        }
        else
        {
          jvmti->Deallocate((unsigned char *)entries);
        }
      }

      // If we don't find anything in scope, try again
      if (entries == NULL)
      {
        // Clear call frames here before we return to the beginning of the while loop and sample for call frames again
        // Previous comment ... TODO(dcv): Should we clear the call frames here?
        logger->info("No in scope frames found. Clearing call frames and then trying again.");
        call_frames.clear();
        continue;
      }
      
      logger->debug("Found in scope frames. Choosing a frame and running experiment...");
      current_experiment.method_id = exp_frame.method_id;
      current_experiment.bci = exp_frame.lineno;
      jint start_line;
      jint end_line; // exclusive
      std::vector<std::pair<jint, jint>> location_ranges;

      for (int i = 1; i <= num_entries; i++)
      {
        if (i == num_entries || entries[i].start_location > exp_frame.lineno)
        {
          current_experiment.lineno = entries[i - 1].line_number;
          break;
        }
      }

      for (int i = 0; i < num_entries; i++)
      {
        if (entries[i].line_number == current_experiment.lineno)
        {
          if (i < num_entries - 1)
          {
            location_ranges.push_back(
                std::pair<jint, jint>(entries[i].start_location,
                                      entries[i + 1].start_location));
          }
          else
          {
            location_ranges.push_back(
                std::pair<jint, jint>(entries[i].start_location,
                                      MAX_BCI + 1));
          }
        }
      }

      current_experiment.num_ranges = location_ranges.size();
      current_experiment.location_ranges =
          new std::pair<jint, jint>[location_ranges.size()];
      for (int i = 0; i < location_ranges.size(); i++)
      {
        current_experiment.location_ranges[i] = location_ranges[i];
      }
      call_index = 0;

      runExperiment(jni_env);

      call_frames.clear();

      // Synchronize on clearing shared static_call_frames
      while (!__sync_bool_compare_and_swap(&frame_lock, 0, 1))
        ;
      std::atomic_thread_fence(std::memory_order_acquire);
      memset(static_call_frames, 0, NUM_STATIC_CALL_FRAMES * sizeof(JVMPI_CallFrame));
      frame_lock = 0;
      std::atomic_thread_fence(std::memory_order_release);

      jvmti->Deallocate((unsigned char *)entries);
      logger->trace("Finished clearing frames and deallocating entries...");
    }
    else
    {
      logger->debug("No frames found in agent thread. Trying sampling loop again...");
    }
  }

  logger->info("Profiler done running");
  profile_done = true;
}

bool Profiler::thread_in_main(jthread thread)
{
  jvmtiThreadInfo info;
  jvmtiError err = jvmti->GetThreadInfo(thread, &info);
  if (err != JVMTI_ERROR_NONE)
  {
    if (err == JVMTI_ERROR_WRONG_PHASE)
    {
      return false;
    }
    else
    {
      logger->critical("JVMTI::GetThreadInfo returned unhandled JVMTIError. Exiting program.");
      exit(1);
    }
  }

  jvmtiThreadGroupInfo thread_grp;
  err = jvmti->GetThreadGroupInfo(info.thread_group, &thread_grp);
  if (err != JVMTI_ERROR_NONE)
  {
    if (err == JVMTI_ERROR_WRONG_PHASE)
    {
      return false;
    }
    else
    {
      logger->critical("JVMTI::GetThreadGroupInfo returned unhandled JVMTIError. Exiting program.");
      exit(1);
    }
  }

  return !strcmp(thread_grp.name, "main");
}

void Profiler::addUserThread(jthread thread)
{
  if (thread_in_main(thread))
  {
    logger->debug("Adding user thread");
    curr_ut = new struct UserThread();
    curr_ut->thread = pthread_self();
    curr_ut->local_delay = global_delay;
    curr_ut->java_thread = thread;
    curr_ut->points_hit = 0;

    // user threads lock
    while (!__sync_bool_compare_and_swap(&user_threads_lock, 0, 1))
      ;
    std::atomic_thread_fence(std::memory_order_acquire);
    user_threads.insert(curr_ut);
    user_threads_lock = 0;
    std::atomic_thread_fence(std::memory_order_release);
  }
  else
  {
    curr_ut = NULL;
  }
}

void Profiler::removeUserThread(jthread thread)
{
  if (curr_ut != NULL)
  {
    logger->debug("Removing user thread");
    points_hit += curr_ut->points_hit;
    curr_ut->points_hit = 0;

    long sleep_time = global_delay - curr_ut->local_delay;
    if (sleep_time > 0)
    {
      jcoz_sleep(std::max(0L, sleep_time));
    }
    else
    {
      global_delay += std::labs(sleep_time);
    }

    // user threads lock
    while (!__sync_bool_compare_and_swap(&user_threads_lock, 0, 1))
      ;
    std::atomic_thread_fence(std::memory_order_acquire);
    user_threads.erase(curr_ut);
    user_threads_lock = 0;
    std::atomic_thread_fence(std::memory_order_release);

    delete curr_ut;
  }
}

bool inline Profiler::inExperiment(JVMPI_CallFrame &curr_frame)
{
  if (curr_frame.method_id != current_experiment.method_id)
  {
    return false;
  }

  for (int i = 0; i < current_experiment.num_ranges; i++)
  {
    if (curr_frame.lineno >= current_experiment.location_ranges[i].first && curr_frame.lineno < current_experiment.location_ranges[i].second)
    {
      return true;
    }
  }
  return false;
}

bool inline Profiler::frameInScope(JVMPI_CallFrame &curr_frame)
{
  return in_scope_ids.count((void *)curr_frame.method_id) > 0;
}

void Profiler::addInScopeMethods(jint method_count, jmethodID *methods)
{
  logger->debug("Adding {:d} in scope methods\n", method_count);
  while (!__sync_bool_compare_and_swap(&in_scope_lock, 0, pthread_self()))
    ;
  std::atomic_thread_fence(std::memory_order_acquire);
  for (int i = 0; i < method_count; i++)
  {
    void *method = (void *)methods[i];
    logger->trace("Adding in scope method {}\n", method);
    in_scope_ids.insert(method);
  }
  in_scope_lock = 0;
  std::atomic_thread_fence(std::memory_order_release);
}

void Profiler::clearInScopeMethods()
{
  logger->debug("Clearing current in scope methods.");
  while (!__sync_bool_compare_and_swap(&in_scope_lock, 0, pthread_self()))
    ;
  in_scope_ids.clear();
  in_scope_lock = 0;
}

void Profiler::addProgressPoint(jint method_count, jmethodID *methods)
{
  logger->debug("Within Profiler::addProgressPoint");
  // Only ever set progress point once
  if (end_to_end || ((progress_point->method_id) != nullptr))
  {
    logger->debug("Progress point has already been set - returning from Profiler::addProgressPoint");
    return;
  }

  for (int i = 0; i < method_count; i++)
  {
    jint entry_count;
    JvmtiScopedPtr<jvmtiLineNumberEntry> entries(jvmti);
    jvmtiError err = jvmti->GetLineNumberTable(methods[i], &entry_count, entries.GetRef());
    if (err != JVMTI_ERROR_NONE)
    {
      printf("Error getting line number entry table in addProgressPoint. Error: %d\n", err);

      continue;
    }

    for (int j = 0; j < entry_count; j++)
    {
      jvmtiLineNumberEntry curr_entry = entries.Get()[j];
      jint curr_lineno = curr_entry.line_number;
      if (curr_lineno == (progress_point->lineno))
      {
        progress_point->method_id = methods[i];
        progress_point->location = curr_entry.start_location;
        jvmti->SetBreakpoint(progress_point->method_id, progress_point->location);
        logger->info("Progress point set");
        return;
      }
    }
  }
  logger->critical("Progress point not set - check that correct line number has been passed on cli. Exiting program");
  exit(1);
}

void Profiler::setJNI(JNIEnv *jni)
{
  jni_ = jni;
}

void Profiler::prepare_scope(std::string &scope)
{
  std::replace(scope.begin(), scope.end(), '.', '/');
}

void Profiler::add_search_scope(std::string &scope)
{
  search_scopes.push_back(scope);
}

void Profiler::add_ignored_scope(std::string &scope)
{
  ignored_scopes.push_back(scope);
}

void Profiler::Handle(int signum, siginfo_t *info, void *context)
{
  if (!prof_ready)
  {
    logger->debug("Profiler::Handle - Profiler not ready; signal not handled");
    return;
  }
  IMPLICITLY_USE(signum);
  IMPLICITLY_USE(info);

  JNIEnv *env = Accessors::CurrentJniEnv();
  if (env == NULL)
  {
    logger->debug("Profiler::Handle - Current JNI env is NULL; signal not handled");
    return;
  }

  JVMPI_CallTrace trace;
  JVMPI_CallFrame frames[kMaxFramesToCapture];
  // We have to set every byte to 0 instead of just initializing the
  // individual fields, because the structs might be padded, and we
  // use memcmp on it later.  We can't use memset, because it isn't
  // async-safe.
  char *base = reinterpret_cast<char *>(frames);
  for (char *p = base;
       p < base + sizeof(JVMPI_CallFrame) * kMaxFramesToCapture; p++)
  {
    *p = 0;
  }

  trace.frames = frames;
  trace.env_id = env;

  ASGCTType asgct = Asgct::GetAsgct();
  (*asgct)(&trace, kMaxFramesToCapture, context);

  if (trace.num_frames < 0)
  {
    int idx = -trace.num_frames;
    if (idx > kNumCallTraceErrors)
    {
      logger->debug("Profiler::Handle - Num frames < 0 and error code outside of range of enum kNumCallTraceErrors; signal not handled");
      return;
    }
  }

  if (!in_experiment)
  {
    // lock in scope
    curr_ut->local_delay = 0;
    bool has_lock = in_scope_lock == pthread_self();
    if (!has_lock)
    {
      while (!__sync_bool_compare_and_swap(&in_scope_lock, 0,
                                           pthread_self()))
        ;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int i = 0; i < trace.num_frames; i++)
    {
      JVMPI_CallFrame &curr_frame = trace.frames[i];
      if (frameInScope(curr_frame))
      {
        // lock frame lock
        while (!__sync_bool_compare_and_swap(&frame_lock, 0, 1))
          ;
        std::atomic_thread_fence(std::memory_order_acquire);
        int index = call_index.fetch_add(1);
        if (index < NUM_STATIC_CALL_FRAMES)
        {
          static_call_frames[index] = curr_frame;
        }
        frame_lock = 0;
        std::atomic_thread_fence(std::memory_order_release);
        break;
      }
    }
    if (!has_lock)
    {
      in_scope_lock = 0;
    }
    std::atomic_thread_fence(std::memory_order_release);
  }
  else
  {
    curr_ut->num_signals_received++;
    for (int i = 0; i < trace.num_frames; i++)
    {
      JVMPI_CallFrame &curr_frame = trace.frames[i];
      if (inExperiment(curr_frame))
      {
        curr_ut->local_delay += current_experiment.delay;
        break;
      }
    }

    if (curr_ut->num_signals_received == 10)
    {
      long sleep_diff = global_delay - curr_ut->local_delay;
      if (sleep_diff > 0)
      {
        curr_ut->local_delay += jcoz_sleep(sleep_diff);
      }
      else
      {
        global_delay += std::labs(sleep_diff);
      }

      curr_ut->num_signals_received = 0;
    }

    points_hit += curr_ut->points_hit;
    curr_ut->points_hit = 0;
  }
}

struct sigaction SignalHandler::SetAction(
    void (*action)(int, siginfo_t *, void *))
{
  struct sigaction sa;
  sa.sa_handler = NULL;
  sa.sa_sigaction = action;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;

  sigemptyset(&sa.sa_mask);

  struct sigaction old_handler;
  if (sigaction(SIGPROF, &sa, &old_handler) != 0)
  {
    return old_handler;
  }

  return old_handler;
}

void Profiler::Start()
{
  logger->info("Starting profiler ...");
  action_for_sigprof_ = handler_.SetAction(&Profiler::Handle);
  call_frames.reserve(2000);
  _running = true;
}

char *Profiler::getClassFromMethodIDLocation(jmethodID id)
{
  jclass clazz;
  jvmtiError classErr = jvmti->GetMethodDeclaringClass(id, &clazz);
  if (classErr != JVMTI_ERROR_NONE)
  {
    return NULL;
  }

  char *sig;
  jvmtiError classSigErr = jvmti->GetClassSignature(clazz, &sig, NULL);
  if (classSigErr != JVMTI_ERROR_NONE)
  {
    return NULL;
  }

  return sig;
}

void Profiler::cleanSignature(char *sig)
{
  int sig_len = strlen(sig);
  if (sig_len < 3)
  {
    return;
  }

  for (int i = 0; i < sig_len - 1; i++)
  {
    sig[i] = sig[i + 1];
  }

  sig[sig_len - 2] = '\0';

  for (int i = 0; i < sig_len - 2; i++)
  {
    if (sig[i] == '/')
    {
      sig[i] = '.';
    }
    else if (sig[i] == '$')
    {
      sig[i] = '\0';
      return;
    }
  }
}

void Profiler::clearProgressPoint()
{
  if (!end_to_end && (progress_point->method_id != nullptr))
  {
    logger->info("Clearing progress point");
    jvmti->ClearBreakpoint(progress_point->method_id, progress_point->location);
    progress_point->method_id = nullptr;
  }
}

void Profiler::Stop()
{

  // Wait until we get to the end of the run
  // and then flush the profile output
  logger->info("Stopping profiler");
  if (_running)
  {
    if (end_to_end)
    {
      points_hit++;
    }

    _running = false;

    logger->info("Waiting for profiler to finish current cycle...");
    while (!profile_done)
      ;

    logger->info("Profiler finished current cycle...");
  }

  std::vector<std::string> hits = bci_hits::create_dump(jvmti);
  for (std::string &hit : hits)
  {
    logger->info("{}", hit);
  }
  clearInScopeMethods();
  signal(SIGPROF, SIG_IGN);
  logger->flush();
}

void Profiler::setJVMTI(jvmtiEnv *jvmti_env)
{
  jvmti = jvmti_env;
}

void JNICALL
Profiler::HandleBreakpoint(
    jvmtiEnv *jvmti,
    JNIEnv *jni_env,
    jthread thread,
    jmethodID method_id,
    jlocation location)
{
  curr_ut->points_hit += in_experiment;
}