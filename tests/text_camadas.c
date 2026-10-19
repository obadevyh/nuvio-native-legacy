// Two zoomed layers in the same frame (Settings at 80% and the menu pill at its
// fixed 0.9) must not evict each other's fonts and lines. Before 04/10 the
// single "camada ampliada" closed everything whenever the factor changed, so
// every frame re-rasterized both layers and, on a slow TV, the per-frame text
// budget ran out before the last lines (empty pills and key hints in
// Settings). Needs GL (Mac graphics context).
#include "gfx.h"
#include "text.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>

static void desenhaEm(float esc, const char *a, const char *b) {
  float ant = gfx_escala();
  gfx_escala_sair(esc);
  txt_linha(TXT_BODY, a, 255, 255, 255, 255);
  txt_linha(TXT_CAPTION, b, 255, 255, 255, 255);
  gfx_escala_sair(ant);
}

int main(void) {
  SDL_SetHint("SDL_MAC_BACKGROUND_APP", "1");
  assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_Window *win = SDL_CreateWindow("t", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  assert(win);
  SDL_GLContext gl = SDL_GL_CreateContext(win); assert(gl);
  glViewport(0, 0, 1920, 1080); gfx_tamanho_alvo(1920, 1080); assert(gfx_iniciar());
  assert(txt_iniciar("deploy/app", 1));
  int q, r0 = 0;
  for (q = 0; q < 6; q++) {
    txt_novo_quadro();
    r0 = txt_rasterizadas;
    desenhaEm(0.8f, "Source selection", "options · 5 advanced");
    desenhaEm(0.9f, "Settings", "Back");
    desenhaEm(1.0f, "10:34", "Home");
    desenhaEm(1.2f, "Interface", "Bigger");
  }
  printf("ultimo quadro: %d linhas rasterizadas (esperado 0)\n", txt_rasterizadas - r0);
  assert(txt_rasterizadas - r0 == 0);
  // A fifth factor reuses the least used slot and still renders.
  txt_novo_quadro();
  { float ant = gfx_escala(); gfx_escala_sair(1.5f);
    TxtLinha l = txt_linha(TXT_BODY, "Nova escala", 255, 255, 255, 255);
    gfx_escala_sair(ant);
    assert(l.w > 0 && l.tex); }
  puts("ok");
  return 0;
}
