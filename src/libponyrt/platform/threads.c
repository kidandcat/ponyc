#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <platform.h>

#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_BSD) || defined(PLATFORM_IS_EMSCRIPTEN)
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#endif

#if defined(PLATFORM_IS_FREEBSD) || defined(PLATFORM_IS_DRAGONFLY) || defined(PLATFORM_IS_OPENBSD)
#include <pthread_np.h>
#endif
#if defined(PLATFORM_IS_FREEBSD)
#include <sys/cpuset.h>
typedef cpuset_t cpu_set_t;
#endif

#if defined(PLATFORM_IS_LINUX)

#include <dlfcn.h>
#include <stdio.h>

struct bitmask
{
  unsigned long size;
  unsigned long *maskp;
};

static int (*_numa_available)();
static int (*_numa_num_task_cpus)();
static void (*_numa_set_localalloc)();
static int (*_numa_node_of_cpu)(int cpu);
static void* (*_numa_alloc_onnode)(size_t size, int node);

static void* (*_numa_alloc)(size_t size);
static void (*_numa_free)(void* mem, size_t size);

static struct bitmask** _numa_all_nodes_ptr;
static struct bitmask* (*_numa_allocate_cpumask)();
static void (*_numa_bitmask_free)(struct bitmask*);
static int (*_numa_node_to_cpus)(int, struct bitmask*);
static unsigned int (*_numa_bitmask_weight)(const struct bitmask *);
static int (*_numa_bitmask_isbitset)(const struct bitmask*, unsigned int);

static bool use_numa = false;

#define LOAD_SYMBOL(sym) \
{ \
  typedef typeof(_##sym) f; \
  _##sym = (f)dlsym(lib, #sym); \
  err += (_##sym == NULL); \
}

bool ponyint_numa_init()
{
  void* lib = dlopen("libnuma.so.1", RTLD_LAZY);
  int err = 0;

  if(lib == NULL)
    return false;

  LOAD_SYMBOL(numa_available);
  LOAD_SYMBOL(numa_num_task_cpus);
  LOAD_SYMBOL(numa_set_localalloc);
  LOAD_SYMBOL(numa_node_of_cpu);
  LOAD_SYMBOL(numa_alloc_onnode);

  LOAD_SYMBOL(numa_alloc);
  LOAD_SYMBOL(numa_free);

  LOAD_SYMBOL(numa_all_nodes_ptr);
  LOAD_SYMBOL(numa_allocate_cpumask);
  LOAD_SYMBOL(numa_bitmask_free);
  LOAD_SYMBOL(numa_node_to_cpus);
  LOAD_SYMBOL(numa_bitmask_weight);
  LOAD_SYMBOL(numa_bitmask_isbitset);

  if(err != 0)
    return false;

  if(_numa_available() == -1)
    return false;

  use_numa = true;
  _numa_set_localalloc();
  return true;
}

uint32_t ponyint_numa_cores()
{
  if(use_numa)
    return _numa_num_task_cpus();

  return 0;
}

static uint32_t numa_core_list(cpu_set_t* mask, uint32_t index, uint32_t* list)
{
  uint32_t nodes = _numa_bitmask_weight(*_numa_all_nodes_ptr);
  struct bitmask* cpu = _numa_allocate_cpumask();
  uint32_t node_count = 0;

  for(uint32_t i = 0; node_count < nodes; i++)
  {
    if(!_numa_bitmask_isbitset(*_numa_all_nodes_ptr, i))
      continue;

    node_count++;
    _numa_node_to_cpus(i, cpu);

    uint32_t cpus = _numa_bitmask_weight(cpu);
    uint32_t cpu_count = 0;

    for(uint32_t j = 0; cpu_count < cpus; j++)
    {
      if(!_numa_bitmask_isbitset(cpu, j))
        continue;

      cpu_count++;

      if(CPU_ISSET(j, mask))
        list[index++] = j;
    }
  }

  _numa_bitmask_free(cpu);
  return index;
}

uint32_t ponyint_numa_core_list(cpu_set_t* hw_cpus, cpu_set_t* ht_cpus,
  uint32_t* list)
{
  if(!use_numa)
    return 0;

  // Create an ordered list of cpus. Physical CPUs grouped by NUMA node come
  // first, followed by hyperthreaded CPUs grouped by NUMA node.
  uint32_t index = 0;

  index = numa_core_list(hw_cpus, index, list);
  index = numa_core_list(ht_cpus, index, list);

  return index;
}

uint32_t ponyint_numa_node_of_cpu(uint32_t cpu)
{
  if(use_numa)
    return _numa_node_of_cpu(cpu);

  return 0;
}

#endif

bool ponyint_thread_create(pony_thread_id_t* thread, thread_fn start,
  uint32_t cpu, void* arg)
{
  bool ret = true;
  (void)cpu;

#if defined(PLATFORM_IS_WINDOWS)
  uintptr_t p = _beginthreadex(NULL, 0, start, arg, 0, NULL);

  if(!p)
    return false;

  *thread = (HANDLE)p;
#elif defined(PLATFORM_IS_EMSCRIPTEN)
  if(pthread_create(thread, NULL, start, arg))
    ret = false;
#else
  bool setstack_called = false;
  struct rlimit limit;
  pthread_attr_t attr;
  pthread_attr_t* attr_p = &attr;
  pthread_attr_init(attr_p);

  // Some systems, e.g., macOS, hav a different default default
  // stack size than the typical system's RLIMIT_STACK.
  // Let's use RLIMIT_STACK's current limit if it is sane.
  if(getrlimit(RLIMIT_STACK, &limit) == 0 &&
    limit.rlim_cur != RLIM_INFINITY &&
    limit.rlim_cur >= (rlim_t)PTHREAD_STACK_MIN)
  {
#if defined(PLATFORM_IS_LINUX)
    if(cpu != (uint32_t)-1 && use_numa)
    {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpu, &set);

      int node = _numa_node_of_cpu(cpu);
      void* stack = _numa_alloc_onnode((size_t)limit.rlim_cur, node);
      if (stack != NULL) {
        pthread_attr_setstack(&attr, stack, (size_t)limit.rlim_cur);
        setstack_called = true;
      }
    }
#endif
    if(! setstack_called)
      pthread_attr_setstacksize(&attr, (size_t)limit.rlim_cur);
  } else {
    attr_p = NULL;
  }

  if(pthread_create(thread, attr_p, start, arg))
    ret = false;
  pthread_attr_destroy(&attr);
#endif
  return ret;
}

bool ponyint_thread_join(pony_thread_id_t thread)
{
#ifdef PLATFORM_IS_WINDOWS
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
#else
  if(pthread_join(thread, NULL))
    return false;
#endif

  return true;
}

void ponyint_thread_detach(pony_thread_id_t thread)
{
#ifndef PLATFORM_IS_WINDOWS
  pthread_detach(thread);
#endif
}

pony_thread_id_t ponyint_thread_self()
{
#ifdef PLATFORM_IS_WINDOWS
  return GetCurrentThread();
#else
  return pthread_self();
#endif
}

#if defined(USE_SCHEDULER_SCALING_PTHREADS)
void ponyint_thread_suspend(pony_signal_event_t signal, pthread_mutex_t* mut)
#else
void ponyint_thread_suspend(pony_signal_event_t signal)
#endif
{
#ifdef PLATFORM_IS_WINDOWS
  WaitForSingleObject(signal, INFINITE);
#elif defined(USE_SCHEDULER_SCALING_PTHREADS)
  int ret;

  // wait for condition variable (will sleep and release mutex)
  ret = pthread_cond_wait(signal, mut);
  // TODO: What to do if `ret` is an unrecoverable error?
  (void) ret;
#else
  int sig;
  sigset_t sigmask;
  sigemptyset(&sigmask);         /* zero out all bits */
  sigaddset(&sigmask, signal);   /* unblock desired signal */

  // sleep waiting for signal to wake up again
  sigwait(&sigmask, &sig);
#endif
}

int ponyint_thread_wake(pony_thread_id_t thread, pony_signal_event_t signal)
{
  int ret;
#if defined(PLATFORM_IS_WINDOWS)
  (void) thread;
  ret = !SetEvent(signal);
#elif defined(USE_SCHEDULER_SCALING_PTHREADS)
  (void) thread;
  // signal condition variable
  ret = pthread_cond_signal(signal);
#else
  ret = pthread_kill(thread, signal);
#endif
  return ret;
}

#ifdef PLATFORM_IS_ARM32
uint32_t __pony_popcount(uint32_t x) {
  return (uint32_t)__builtin_popcount(x);
}

uint32_t __pony_ffs(uint32_t x) {
  return (uint32_t)__builtin_ffs((int)x);
}

uint32_t __pony_ffsll(uint64_t x) {
  return (uint32_t)__builtin_ffsll((long long int)x);
}

uint32_t __pony_ctz(uint32_t x) {
  return (uint32_t)__builtin_ctz(x);
}

uint32_t __pony_clz(uint32_t x) {
  return (uint32_t)__builtin_clz(x);
}

uint32_t __pony_clzll(uint64_t x) {
  return (uint32_t)__builtin_clzll(x);
}
#endif
