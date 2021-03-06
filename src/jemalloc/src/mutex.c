#define	JEMALLOC_MUTEX_C_
#include "jemalloc/internal/jemalloc_internal.h"

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
#include <dlfcn.h>
#endif

#ifndef _CRT_SPINCOUNT
#define	_CRT_SPINCOUNT 4000
#endif

/******************************************************************************/
/* Data. */

#ifdef JEMALLOC_LAZY_LOCK
bool isthreaded = false;
#endif
#ifdef JEMALLOC_MUTEX_INIT_CB
static bool		postpone_init = true;
static malloc_mutex_t	*postponed_mutexes = NULL;
#endif

/******************************************************************************/
/*
 * We intercept pthread_create() calls in order to toggle isthreaded if the
 * process goes multi-threaded.
 */

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
static void	pthread_create_once(void);
static int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_once(void)
{

	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
	if (pthread_create_fptr == NULL) {
		malloc_write("<jemalloc>: Error in dlsym(RTLD_NEXT, "
		    "\"pthread_create\")\n");
		abort();
	}

	isthreaded = true;
}

JEMALLOC_EXPORT int
pthread_create(pthread_t *__restrict thread,
    const pthread_attr_t *__restrict attr, void *(*start_routine)(void *),
    void *__restrict arg)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	pthread_once(&once_control, pthread_create_once);

	return (pthread_create_fptr(thread, attr, start_routine, arg));
}
#endif

/******************************************************************************/

#ifdef JEMALLOC_MUTEX_INIT_CB
JEMALLOC_EXPORT int	_pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
    void *(calloc_cb)(size_t, size_t));

static void *
base_calloc_wrapper(size_t number, size_t size)
{
	return base_calloc(&base_pool, number, size);
}

/* XXX We need somewhere to allocate mutexes from during early initialization */
#define BOOTSTRAP_POOL_SIZE 4096
#define BP_MASK 0xfffffffffffffff0UL
static char bootstrap_pool[BOOTSTRAP_POOL_SIZE] __attribute__((aligned (16)));
static char *bpp = bootstrap_pool;

static void *
bootstrap_calloc(size_t number, size_t size)
{
	size_t my_size = ((number * size) + 0xf) & BP_MASK;
	bpp += my_size;
	if ((bpp - bootstrap_pool) > BOOTSTRAP_POOL_SIZE) {
		return NULL;
	}
	return (void *)(bpp - my_size);
}
#endif

bool
malloc_mutex_init(malloc_mutex_t *mutex)
{

#ifdef _WIN32
	if (!InitializeCriticalSectionAndSpinCount(&mutex->lock,
	    _CRT_SPINCOUNT))
		return (true);
#elif (defined(JEMALLOC_OSSPIN))
	mutex->lock = 0;
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
	if (postpone_init) {
		mutex->postponed_next = postponed_mutexes;
		postponed_mutexes = mutex;
	} else {
		if (_pthread_mutex_init_calloc_cb(&mutex->lock,
			base_calloc_wrapper) != 0)
			return (true);
	}
#else
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0)
		return (true);
	pthread_mutexattr_settype(&attr, MALLOC_MUTEX_TYPE);
	if (pthread_mutex_init(&mutex->lock, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return (true);
	}
	pthread_mutexattr_destroy(&attr);
#endif
	return (false);
}

void
malloc_mutex_prefork(malloc_mutex_t *mutex)
{

	malloc_mutex_lock(mutex);
}

void
malloc_mutex_postfork_parent(malloc_mutex_t *mutex)
{

	malloc_mutex_unlock(mutex);
}

bool
mutex_boot(void)
{

#ifdef JEMALLOC_MUTEX_INIT_CB
	postpone_init = false;
	while (postponed_mutexes != NULL) {
		if (_pthread_mutex_init_calloc_cb(&postponed_mutexes->lock,
		    bootstrap_calloc) != 0)
			return (true);
		postponed_mutexes = postponed_mutexes->postponed_next;
	}
#endif
	return (false);
}

void
malloc_mutex_postfork_child(malloc_mutex_t *mutex)
{

#if (defined(JEMALLOC_MUTEX_INIT_CB) || defined(JEMALLOC_DISABLE_BSD_MALLOC_HOOKS))
	malloc_mutex_unlock(mutex);
#else
	if (malloc_mutex_init(mutex)) {
		malloc_printf("<jemalloc>: Error re-initializing mutex in "
		    "child\n");
		if (opt_abort)
			abort();
	}
#endif
}

void
malloc_rwlock_prefork(malloc_rwlock_t *rwlock)
{

	malloc_rwlock_wrlock(rwlock);
}

void
malloc_rwlock_postfork_parent(malloc_rwlock_t *rwlock)
{

	malloc_rwlock_unlock(rwlock);
}

void
malloc_rwlock_postfork_child(malloc_rwlock_t *rwlock)
{

#if (defined(JEMALLOC_MUTEX_INIT_CB) || defined(JEMALLOC_DISABLE_BSD_MALLOC_HOOKS))
	malloc_rwlock_unlock(rwlock);
#else
	if (malloc_rwlock_init(rwlock)) {
		malloc_printf("<jemalloc>: Error re-initializing rwlock in "
		    "child\n");
		if (opt_abort)
			abort();
	}
#endif
}
