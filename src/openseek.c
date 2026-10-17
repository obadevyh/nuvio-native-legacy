// OPENSEEK: o lado com rede, fios, SDL e GL. Ver openseek.h.
//
// FIOS (todos fora do desenho; o desenho so troca ponteiros sob a trava):
//   controle   — cria o trabalho (POST /v1/previews), consulta o status (long-poll
//                curto enquanto avanca, 10 s parado, nada depois de complete),
//                baixa o VTT novo quando vtt_version sobe e o troca SEM soltar
//                texturas cuja chave (URL sem t/exp/cap) nao mudou; renova as
//                URLs de capacidade antes do exp.
//   prioridade — POST /priority com o alvo do avanco, no maximo 8/s, o ultimo
//                vence; at_ms = null quando o avanco termina. Ninguem espera.
//   busca      — um quadro por vez (o atual primeiro, depois os vizinhos):
//                baixa, confere bytes e dimensoes ANTES de decodificar, recorta.
//                JPEGs num LRU de 8, a folha decodificada (so no layout de
//                folhas) uma por vez, como no Seekr.
// O desenho (openseek_quadros) sobe os recortes prontos para texturas num LRU
// de 12 e nunca espera rede nem decodificacao. Sem quadro do ponto: placeholder
// com o anel e o tempo-alvo — nunca a ultima textura usada (SCRUBBING §5).
#include "openseek.h"
#include "openseekproto.h"
#include "osp1.h"
#include "seekrvtt.h"
#include "rede.h"
#include "dados.h"
#include "gfx.h"
#include "jpegrapido.h"
#include <SDL2/SDL.h>
#ifndef __EMSCRIPTEN__
#include <SDL2/SDL_image.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

_Static_assert(OS_SEEKR_DESLIGADO == SEEKR_DESLIGADO && OS_SEEKR_BUSCANDO == SEEKR_BUSCANDO &&
               OS_SEEKR_PRONTO == SEEKR_PRONTO && OS_SEEKR_SEM_PREVIA == SEEKR_SEM_PREVIA &&
               OS_SEEKR_CHAVE_RECUSADA == SEEKR_CHAVE_RECUSADA &&
               OS_SEEKR_LIMITE_PROVEDOR == SEEKR_LIMITE_PROVEDOR &&
               OS_SEEKR_REDE_INDISPONIVEL == SEEKR_REDE_INDISPONIVEL,
               "openseekproto.h mirrors the SEEKR_* states");

#define OS_JPG_LRU 8
#define OS_TEX_LRU 12
#define OS_PRONTOS 6
#define OS_QUERO 3
#define OS_INICIO_MS 8000L     // SCRUBBING §1: 8 s de reproducao (ou o 1o avanco)
#define OS_PRIO_MIN_MS 125u    // <= 8/s
#define OS_FALHA_MS 2000u      // um quadro que falhou espera isto antes de tentar de novo

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sinal = PTHREAD_COND_INITIALIZER;

// --- configuracao (openseek.txt) -------------------------------------------------
static int  carregado;
static char cfgBase[OS_BASE_MAX], cfgPino[OS_PINO_MAX], cfgToken[OS_TOKEN_MAX], cfgDevice[20];
static int  uiEstado = OSU_NAO_CONFIGURADO;   // ultimo resultado de parear/verificar
static int  pareando;                          // 1 com o fio de pareamento vivo
static int  verificando;                       // 1 com o fio de conferencia vivo

static void zerar(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }

static void gravarCfg(void) {   // com a trava
  char t[OS_BASE_MAX + OS_PINO_MAX + OS_TOKEN_MAX + 96];
  if (!cfgBase[0]) { dados_apagar("openseek.txt"); return; }
  snprintf(t, sizeof t, "base=%s\npino=%s\ntoken=%s\ndevice=%s\n", cfgBase,
           cfgToken[0] ? cfgPino : "", cfgToken, cfgToken[0] ? cfgDevice : "");
  if (!dados_gravar("openseek.txt", t)) { printf("[openseek] config save=failed\n"); fflush(stdout); }
  zerar(t, sizeof t);
}
static void linhaCfg(const char *txt, const char *nome, char *dst, size_t n) {
  size_t k = strlen(nome);
  const char *l = txt;
  dst[0] = 0;
  while (l && *l) {
    const char *fim = l + strcspn(l, "\r\n");
    if ((size_t)(fim - l) > k && !strncmp(l, nome, k) && l[k] == '=') {
      size_t v = (size_t)(fim - l) - k - 1;
      if (v < n) { memcpy(dst, l + k + 1, v); dst[v] = 0; }
      return;
    }
    l = fim + strspn(fim, "\r\n");
  }
}
void openseek_carregar(void) {
  char *t;
  pthread_mutex_lock(&trava);
  if (carregado) { pthread_mutex_unlock(&trava); return; }
  carregado = 1;
  t = dados_ler("openseek.txt");
  if (t) {
    char b[OS_BASE_MAX];
    linhaCfg(t, "base", b, sizeof b);
    if (!openseek_normalizar_endereco(b, cfgBase, sizeof cfgBase)) cfgBase[0] = 0;
    linhaCfg(t, "pino", cfgPino, sizeof cfgPino);
    linhaCfg(t, "token", cfgToken, sizeof cfgToken);
    linhaCfg(t, "device", cfgDevice, sizeof cfgDevice);
    if (!rede_pino_valido(cfgPino) || !cfgToken[0] || !cfgBase[0]) {
      cfgPino[0] = cfgToken[0] = cfgDevice[0] = 0;
    }
    zerar(t, strlen(t)); free(t);
  }
  uiEstado = !cfgBase[0] ? OSU_NAO_CONFIGURADO : cfgToken[0] ? OSU_PAREADO : OSU_NAO_PAREADO;
  pthread_mutex_unlock(&trava);
}
const char *openseek_endereco(void) {
  openseek_carregar();
  return cfgBase;   // escrita so pelos Ajustes, no mesmo fio
}
int openseek_pareado(void) {
  int r;
  openseek_carregar();
  pthread_mutex_lock(&trava); r = cfgToken[0] && cfgPino[0] && cfgBase[0]; pthread_mutex_unlock(&trava);
  return r;
}
static void sessaoEncerrar(void);  // adiante
int openseek_definir_endereco(const char *dig) {
  char nova[OS_BASE_MAX];
  openseek_carregar();
  if (!dig || !*dig) nova[0] = 0;
  else if (!openseek_normalizar_endereco(dig, nova, sizeof nova)) return 0;
  sessaoEncerrar();
  pthread_mutex_lock(&trava);
  if (strcmp(nova, cfgBase)) {
    snprintf(cfgBase, sizeof cfgBase, "%s", nova);
    zerar(cfgPino, sizeof cfgPino); zerar(cfgToken, sizeof cfgToken); cfgDevice[0] = 0;
    uiEstado = cfgBase[0] ? OSU_NAO_PAREADO : OSU_NAO_CONFIGURADO;
    gravarCfg();
  }
  pthread_mutex_unlock(&trava);
  return 1;
}
void openseek_esquecer(void) {
  openseek_carregar();
  sessaoEncerrar();
  pthread_mutex_lock(&trava);
  zerar(cfgPino, sizeof cfgPino); zerar(cfgToken, sizeof cfgToken); cfgDevice[0] = 0;
  uiEstado = cfgBase[0] ? OSU_NAO_PAREADO : OSU_NAO_CONFIGURADO;
  gravarCfg();
  pthread_mutex_unlock(&trava);
}
int openseek_status(char *host, size_t n) {
  int st;
  openseek_carregar();
  pthread_mutex_lock(&trava);
  st = pareando ? OSU_PAREANDO : uiEstado;
  if (host && n) {
    const char *h = cfgBase[0] ? cfgBase + 8 : "";
    size_t k = strlen(h);
    const char *dois = h[0] == '[' ? strstr(h, "]:") : strrchr(h, ':');
    if (dois) k = (size_t)(dois - h) + (h[0] == '[');
    if (k >= n) k = n - 1;
    memcpy(host, h, k); host[k] = 0;
  }
  pthread_mutex_unlock(&trava);
  return st;
}
const char *openseek_status_rotulo(int st) {
  switch (st) {
    case OSU_NAO_CONFIGURADO: return "Informe o endereço do servidor";
    case OSU_NAO_PAREADO: return "Não pareado";
    case OSU_PAREANDO: return "Pareando…";
    case OSU_VERIFICANDO: return "Conferindo o servidor…";
    case OSU_PAREADO: return "Pareado";
    case OSU_SEM_CONEXAO: return "Sem conexão com o servidor";
    case OSU_CERTIFICADO: return "O certificado do servidor mudou";
    case OSU_SEM_SUPORTE: return "Este aparelho não suporta conexão segura com o OpenSeek";
    default: return "O pareamento falhou";
  }
}

// --- pedidos ---------------------------------------------------------------------
typedef struct { volatile int *fim; } Parar;
static int pararFn(void *u) { const Parar *p = u; return p && p->fim && *p->fim; }

// Um pedido do OpenSeek. Devolve o status HTTP (0 sem resposta) e preenche r.
static int chamar(const char *metodo, const char *url, const char *token, const char *pino,
                  int semPino, const char *corpo, unsigned prazo, volatile int *fim,
                  RedeResposta *r) {
  OpenSeekPedido o;
  Parar pa = { fim };
  memset(r, 0, sizeof *r);
  if (!openseek_pedido(&o, metodo, url, token, pino, semPino, corpo, corpo ? strlen(corpo) : 0, prazo)) {
    r->erro = REDE_ENTRADA; return 0;
  }
  if (fim) { o.p.parar = pararFn; o.p.parar_usuario = &pa; }
  rede_pedir(&o.p, r);
  zerar(o.auth, sizeof o.auth);
  return r->status;
}
static int uiDeErro(const RedeResposta *r) {
  if (r->erro == REDE_INDISPONIVEL || r->erro == REDE_ENTRADA) return OSU_SEM_SUPORTE;
  if (r->curl_erro == REDE_CURL_PINO_DIFERENTE) return OSU_CERTIFICADO;
  if (r->erro != REDE_OK || !r->status || r->status >= 500) return OSU_SEM_CONEXAO;
  return OSU_FALHOU;
}

// --- pareamento (OSP1) -----------------------------------------------------------
typedef struct { char base[OS_BASE_MAX], codigo[OSP1_CODE_LEN + 1]; } Pareio;
static void *parearFio(void *u) {
  Pareio *pp = u;
  OpenSeekStart s;
  OpenSeekClaim c;
  RedeResposta r;
  unsigned char k[32];
  char url[OS_BASE_MAX + 32], corpo[256], prova[65], esperada[65];
  int res = OSU_FALHOU, http;
  const char *device = "Nuvio";
  snprintf(url, sizeof url, "%s/v1/pair/start", pp->base);
  // Primeiro contato: TLS sem pino, sem segredo nenhum (CONTRACT-v1).
  http = chamar("POST", url, NULL, NULL, 1, "{}", OS_PRAZO_API_MS, NULL, &r);
  if (http != 200 || !r.corpo || !openseek_ler_start(r.corpo, &s)) {
    res = http == 200 || http == 409 || http == 429 ? OSU_FALHOU : uiDeErro(&r);
    printf("[openseek] pair step=start http=%d result=%d\n", http, res); fflush(stdout);
    rede_resposta_limpar(&r);
    goto fim;
  }
  rede_resposta_limpar(&r);
  if (s.iter < OSP1_ITER_MIN) {            // rebaixamento: recusa
    printf("[openseek] pair step=kdf result=iterations_refused\n"); fflush(stdout);
    goto fim;
  }
  {
    Uint32 t0 = SDL_GetTicks();
    if (!osp1_derivar(pp->codigo, s.pairId, s.salt, s.pino, s.iter, OSP1_ITER_MIN, k, NULL, NULL)) goto fim;
    printf("[openseek] pair step=kdf ms=%u\n", SDL_GetTicks() - t0); fflush(stdout);
  }
  osp1_prova_cliente(k, s.pairId, device, prova);
  if (!openseek_json_claim(corpo, sizeof corpo, s.pairId, device, prova)) goto fim;
  snprintf(url, sizeof url, "%s/v1/pair/claim", pp->base);
  // Daqui em diante: so com o pino que o start mostrou (SR-2.4).
  http = chamar("POST", url, NULL, s.pino, 0, corpo, OS_PRAZO_API_MS, NULL, &r);
  if (http != 200 || !r.corpo || !openseek_ler_claim(r.corpo, &c)) {
    res = http == 403 || http == 200 ? OSU_FALHOU : uiDeErro(&r);
    printf("[openseek] pair step=claim http=%d result=%d\n", http, res); fflush(stdout);
    rede_resposta_limpar(&r);
    goto fim;
  }
  rede_resposta_limpar(&r);
  osp1_prova_servidor(k, s.pairId, c.token, esperada);
  if (!osp1_iguais(esperada, c.prova)) {   // SR-2.6: sem prova, nada e guardado
    printf("[openseek] pair step=server_proof result=mismatch\n"); fflush(stdout);
    goto fim;
  }
  pthread_mutex_lock(&trava);
  if (!strcmp(cfgBase, pp->base)) {
    snprintf(cfgPino, sizeof cfgPino, "%s", s.pino);
    snprintf(cfgToken, sizeof cfgToken, "%s", c.token);
    snprintf(cfgDevice, sizeof cfgDevice, "%s", c.deviceId);
    gravarCfg();
    res = OSU_PAREADO;
  }
  pthread_mutex_unlock(&trava);
  printf("[openseek] pair result=%s\n", res == OSU_PAREADO ? "paired" : "address_changed"); fflush(stdout);
fim:
  zerar(k, sizeof k); zerar(&s, sizeof s); zerar(&c, sizeof c); zerar(corpo, sizeof corpo);
  zerar(prova, sizeof prova); zerar(esperada, sizeof esperada);
  pthread_mutex_lock(&trava);
  uiEstado = res; pareando = 0;
  pthread_mutex_unlock(&trava);
  zerar(pp, sizeof *pp); free(pp);
  return NULL;
}
int openseek_parear(const char *codigo) {
  Pareio *pp;
  pthread_t fio;
  openseek_carregar();
  pp = calloc(1, sizeof *pp);
  if (!pp) return 0;
  if (!osp1_normalizar_codigo(codigo, pp->codigo)) { free(pp); return 0; }
  sessaoEncerrar();
  pthread_mutex_lock(&trava);
  if (pareando || !cfgBase[0]) { pthread_mutex_unlock(&trava); zerar(pp, sizeof *pp); free(pp); return 0; }
  snprintf(pp->base, sizeof pp->base, "%s", cfgBase);
  // Parear de novo substitui o anterior so se der certo; ate la nada muda.
  pareando = 1;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&fio, NULL, parearFio, pp) == 0) { pthread_detach(fio); return 1; }
  pthread_mutex_lock(&trava); pareando = 0; uiEstado = OSU_FALHOU; pthread_mutex_unlock(&trava);
  zerar(pp, sizeof *pp); free(pp);
  return 0;
}
typedef struct { char base[OS_BASE_MAX], pino[OS_PINO_MAX], token[OS_TOKEN_MAX]; } Credencial;
static void *verificarFio(void *u) {
  Credencial *cr = u;
  char url[OS_BASE_MAX + 32];
  RedeResposta r;
  int http, res;
  snprintf(url, sizeof url, "%s/v1/pair/status", cr->base);
  http = chamar(NULL, url, cr->token, cr->pino, 0, NULL, OS_PRAZO_API_MS, NULL, &r);
  res = http == 200 ? OSU_PAREADO : http == 401 ? OSU_FALHOU : uiDeErro(&r);
  rede_resposta_limpar(&r);
  printf("[openseek] check http=%d result=%d\n", http, res); fflush(stdout);
  pthread_mutex_lock(&trava);
  if (!pareando && !strcmp(cfgToken, cr->token)) uiEstado = res;
  verificando = 0;
  pthread_mutex_unlock(&trava);
  zerar(cr, sizeof *cr); free(cr);
  return NULL;
}
void openseek_verificar(void) {
  Credencial *cr;
  pthread_t fio;
  openseek_carregar();
  cr = calloc(1, sizeof *cr);
  if (!cr) return;
  pthread_mutex_lock(&trava);
  if (pareando || verificando || !cfgToken[0]) { pthread_mutex_unlock(&trava); free(cr); return; }
  snprintf(cr->base, sizeof cr->base, "%s", cfgBase);
  snprintf(cr->pino, sizeof cr->pino, "%s", cfgPino);
  snprintf(cr->token, sizeof cr->token, "%s", cfgToken);
  verificando = 1; uiEstado = OSU_VERIFICANDO;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&fio, NULL, verificarFio, cr) == 0) pthread_detach(fio);
  else {
    pthread_mutex_lock(&trava); verificando = 0; uiEstado = OSU_PAREADO; pthread_mutex_unlock(&trava);
    zerar(cr, sizeof *cr); free(cr);
  }
}

// --- sessao (um video tocando) -----------------------------------------------------
typedef struct {
  char fetch[2400];              // URL da folha/quadro (com exp): capacidade
  char chaveJpg[240], chaveTex[256];
  int x, y, w, h;
} Quero;
typedef struct {
  unsigned char *px; int w, h; unsigned g;
  char chaveTex[256];
} Pronto;

typedef struct Sessao {
  int refs;                 // com a trava
  volatile int fim;
  unsigned g;               // geracao do desenho quando nasceu
  char base[OS_BASE_MAX], pino[OS_PINO_MAX], token[OS_TOKEN_MAX];
  char *url, *cabs;         // SEGREDOS: so no corpo do POST
  long durMs;
  char imdb[32], hash[24], arquivo[256];
  int t, e;
  unsigned long long tamanho;
  // dinamico (trava)
  long playheadMs, atMs, ultimoPlay;
  int scrubbing, iniciar;
  long jogadoMs;
  int prioSujo; Uint32 prioTick, prioPlayTick;
  char id[33], vttUrl[2300];
  long versaoServidor, versaoAplicada;
  int jobEstado, renovar, temVtt;
  long long exp;
  Quero quero[OS_QUERO]; int nQuero;
  char emVoo[256];          // chave de textura que o fio de busca esta trazendo
  Uint32 ultimoCriar;
} Sessao;

static Sessao *atual;
static unsigned geracao = 1;
static int  estado = SEEKR_DESLIGADO, motivo = OSM_NENHUM;
static long long retryUtc;
static unsigned ultimaLatencia; static int ultimoHttp;
static SeekrVtt vtt;                // o VTT em uso (desenho le sob a trava)
static char (*vttChaves)[256];      // chave de textura de cada cue
static Pronto prontos[OS_PRONTOS];

static void soltarSessao(Sessao *s) {   // com a trava
  if (!s || --s->refs > 0) return;
  if (s->url) { zerar(s->url, strlen(s->url)); free(s->url); }
  if (s->cabs) { zerar(s->cabs, strlen(s->cabs)); free(s->cabs); }
  zerar(s->token, sizeof s->token); zerar(s->vttUrl, sizeof s->vttUrl);
  for (int i = 0; i < OS_QUERO; i++) zerar(s->quero[i].fetch, sizeof s->quero[i].fetch);
  free(s);
}
static void soltarVtt(void) {          // com a trava
  seekr_vtt_liberar(&vtt); free(vttChaves); vttChaves = NULL;
}
static void soltarProntos(void) {
  for (int i = 0; i < OS_PRONTOS; i++) { free(prontos[i].px); prontos[i].px = NULL; }
}
static void definirEstado(Sessao *s, int est, int mot) {   // com a trava
  if (s != atual || est < 0) return;
  estado = est; motivo = mot;
}

// DELETE de cortesia ao sair (o servidor para o trabalho; quadros prontos ficam).
typedef struct { char url[OS_BASE_MAX + 64], pino[OS_PINO_MAX], token[OS_TOKEN_MAX]; } Cancelar;
static void *cancelarFio(void *u) {
  Cancelar *c = u;
  RedeResposta r;
  int http = chamar("DELETE", c->url, c->token, c->pino, 0, NULL, 3000, NULL, &r);
  rede_resposta_limpar(&r);
  printf("[openseek] cancel http=%d\n", http); fflush(stdout);
  zerar(c, sizeof *c); free(c);
  return NULL;
}
static void sessaoEncerrar(void) {
  Sessao *s;
  Cancelar *c = NULL;
  pthread_mutex_lock(&trava);
  s = atual; atual = NULL;
  geracao++;
  soltarVtt(); soltarProntos();
  estado = SEEKR_DESLIGADO; motivo = OSM_NENHUM;
  if (s) {
    s->fim = 1;
    if (s->id[0] && (c = calloc(1, sizeof *c)) != NULL) {
      snprintf(c->url, sizeof c->url, "%s/v1/previews/%s", s->base, s->id);
      snprintf(c->pino, sizeof c->pino, "%s", s->pino);
      snprintf(c->token, sizeof c->token, "%s", s->token);
    }
    soltarSessao(s);
  }
  pthread_cond_broadcast(&sinal);
  pthread_mutex_unlock(&trava);
  if (c) {
    pthread_t fio;
    if (pthread_create(&fio, NULL, cancelarFio, c) == 0) pthread_detach(fio);
    else { zerar(c, sizeof *c); free(c); }
  }
}

// Espera no sinal ate `ms` (ou ate a sessao acabar). Com a trava.
static void esperar(Sessao *s, unsigned ms) {
  struct timespec ts;
  if (s->fim) return;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  pthread_cond_timedwait(&sinal, &trava, &ts);
}

// Espera ate o prazo de verdade: o sinal acorda a cada passo do avanco, e uma
// espera so de `esperar` encolheria os recuos. Com a trava.
static void esperarPrazo(Sessao *s, unsigned ms) {
  Uint32 ate = SDL_GetTicks() + ms;
  for (;;) {
    Sint32 falta = (Sint32)(ate - SDL_GetTicks());
    if (s->fim || falta <= 0 || s->renovar) return;
    esperar(s, falta > 1000 ? 1000u : (unsigned)falta);
  }
}
// A linha de estado dos Ajustes acompanha o ultimo pedido de API. Com a trava.
static void notarUi(int http, const RedeResposta *r) {
  if (pareando || verificando) return;
  if (http == 401) uiEstado = OSU_FALHOU;
  else if (http >= 200 && http < 300) uiEstado = OSU_PAREADO;
  else if (r->erro == REDE_CANCELADO || r->erro == REDE_GERACAO) return;
  else uiEstado = uiDeErro(r) == OSU_FALHOU ? OSU_PAREADO : uiDeErro(r);
}

// Instala um VTT novo SEM soltar texturas: as chaves (URL sem t/exp/cap) nao
// mudam para o mesmo quadro, e o LRU do desenho e por chave, nao por indice.
static void trocarVtt(Sessao *s, SeekrVtt *novo) {   // com a trava
  char (*ch)[256] = NULL;
  if (s != atual) { seekr_vtt_liberar(novo); return; }
  if (novo->nCues > 0) ch = calloc((size_t)novo->nCues, sizeof *ch);
  if (novo->nCues > 0 && !ch) { seekr_vtt_liberar(novo); return; }
  for (int i = 0; i < novo->nCues; i++) {
    const SeekrCue *q = &novo->cues[i];
    char k[240];
    if (openseek_chave(novo->folhas[q->folha], k, sizeof k))
      snprintf(ch[i], sizeof ch[i], "%s#%d,%d,%d,%d", k, q->x, q->y, q->w, q->h);
  }
  soltarVtt();
  vtt = *novo; memset(novo, 0, sizeof *novo);
  vttChaves = ch;
}

static int criarTrabalho(Sessao *s) {
  char url[OS_BASE_MAX + 32], *corpo;
  OpenSeekFonte f;
  OpenSeekPrevia p;
  RedeResposta r;
  int http, est, mot = OSM_NENHUM, ok = 0;
  Uint32 t0 = SDL_GetTicks();
  corpo = malloc(16385);
  if (!corpo) return 0;
  pthread_mutex_lock(&trava);
  f = (OpenSeekFonte){ s->url, s->cabs, s->durMs, s->imdb, s->t, s->e, s->tamanho,
                       s->hash, s->arquivo, s->playheadMs, s->scrubbing ? s->atMs : -1 };
  if (!openseek_json_previa(corpo, 16385, &f)) corpo[0] = 0;
  pthread_mutex_unlock(&trava);
  if (!corpo[0]) {
    free(corpo);
    pthread_mutex_lock(&trava); definirEstado(s, SEEKR_SEM_PREVIA, OSM_FONTE); pthread_mutex_unlock(&trava);
    return -1;
  }
  snprintf(url, sizeof url, "%s/v1/previews", s->base);
  // SR-4: URL e cabecalhos so neste corpo, so com pino (openseek_pedido recusa sem).
  http = chamar("POST", url, s->token, s->pino, 0, corpo, OS_PRAZO_API_MS, &s->fim, &r);
  zerar(corpo, 16385); free(corpo);
  int leu = (http == 200 || http == 202) && r.corpo && openseek_ler_previa(r.corpo, s->base, &p);
  est = openseek_mapear(http, r.erro, r.curl_erro, leu ? &p : NULL, 0, &mot);
  pthread_mutex_lock(&trava);
  ultimoHttp = http; ultimaLatencia = SDL_GetTicks() - t0;
  notarUi(http, &r);
  if (s == atual && est >= 0) {
    if (leu) {
      int mudou = strcmp(s->vttUrl, p.vtt) != 0;
      snprintf(s->id, sizeof s->id, "%s", p.id);
      snprintf(s->vttUrl, sizeof s->vttUrl, "%s", p.vtt);
      s->exp = p.exp; s->jobEstado = p.estado; s->renovar = 0;
      if (p.versao > s->versaoServidor || mudou) s->versaoServidor = p.versao;
      if (mudou) s->temVtt = 0;
      ok = 1;
      if (p.estado == OS_FALHOU) definirEstado(s, SEEKR_SEM_PREVIA, OSM_FONTE);
      else if (!vtt.nCues) definirEstado(s, est, mot);
      retryUtc = 0;
    } else {
      definirEstado(s, est, mot);
      retryUtc = (long long)time(NULL) + (r.retry_after_s > 0 ? r.retry_after_s : 30);
    }
  }
  pthread_mutex_unlock(&trava);
  printf("[openseek] job create http=%d state=%d reason=%d latency_ms=%u\n", http, est, mot, ultimaLatencia);
  fflush(stdout);
  rede_resposta_limpar(&r);
  if (ok) return 1;
  // 401 (pareamento revogado), 400/413 (fonte recusada): nao insistir.
  return http == 401 || http == 400 || http == 413 ? -1 : 0;
}

static int baixarVtt(Sessao *s) {
  char url[2300];
  long versao;
  RedeResposta r;
  SeekrVtt v;
  int http, n = 0;
  pthread_mutex_lock(&trava);
  snprintf(url, sizeof url, "%s", s->vttUrl); versao = s->versaoServidor;
  pthread_mutex_unlock(&trava);
  if (!url[0]) return 0;
  // Capacidade: sem cabecalho de autorizacao (CONTRACT-v1 "Data plane").
  http = chamar(NULL, url, NULL, s->pino, 0, NULL, OS_PRAZO_DADOS_MS, &s->fim, &r);
  memset(&v, 0, sizeof v);
  if (http == 200 && r.corpo) n = seekr_vtt_ler_openseek(r.corpo, s->base, &v);
  pthread_mutex_lock(&trava);
  if (http == 200 && r.corpo && s == atual) {
    trocarVtt(s, &v);
    s->temVtt = 1;
    if (versao > s->versaoAplicada) s->versaoAplicada = versao;
    if (s->jobEstado == OS_FALHOU) definirEstado(s, SEEKR_SEM_PREVIA, OSM_FONTE);
    else if (!vtt.nCues && s->jobEstado == OS_PREPARANDO) definirEstado(s, SEEKR_BUSCANDO, OSM_PREPARANDO);
    else definirEstado(s, SEEKR_PRONTO, OSM_NENHUM);
  } else if (http == 403) {
    s->renovar = 1;                     // capacidade vencida: novo POST
  } else if (s == atual && !vtt.nCues) {
    int mot = OSM_NENHUM, est = openseek_mapear(http, r.erro, r.curl_erro, NULL, 0, &mot);
    definirEstado(s, est, mot);
  }
  pthread_mutex_unlock(&trava);
  seekr_vtt_liberar(&v);
  printf("[openseek] vtt http=%d cues=%d version=%ld\n", http, n, versao); fflush(stdout);
  rede_resposta_limpar(&r);
  return http == 200;
}

static void consultarStatus(Sessao *s, int longo) {
  char url[OS_BASE_MAX + 96];
  OpenSeekPrevia p;
  RedeResposta r;
  int http, leu;
  pthread_mutex_lock(&trava);
  if (longo) snprintf(url, sizeof url, "%s/v1/previews/%s?wait_version=%ld&wait_ms=500", s->base, s->id, s->versaoServidor);
  else snprintf(url, sizeof url, "%s/v1/previews/%s", s->base, s->id);
  pthread_mutex_unlock(&trava);
  http = chamar(NULL, url, s->token, s->pino, 0, NULL, OS_PRAZO_API_MS, &s->fim, &r);
  leu = http == 200 && r.corpo && openseek_ler_previa(r.corpo, s->base, &p);
  pthread_mutex_lock(&trava);
  notarUi(http, &r);
  if (s == atual) {
    if (leu) {
      s->jobEstado = p.estado;
      if (p.versao > s->versaoServidor) s->versaoServidor = p.versao;
      if (strcmp(p.vtt, s->vttUrl)) { snprintf(s->vttUrl, sizeof s->vttUrl, "%s", p.vtt); s->exp = p.exp; s->temVtt = 0; }
      if (p.estado == OS_FALHOU) definirEstado(s, SEEKR_SEM_PREVIA, OSM_FONTE);
    } else if (http == 404) {
      s->renovar = 1;                   // trabalho esquecido pelo servidor: recriar
    } else if (http == 401) {
      definirEstado(s, SEEKR_CHAVE_RECUSADA, OSM_RECUSADO);
      s->fim = 1;
    } else if (!vtt.nCues) {
      int mot = OSM_NENHUM, est = openseek_mapear(http, r.erro, r.curl_erro, NULL, 0, &mot);
      definirEstado(s, est, mot);
    }
  }
  pthread_mutex_unlock(&trava);
  rede_resposta_limpar(&r);
}

static void *controleFio(void *u) {
  Sessao *s = u;
  int falhas = 0;
  Uint32 ultimoStatus = 0;
  pthread_mutex_lock(&trava);
  while (!s->fim && !s->iniciar) esperar(s, 1000);
  pthread_mutex_unlock(&trava);
  while (!s->fim) {
    // Renovar em laco (relogio do servidor torto, 403 seguidos) custa no
    // maximo um POST a cada 5 s.
    pthread_mutex_lock(&trava);
    while (!s->fim && s->ultimoCriar && SDL_GetTicks() - s->ultimoCriar < 5000u) esperar(s, 1000);
    s->ultimoCriar = SDL_GetTicks() | 1u;
    pthread_mutex_unlock(&trava);
    if (s->fim) break;
    int r = criarTrabalho(s);
    if (r < 0) break;
    if (r == 0) {
      unsigned espera = 30000;
      pthread_mutex_lock(&trava);
      long long falta = retryUtc - (long long)time(NULL);
      if (falta > 0 && falta < 600) espera = (unsigned)falta * 1000u;
      if (++falhas > 1 && espera < 30000) espera = 30000;
      s->renovar = 0;                   // pTentar() pode encurtar o recuo
      esperarPrazo(s, espera);
      pthread_mutex_unlock(&trava);
      continue;
    }
    falhas = 0;
    // Laco do trabalho: VTT novo quando a versao sobe; status ate complete.
    while (!s->fim) {
      int precisaVtt, completo, rolando, renovar;
      long long agora = (long long)time(NULL);
      pthread_mutex_lock(&trava);
      precisaVtt = !s->temVtt || s->versaoServidor > s->versaoAplicada;
      completo = s->jobEstado == OS_COMPLETO || s->jobEstado == OS_FALHOU;
      rolando = s->scrubbing;
      renovar = s->renovar || (s->exp && s->exp - agora < 120);
      pthread_mutex_unlock(&trava);
      if (renovar) break;               // volta ao POST: capacidades novas
      if (precisaVtt) {
        if (!baixarVtt(s)) {
          pthread_mutex_lock(&trava); esperarPrazo(s, 2000); pthread_mutex_unlock(&trava);
        }
        continue;
      }
      if (completo) {
        // Nada a consultar; acorda para renovar a capacidade antes do exp.
        pthread_mutex_lock(&trava);
        while (!s->fim && !s->renovar && !(s->exp && s->exp - (long long)time(NULL) < 120)) esperar(s, 5000);
        pthread_mutex_unlock(&trava);
        continue;
      }
      if (rolando) {
        // Long-poll curto (<= 2/s); um servidor que responde na hora nao vira laco.
        Uint32 desde = SDL_GetTicks() - ultimoStatus;
        if (ultimoStatus && desde < 500) {
          pthread_mutex_lock(&trava); esperar(s, 500 - desde); pthread_mutex_unlock(&trava);
          continue;
        }
        ultimoStatus = SDL_GetTicks();
        consultarStatus(s, 1);
      }
      else {
        pthread_mutex_lock(&trava);
        Uint32 ate = SDL_GetTicks() + 10000;
        while (!s->fim && !s->scrubbing && !s->renovar && (Sint32)(ate - SDL_GetTicks()) > 0 &&
               s->versaoServidor <= s->versaoAplicada) esperar(s, 1000);
        int pular = s->versaoServidor > s->versaoAplicada || s->renovar;
        pthread_mutex_unlock(&trava);
        if (!pular && !s->fim) consultarStatus(s, 0);
      }
    }
  }
  pthread_mutex_lock(&trava); soltarSessao(s); pthread_mutex_unlock(&trava);
  return NULL;
}

static void *prioridadeFio(void *u) {
  Sessao *s = u;
  char url[OS_BASE_MAX + 64], corpo[96];
  pthread_mutex_lock(&trava);
  while (!s->fim) {
    long at, play;
    RedeResposta r;
    int http;
    Uint32 desde = SDL_GetTicks() - s->prioTick;
    if (!s->prioSujo || !s->id[0]) { esperar(s, 1000); continue; }
    if (desde < OS_PRIO_MIN_MS) { esperar(s, OS_PRIO_MIN_MS - desde); continue; }
    at = s->scrubbing ? s->atMs : -1; play = s->playheadMs;
    s->prioSujo = 0; s->prioTick = SDL_GetTicks();
    snprintf(url, sizeof url, "%s/v1/previews/%s/priority", s->base, s->id);
    pthread_mutex_unlock(&trava);
    openseek_json_prioridade(corpo, sizeof corpo, at, play);
    http = chamar("POST", url, s->token, s->pino, 0, corpo, OS_PRAZO_API_MS, &s->fim, &r);
    pthread_mutex_lock(&trava);
    if (http == 200 && r.corpo && s == atual) {
      long v; int est;
      if (openseek_ler_versao(r.corpo, &v, &est)) {
        if (v > s->versaoServidor) { s->versaoServidor = v; pthread_cond_broadcast(&sinal); }
        if (est) s->jobEstado = est;
      }
    } else if (http == 404 && s == atual) { s->renovar = 1; pthread_cond_broadcast(&sinal); }
    pthread_mutex_unlock(&trava);
    rede_resposta_limpar(&r);
    pthread_mutex_lock(&trava);
  }
  soltarSessao(s);
  pthread_mutex_unlock(&trava);
  return NULL;
}

// --- busca e recorte ----------------------------------------------------------------
static SDL_Surface *decodificar(const unsigned char *b, size_t n) {
  SDL_Surface *s, *c;
  int w0 = 0, h0 = 0;
  s = jpeg_rapido_carregar_mem(b, n, 1 << 20, &w0, &h0);
#ifndef __EMSCRIPTEN__
  if (!s) { SDL_RWops *rw = SDL_RWFromConstMem(b, (int)n); s = rw ? IMG_Load_RW(rw, 1) : NULL; }
#endif
  if (!s) return NULL;
  if (s->format->format == SDL_PIXELFORMAT_ABGR8888) return s;
  c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
  SDL_FreeSurface(s);
  return c;
}
static unsigned char *recorte(SDL_Surface *s, int x, int y, int w, int h) {
  unsigned char *px;
  if (!s || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s->w || y + h > s->h) return NULL;
  px = malloc((size_t)w * (size_t)h * 4u);
  if (!px) return NULL;
  if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
  for (int k = 0; k < h; k++)
    memcpy(px + (size_t)k * (size_t)w * 4u,
           (const unsigned char *)s->pixels + (size_t)(y + k) * (size_t)s->pitch + (size_t)x * 4u,
           (size_t)w * 4u);
  if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
  return px;
}

static void *buscaFio(void *u) {
  Sessao *s = u;
  OsLru jl;                         // JPEGs (so este fio mexe)
  char *jpg[OS_JPG_LRU] = { 0 }; size_t jpgN[OS_JPG_LRU] = { 0 };
  SDL_Surface *folha = NULL; char folhaChave[240] = "";
  Uint32 ultimoTrabalho = 0;
  char falhou[240] = ""; Uint32 falhouTick = 0;
  openseek_lru_ini(&jl, OS_JPG_LRU);
  pthread_mutex_lock(&trava);
  while (!s->fim) {
    Quero q;
    int achou = -1;
    unsigned g = s->g;
    for (int i = 0; i < s->nQuero; i++) {
      if (!strcmp(s->quero[i].chaveJpg, falhou) && SDL_GetTicks() - falhouTick < OS_FALHA_MS) continue;
      achou = i; break;
    }
    if (achou < 0) {
      // Fim do avanco: a folha decodificada (ate ~22 MB) nao fica parada.
      if (folha && SDL_GetTicks() - ultimoTrabalho > 2000u) {
        SDL_Surface *f = folha; folha = NULL; folhaChave[0] = 0;
        pthread_mutex_unlock(&trava); SDL_FreeSurface(f); pthread_mutex_lock(&trava);
        continue;
      }
      esperar(s, 500);
      continue;
    }
    q = s->quero[achou];
    // Tirado da fila: o desenho pede de novo se ainda quiser.
    memmove(&s->quero[achou], &s->quero[achou + 1], (size_t)(s->nQuero - achou - 1) * sizeof q);
    s->nQuero--;
    snprintf(s->emVoo, sizeof s->emVoo, "%s", q.chaveTex);
    pthread_mutex_unlock(&trava);

    unsigned char *px = NULL;
    int slot = openseek_lru_achar(&jl, q.chaveJpg), http = 200, w = 0, h = 0;
    Uint32 t0 = SDL_GetTicks();
    if (slot < 0 && !openseek_na_origem(q.fetch, s->base)) http = -2;
    else if (slot < 0) {
      RedeResposta r;
      http = chamar(NULL, q.fetch, NULL, s->pino, 0, NULL, OS_PRAZO_DADOS_MS, &s->fim, &r);
      // Bytes e dimensoes conferidos ANTES de decodificar (NUVIO-REVIEW 4).
      if (http == 200 && r.corpo && r.n_corpo > 0 && r.n_corpo <= OS_TILE_BYTES_MAX &&
          openseek_jpeg_dimensoes((unsigned char *)r.corpo, r.n_corpo, &w, &h) &&
          w > 0 && h > 0 && w <= OS_TILE_LADO_MAX && h <= OS_TILE_ALTO_MAX) {
        const char *prot[1] = { folhaChave };
        slot = openseek_lru_lugar(&jl, q.chaveJpg, prot, 1);
        if (slot >= 0) {
          size_t total = 0;
          free(jpg[slot]); jpg[slot] = r.corpo; jpgN[slot] = r.n_corpo; r.corpo = NULL;
          // Teto de 8 MiB em JPEGs guardados (folhas completas sao ~1-2 MB).
          for (int i = 0; i < OS_JPG_LRU; i++) total += jl.it[i].ocupado ? jpgN[i] : 0;
          while (total > 8u * 1024u * 1024u) {
            int velho = -1;
            for (int i = 0; i < OS_JPG_LRU; i++)
              if (i != slot && jl.it[i].ocupado && (velho < 0 || jl.it[i].uso < jl.it[velho].uso)) velho = i;
            if (velho < 0) break;
            total -= jpgN[velho]; free(jpg[velho]); jpg[velho] = NULL; jpgN[velho] = 0;
            jl.it[velho].ocupado = 0; jl.it[velho].chave[0] = 0;
          }
        }
      } else if (http == 200) http = -1;   // grande demais ou nao e JPEG
      rede_resposta_limpar(&r);
    }
    if (slot >= 0 && jpg[slot]) {
      if (!folha || strcmp(folhaChave, q.chaveJpg)) {
        if (folha) SDL_FreeSurface(folha);
        folha = decodificar((unsigned char *)jpg[slot], jpgN[slot]);
        snprintf(folhaChave, sizeof folhaChave, "%s", folha ? q.chaveJpg : "");
      }
      px = recorte(folha, q.x, q.y, q.w, q.h);
      // Quadro avulso (320x180) nao vale ficar decodificado; folha sim.
      if (folha && folha->w <= q.w && folha->h <= q.h) { SDL_FreeSurface(folha); folha = NULL; folhaChave[0] = 0; }
    }
    if (!px) { snprintf(falhou, sizeof falhou, "%s", q.chaveJpg); falhouTick = SDL_GetTicks(); }
    printf("[openseek] tile http=%d ok=%d source=%s latency_ms=%u\n", http, px != NULL,
           http == 200 && slot >= 0 ? "cache_or_network" : "network", SDL_GetTicks() - t0);
    fflush(stdout);
    zerar(q.fetch, sizeof q.fetch);
    ultimoTrabalho = SDL_GetTicks();
    pthread_mutex_lock(&trava);
    s->emVoo[0] = 0;
    if (http == 403 && s == atual) { s->renovar = 1; pthread_cond_broadcast(&sinal); }
    if (px && s == atual && g == geracao) {
      int k, livre = 0;
      for (k = 0; k < OS_PRONTOS; k++) if (!prontos[k].px) { livre = k; break; }
      if (k == OS_PRONTOS) livre = 0;
      free(prontos[livre].px);
      prontos[livre].px = px; prontos[livre].w = q.w; prontos[livre].h = q.h; prontos[livre].g = g;
      snprintf(prontos[livre].chaveTex, sizeof prontos[livre].chaveTex, "%s", q.chaveTex);
      px = NULL;
    }
    free(px);
  }
  soltarSessao(s);
  pthread_mutex_unlock(&trava);
  if (folha) SDL_FreeSurface(folha);
  for (int i = 0; i < OS_JPG_LRU; i++) free(jpg[i]);
  return NULL;
}

// --- provedor (seekr.h) -------------------------------------------------------------
static int pTemChave(void) { return openseek_pareado(); }
static int pEstado(void) { int r; pthread_mutex_lock(&trava); r = estado; pthread_mutex_unlock(&trava); return r; }
static const char *pRotulo(int est) {
  int m;
  pthread_mutex_lock(&trava); m = motivo; pthread_mutex_unlock(&trava);
  switch (est) {
    case SEEKR_BUSCANDO: return "Preparando miniaturas no OpenSeek";
    case SEEKR_PRONTO: return "Miniaturas disponíveis";
    case SEEKR_SEM_PREVIA:
      return m == OSM_FONTE ? "O OpenSeek não conseguiu ler este vídeo" : "Este título não tem miniaturas";
    case SEEKR_CHAVE_RECUSADA: return "O OpenSeek recusou este aparelho; pareie de novo";
    case SEEKR_LIMITE_PROVEDOR: return "Servidor OpenSeek ocupado";
    case SEEKR_REDE_INDISPONIVEL:
      return m == OSM_PINO_MUDOU ? "O certificado do servidor mudou" :
             m == OSM_SEM_PINO ? "Este aparelho não suporta conexão segura com o OpenSeek" :
             "Servidor OpenSeek sem conexão";
    default: return openseek_pareado() ? "Miniaturas não solicitadas" : "OpenSeek não pareado";
  }
}
static void pUso(SeekrUso *u) {
  memset(u, 0, sizeof *u);
  pthread_mutex_lock(&trava);
  u->retryUtc = retryUtc; u->http = ultimoHttp; u->latenciaMs = ultimaLatencia;
  u->persistente = 1; u->chavePropria = 1;   // sem cota local: o servidor e da pessoa
  pthread_mutex_unlock(&trava);
}
static void pTentar(void) {
  pthread_mutex_lock(&trava);
  retryUtc = 0;
  if (atual && estado != SEEKR_PRONTO && estado != SEEKR_BUSCANDO) { atual->renovar = 1; pthread_cond_broadcast(&sinal); }
  pthread_mutex_unlock(&trava);
}
static void pDesligar(void) { sessaoEncerrar(); }

static char *dup(const char *s) {
  size_t n = s ? strlen(s) : 0;
  char *d = malloc(n + 1);
  if (d) { if (n) memcpy(d, s, n); d[n] = 0; }
  return d;
}

static void pPedir(const SeekrFonte *f) {
  Sessao *s;
  int mesma, novo = 0;
  if (!f || !f->url || strncmp(f->url, "http", 4) || f->durMs <= 0) return;
  if (!openseek_pareado()) return;
  pthread_mutex_lock(&trava);
  s = atual;
  mesma = s && !strcmp(s->url, f->url) && labs(s->durMs - f->durMs) < 2000;
  if (s && !mesma) { pthread_mutex_unlock(&trava); sessaoEncerrar(); pthread_mutex_lock(&trava); s = NULL; }
  if (!s) {
    s = calloc(1, sizeof *s);
    if (!s || !(s->url = dup(f->url)) || !(s->cabs = dup(f->cabecalhos ? f->cabecalhos : ""))) {
      if (s) { free(s->url); free(s); }
      pthread_mutex_unlock(&trava); return;
    }
    snprintf(s->base, sizeof s->base, "%s", cfgBase);
    snprintf(s->pino, sizeof s->pino, "%s", cfgPino);
    snprintf(s->token, sizeof s->token, "%s", cfgToken);
    snprintf(s->imdb, sizeof s->imdb, "%s", f->imdb ? f->imdb : "");
    snprintf(s->hash, sizeof s->hash, "%s", f->videoHash ? f->videoHash : "");
    snprintf(s->arquivo, sizeof s->arquivo, "%s", f->arquivo ? f->arquivo : "");
    s->durMs = f->durMs; s->t = f->t; s->e = f->e; s->tamanho = f->tamanho;
    s->atMs = -1; s->ultimoPlay = f->playheadMs; s->g = ++geracao;
    s->refs = 1;   // a de `atual`
    atual = s; novo = 1;
    soltarVtt(); soltarProntos();
    estado = SEEKR_BUSCANDO; motivo = OSM_NENHUM; retryUtc = 0;
  }
  // SCRUBBING §1: so depois de 8 s tocando ou no primeiro avanco.
  if (!f->scrubbing) {
    long d = f->playheadMs - s->ultimoPlay;
    if (d > 0 && d < 2000) s->jogadoMs += d;
  }
  s->ultimoPlay = f->playheadMs;
  s->playheadMs = f->playheadMs;
  if (!s->iniciar && (f->scrubbing || s->jogadoMs >= OS_INICIO_MS)) { s->iniciar = 1; pthread_cond_broadcast(&sinal); }
  // Prioridade: o alvo do avanco (ultimo vence) e o fim do avanco (null).
  if (f->scrubbing && (!s->scrubbing || labs(f->posMs - s->atMs) >= 1)) {
    s->atMs = f->posMs; s->prioSujo = 1;
    if (!s->scrubbing) pthread_cond_broadcast(&sinal);   // o controle passa ao long-poll
  } else if (!f->scrubbing && s->scrubbing) { s->atMs = -1; s->prioSujo = 1; pthread_cond_broadcast(&sinal); }
  else if (!f->scrubbing && SDL_GetTicks() - s->prioPlayTick > 60000u) {
    s->prioPlayTick = SDL_GetTicks();     // janela P1 em volta do playhead
    if (s->id[0]) s->prioSujo = 1;
  }
  s->scrubbing = f->scrubbing;
  if (s->prioSujo) pthread_cond_broadcast(&sinal);
  if (novo) {
    pthread_t a, b, c;
    s->refs += 3; s->prioPlayTick = SDL_GetTicks();
    pthread_mutex_unlock(&trava);
    if (pthread_create(&a, NULL, controleFio, s) == 0) pthread_detach(a);
    else { pthread_mutex_lock(&trava); soltarSessao(s); pthread_mutex_unlock(&trava); }
    if (pthread_create(&b, NULL, prioridadeFio, s) == 0) pthread_detach(b);
    else { pthread_mutex_lock(&trava); soltarSessao(s); pthread_mutex_unlock(&trava); }
    if (pthread_create(&c, NULL, buscaFio, s) == 0) pthread_detach(c);
    else { pthread_mutex_lock(&trava); soltarSessao(s); pthread_mutex_unlock(&trava); }
    printf("[openseek] session start\n"); fflush(stdout);
    return;
  }
  pthread_mutex_unlock(&trava);
}

// Desenho: LRU de texturas por chave (so este fio mexe).
static OsLru texLru;
static GLuint texs[OS_LRU_MAX];
static unsigned texG;

static int pQuadros(double posSeg, int n, GLuint *out, double *cueSeg) {
  Pronto pr[OS_PRONTOS];
  int i, k, nq = 0, quer[3], achou = 0;
  unsigned g;
  if (n < 1) return 0;
  if (n > 3) n = 3;
  for (k = 0; k < n; k++) { out[k] = 0; if (cueSeg) cueSeg[k] = -1.0; }
  pthread_mutex_lock(&trava);
  g = geracao;
  memcpy(pr, prontos, sizeof pr); memset(prontos, 0, sizeof prontos);
  if (texG != g) { openseek_lru_ini(&texLru, OS_TEX_LRU); texG = g; }
  if (estado != SEEKR_PRONTO || !atual || !vtt.nCues) {
    pthread_mutex_unlock(&trava);
    for (k = 0; k < OS_PRONTOS; k++) free(pr[k].px);
    return 0;
  }
  // Regra do OpenSeek: so dentro de [ini, fim) (SCRUBBING §3).
  i = seekr_vtt_cue_estrita(&vtt, (long)(posSeg * 1000.0));
  if (i >= 0) {
    quer[nq++] = i;                           // a atual primeiro, depois a fita
    if (i + 1 < vtt.nCues) quer[nq++] = i + 1;
    if (i > 0) quer[nq++] = i - 1;
  }
  pthread_mutex_unlock(&trava);
  // Sobe o que ficou pronto (fio de desenho: aqui pode GL).
  for (k = 0; k < OS_PRONTOS; k++) {
    if (!pr[k].px) continue;
    if (pr[k].g == g && openseek_lru_achar(&texLru, pr[k].chaveTex) < 0) {
      const char *prot[3]; int np = 0, sl;
      pthread_mutex_lock(&trava);
      for (int j = 0; j < nq && vttChaves; j++) prot[np++] = vttChaves[quer[j]];
      sl = openseek_lru_lugar(&texLru, pr[k].chaveTex, prot, np);
      pthread_mutex_unlock(&trava);
      if (sl >= 0) {
        if (!texs[sl]) glGenTextures(1, &texs[sl]);
        glBindTexture(GL_TEXTURE_2D, texs[sl]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pr[k].w, pr[k].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pr[k].px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gfx_tex_esquecer(0);
      }
    }
    free(pr[k].px);
  }
  if (i < 0) return 0;
  pthread_mutex_lock(&trava);
  if (g != geracao || !vttChaves || i >= vtt.nCues) { pthread_mutex_unlock(&trava); return 0; }
  {
    Sessao *s = atual;
    Quero novos[OS_QUERO];
    int nn = 0;
    for (k = 0; k < nq; k++) {
      int c = quer[k], sl = vttChaves[c][0] ? openseek_lru_achar(&texLru, vttChaves[c]) : -1;
      int pos = n == 1 ? (c == i ? 0 : -1) : (c == i ? 1 : c < i ? 0 : 2);
      if (sl >= 0) {
        if (pos >= 0) {
          out[pos] = texs[sl];
          if (cueSeg) cueSeg[pos] = (vtt.cues[c].rotulo >= 0 ? vtt.cues[c].rotulo : vtt.cues[c].ini) / 1000.0;
          if (c == i) achou = 1;
        }
      } else if (s && vttChaves[c][0] && strcmp(s->emVoo, vttChaves[c])) {
        // Sem textura, sem tempo: o player mostra o placeholder com o anel e
        // o tempo-alvo. O fio de busca traz (a atual primeiro, depois a fita).
        const SeekrCue *q = &vtt.cues[c];
        int pronto = 0;
        for (int j = 0; j < OS_PRONTOS; j++)
          if (prontos[j].px && !strcmp(prontos[j].chaveTex, vttChaves[c])) pronto = 1;
        if (pronto) continue;
        Quero *w = &novos[nn++];
        snprintf(w->fetch, sizeof w->fetch, "%s", vtt.folhas[q->folha]);
        openseek_chave(vtt.folhas[q->folha], w->chaveJpg, sizeof w->chaveJpg);
        snprintf(w->chaveTex, sizeof w->chaveTex, "%s", vttChaves[c]);
        w->x = q->x; w->y = q->y; w->w = q->w; w->h = q->h;
      }
    }
    // O ultimo pedido vence: a fila e trocada inteira (latest wins).
    int igual = s && nn == s->nQuero;
    for (k = 0; igual && k < nn; k++) igual = !strcmp(novos[k].chaveTex, s->quero[k].chaveTex);
    if (s && !igual) {
      memcpy(s->quero, novos, (size_t)nn * sizeof *novos);
      s->nQuero = nn;
      if (nn) pthread_cond_broadcast(&sinal);
    }
    for (k = 0; k < nn; k++) zerar(novos[k].fetch, sizeof novos[k].fetch);
  }
  pthread_mutex_unlock(&trava);
  return achou;
}
static int pVelho(void) { return 0; }   // nunca a ultima usada (SCRUBBING §5)
static void pOcioso(void) {}            // a folha decodificada vive no fio de busca

static const SeekrProvedor PROVEDOR = {
  pTemChave, pEstado, pRotulo, pUso, pTentar, pPedir, pDesligar, pQuadros, pVelho, pOcioso
};
const SeekrProvedor *openseek_provedor(void) { return &PROVEDOR; }
