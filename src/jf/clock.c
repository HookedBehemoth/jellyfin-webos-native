#include "clock.h"

#include <pthread.h>
#include <time.h>

/* A frame that lands this far behind the last one is a stray, not a rewind. */
#define BACKWARD_TOLERANCE_NS (100 * 1000000LL)
/* Past this, the pipeline has stopped reporting and the anchor is not worth
 * projecting. */
#define FRESHNESS_NS (250 * 1000000LL)

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool valid;
static bool running;
static int64_t sample_pts;
static int64_t sample_host;

int64_t jf_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void jf_clock_reset(void) {
  pthread_mutex_lock(&mutex);
  valid = false;
  running = false;
  sample_pts = 0;
  sample_host = 0;
  pthread_mutex_unlock(&mutex);
}

void jf_clock_sample(int64_t pts_ns, int64_t host_ns) {
  if (pts_ns < 0)
    return;
  pthread_mutex_lock(&mutex);
  /* A repeated value is the same frame seen twice, not time standing still: a
   * polled clock is quantized to the frame, so moving the anchor forward on a
   * repeat would make the projection sag by up to a frame interval. Keep the
   * first sighting. */
  if (!valid ||
      (pts_ns != sample_pts && pts_ns + BACKWARD_TOLERANCE_NS >= sample_pts)) {
    sample_pts = pts_ns;
    sample_host = host_ns;
    valid = true;
  }
  pthread_mutex_unlock(&mutex);
}

void jf_clock_set_running(bool value) {
  pthread_mutex_lock(&mutex);
  running = value;
  pthread_mutex_unlock(&mutex);
}

bool jf_clock_ready(void) {
  pthread_mutex_lock(&mutex);
  bool ready = valid;
  pthread_mutex_unlock(&mutex);
  return ready;
}

int64_t jf_clock_pts(void) {
  const int64_t now = jf_now_ns();
  pthread_mutex_lock(&mutex);
  int64_t result = JF_CLOCK_NONE;
  if (valid) {
    if (!running) {
      result = sample_pts; /* held, not projected */
    } else {
      const int64_t age = now - sample_host;
      if (age >= 0 && age <= FRESHNESS_NS)
        result = sample_pts + age;
    }
  }
  pthread_mutex_unlock(&mutex);
  return result;
}

int64_t jf_clock_host_for(int64_t pts_ns) {
  pthread_mutex_lock(&mutex);
  /* Deliberately not gated on freshness: this answers "when is this timestamp
   * due", and the anchor stays correct while frames are merely not being
   * reported - which is exactly the stretch in which audio must keep flowing to
   * the same line. */
  int64_t result = valid ? sample_host + (pts_ns - sample_pts) : JF_CLOCK_NONE;
  pthread_mutex_unlock(&mutex);
  return result;
}
