#include <assert.h>
#include "../src/miniatura_progress.h"
int main(void) {
  MiniProgress p={0};
  assert(mini_progress(&p,1,1,0,0,0)==1);
  assert(mini_progress(&p,1,1,1,0,3999)==1);
  assert(mini_progress(&p,1,1,1,0,4000)==2);
  assert(mini_progress(&p,1,1,1,1,8000)==3);
  assert(mini_progress(&p,1,1,1,1,12000)==0);
  assert(mini_progress(&p,2,1,1,1,13000)==1);
  assert(mini_progress(&p,2,0,1,1,13001)==0);
  assert(mini_progress(&p,3,1,1,1,14000)==1);
  assert(mini_progress(&p,3,1,1,1,18000)==2);
  assert(mini_progress(&p,3,1,1,1,22000)==3);
  return 0;
}
