// thready.c
//
// https://github.com/tylerneylon/thready
//

#include "thready.h"

#include "../cstructs/cstructs.h"

#ifdef _WIN32
#include "pthreads_win.h"
#else
#include <pthread.h>
#define pthread_rwlock_rdunlock pthread_rwlock_unlock
#define pthread_rwlock_wrunlock pthread_rwlock_unlock
#define pthread_mutex_release(x)
#endif

#include <stdint.h>

// This may be useful for debugging.
#if 0
#include "../test/winutil.h"
#define prline printf("%s:%d(%s) (tid=%p)\n", basename(__FILE__), __LINE__, __FUNCTION__, pthread_self())
#endif


// Internal types and data.

typedef struct {
  void *   msg;
  thready__Id from;
} Envelope;

typedef struct {
  Array            inbox;
  pthread_mutex_t  inbox_mutex;
  pthread_cond_t   inbox_signal;  // Goes off when the inbox becomes nonempty.
} Thread;

// Maps pthread_t -> Thread *.
static Map threads = NULL;

// This is the lock for `threads`.
static pthread_rwlock_t threads_lock = PTHREAD_RWLOCK_INITIALIZER;

// This is a thread-safe way to make sure init is called exactly once.
static pthread_once_t init_control = PTHREAD_ONCE_INIT;


// Internal functions.

static int hash(void *v) {
  return (int)(intptr_t)v;
}

int eq(void *v1, void *v2) {
  return v1 == v2;
}

static Thread *new_thread_struct() {
  Thread *thread       = malloc(sizeof(Thread));
  thread->inbox        = array__new(4, sizeof(Envelope));
  thread->inbox_mutex  = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
  thread->inbox_signal = (pthread_cond_t)  PTHREAD_COND_INITIALIZER;
  return thread;
}

static void thread_releaser(void *thread_vp) {
  Thread *thread = (Thread *)thread_vp;
  pthread_mutex_release(&thread->inbox_mutex);
  array__delete(thread->inbox);
  free(thread);
}

static void init() {
  threads = map__new(hash, eq);
  threads->value_releaser = thread_releaser;

  pthread_rwlock_wrlock(&threads_lock);
  Thread *thread = new_thread_struct();
  map__set(threads, (void *)(intptr_t)pthread_self(), thread);
  pthread_rwlock_wrunlock(&threads_lock);
}

// This function runs the primary loop of all threads created with thready.
static void *thread_runner(void *receiver_vp) {
  thready__Receiver receiver = (thready__Receiver)receiver_vp;
  while (1) thready__runloop(receiver, thready__blocking);
  return NULL;
}

static void send_out_first_msg(Thread *thread, thready__Receiver receiver) {
  // Read out the first message and remove it from the inbox.
  pthread_mutex_lock(&thread->inbox_mutex);
  Envelope *orig_envelope = array__item_ptr(thread->inbox, 0);
  Envelope envelope = *orig_envelope;  // Make a copy as we're about to delete the original.
  array__remove_item(thread->inbox, orig_envelope);
  pthread_mutex_unlock(&thread->inbox_mutex);

  receiver(envelope.msg, envelope.from);
}


// Public constants.

const thready__Id thready__error   = NULL;
const thready__Id thready__success = (thready__Id) 0x1;


// Public functions.

thready__Id thready__create(thready__Receiver receiver) {
  pthread_once(&init_control, init);

  // Write-lock `threads` now so the new thread doesn't read from it before we write to it.
  pthread_rwlock_wrlock(&threads_lock);

  pthread_t pthread;
  int err = pthread_create(&pthread,       // receive thread id
                           NULL,           // NULL --> use default attributes
                           thread_runner,  // init function
                           receiver);      // init function arg
  if (err) {
    pthread_rwlock_wrunlock(&threads_lock);
    return thready__error;
  }

  // Allocate and set the new thread's inbox.
  Thread *thread = new_thread_struct();
  map__set(threads, (void *)(intptr_t)pthread, thread);  // threads[pthread] = thread

  pthread_rwlock_wrunlock(&threads_lock);

  return (thready__Id)thread;
}

void thready__exit() {
  pthread_rwlock_wrlock(&threads_lock);
  map__unset(threads, (void *)(intptr_t)pthread_self());
  pthread_rwlock_wrunlock(&threads_lock);
  pthread_exit(NULL);  // NULL -> Unused return value to pthread_join.
}

thready__Id thready__runloop(thready__Receiver receiver, int blocking) {
  pthread_once(&init_control, init);

  // Get this thread's Thread object.
  Thread *thread = (Thread *)thready__my_id();
  if (thread == thready__error) return thready__error;

  // Check if the inbox has messages.
  pthread_mutex_lock(&thread->inbox_mutex);
  int msg_count = thread->inbox->count;

  // If the inbox is empty and this call is blocking, wait for a message.
  if (blocking && msg_count == 0) {
    pthread_cond_wait(&thread->inbox_signal, &thread->inbox_mutex);
    msg_count = thread->inbox->count;
  }

  pthread_mutex_unlock(&thread->inbox_mutex);

  for (int i = 0; i < msg_count; ++i) send_out_first_msg(thread, receiver);

  return thread;
}


thready__Id thready__send(void *msg, thready__Id to_id) {
  pthread_once(&init_control, init);

  // Get this thread's Thread object.
  Thread *from = (Thread *)thready__my_id();
  if (from == thready__error) { return thready__error; }

  Thread *to = (Thread *)to_id;

  pthread_mutex_lock(&to->inbox_mutex);
  array__new_val(to->inbox, Envelope) = (Envelope){ .msg = msg, .from = from };
  // If the inbox used to be empty, let any possibly-waiting threads know it has a message.
  if (to->inbox->count == 1) pthread_cond_signal(&to->inbox_signal);
  pthread_mutex_unlock(&to->inbox_mutex);

  return thready__success;
}

thready__Id thready__my_id() {
  pthread_once(&init_control, init);

  pthread_rwlock_rdlock(&threads_lock);
  map__key_value *pair = map__find(threads, (void *)(intptr_t)pthread_self());
  pthread_rwlock_rdunlock(&threads_lock);

  if (pair == NULL) return thready__error;

  return (thready__Id)pair->value;
}
