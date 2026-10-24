#include <assert.h>
#include "../src/miniatura_lifecycle.h"

int main(void) {
  MiniPauseAdmission p = {0};
  assert(!mini_pause_admit(&p, 0, 0, 0, 0));
  assert(!mini_pause_admit(&p, 10, 1, 1, 0));
  assert(!mini_pause_admit(&p, 4000, 1, 1, 0));
  assert(!mini_pause_admit(&p, 5000, 1, 0, 1));
  assert(!mini_pause_admit(&p, 6000, 1, 0, 0));
  assert(!mini_pause_admit(&p, 7999, 1, 0, 0));
  assert(mini_pause_admit(&p, 8000, 1, 0, 0));
  // Timeout, memory release or failure cannot grant another attempt.
  assert(!mini_pause_admit(&p, 20000, 1, 0, 0));
  assert(!mini_pause_admit(&p, 21000, 0, 1, 0));
  assert(!mini_pause_admit(&p, 22000, 1, 0, 0));
  assert(!mini_pause_admit(&p, 25000, 1, 0, 0));
  assert(!mini_pause_admit(&p, 26000, 0, 0, 0));
  assert(!mini_pause_admit(&p, 27000, 1, 0, 0));
  assert(mini_pause_admit(&p, 29000, 1, 0, 0));
  return 0;
}
