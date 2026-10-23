#define _GNU_SOURCE   // strcasestr
// SEEK-BAR THUMBNAILS MADE ON THIS DEVICE (Android, test): see miniatura_android.h.
#include "miniatura_android.h"

#ifndef NV_ANDROID
const SeekrProvedor *miniatura_provedor(void) { return NULL; }
int miniatura_prontas_agora(void) { return 0; }
#else
#include "dados.h"
#include "streams.h"
#include "video.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <ctype.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GRADE_MS 10000L
#define TOL_MS 5000L
#define LOTE 32                      // most keyframes per multi-range request (the engine sizes each batch to ~4 MB)
// Copy choice and the optimistic start, named so a test can tune them (22/10, converged with the review):
#define RAZAO_1080 0.80              // a 1080p copy only when <= 80 % of the best 720p's size
#define RAZAO_MESMA_RES 0.70         // a copy at the playing file's resolution only when <= 70 % of its size
#define PISO_MBPS_1080 4.0           // bitrate floors: a tiny bad encode must not win on size alone
#define PISO_MBPS_720 2.0
#define PISO_MBPS_SD 1.0
#define OFFSET_MANTER_MS 3000.0      // optimistic thumbnails are kept (relabelled) up to this offset, redone above
#define TOL_COPIA_MS 8000L          // a copy keyframe this close stands in for its slot (labelled with its own time)
#define PISO_KB (300L * 1024)        // MemAvailable kept free for the player and the app
#define SIG_N (32 * 18)
#define CAB 9                        // result header: 8 bytes pts + 1 flag
#define NTEX 8
#define VIZINHO_SLOTS 30             // the bar shows the nearest made tile up to 5 min away
// timeline check (OpenSeek timeline.py)
#define N_ANCORAS 20
#define MIN_CORR 0.60
#define MIN_MARGEM 0.05
#define MIN_INLIERS 8
#define MAX_RESIDUO_MS 8000.0
#define MAX_ERRO_MS 3500.0
#define MAX_RAZAO_ERRO 0.10
#define JANELA_LARGA_MS 12000
#define PASSO_LARGO_MS 3000
#define JANELA_ESTREITA_MS 3000
#define PASSO_ESTREITO_MS 1000
#define MIN_DESVIO 4.0
#define MIN_POR_QUARTO 2
#define TOMADA_MS 8000               // candidates this far from the best are another shot
#define TENTATIVAS 8                 // smaller copies tried per title (a wrong runtime/codec costs only the header, ~1 s)

enum { M_PARADO = 0, M_BUSCANDO, M_PRONTO, M_FALHOU };

typedef struct { double a, b, erro; int inliers; } Mapa;
// prov = made at offset 0 while the copy's check was still running (optimistic start): relabelled or discarded
// when the check decides.
typedef struct { unsigned char *jpg; int n; long pts; unsigned char prov; } Tile;
// What the playing file showed at each check anchor: decoded ONCE per title and reused for every copy checked
// (the 4K decode is what the player feels; a second copy must not pay it again).
typedef struct { int estado; long pA; int ok; float sig[SIG_N]; } Ancora;   // estado: 0 not yet, 1 read, -1 failed


typedef struct {
  char url[4096], cab[512];
  int altura;
  unsigned long long tamanho;
  char nome[160];
} Copia;

typedef struct {
  unsigned g;
  char urlA[4096], cabA[512];
  int alturaA;
  long dur;
  Copia copias[TENTATIVAS];
  int nCopias;
  Tile *tiles; unsigned char *tentado; int n;
  Ancora anc[N_ANCORAS];
  char arq[640];             // the saved set on this device ("" = cannot save)
  int feitos;
  Uint32 lacunaDesde;        // when the exact-frame gap pass started (0 = not yet)
  Mapa mapa;                 // copy time = a x play time + b, read by the lanes (under mu)
  int otimista;              // lanes are running at offset 0 before the check decided
  volatile int pararFaixas;  // stop the lanes only (a rejected copy), the job goes on
  // per-title summary (one SUMMARY line at the end: the acceptance record of a test)
  char titulo[48], motivos[320], escolhida[200];
  double offset;
  int refeito;
  int nRecheck, falhasRecheck, resumido;   // re-check while watching; summary printed once
  long caiuEm;                             // play time where the copy stopped matching (0 = never)
  long ultimoBom;                          // last play time the copy was CONFIRMED to match (check anchors, re-checks)
  int reancoras;                           // times a new offset was found after the copy stopped matching
  long long cont0[12];
  volatile int parar;
  int vivo;
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Job *job;
static int estado = M_PARADO;
static unsigned geracao;
static char urlAtual[4096];
static volatile long posMs, playheadMs;
static volatile int scrubbing, bufferando;
static char origem[48] = "";      // what the thumbnails came from (log + label)
static volatile unsigned avisoPronto;
static volatile int evRebuffer, evErro;   // playback events during the title (summary)
static Uint32 prontoDesde;          // when the playing title's set completed (the player shows it 4 s)
static int motivoSem;            // M_FALHOU because: 0 the video would not open, 1 no smaller copy usable
   // generation whose set just completed (pPedir shows the notice)

// ---- JNI (MiniaturaMotor.kt) ----
static jclass cls;
static jmethodID jAbrir, jInfo, jQuadro, jExato, jSoltar, jFechar, jLidos, jRitmo, jFalhaRede, jTela, jPreparar, jCont, jAlvo, jLote;

static int jniPreparar(void) {
  if (cls) return 1;
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return 0;
  jclass ac = (*env)->GetObjectClass(env, act);
  jmethodID gl = (*env)->GetMethodID(env, ac, "getClassLoader", "()Ljava/lang/ClassLoader;");
  jobject loader = (*env)->CallObjectMethod(env, act, gl);
  jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
  jmethodID lcm = (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring nome = (*env)->NewStringUTF(env, "space.nuvio.nativelegacy.MiniaturaMotor");
  jclass c = (jclass)(*env)->CallObjectMethod(env, loader, lcm, nome);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); c = NULL; }
  if (c) {
    jAbrir = (*env)->GetStaticMethodID(env, c, "abrir", "(Ljava/lang/String;Ljava/lang/String;Z)J");
    jInfo = (*env)->GetStaticMethodID(env, c, "info", "(J)[J");
    jQuadro = (*env)->GetStaticMethodID(env, c, "quadro", "(JJZZ)[B");
    jExato = (*env)->GetStaticMethodID(env, c, "quadroExato", "(JJZ)[B");
    jSoltar = (*env)->GetStaticMethodID(env, c, "soltarDecoder", "(J)V");
    jFechar = (*env)->GetStaticMethodID(env, c, "fechar", "(J)V");
    jLidos = (*env)->GetStaticMethodID(env, c, "lidos", "(J)J");
    jRitmo = (*env)->GetStaticMethodID(env, c, "ritmo", "()J");
    jFalhaRede = (*env)->GetStaticMethodID(env, c, "falhaRede", "(J)I");
    jTela = (*env)->GetStaticMethodID(env, c, "tela", "()[B");
    jPreparar = (*env)->GetStaticMethodID(env, c, "preparar", "(J[J)I");
    jCont = (*env)->GetStaticMethodID(env, c, "contadores", "()[J");
    jAlvo = (*env)->GetStaticMethodID(env, c, "alvo", "(J)Ljava/lang/String;");
    jLote = (*env)->GetStaticMethodID(env, c, "lote", "(J)I");
    if ((*env)->ExceptionCheck(env) || !jAbrir || !jInfo || !jQuadro || !jExato || !jSoltar || !jFechar || !jLidos || !jRitmo || !jFalhaRede || !jTela || !jPreparar || !jCont || !jAlvo || !jLote) {
      (*env)->ExceptionClear(env);
    } else {
      cls = (jclass)(*env)->NewGlobalRef(env, c);
    }
    (*env)->DeleteLocalRef(env, c);
  }
  (*env)->DeleteLocalRef(env, nome);
  (*env)->DeleteLocalRef(env, lc);
  (*env)->DeleteLocalRef(env, loader);
  (*env)->DeleteLocalRef(env, ac);
  (*env)->DeleteLocalRef(env, act);
  if (!cls) { printf("[miniatura] engine class missing\n"); fflush(stdout); }
  return cls != NULL;
}

// `manter`: keep the socket between requests (the copy). The playing file closes it after every request.
static long long mAbrir(const char *url, const char *cab, int manter) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jstring u = (*env)->NewStringUTF(env, url), c = (*env)->NewStringUTF(env, cab ? cab : "");
  jlong h = (*env)->CallStaticLongMethod(env, cls, jAbrir, u, c, manter ? JNI_TRUE : JNI_FALSE);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); h = 0; }
  (*env)->DeleteLocalRef(env, u); (*env)->DeleteLocalRef(env, c);
  return h;
}
// [duration ms, width, height, hdr, bytes read, codec usable (H.264/HEVC/DV)]
static void mInfo(long long h, long long out[6]) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  memset(out, 0, 6 * sizeof *out);
  jlongArray a = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, jInfo, (jlong)h);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
  if (a) { (*env)->GetLongArrayRegion(env, a, 0, 6, (jlong *)out); (*env)->DeleteLocalRef(env, a); }
}
// Unpacks MiniaturaMotor's reply: pts (ms), signature (unit-norm, mean-removed; 0 when flat), JPEG (malloc'd).
static int desempacotar(JNIEnv *env, jbyteArray a, long *pts, float sig[SIG_N], int *temSig, unsigned char **jpg, int *nJpg);
// One keyframe (closest or at/before tMs).
static int mQuadro(long long h, long tMs, int closest, int querJpeg, long *pts, float sig[SIG_N], int *temSig,
                   unsigned char **jpg, int *nJpg) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, jQuadro, (jlong)h, (jlong)tMs,
                                                            closest ? JNI_TRUE : JNI_FALSE, querJpeg ? JNI_TRUE : JNI_FALSE);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
  return desempacotar(env, a, pts, sig, temSig, jpg, nJpg);
}
// The frame AT tMs (decoded forward from the keyframe), signature only: the fine pass of the check.
static int mExato(long long h, long tMs, long *pts, float sig[SIG_N], int *temSig, unsigned char **jpg, int *nJpg) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, jExato, (jlong)h, (jlong)tMs, jpg ? JNI_TRUE : JNI_FALSE);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
  return desempacotar(env, a, pts, sig, temSig, jpg, nJpg);
}
static int desempacotar(JNIEnv *env, jbyteArray a, long *pts, float sig[SIG_N], int *temSig, unsigned char **jpg, int *nJpg) {
  if (!a) return 0;
  jsize n = (*env)->GetArrayLength(env, a);
  unsigned char *b = n >= CAB + SIG_N ? malloc((size_t)n) : NULL;   // signature-only replies carry no JPEG
  if (b) (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)b);
  (*env)->DeleteLocalRef(env, a);
  if (!b) return 0;
  long long p = 0;
  for (int i = 0; i < 8; i++) p = (p << 8) | b[i];
  *pts = (long)p;
  double m = 0, s2 = 0;
  for (int i = 0; i < SIG_N; i++) m += b[CAB + i];
  m /= SIG_N;
  for (int i = 0; i < SIG_N; i++) { double d = b[CAB + i] - m; s2 += d * d; }
  double desvio = sqrt(s2 / SIG_N), norma = sqrt(s2);
  *temSig = desvio >= MIN_DESVIO && norma > 0;
  for (int i = 0; i < SIG_N; i++) sig[i] = *temSig ? (float)((b[CAB + i] - m) / norma) : 0.0f;
  if (jpg) {
    *nJpg = n - CAB - SIG_N;
    *jpg = *nJpg > 0 ? malloc((size_t)*nJpg) : NULL;
    if (*jpg) memcpy(*jpg, b + CAB + SIG_N, (size_t)*nJpg);
  }
  free(b);
  return 1;
}
static void mSoltar(long long h) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  (*env)->CallStaticVoidMethod(env, cls, jSoltar, (jlong)h);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
static void mFechar(long long h) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  if (h) (*env)->CallStaticVoidMethod(env, cls, jFechar, (jlong)h);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
static long long mLidos(long long h) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jlong r = h ? (*env)->CallStaticLongMethod(env, cls, jLidos, (jlong)h) : 0;
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 0; }
  return r;
}

static int mFalhaRede(long long h) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jint r = h ? (*env)->CallStaticIntMethod(env, cls, jFalhaRede, (jlong)h) : 0;
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 0; }
  return r;
}
// One multi-range request for the keyframes nearest these copy times (MiniaturaMotor.preparar): >= 0 fetched,
// -1 batches not possible for this file, -2 refused.
static int mPreparar(long long h, const long *tempos, int n) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jlongArray a = (*env)->NewLongArray(env, n);
  if (!a) return -1;
  jlong v[LOTE];
  for (int i = 0; i < n && i < LOTE; i++) v[i] = tempos[i];
  (*env)->SetLongArrayRegion(env, a, 0, n, v);
  jint r = (*env)->CallStaticIntMethod(env, cls, jPreparar, (jlong)h, a);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
  (*env)->DeleteLocalRef(env, a);
  return r;
}
// MiniaturaMotor.contadores() since the app started; the summary takes deltas.
#define NCONT 12
static int mLote(long long h) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jint r = (*env)->CallStaticIntMethod(env, cls, jLote, (jlong)h);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 8; }
  return r < 4 ? 4 : r > LOTE ? LOTE : r;
}
static void mContadores(long long out[NCONT]) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  memset(out, 0, NCONT * sizeof *out);
  jlongArray a = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, jCont);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
  if (a) { (*env)->GetLongArrayRegion(env, a, 0, NCONT, (jlong *)out); (*env)->DeleteLocalRef(env, a); }
}
// The copy's link after the add-on's redirect, so further sessions do not make the add-on mint another debrid link.
static void mAlvo(long long h, const char *reserva, char *out, size_t n) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  snprintf(out, n, "%s", reserva);
  jstring sj = (jstring)(*env)->CallStaticObjectMethod(env, cls, jAlvo, (jlong)h);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
  if (sj) {
    const char *c = (*env)->GetStringUTFChars(env, sj, NULL);
    if (c && c[0]) snprintf(out, n, "%s", c);
    if (c) (*env)->ReleaseStringUTFChars(env, sj, c);
    (*env)->DeleteLocalRef(env, sj);
  }
}
static long long mRitmo(void) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jlong r = (*env)->CallStaticLongMethod(env, cls, jRitmo);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 0; }
  return r;
}

static long memDisponivelKB(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  char l[128]; long v = -1;
  if (!f) return -1;
  while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %ld kB", &v) == 1) break;
  fclose(f);
  return v;
}

static int parou(const Job *j) { return j->parar || j->pararFaixas; }
static void dormir(const Job *j, int ms) { for (int t = 0; t < ms && !parou(j); t += 50) usleep(50000); }

// The player first: nothing is read while it rebuffers, reconnects or has failed. Measured 22/10 (Dune 2, 4K
// through a StremThru proxy): a burst of thumbnail reads on the playing link and the player's reconnects were
// refused together (ConnectException) until it gave up.
static int folgaBaixa(void) {
  double bf = video_buffer_fim(), pos = video_pos(), dur = video_duracao();
  double quer = 20.0;
  if (dur > 0 && dur - pos - 1.0 < quer) quer = dur - pos - 1.0;
  return bf > 0.5 && quer > 0 && bf - pos < quer;
}
static void esperarPlayer(const Job *j) {
  int avisou = 0;
  while (!parou(j) && (bufferando || video_reconectando() || video_falhou() || folgaBaixa())) {
    if (!avisou) { printf("[miniatura] waiting: the player is buffering/reconnecting\n"); fflush(stdout); avisou = 1; }
    dormir(j, 1000);
  }
}
// A failed frame: when a READ failed (the engine says so), back off 5 s, 10 s, 20 s and end the job on the 4th in a
// row (the link is refusing us). A frame that only would not DECODE is skipped at once; 3 of those in a row end it
// (22/10: a copy whose frames all threw made decoders in a burst next to the player, whose own decoder then failed).
static __thread int falhasSeguidas, decodSeguidas;   // per lane
static int mFalhaRede(long long h);
static int falhou(const Job *j, int ok, long long h) {
  if (ok) { falhasSeguidas = 0; decodSeguidas = 0; return 0; }
  int rede = mFalhaRede(h);
  if (rede == 2) {                      // refused or silent: the debrid is at its connection limit; the player comes first
    printf("[miniatura] stopping: the source refused/stalled a connection (protecting playback)\n"); fflush(stdout);
    return 1;
  }
  if (!rede) {
    if (++decodSeguidas >= 3) { printf("[miniatura] stopping: %d frames in a row would not decode\n", decodSeguidas); fflush(stdout); return 1; }
    return 0;
  }
  if (++falhasSeguidas >= 4) {
    printf("[miniatura] stopping: %d reads in a row failed (source refusing)\n", falhasSeguidas); fflush(stdout);
    return 1;
  }
  printf("[miniatura] read failed, backing off %d s\n", 5 << (falhasSeguidas - 1)); fflush(stdout);
  dormir(j, 5000 << (falhasSeguidas - 1));
  return 0;
}

// ---- choosing the smaller copy (OpenSeek altsource.py, on the list the app already has) ----
static const char *EDICOES[] = { "extended", "directors", "director", "theatrical", "unrated", "imax", "remastered",
                                 "final cut", "special edition", "uncut", "redux", "open matte", "3d", "bd3d", "sbs", "hsbs" };
#define N_EDICOES (int)(sizeof EDICOES / sizeof *EDICOES)
static unsigned edicoes(const Stream *s) {
  char t[1200]; size_t k = 0;
  const char *fontes[] = { s->arquivo, s->rotulo, s->descricao };
  for (int f = 0; f < 3; f++)
    for (const char *p = fontes[f]; *p && k < sizeof t - 2; p++)
      t[k++] = (*p == '.' || *p == '_' || *p == '-' || *p == '\n') ? ' ' : (char)tolower((unsigned char)*p);
  t[k] = 0;
  unsigned m = 0;
  for (int i = 0; i < N_EDICOES; i++) {
    size_t n = strlen(EDICOES[i]);
    for (const char *p = t; (p = strstr(p, EDICOES[i])); p += n) {
      int ini = p == t || !isalnum((unsigned char)p[-1]), fim = !isalnum((unsigned char)p[n]);
      if (ini && fim) { m |= 1u << i; break; }
    }
  }
  return m;
}
static unsigned long long tamanho(const Stream *s) {
  return s->tamanhoBytes ? s->tamanhoBytes : (unsigned long long)s->tamanhoMB << 20;
}
// Size first: the cheapest copy that can be the same video, with guards (same edition words, cached, no samples,
// a resolution below the playing file's, or the same one at <= half its size, the 1080p 0.80 rule, bitrate floors).
// Up to TENTATIVAS, smallest first; unknown sizes last. `motivos` gets the counts behind the choice.
static int escolherCopias(const Stream *play, Copia *out, long durMs, char *motivos, size_t nmot) {
  enum { R_CACHE, R_RES, R_EDICAO, R_AMOSTRA, R_REPETIDA, R_BITRATE, R_1080, R_N };
  static const char *NOMES[R_N] = { "not-cached", "resolution", "edition", "sample", "duplicate", "bitrate", "1080-rule" };
  int rej[R_N] = {0}, nv = 0, cap = stream_n() > 0 ? stream_n() : 1;
  int *v = malloc(sizeof(int) * (size_t)cap);
  unsigned edA = edicoes(play);
  unsigned long long tamA = tamanho(play);
  if (!v) return 0;
  for (int i = 0; i < stream_n(); i++) {
    const Stream *c = stream_item(i);
    if (!c || i == stream_atual() || c->naoVideo || !c->url[0] || !strcmp(c->url, play->url)) continue;
    if (!(c->emCache || c->cacheNativo)) { rej[R_CACHE]++; continue; }   // never start a download for thumbnails
    unsigned long long tc = tamanho(c);
    int resOk = c->altura >= 480 && c->altura <= 1080 &&
                (play->altura <= 0 || c->altura < play->altura ||
                 (c->altura == play->altura && tc && tamA && tc <= (unsigned long long)(tamA * RAZAO_MESMA_RES)));
    if (!resOk) { rej[R_RES]++; continue; }
    if (edicoes(c) != edA) { rej[R_EDICAO]++; continue; }
    // Sample clips (22/10: the two "copies" of a 21.6 min episode were 30 s samples): by name, or under 50 MB.
    // A bigger sample is still caught by the runtime check, from the header alone (~1 s, ~0.4 MB).
    if (strcasestr(c->arquivo, "sample") || strcasestr(c->rotulo, "sample") || (tc && tc < (50ULL << 20))) { rej[R_AMOSTRA]++; continue; }
    if (tc && durMs > 60000) {
      double mbps = tc * 8.0 / (durMs / 1000.0) / 1e6;
      double piso = c->altura >= 1080 ? PISO_MBPS_1080 : c->altura >= 720 ? PISO_MBPS_720 : PISO_MBPS_SD;
      if (mbps < piso) { rej[R_BITRATE]++; continue; }
    }
    int repetida = 0;                   // the same file from another add-on: checking it twice is pure waste
    for (int k = 0; k < nv && !repetida; k++) {
      const Stream *o = stream_item(v[k]);
      repetida = (tc && tc == tamanho(o)) || (c->arquivo[0] && !strcmp(c->arquivo, o->arquivo));
    }
    if (repetida) { rej[R_REPETIDA]++; continue; }
    v[nv++] = i;
  }
  unsigned long long min720 = 0;
  for (int k = 0; k < nv; k++) {
    const Stream *c = stream_item(v[k]);
    if (c->altura == 720 && tamanho(c) && (!min720 || tamanho(c) < min720)) min720 = tamanho(c);
  }
  int m = 0;
  for (int k = 0; k < nv; k++) {
    const Stream *c = stream_item(v[k]);
    if (c->altura >= 1080 && min720 && (!tamanho(c) || tamanho(c) > (unsigned long long)(min720 * RAZAO_1080))) { rej[R_1080]++; continue; }
    v[m++] = v[k];
  }
  nv = m;
  for (int x = 1; x < nv; x++) {        // smallest first, unknown sizes last (insertion sort: lists are short)
    int y = x, cur = v[x];
    unsigned long long tcur = tamanho(stream_item(cur));
    while (y > 0) {
      unsigned long long tp = tamanho(stream_item(v[y - 1]));
      if (!(tcur && (!tp || tcur < tp))) break;
      v[y] = v[y - 1]; y--;
    }
    v[y] = cur;
  }
  int n = nv < TENTATIVAS ? nv : TENTATIVAS;
  for (int k = 0; k < n; k++) {
    const Stream *c = stream_item(v[k]);
    snprintf(out[k].url, sizeof out[k].url, "%s", c->url);
    snprintf(out[k].cab, sizeof out[k].cab, "%s", c->cabecalhos);
    out[k].altura = c->altura;
    out[k].tamanho = tamanho(c);
    snprintf(out[k].nome, sizeof out[k].nome, "%s", c->arquivo[0] ? c->arquivo : c->rotulo);
  }
  size_t o = (size_t)snprintf(motivos, nmot, "eligible %d (trying %d), rejected:", nv, n);
  for (int r = 0; r < R_N && o < nmot; r++) if (rej[r]) o += (size_t)snprintf(motivos + o, nmot - o, " %s %d", NOMES[r], rej[r]);
  free(v);
  return n;
}

// ---- the timeline check (OpenSeek timeline.verify_alignment) ----
typedef struct { double pa, pb; } Par;

static int cmpDouble(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double mediana(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmpDouble); return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2; }

// Best (a, b) over the speed hypotheses (same speed; the duration ratio when it could be a real speed change).
static int hipotese(const Par *m, int n, double razao, double *a, double *b) {
  double slopes[2] = { 1.0, razao }, melhorRms = 1e300, *bs = malloc(sizeof(double) * (size_t)(n ? n : 1));
  int ns = fabs(razao - 1.0) >= 0.01 ? 2 : 1;
  if (n < 2 || !bs) { free(bs); return 0; }
  for (int h = 0; h < ns; h++) {
    for (int i = 0; i < n; i++) bs[i] = m[i].pb - slopes[h] * m[i].pa;
    double med = mediana(bs, n), rms = 0;
    for (int i = 0; i < n; i++) rms += (m[i].pb - slopes[h] * m[i].pa - med) * (m[i].pb - slopes[h] * m[i].pa - med);
    rms = sqrt(rms / n);
    if (rms < melhorRms) { melhorRms = rms; *a = slopes[h]; *b = med; }
  }
  free(bs);
  return 1;
}


// Room for the player before each 4K read: at least 8 s buffered ahead (when the player reports it).
static void esperarFolga(const Job *j) {
  for (int t = 0; t < 30 && !parou(j); t++) {
    double bf = video_buffer_fim(), pos = video_pos();
    if (bf <= 0.5 || bf - pos >= 8.0) return;
    dormir(j, 500);
  }
}

// Fine pass (OpenSeek timeline._refine): for up to 8 spread anchors, the copy's frame at EXACT times around the
// coarse prediction, the similarity peak by a parabola; keyframe spacing no longer limits the offset. Copy only.
#define REFINAR 8
#define MIN_REFINADOS 5
static int refinar(Job *j, long long hB, const Ancora *anc, const Par *in, int ni, double a, double b, long durA,
                   double *b2, double *erro, int *nref, double minCorr) {
  static const long DES[5] = { -2000, -1000, 0, 1000, 2000 };
  Par ord[N_ANCORAS], ref[REFINAR];
  int n = 0;
  float sb[SIG_N];
  memcpy(ord, in, sizeof(Par) * (size_t)ni);
  for (int x = 0; x < ni; x++) for (int y = x + 1; y < ni; y++) if (ord[y].pa < ord[x].pa) { Par t = ord[x]; ord[x] = ord[y]; ord[y] = t; }
  int passo = ni / REFINAR > 0 ? ni / REFINAR : 1;
  for (int q = 0; q < ni && n < REFINAR && !parou(j); q += passo) {
    const float *sa = NULL;
    for (int k = 0; k < N_ANCORAS; k++) if (anc[k].estado == 1 && anc[k].ok && anc[k].pA == (long)ord[q].pa) sa = anc[k].sig;
    if (!sa) continue;
    double cs[5], ps[5]; int nc = 0;
    for (int d = 0; d < 5; d++) {
      long t = (long)(a * ord[q].pa + b) + DES[d], p; int ok;
      if (t < 0) t = 0;
      esperarPlayer(j);
      int lido = mExato(hB, t, &p, sb, &ok, NULL, NULL);
      if (falhou(j, lido, hB)) return 0;
      if (!lido || !ok) continue;
      double c = 0;
      for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
      cs[nc] = c; ps[nc] = p; nc++;
    }
    if (nc < 3) continue;
    int m = 0;
    for (int k = 1; k < nc; k++) if (cs[k] > cs[m]) m = k;
    if (cs[m] < minCorr) continue;
    double t = ps[m];
    if (m > 0 && m < nc - 1 && ps[m + 1] > ps[m - 1]) {                 // parabola through the peak and its neighbours
      double den = cs[m - 1] - 2 * cs[m] + cs[m + 1];
      if (den < -1e-9) {
        double s = 0.5 * (cs[m - 1] - cs[m + 1]) / den;
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        t = ps[m] + s * (fabs(ps[m + 1] - ps[m]) + fabs(ps[m] - ps[m - 1])) / 2;
      }
    }
    ref[n].pa = ord[q].pa; ref[n].pb = t; n++;
  }
  *nref = n;
  if (n < MIN_REFINADOS) return 0;
  double soma = 0, sp = 0, sp2 = 0, s2 = 0;
  for (int k = 0; k < n; k++) { soma += ref[k].pb - a * ref[k].pa; sp += ref[k].pa; sp2 += ref[k].pa * ref[k].pa; }
  *b2 = soma / n;
  for (int k = 0; k < n; k++) { double r = ref[k].pb - (a * ref[k].pa + *b2); s2 += r * r; }
  double sigma = sqrt(s2 / n), spread = sqrt(fmax(0, sp2 / n - (sp / n) * (sp / n)));
  if (spread <= 0) spread = 1;
  *erro = 3 * sigma / sqrt(n) + 2.0 * (sigma / (sqrt(n) * spread)) * durA / 2 + 500.0;
  return 1;
}

// ---- the check from the SCREEN: frames the player already shows, no 4K read ----
// For a copy whose runtime matches the playing file (within 2.5 s: the same cut), the frames on screen are the
// anchors: NvPlayer.assinaturaTela copies the frame being shown (PixelCopy, 160x90) with its position. Each one is
// matched against EXACT frames of the copy around the predicted time (the first one after a keyframe scan), and
// the parabola peak gives the offset. Accepted with 4+ anchors over 10+ s of playback that agree within 1.5 s.
// Measured before (22/10): the 4K anchors cost 74-82 MB of the playing file and the hitches at the start.
#define TELA_ANCORAS 5
#define TELA_MIN_CORR 0.55
#define TELA_ESPACO_MS 4000L
#define TELA_ACORDO_MS 3000.0      // measured 22/10: real matches spread 1.7-1.9 s (screen timing + 1 s exact steps)
#define TELA_PRAZO_MS 120000u
#define TELA_DUR_IGUAL_MS 30000L    // copies up to 30 s longer/shorter go to the screen check, which measures the offset
                                    // (22/10: a Bond film's only same-codec copy was 12 s off: logo/intro variants)
#define OTIMISTA_DUR_MS 4000L       // the optimistic start only for (nearly) the same length

static int mTela(long *pts, float sig[SIG_N], int *temSig) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, jTela);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
  return desempacotar(env, a, pts, sig, temSig, NULL, NULL);
}

// Similarity peak of the copy's exact frames at centro + DES[i]; *t = interpolated copy time. 0 = no frame/weak.
static int picoExato(Job *j, long long hB, const float *sa, double centro, const long *des, int nd, double minCorr,
                     double *t, double *rMelhor) {
  double cs[8], ps[8]; int nc = 0;
  float sb[SIG_N];
  for (int d = 0; d < nd && !parou(j); d++) {
    long alvo = (long)(centro + des[d]), p; int ok;
    if (alvo < 0) alvo = 0;
    esperarPlayer(j);
    int lido = mExato(hB, alvo, &p, sb, &ok, NULL, NULL);
    // A check frame that did not come (a slow forward decode) only skips this probe; a READ failure still backs
    // off, and a refused connection still ends the title (23/10: three exact-frame time-outs ended a good copy).
    if (!lido && mFalhaRede(hB) && falhou(j, lido, hB)) return -1;
    if (lido) falhou(j, lido, hB);
    if (!lido || !ok) continue;
    double c = 0;
    for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
    cs[nc] = c; ps[nc] = p; nc++;
  }
  if (nc < 2) return 0;
  int m = 0;
  for (int k = 1; k < nc; k++) if (cs[k] > cs[m]) m = k;
  *rMelhor = cs[m];
  if (cs[m] < minCorr) return 0;
  *t = ps[m];
  if (m > 0 && m < nc - 1 && ps[m + 1] > ps[m - 1]) {
    double den = cs[m - 1] - 2 * cs[m] + cs[m + 1];
    if (den < -1e-9) {
      double s = 0.5 * (cs[m - 1] - cs[m + 1]) / den;
      if (s > 1) s = 1;
      if (s < -1) s = -1;
      *t = ps[m] + s * (fabs(ps[m + 1] - ps[m]) + fabs(ps[m] - ps[m - 1])) / 2;
    }
  }
  return 1;
}

// 1 accepted (mapa filled), 0 rejected, -1 the screen gave no frames (the caller falls back to the 4K anchors).
static int conferirTela(Job *j, long long hB, long durB, Mapa *mapa) {
  static const long LARGO[] = { -2000, -1000, 0, 1000, 2000 }, ESTREITO[] = { -1000, 0, 1000 };
  Par m[TELA_ANCORAS];
  int nm = 0, telas = 0, falhasTela = 0, provas = 0;
  long ultimo = -1000000;
  double b = 0;
  float sa[SIG_N], sb[SIG_N];
  Uint32 ini = SDL_GetTicks();
  while (nm < TELA_ANCORAS && !parou(j) && SDL_GetTicks() - ini < TELA_PRAZO_MS && provas < TELA_ANCORAS * 3) {
    esperarPlayer(j);
    if (scrubbing) { dormir(j, 500); continue; }
    long pA; int ok;
    if (!mTela(&pA, sa, &ok)) { if (++falhasTela >= 10 && !telas) return -1; dormir(j, 1000); continue; }
    telas++;
    if (!ok || labs(pA - ultimo) < TELA_ESPACO_MS || pA < 5000) { dormir(j, 1000); continue; }   // flat, too close
    ultimo = pA; provas++;
    double t, r = -2;
    int achou;
    if (nm == 0) {
      // first anchor: the copy's KEYFRAMES +-30 s around the same time (an intro, logo or title sequence of another
      // length shifts what follows; 22/10 a Bond copy 12 s shorter was -4.8 s at the opening and further off later,
      // so a check started mid-film missed it at +-8 s), fetched in ONE multi-range batch, then the exact frames
      double melhor = -2, pMelhor = pA;
      long ultimoKf = -1, ts[31];
      int nt = 0;
      for (long d = -30000; d <= 30000; d += 2000) if (pA + d >= 0 && pA + d < durB) ts[nt++] = pA + d;
      if (nt > 1) mPreparar(hB, ts, nt);
      for (int i = 0; i < nt && !parou(j); i++) {
        long alvo = ts[i], p; int okB;
        esperarPlayer(j);
        int lido = mQuadro(hB, alvo, 1, 0, &p, sb, &okB, NULL, NULL);
        if (falhou(j, lido, hB)) return 0;
        if (!lido || !okB || p == ultimoKf) continue;
        ultimoKf = p;
        double c = 0;
        for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
        if (c > melhor) { melhor = c; pMelhor = p; }
      }
      if (melhor < TELA_MIN_CORR - 0.15) {
        printf("[miniatura] screen check at %lds: no similar copy frame nearby (best r=%.2f)\n", pA / 1000, melhor); fflush(stdout);
        continue;
      }
      achou = picoExato(j, hB, sa, pMelhor, LARGO, 5, TELA_MIN_CORR, &t, &r);
    } else {
      achou = picoExato(j, hB, sa, pA + b, ESTREITO, 3, TELA_MIN_CORR, &t, &r);
    }
    if (achou < 0) return 0;
    printf("[miniatura] screen check at %lds: r=%.2f %s (offset %+.2fs)\n", pA / 1000, r, achou ? "match" : "no",
           achou ? (t - pA) / 1000.0 : 0.0);
    fflush(stdout);
    if (!achou) continue;
    m[nm].pa = pA; m[nm].pb = t; nm++;
    double soma = 0;
    for (int k = 0; k < nm; k++) soma += m[k].pb - m[k].pa;
    b = soma / nm;
  }
  if (!telas) return -1;
  if (nm < 4) { printf("[miniatura] screen check: rejected (%d of %d agree)\n", nm, provas); fflush(stdout); return 0; }
  // the median offset; the one anchor farthest from it is dropped when 5+ agreed (a static scene can peak wrong)
  double offs[TELA_ANCORAS];
  for (int k = 0; k < nm; k++) offs[k] = m[k].pb - m[k].pa;
  b = mediana(offs, nm);
  if (nm >= 5) {
    int pior = 0;
    for (int k = 1; k < nm; k++) if (fabs(m[k].pb - m[k].pa - b) > fabs(m[pior].pb - m[pior].pa - b)) pior = k;
    m[pior] = m[--nm];
    for (int k = 0; k < nm; k++) offs[k] = m[k].pb - m[k].pa;
    b = mediana(offs, nm);
  }
  double lo = 1e18, hi = -1e18, s2 = 0, aMin = 1e18, aMax = -1e18;
  for (int k = 0; k < nm; k++) {
    double o = m[k].pb - m[k].pa;
    if (o < lo) lo = o;
    if (o > hi) hi = o;
    s2 += (o - b) * (o - b);
    if (m[k].pa < aMin) aMin = m[k].pa;
    if (m[k].pa > aMax) aMax = m[k].pa;
  }
  if (hi - lo > TELA_ACORDO_MS || aMax - aMin < 10000) {
    printf("[miniatura] screen check: rejected (offsets spread %.1fs over %.0fs of playback)\n", (hi - lo) / 1000, (aMax - aMin) / 1000);
    fflush(stdout);
    return 0;
  }
  double erro = 3 * sqrt(s2 / nm) / sqrt(nm) + 500.0;
  mapa->a = 1.0; mapa->b = b; mapa->erro = erro; mapa->inliers = nm;
  j->ultimoBom = (long)aMax;           // the latest playback time this copy was seen to match
  printf("[miniatura] screen check: ACCEPTED t_copy = t_play %+.2fs (%d anchors over %.0fs, +-%.1fs) in %.1f s, no 4K read\n",
         b / 1000, nm, (aMax - aMin) / 1000, erro / 1000, (SDL_GetTicks() - ini) / 1000.0);
  fflush(stdout);
  return 1;
}

// One re-check: the frame on screen against the copy's exact frames at the mapped time (-1, 0, +1 s). 1 = matches,
// 0 = does not (best r < RECHECK_MIN), -1 = no usable screen frame or copy frame (paused, flat scene, read failure).
#define RECHECK_MS 180000u
#define RECHECK_MIN 0.45
static int reconferir(Job *j, long long h, Mapa m, long *pA) {
  float sa[SIG_N], sb[SIG_N];
  int ok, vistos = 0;
  long p;
  if (!h || !mTela(pA, sa, &ok) || !ok || *pA < 5000) return -1;
  double melhor = -2;
  for (long d = -1000; d <= 1000 && !parou(j); d += 1000) {
    long alvo = (long)(m.a * *pA + m.b) + d;
    int okB;
    if (alvo < 0 || !mExato(h, alvo, &p, sb, &okB, NULL, NULL) || !okB) continue;
    vistos++;
    double c = 0;
    for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
    if (c > melhor) melhor = c;
  }
  if (!vistos) return -1;
  return melhor >= RECHECK_MIN ? 1 : 0;
}

// After the copy stopped matching (an opening, a scene or credits of another length): find its NEW offset. Two screen
// frames, each searched over the copy's keyframes +-30 s around the old prediction (fetched in ONE multi-range batch),
// then exact frames around the best; accepted when the two offsets agree within 2 s. 1 = found (*novoB).
// Measured 22/10 (Bond, 164 min): the offset was -4.8 s for the opening, then the copy diverged.
static int reancorar(Job *j, long long h, Mapa m, double *novoB) {
  static const long LARGO[] = { -2000, -1000, 0, 1000, 2000 };
  double offs[2];
  int n = 0, provas = 0;
  long ultimo = -1000000;
  float sa[SIG_N], sb[SIG_N];
  Uint32 ini = SDL_GetTicks();
  while (n < 2 && provas < 6 && !parou(j) && SDL_GetTicks() - ini < 120000u) {
    esperarPlayer(j);
    if (scrubbing) { dormir(j, 500); continue; }
    long pA; int ok;
    if (!mTela(&pA, sa, &ok) || !ok || labs(pA - ultimo) < 3000) { dormir(j, 1000); continue; }
    ultimo = pA; provas++;
    double centro = m.a * pA + m.b, melhor = -2, pMelhor = 0;
    long ts[31]; int nt = 0;
    for (long d = -30000; d <= 30000; d += 2000) if ((long)centro + d >= 0) ts[nt++] = (long)centro + d;
    if (nt > 1) mPreparar(h, ts, nt);                      // the whole window in one request
    long ultimoKf = -1;
    for (int i = 0; i < nt && !parou(j); i++) {
      long p; int okB;
      if (!mQuadro(h, ts[i], 1, 0, &p, sb, &okB, NULL, NULL) || !okB || p == ultimoKf) continue;
      ultimoKf = p;
      double c = 0;
      for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
      if (c > melhor) { melhor = c; pMelhor = p; }
    }
    double t = 0, r = -2;
    int achou = melhor >= TELA_MIN_CORR && picoExato(j, h, sa, pMelhor, LARGO, 5, TELA_MIN_CORR, &t, &r) == 1;
    printf("[miniatura] re-anchor probe at %lds: best r=%.2f %s\n", pA / 1000, achou ? r : melhor,
           achou ? "match" : "no");
    fflush(stdout);
    if (achou) {
      char extra[48] = "";
      offs[n++] = t - pA;
      if (n == 2) snprintf(extra, sizeof extra, " / %+.2fs", offs[1] / 1000);
      printf("[miniatura] re-anchor offset %+.2fs%s\n", offs[0] / 1000, extra); fflush(stdout);
    }
  }
  if (n < 2 || fabs(offs[0] - offs[1]) > 2000) return 0;
  *novoB = (offs[0] + offs[1]) / 2;
  return 1;
}

static int conferir(Job *j, long long hA, long long hB, long durA, long durB, Ancora *anc, Mapa *mapa, int hdr) {
  double minCorr = hdr ? 0.50 : MIN_CORR;
  double razao = (double)durB / (double)durA;
  Par matches[N_ANCORAS];
  int nm = 0, chamadasB = 0;
  float sb[SIG_N];
  if (fabs(razao - 1.0) > MAX_RAZAO_ERRO) { printf("[miniatura] check: runtime differs %.1f%%\n", (razao - 1) * 100); return 0; }
  for (int v = 0; v < N_ANCORAS && !parou(j); v++) {
    int i = (v * 7) % N_ANCORAS;
    Ancora *an = &anc[i];
    if (an->estado == 0) {
      double frac = (i + 0.2 + 0.6 * fmod(i * 0.6180339887, 1.0)) / N_ANCORAS;
      long tA = (long)(durA * frac);
      tA = (tA + GRADE_MS / 2) / GRADE_MS * GRADE_MS;
      unsigned char *jpg = NULL; int nj = 0;
      esperarPlayer(j);
      esperarFolga(j);
      int lido = mQuadro(hA, tA, 1, 1, &an->pA, an->sig, &an->ok, &jpg, &nj);
      if (falhou(j, lido, hA)) { mSoltar(hA); return 0; }
      mSoltar(hA);                                           // the 4K decoder goes back to the player between anchors
      an->estado = lido ? 1 : -1;
      if (lido) {                                            // the anchor is also a finished tile when it lands on its slot
        int k = (int)((an->pA + GRADE_MS / 2) / GRADE_MS);
        pthread_mutex_lock(&mu);
        if (jpg && k >= 0 && k < j->n && labs(an->pA - k * GRADE_MS) <= TOL_MS && !j->tiles[k].jpg) {
          j->tiles[k].jpg = jpg; j->tiles[k].n = nj; j->tiles[k].pts = an->pA; j->tentado[k] = 1;
          j->feitos++; estado = M_PRONTO; jpg = NULL;
        }
        pthread_mutex_unlock(&mu);
      }
      free(jpg);
      dormir(j, 400);                                        // spread the 4K reads out
    }
    if (an->estado != 1 || !an->ok) continue;                // failed, or a flat frame: no information
    long pA = an->pA;
    const float *sa = an->sig;
    double a, b, centro;
    long janela, passo;
    if (hipotese(matches, nm, razao, &a, &b)) { centro = a * pA + b; janela = JANELA_ESTREITA_MS; passo = PASSO_ESTREITO_MS; }
    else { centro = razao * pA; janela = JANELA_LARGA_MS; passo = PASSO_LARGO_MS; }
    double melhor = -2, pMelhor = 0;
    double cs[64], ps[64]; int nc = 0;
    for (long t = (long)centro - janela; t <= (long)centro + janela && nc < 64 && !parou(j); t += passo) {
      long tb = t < 0 ? 0 : t >= durB ? durB - 1 : t, pB; int okB;
      esperarPlayer(j);
      int lidoB = mQuadro(hB, tb, 1, 0, &pB, sb, &okB, NULL, NULL);
      if (falhou(j, lidoB, hB)) return 0;
      if (!lidoB) continue;
      chamadasB++;
      if (!okB) continue;
      int visto = 0;
      for (int k = 0; k < nc; k++) if (ps[k] == pB) visto = 1;
      if (visto) continue;
      double c = 0;
      for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
      cs[nc] = c; ps[nc] = pB; nc++;
      if (c > melhor) { melhor = c; pMelhor = pB; }
    }
    double segundo = -1;
    for (int k = 0; k < nc; k++) if (fabs(ps[k] - pMelhor) > TOMADA_MS && cs[k] > segundo) segundo = cs[k];
    int casou = nc && melhor >= minCorr && melhor - segundo >= MIN_MARGEM;
    if (casou) { matches[nm].pa = pA; matches[nm].pb = pMelhor; nm++; }
    printf("[miniatura] check anchor %d/%d at %lds: best r=%.2f next r=%.2f %s (offset %+.1fs)\n", v + 1, N_ANCORAS,
           pA / 1000, melhor, segundo, casou ? "match" : "no", nc ? (pMelhor - pA) / 1000.0 : 0.0);
    fflush(stdout);
    // Enough already: from 12 anchors on, 8+ agreeing on one mapping with 2+ in every quarter is what the full 20
    // would have to show anyway (the fine pass still checks the timing). Saves ~8 4K reads and ~25 s.
    if (v + 1 >= 12 && nm >= MIN_INLIERS) {
      double a0, b0;
      int q[4] = {0}, ok = 0;
      if (hipotese(matches, nm, razao, &a0, &b0))
        for (int k = 0; k < nm; k++)
          if (fabs(matches[k].pb - (a0 * matches[k].pa + b0)) <= MAX_RESIDUO_MS) {
            int qq = (int)(matches[k].pa * 4 / durA);
            if (qq >= 0 && qq < 4) q[qq]++;
            ok++;
          }
      if (ok >= MIN_INLIERS && q[0] >= MIN_POR_QUARTO && q[1] >= MIN_POR_QUARTO && q[2] >= MIN_POR_QUARTO && q[3] >= MIN_POR_QUARTO) {
        printf("[miniatura] check: clear after %d anchors (%d agree)\n", v + 1, ok); fflush(stdout);
        break;
      }
    }
  }
  double a, b;
  if (!hipotese(matches, nm, razao, &a, &b)) { printf("[miniatura] check: no visual match (%d)\n", nm); return 0; }
  Par in[N_ANCORAS]; int ni = 0;
  for (int k = 0; k < nm; k++) if (fabs(matches[k].pb - (a * matches[k].pa + b)) <= MAX_RESIDUO_MS) in[ni++] = matches[k];
  int quartos[4] = {0};
  for (int k = 0; k < ni; k++) { int q = (int)(in[k].pa * 4 / durA); if (q >= 0 && q < 4) quartos[q]++; }
  if (ni < MIN_INLIERS || quartos[0] < MIN_POR_QUARTO || quartos[1] < MIN_POR_QUARTO || quartos[2] < MIN_POR_QUARTO ||
      quartos[3] < MIN_POR_QUARTO) {
    printf("[miniatura] check: rejected (%d anchors agree, quarters %d/%d/%d/%d)\n", ni, quartos[0], quartos[1], quartos[2], quartos[3]);
    return 0;
  }
  hipotese(in, ni, razao, &a, &b);
  double res = 0;
  for (int k = 0; k < ni; k++) { double r = fabs(in[k].pb - (a * in[k].pa + b)); if (r > res) res = r; }
  if (res > MAX_RESIDUO_MS || fabs(a - 1.0) > MAX_RAZAO_ERRO) { printf("[miniatura] check: rejected (residual %.0f ms)\n", res); return 0; }
  // the fine pass, under each plausible speed (same speed; the duration ratio when that could be a real speed change)
  double slopes[2] = { a, fabs(razao - 1.0) >= 0.01 ? (a == 1.0 ? razao : 1.0) : a };
  int ns = slopes[1] != slopes[0] ? 2 : 1;
  double melhorErro = 1e300, melhorA = a, melhorB = b;
  int melhorN = 0;
  for (int h = 0; h < ns && !parou(j); h++) {
    double bh = b, b2, erro; int nref = 0;
    if (h == 1) {                                            // offset for the other slope: median over the inliers
      double bs[N_ANCORAS];
      for (int k = 0; k < ni; k++) bs[k] = in[k].pb - slopes[1] * in[k].pa;
      bh = mediana(bs, ni);
    }
    if (refinar(j, hB, anc, in, ni, slopes[h], bh, durA, &b2, &erro, &nref, minCorr) && fabs(b2 - bh) <= MAX_RESIDUO_MS && erro < melhorErro) {
      melhorErro = erro; melhorA = slopes[h]; melhorB = b2; melhorN = nref;
    }
    printf("[miniatura] fine pass (speed %.4f): %d anchors refined%s\n", slopes[h], nref,
           nref >= MIN_REFINADOS ? "" : " (too few)");
    fflush(stdout);
  }
  if (melhorN < MIN_REFINADOS) { printf("[miniatura] check: fine pass failed\n"); fflush(stdout); return 0; }
  if (melhorErro > MAX_ERRO_MS) { printf("[miniatura] check: mapping uncertain (%.0f ms after the fine pass)\n", melhorErro); return 0; }
  mapa->a = melhorA; mapa->b = melhorB; mapa->erro = melhorErro; mapa->inliers = ni;
  printf("[miniatura] check: ACCEPTED t_copy = %.5f x t_play %+.2fs (%d anchors, fine %d, +-%.1fs, %d copy reads)\n", melhorA,
         melhorB / 1000, ni, melhorN, melhorErro / 1000, chamadasB);
  fflush(stdout);
  return 1;
}

// ---- the set on this device's storage: a rewatch or a resume shows it at once and reads nothing ----
// <data>/miniaturas/<fnv64 of the file identity>.nvm : "NVM3" (older NVM1/NVM2 sets are ignored: squeezed 320x180, or saved before the re-check existed), n, dur, complete, then (k, pts, len, jpeg)...
// Identity: the file name + exact size the add-on declared (the same file through any link), else the link.
// Kept 2 hours after the last use (long enough to come back to the film the same evening), then deleted; never
// more than 200 MB in total, least recently used first.
#define CACHE_MAX_BYTES (200LL << 20)
#define CACHE_TTL_S (2 * 3600)

static void chaveArquivo(const SeekrFonte *f, char *out, size_t n) {
  char id[700];
  const char *nome = f->arquivo && f->arquivo[0] ? f->arquivo : "";
  const char *barra = strrchr(nome, '/');
  if (barra) nome = barra + 1;
  // Never the link: add-on/proxy links are signed per request (measured: a resumed episode missed its saved set).
  // The title + episode, then the file's exact size and name when the add-on declares them, else its duration.
  if (f->tamanho || nome[0])
    snprintf(id, sizeof id, "t|%s:%d:%d|%llu|%s", f->imdb ? f->imdb : "", f->t, f->e, (unsigned long long)f->tamanho, nome);
  else snprintf(id, sizeof id, "d|%s:%d:%d|%ld", f->imdb ? f->imdb : "", f->t, f->e, f->durMs / 1000);
  unsigned long long h = 1469598103934665603ULL;
  for (const char *p = id; *p; p++) { h ^= (unsigned char)tolower((unsigned char)*p); h *= 1099511628211ULL; }
  char dir[512];
  if (!dados_caminho(dir, sizeof dir, "miniaturas")) { out[0] = 0; return; }
  snprintf(out, n, "%s/%016llx.nvm", dir, h);
}

static void escreverI32(FILE *f, long v) { unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; fwrite(b, 1, 4, f); }
static int lerI32(FILE *f, long *v) { unsigned char b[4]; if (fread(b, 1, 4, f) != 4) return 0; *v = (long)((unsigned long)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]); return 1; }

// Loads what an earlier run made. 1 = the set was complete (nothing to read from the network).
static int carregar(Job *j) {
  if (!j->arq[0]) return 0;
  struct stat st;
  if (stat(j->arq, &st) == 0 && time(NULL) - st.st_mtime > CACHE_TTL_S) { unlink(j->arq); return 0; }
  FILE *f = fopen(j->arq, "rb");
  if (!f) return 0;
  char mag[4]; long n, dur, completo, k, pts, len;
  int lidos = 0;
  if (fread(mag, 1, 4, f) != 4 || memcmp(mag, "NVM3", 4) || !lerI32(f, &n) || !lerI32(f, &dur) || !lerI32(f, &completo) ||
      n != j->n) { fclose(f); return 0; }
  while (lerI32(f, &k) && lerI32(f, &pts) && lerI32(f, &len)) {
    if (k < 0 || k >= j->n || len <= 0 || len > (1 << 20)) break;
    unsigned char *b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); break; }
    pthread_mutex_lock(&mu);
    if (!j->tiles[k].jpg) { j->tiles[k].jpg = b; j->tiles[k].n = (int)len; j->tiles[k].pts = pts; j->feitos++; lidos++; b = NULL; }
    j->tentado[k] = 1;
    if (job == j && j->feitos) estado = M_PRONTO;
    pthread_mutex_unlock(&mu);
    free(b);
  }
  fclose(f);
  utime(j->arq, NULL);                                       // recently used: the cap removes others first
  printf("[miniatura] loaded %d saved thumbnails%s\n", lidos, completo ? " (complete: nothing to read)" : ""); fflush(stdout);
  return completo && lidos > 0;
}

typedef struct { char nome[300]; long long tam; time_t quando; } ArqCache;
static void limparCache(const char *dirArq) {
  char dir[512];
  snprintf(dir, sizeof dir, "%s", dirArq);
  char *barra = strrchr(dir, '/');
  if (!barra) return;
  *barra = 0;
  DIR *d = opendir(dir);
  if (!d) return;
  ArqCache *v = NULL; int n = 0, cap = 0;
  long long total = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t L = strlen(e->d_name);
    if (L < 5 || strcmp(e->d_name + L - 4, ".nvm")) continue;
    char p[820]; struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    if (stat(p, &st)) continue;
    if (time(NULL) - st.st_mtime > CACHE_TTL_S && strcmp(p, dirArq)) { unlink(p); continue; }   // expired
    if (n == cap) { cap = cap ? cap * 2 : 32; ArqCache *nv = realloc(v, sizeof *v * (size_t)cap); if (!nv) break; v = nv; }
    snprintf(v[n].nome, sizeof v[n].nome, "%s", p); v[n].tam = st.st_size; v[n].quando = st.st_mtime; n++;
    total += st.st_size;
  }
  closedir(d);
  while (total > CACHE_MAX_BYTES && n > 1) {
    int velhoI = 0;
    for (int i = 1; i < n; i++) if (v[i].quando < v[velhoI].quando) velhoI = i;
    if (!strcmp(v[velhoI].nome, dirArq)) { v[velhoI].quando = (time_t)0x7fffffff; continue; }
    unlink(v[velhoI].nome);
    total -= v[velhoI].tam;
    v[velhoI] = v[--n];
  }
  free(v);
}

static void salvar(Job *j, int completo) {
  if (!j->arq[0]) return;
  if (!j->feitos) { unlink(j->arq); return; }   // nothing left (a dropped copy): no stale set may survive (22/10, Bond)
  char dir[512], tmp[620];
  snprintf(dir, sizeof dir, "%s", j->arq);
  char *barra = strrchr(dir, '/');
  if (barra) { *barra = 0; mkdir(dir, 0700); }
  snprintf(tmp, sizeof tmp, "%s.tmp", j->arq);
  FILE *f = fopen(tmp, "wb");
  if (!f) return;
  fwrite("NVM3", 1, 4, f);
  escreverI32(f, j->n); escreverI32(f, j->dur); escreverI32(f, completo);
  for (int k = 0; k < j->n; k++) {
    pthread_mutex_lock(&mu);
    unsigned char *b = j->tiles[k].jpg; int len = j->tiles[k].n; long pts = j->tiles[k].pts;
    pthread_mutex_unlock(&mu);                                // tiles are only ever added while the job lives
    if (!b) continue;
    escreverI32(f, k); escreverI32(f, pts); escreverI32(f, len);
    fwrite(b, 1, (size_t)len, f);
  }
  int ok = fclose(f) == 0;
  if (ok && rename(tmp, j->arq) == 0) limparCache(j->arq);
  else unlink(tmp);
}

// ---- which slot next ----
static int livre(const Job *j, int k) { return k >= 0 && k < j->n && !j->tentado[k]; }
static int proximo(const Job *j) {
  int n = j->n, ph = (int)(playheadMs / GRADE_MS), sc = (int)((posMs + GRADE_MS / 2) / GRADE_MS);
  if (scrubbing) for (int d = 0; d <= 1; d++) { if (livre(j, sc + d)) return sc + d; if (livre(j, sc - d)) return sc - d; }
  for (int d = 0; d <= 6; d++) { if (livre(j, ph + d)) return ph + d; if (livre(j, ph - d)) return ph - d; }
  for (int k = 0; k < n; k += 30) if (livre(j, k)) return k;
  int fatia = (int)(n * 0.05) + 1;
  for (int d = 0; d <= fatia; d++) { if (livre(j, ph + d)) return ph + d; if (livre(j, ph - d)) return ph - d; }
  for (int k = 0; k < n; k += 6) if (livre(j, k)) return k;
  for (int k = 0; k < n; k++) if (livre(j, k)) return k;
  return -1;
}

// A slot the keyframe pass left empty (the copy's keyframes too far apart), nearest to the playhead first.
static int coberto(const Job *j, int k) {
  for (int d = -1; d <= 1; d++) {
    int v = k + d;
    if (v >= 0 && v < j->n && j->tiles[v].jpg && labs(j->tiles[v].pts - k * GRADE_MS) <= TOL_COPIA_MS) return 1;
  }
  return 0;
}
// Only real gaps (nothing made within 8 s): measured 22/10, exact-frame fills of covered slots were the last
// ~50 s of a 3 h film. And only while they are worth it: not at all once 90 % of the slots have their own
// thumbnail, and never more than 10 s (measured 22/10 on 102 min: ~20 fills cost 65 s and 80 MB, ~3 s and
// 4 MB each, for spots the bar already covered with a neighbour). Called with `mu` held.
#define LACUNA_COBERTURA 0.90
#define LACUNA_PRAZO_MS 10000u
static int lacunaPermitida(const Job *j) {
  if (j->feitos >= (int)(j->n * LACUNA_COBERTURA)) return 0;
  return !j->lacunaDesde || SDL_GetTicks() - j->lacunaDesde < LACUNA_PRAZO_MS;
}
static int proximaLacuna(const Job *j) {
  int ph = (int)(playheadMs / GRADE_MS), melhor = -1;
  for (int k = 0; k < j->n; k++)
    if (j->tentado[k] == 1 && !j->tiles[k].jpg && !coberto(j, k) && (melhor < 0 || abs(k - ph) < abs(melhor - ph))) melhor = k;
  return melhor;
}

// One lane of thumbnail making (see trabalhar). Slots are claimed under the lock, so two lanes never make the same one.
typedef struct { Job *j; long long h; int usaB; int id; int feitos; unsigned long somaMs; long long lido; volatile int acabou; } Faixa;
static Mapa mapaAtual(Job *j) { pthread_mutex_lock(&mu); Mapa m = j->mapa; pthread_mutex_unlock(&mu); return m; }
static void *faixa(void *u) {
  Faixa *f = u;
  Job *j = f->j;
  long long lido0 = mLidos(f->h);
  falhasSeguidas = 0; decodSeguidas = 0;
  // Batches: claim LOTE slots, fetch their keyframes with ONE multi-range request, then decode them from memory.
  int fila[LOTE], nfila = 0, ifila = 0, lote = f->usaB ? LOTE : 0, lotes = 0, trazidos = 0;
  while (!parou(j)) {
    if (bufferando || video_reconectando() || video_falhou() || folgaBaixa()) { mSoltar(f->h); esperarPlayer(j); continue; }
    long disp = memDisponivelKB();
    if (disp >= 0 && disp < PISO_KB) { mSoltar(f->h); dormir(j, 1500); continue; }
    int k = -1, exato = 0;
    if (ifila < nfila) k = fila[ifila++];
    else {
      pthread_mutex_lock(&mu);
      nfila = ifila = 0;
      if (lote > 0) lote = mLote(f->h);   // sized to ~4 MB of this copy's keyframe windows
      for (int quer = lote > 0 ? lote : 1; nfila < quer; ) {
        int q = proximo(j);
        if (q < 0) break;
        j->tentado[q] = 1; fila[nfila++] = q;
      }
      if (!nfila && f->usaB && lacunaPermitida(j) && (k = proximaLacuna(j)) >= 0) {   // second pass: the exact frame
        if (!j->lacunaDesde) j->lacunaDesde = SDL_GetTicks();
        j->tentado[k] = 2; exato = 1;
      }
      pthread_mutex_unlock(&mu);
      if (nfila > 1 && lote > 0) {
        long ts[LOTE];
        Mapa mp = mapaAtual(j);
        for (int i = 0; i < nfila; i++) { ts[i] = (long)(mp.a * fila[i] * GRADE_MS + mp.b); if (ts[i] < 0) ts[i] = 0; }
        int r = mPreparar(f->h, ts, nfila);
        if (r == -2) { printf("[miniatura] stopping: the source refused/stalled a connection (protecting playback)\n"); fflush(stdout); break; }
        if (r < 0) { lote = 0; printf("[miniatura] lane %d: multi-range batches not possible for this copy, one at a time\n", f->id); fflush(stdout); }
        else { lotes++; trazidos += r; }
      }
      if (nfila) k = fila[ifila++];
    }
    if (k < 0) break;
    long alvo = k * GRADE_MS, p; int okS;
    float sig[SIG_N]; unsigned char *jpg = NULL; int nj = 0;
    Uint32 s = SDL_GetTicks();
    Mapa mp = mapaAtual(j);
    long tB = f->usaB ? (long)(mp.a * alvo + mp.b) : alvo;
    if (tB < 0) tB = 0;
    int ok = exato ? mExato(f->h, tB, &p, sig, &okS, &jpg, &nj) : mQuadro(f->h, tB, 1, 1, &p, sig, &okS, &jpg, &nj);
    if (falhou(j, ok, f->h)) break;
    if (ok && f->usaB) p = (long)((p - mp.b) / mp.a);                 // back on the playing file's timeline
    long tol = f->usaB && !exato ? TOL_COPIA_MS : TOL_MS;
    if (ok && jpg && labs(p - alvo) <= tol) {
      int dest = (int)((p + GRADE_MS / 2) / GRADE_MS);
      pthread_mutex_lock(&mu);
      if (dest < 0 || dest >= j->n || (j->tiles[dest].jpg && dest != k)) dest = k;
      if (!j->tiles[dest].jpg) {
        j->tiles[dest].jpg = jpg; j->tiles[dest].n = nj; j->tiles[dest].pts = p; j->tiles[dest].prov = (unsigned char)j->otimista;
        jpg = NULL; j->feitos++;
      }
      j->tentado[dest] = 1;
      if (job == j) estado = M_PRONTO;
      pthread_mutex_unlock(&mu);
    }
    free(jpg);
    f->feitos++; f->somaMs += SDL_GetTicks() - s;
    f->lido = mLidos(f->h) - lido0;
    if (f->id == 1 && f->feitos % 100 == 0 && !j->otimista) salvar(j, 0);   // never save unverified (optimistic) thumbnails
    if (f->id == 1 && f->feitos % 30 == 0) {
      printf("[miniatura] %d/%d tiles from %s, lane avg %lu ms, lane read %.1f MB, pace %lld/s, %d batches (%d keyframes), free %ld MB\n",
             j->feitos, j->n, origem, f->somaMs / (unsigned long)f->feitos, f->lido / 1048576.0, mRitmo(), lotes, trazidos, disp / 1024);
      fflush(stdout);
    }
    if (!f->usaB) usleep(20000);
  }
  f->lido = mLidos(f->h) - lido0;
  f->acabou = 1;
  return NULL;
}

static void resumo(Job *j, Uint32 t0) {
  if (j->resumido) return;
  j->resumido = 1;
  long long cont[NCONT];
  mContadores(cont);
  long long nl = cont[5] - j->cont0[5], nk = cont[9] - j->cont0[9];
  printf("[miniatura] SUMMARY %s | play %dp %.1f min | candidates: %s | chosen: %s | offset %+.2fs, redo %s | "
         "decoders %lld, requests %lld, %.1f MB | time: network %.1f s, frames %.1f s | batches %lld (avg %.1f keyframes, %.2f MB), "
         "keyframe avg %.0f KB | keyframe thumbnails: decode %.1f s, resize+JPEG %.1f s | tiles %d/%d in %.0f s | playback: %d rebuffers, %d player errors | re-checks %d (misses %d), re-anchored %d%s\n",
         j->titulo, j->alturaA, j->dur / 60000.0, j->motivos, j->escolhida[0] ? j->escolhida : "none", j->offset / 1000,
         j->refeito ? "yes" : "no", cont[1] - j->cont0[1], cont[0] - j->cont0[0], (cont[2] - j->cont0[2]) / 1048576.0,
         (cont[3] - j->cont0[3]) / 1000.0, (cont[4] - j->cont0[4]) / 1000.0, nl,
         nl ? (double)(cont[7] - j->cont0[7]) / nl : 0.0, nl ? (cont[6] - j->cont0[6]) / 1048576.0 / nl : 0.0,
         nk ? (cont[8] - j->cont0[8]) / 1024.0 / nk : 0.0, (cont[10] - j->cont0[10]) / 1000.0, (cont[11] - j->cont0[11]) / 1000.0,
         j->feitos, j->n, (SDL_GetTicks() - t0) / 1000.0, evRebuffer, evErro, j->nRecheck, j->falhasRecheck,
         j->reancoras, j->caiuEm ? ", copy dropped mid-film" : "");
  fflush(stdout);
}

// The lanes finished (or were stopped): save, raise the "generated" notice when complete, log, summary.
static void fecharLote(Job *j, Uint32 t0, Faixa *f1, Faixa *f2, int faixas) {
  pthread_mutex_lock(&mu);
  int completo = proximo(j) < 0 && (!lacunaPermitida(j) || proximaLacuna(j) < 0) && !parou(j);
  pthread_mutex_unlock(&mu);
  salvar(j, completo);
  if (completo) avisoPronto = j->g;
  printf("[miniatura] done: %d/%d tiles from %s in %.0f s (%d lane%s), read %.1f MB, pace now %lld/s\n", j->feitos, j->n,
         origem, (SDL_GetTicks() - t0) / 1000.0, faixas, faixas == 1 ? "" : "s", (f1->lido + f2->lido) / 1048576.0, mRitmo());
  fflush(stdout);
  resumo(j, t0);
}

static void *trabalhar(void *u) {
  Job *j = u;
  long long hA = 0, hB = 0, info[6];
  Mapa mapa = {1.0, 0.0, 0.0, 0};
  int usaB = 0, copiaUsada = 0;
  Uint32 t0 = SDL_GetTicks();
  falhasSeguidas = 0; decodSeguidas = 0;
  mContadores(j->cont0);
  evRebuffer = evErro = 0;
  if (j->arq[0]) limparCache(j->arq);           // other titles' sets older than 2 h go now
  if (carregar(j)) goto fim;                    // a complete saved set: no network at all
  if (j->nCopias == 0) {                        // nothing smaller to make them from: the playing file is not opened
    printf("[miniatura] no smaller copy in the list: no thumbnails for this title\n"); fflush(stdout);
    pthread_mutex_lock(&mu); if (job == j) { estado = M_FALHOU; motivoSem = 1; } pthread_mutex_unlock(&mu);
    goto fim;
  }
  esperarPlayer(j);
  memset(info, 0, sizeof info);
  // The playing file is NOT opened here: the screen check needs only what the player shows. It is opened only for
  // the 4K-anchor check (a copy whose runtime differs, or no screen frames) or to make thumbnails from a <= 1080p file.
  printf("[miniatura] playing %dp %.1f min; %d smaller cop%s in the list\n", j->alturaA, j->dur / 60000.0, j->nCopias,
         j->nCopias == 1 ? "y" : "ies");
  fflush(stdout);
  Faixa f1, f2;
  memset(&f1, 0, sizeof f1); memset(&f2, 0, sizeof f2);
  pthread_t t1, t2;
  int roda1 = 0, roda2 = 0;
  char alvoB[4096];
  for (int c = 0; c < j->nCopias && !usaB && !parou(j); c++) {
    hB = mAbrir(j->copias[c].url, j->copias[c].cab, 1);
    if (!hB) {
      printf("[miniatura] copy %d (%dp) would not open\n", c + 1, j->copias[c].altura);
      snprintf(j->motivos + strlen(j->motivos), sizeof j->motivos - strlen(j->motivos), " | c%d:no-open", c + 1);
      continue;
    }
    long long ib[6];
    mInfo(hB, ib);
    mAlvo(hB, j->copias[c].url, alvoB, sizeof alvoB);
    printf("[miniatura] checking copy %d: %lldx%lld %.1f min\n", c + 1, ib[1], ib[2], ib[0] / 60000.0); fflush(stdout);
    if (!ib[5]) {                       // not H.264/HEVC: rejected from the header, before any decoder exists
      printf("[miniatura] copy %d: codec not usable for thumbnails (only H.264/HEVC)\n", c + 1); fflush(stdout);
      snprintf(j->motivos + strlen(j->motivos), sizeof j->motivos - strlen(j->motivos), " | c%d:codec", c + 1);
      mFechar(hB); hB = 0;
      continue;
    }
    int igual = ib[0] > 0 && labs((long)ib[0] - j->dur) <= TELA_DUR_IGUAL_MS;
    if (igual && !roda1 && labs((long)ib[0] - j->dur) <= OTIMISTA_DUR_MS) {
      // OPTIMISTIC START: same length within 2.5 s, so one lane starts at offset 0 while the screen check runs; the
      // check then relabels (offset <= 3 s) or discards (larger, or rejected) what it made.
      pthread_mutex_lock(&mu);
      j->mapa.a = 1.0; j->mapa.b = 0.0; j->otimista = 1;
      pthread_mutex_unlock(&mu);
      snprintf(origem, sizeof origem, "%dp copy", j->copias[c].altura);
      f1.j = j; f1.usaB = 1; f1.id = 1; f1.feitos = 0; f1.somaMs = 0; f1.lido = 0;
      f1.h = mAbrir(alvoB, j->copias[c].cab, 1);
      if (f1.h) roda1 = pthread_create(&t1, NULL, faixa, &f1) == 0;
      if (!roda1) { mFechar(f1.h); f1.h = 0; }
      else { printf("[miniatura] optimistic start: thumbnails from copy %d at offset 0 while the check runs\n", c + 1); fflush(stdout); }
    }
    Uint32 c0 = SDL_GetTicks();
    long long lA0 = mLidos(hA);
    int aceita = 0, tela = -1;
    const char *metodo = "none";
    if (igual) { tela = conferirTela(j, hB, (long)ib[0], &mapa); metodo = "screen"; }
    if (tela == 1) aceita = 1;
    else if (tela == -1 && ib[0] > 0 && j->alturaA > 1080 && !parou(j)) {   // never decode a <= 1080p playing file
      // ROUTER (MAX_RAZAO_ERRO, 10 %): runtime differs (maybe another cut: anchors across the whole film), or the
      // screen gave no frames; beyond 10 % the 4K-anchor check refuses at once
      printf("[miniatura] %s: checking with the playing file's own frames\n", igual ? "no screen frames" : "runtime differs");
      fflush(stdout);
      metodo = "4k-anchors";
      if (!hA) { hA = mAbrir(j->urlA, j->cabA, 0); if (hA) { mInfo(hA, info); lA0 = mLidos(hA); } }
      if (hA) aceita = conferir(j, hA, hB, j->dur, (long)ib[0], j->anc, &mapa, info[3] != 0);
    }
    long long lidosB = mLidos(hB);
    size_t o = strlen(j->motivos);
    if (aceita) {
      usaB = 1;
      copiaUsada = c;
      snprintf(origem, sizeof origem, "%dp copy", j->copias[c].altura);
      pthread_mutex_lock(&mu);
      if (j->otimista) {
        if (mapa.a == 1.0 && fabs(mapa.b) <= OFFSET_MANTER_MS) {
          int n = 0;
          for (int k = 0; k < j->n; k++) if (j->tiles[k].jpg && j->tiles[k].prov) { j->tiles[k].pts -= (long)mapa.b; j->tiles[k].prov = 0; n++; }
          printf("[miniatura] optimistic thumbnails kept: %d relabelled by %+.2fs\n", n, -mapa.b / 1000);
        } else {
          for (int k = 0; k < j->n; k++)
            if (!j->tiles[k].jpg || j->tiles[k].prov) {
              if (j->tiles[k].jpg) { free(j->tiles[k].jpg); j->tiles[k].jpg = NULL; j->feitos--; }
              j->tentado[k] = 0;
            }
          j->refeito = 1;
          printf("[miniatura] offset %+.2fs is over %.0fs: optimistic thumbnails redone\n", mapa.b / 1000, OFFSET_MANTER_MS / 1000);
        }
        j->otimista = 0;
      }
      j->mapa = mapa;
      j->offset = mapa.b;
      pthread_mutex_unlock(&mu);
      fflush(stdout);
      snprintf(j->escolhida, sizeof j->escolhida, "%dp %.0f MB %s (via %s)", j->copias[c].altura, j->copias[c].tamanho / 1048576.0,
               j->copias[c].nome, metodo);
      snprintf(j->motivos + o, sizeof j->motivos - o, " | c%d:ACCEPTED", c + 1);
    } else {
      if (roda1) {                       // the optimistic lane was making a wrong copy's thumbnails: stop, discard
        j->pararFaixas = 1;
        pthread_join(t1, NULL);
        roda1 = 0;
        j->pararFaixas = 0;
        mFechar(f1.h); f1.h = 0;
        pthread_mutex_lock(&mu);
        for (int k = 0; k < j->n; k++)
          if (!j->tiles[k].jpg || j->tiles[k].prov) {
            if (j->tiles[k].jpg) { free(j->tiles[k].jpg); j->tiles[k].jpg = NULL; j->feitos--; }
            j->tentado[k] = 0;
          }
        j->otimista = 0;
        pthread_mutex_unlock(&mu);
      }
      mFechar(hB); hB = 0;
      snprintf(j->motivos + o, sizeof j->motivos - o, " | c%d:%s", c + 1,
               ib[0] <= 0 ? "no-duration" : !igual && j->alturaA <= 1080 ? "runtime" : tela == 0 ? "screen-rejected" : "rejected");
    }
    printf("[miniatura] check took %.1f s, read play %.1f MB + copy %.1f MB\n", (SDL_GetTicks() - c0) / 1000.0,
           (mLidos(hA) - lA0) / 1048576.0, lidosB / 1048576.0);
    fflush(stdout);
  }
  // Never thumbnails from the PLAYING file, at any resolution: decoding the very video the player decodes is what
  // preceded the player's own decoder failing (4003) and the source switches (22/10, a 1080p remux episode).
  if (!usaB) {
    printf("[miniatura] no copy passed the check: stopping (thumbnails are never made from the playing file)\n");
    fflush(stdout);
    pthread_mutex_lock(&mu); if (job == j && !j->feitos) { estado = M_FALHOU; motivoSem = 1; } pthread_mutex_unlock(&mu);
    goto fim;
  }
  {
    // Two lanes on the copy, each with its own reader and decoder (both on the copy's resolved link: no new debrid
    // link minted), sharing ONE request in flight (MiniaturaMotor): one decodes while the other's batch is on the wire.
    mAlvo(hB, j->copias[copiaUsada].url, alvoB, sizeof alvoB);
    // hB stays open for the re-check while watching (below)
    if (!roda1 && !parou(j)) {
      f1.j = j; f1.usaB = 1; f1.id = 1; f1.feitos = 0; f1.somaMs = 0; f1.lido = 0;
      f1.h = mAbrir(alvoB, j->copias[copiaUsada].cab, 1);
      if (f1.h) roda1 = pthread_create(&t1, NULL, faixa, &f1) == 0;
      if (!roda1) { mFechar(f1.h); f1.h = 0; }
    }
    if (!parou(j)) {
      f2.j = j; f2.usaB = 1; f2.id = 2;
      f2.h = mAbrir(alvoB, j->copias[copiaUsada].cab, 1);
      if (f2.h) roda2 = pthread_create(&t2, NULL, faixa, &f2) == 0;
      if (!roda2) { mFechar(f2.h); f2.h = 0; }
    }
    // RE-CHECK WHILE WATCHING: the screen check only saw the first minute. Every 3 min of playback one frame on screen
    // is compared with the copy at the mapped time (a single exact frame, no 4K read). Two misses in a row (the second
    // 20 s after the first) mean the copy stopped matching (a scene cut or added in a same-length copy): the lanes
    // stop and every thumbnail after the last good check is dropped.
    int juntou = 0, falhasR = 0;
    long bomAte = j->ultimoBom;        // was 0: a drop then removed EVERY thumbnail (22/10, Bond: 931 of 931)
    Uint32 proxR = SDL_GetTicks() + RECHECK_MS;
    while (!j->parar) {
      if (!juntou && (!roda1 || f1.acabou) && (!roda2 || f2.acabou)) {
        if (roda1) pthread_join(t1, NULL);
        if (roda2) pthread_join(t2, NULL);
        juntou = 1;
        fecharLote(j, t0, &f1, &f2, roda1 + roda2);
      }
      if (!scrubbing && !bufferando && SDL_GetTicks() >= proxR) {
        long pA = 0;
        int r = reconferir(j, hB, mapaAtual(j), &pA);
        if (r == 1) { falhasR = 0; bomAte = pA; proxR = SDL_GetTicks() + RECHECK_MS; j->nRecheck++; }
        else if (r == 0) {
          j->nRecheck++; j->falhasRecheck++;
          printf("[miniatura] re-check at %lds: copy did not match (%d in a row)\n", pA / 1000, falhasR + 1); fflush(stdout);
          if (++falhasR >= 2) {
            // The lanes stop (nothing may be made with the old offset meanwhile), then: a NEW offset found -> every
            // thumbnail after the last confirmed match is redone with it (a piecewise map, segment by segment as
            // re-checks find them); none -> those thumbnails are dropped and the title stops.
            j->pararFaixas = 1;
            if (!juntou) {
              if (roda1) pthread_join(t1, NULL);
              if (roda2) pthread_join(t2, NULL);
              juntou = 1;
            }
            j->pararFaixas = 0;
            double nb = 0;
            int novo = reancorar(j, hB, mapaAtual(j), &nb);
            pthread_mutex_lock(&mu);
            int tirados = 0;
            double antigo = j->mapa.b;
            for (int k = 0; k < j->n; k++)
              if ((long)k * GRADE_MS > bomAte) {
                if (j->tiles[k].jpg) { free(j->tiles[k].jpg); j->tiles[k].jpg = NULL; j->feitos--; tirados++; }
                j->tentado[k] = 0;
              }
            if (novo) { j->mapa.b = nb; j->offset = nb; j->reancoras++; }
            pthread_mutex_unlock(&mu);
            if (novo && !parou(j)) {
              printf("[miniatura] re-anchored after %lds: offset %+.2fs -> %+.2fs, %d thumbnails after it redone\n", bomAte / 1000,
                     antigo / 1000, nb / 1000, tirados);
              fflush(stdout);
              falhasR = 0;
              bomAte = pA;
              f1.acabou = f2.acabou = 0;
              roda1 = f1.h && pthread_create(&t1, NULL, faixa, &f1) == 0;
              roda2 = f2.h && pthread_create(&t2, NULL, faixa, &f2) == 0;
              juntou = 0;
              j->resumido = 0;           // a new SUMMARY when the redo finishes
              proxR = SDL_GetTicks() + RECHECK_MS;
              continue;
            }
            j->caiuEm = pA;
            printf("[miniatura] the copy stopped matching after %lds and no new offset was found: %d thumbnails after it dropped\n",
                   bomAte / 1000, tirados);
            fflush(stdout);
            salvar(j, 0);
            break;
          }
          proxR = SDL_GetTicks() + 20000;
        } else proxR = SDL_GetTicks() + 10000;   // nothing on screen yet (paused): try again soon
      }
      dormir(j, 1000);
    }
    if (!juntou) {                       // the title was closed while the lanes ran
      if (roda1) pthread_join(t1, NULL);
      if (roda2) pthread_join(t2, NULL);
      fecharLote(j, t0, &f1, &f2, roda1 + roda2);
    }
    mFechar(f1.h); mFechar(f2.h);
  }
fim:
  mFechar(hB);
  mFechar(hA);
  resumo(j, t0);
  pthread_mutex_lock(&mu);
  j->vivo = 0;
  int orfao = job != j;
  pthread_mutex_unlock(&mu);
  if (orfao) {                                  // switched off meanwhile: the worker frees its own job
    for (int k = 0; k < j->n; k++) free(j->tiles[k].jpg);
    free(j->tiles); free(j->tentado); free(j);
  }
  return NULL;
}

// ---- display (main/GL thread) ----
static struct { GLuint tex; unsigned g; int k; unsigned uso; } texs[NTEX];
static unsigned usoTex;
static int velho;

static void texLimpar(void) {
  for (int i = 0; i < NTEX; i++) if (texs[i].tex) { glDeleteTextures(1, &texs[i].tex); texs[i].tex = 0; }
}
static GLuint texDe(Job *j, int k) {
  int livreI = 0;
  for (int i = 0; i < NTEX; i++) {
    if (texs[i].tex && texs[i].g == j->g && texs[i].k == k) { texs[i].uso = ++usoTex; return texs[i].tex; }
    if (!texs[i].tex || texs[i].uso < texs[livreI].uso) livreI = i;
  }
  if (!j->tiles[k].jpg) return 0;
  SDL_RWops *rw = SDL_RWFromConstMem(j->tiles[k].jpg, j->tiles[k].n);
  SDL_Surface *s = rw ? IMG_Load_RW(rw, 1) : NULL;
  SDL_Surface *c = s ? SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0) : NULL;
  if (s) SDL_FreeSurface(s);
  if (!c) return 0;
  if (texs[livreI].tex) glDeleteTextures(1, &texs[livreI].tex);
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
  SDL_FreeSurface(c);
  texs[livreI].tex = t; texs[livreI].g = j->g; texs[livreI].k = k; texs[livreI].uso = ++usoTex;
  return t;
}

static int pQuadros(double posSeg, int n, GLuint *out, double *cueSeg) {
  int achou = 0;
  for (int i = 0; i < n; i++) { out[i] = 0; if (cueSeg) cueSeg[i] = -1.0; }
  pthread_mutex_lock(&mu);
  Job *j = job;
  velho = 0;
  if (j && j->n > 0) {
    int k = (int)((posSeg * 1000.0 + GRADE_MS / 2) / GRADE_MS);
    if (k >= j->n) k = j->n - 1;
    if (k < 0) k = 0;
    int atual = -1;
    if (j->tiles[k].jpg) atual = k;
    else {
      for (int d = 1; d <= VIZINHO_SLOTS && atual < 0; d++) {
        if (k - d >= 0 && j->tiles[k - d].jpg) atual = k - d;
        else if (k + d < j->n && j->tiles[k + d].jpg) atual = k + d;
      }
      // a neighbour within 8 s IS this slot's thumbnail (no "still loading" ring); further away it stands in
      velho = atual >= 0 && labs(j->tiles[atual].pts - k * GRADE_MS) > TOL_COPIA_MS;
    }
    int alvos[3] = { k - 1, atual, k + 1 }, base = n == 1 ? 1 : 0;
    for (int i = 0; i < n; i++) {
      int s = alvos[base + i];
      if (s < 0 || s >= j->n || !j->tiles[s].jpg || (i != (n == 1 ? 0 : 1) && velho)) continue;
      out[i] = texDe(j, s);
      if (cueSeg && out[i]) cueSeg[i] = j->tiles[s].pts / 1000.0;
    }
    achou = out[n == 1 ? 0 : 1] != 0;
  }
  pthread_mutex_unlock(&mu);
  return achou;
}
static int pVelho(void) { return velho; }
static void pOcioso(void) { texLimpar(); }

static void encerrar(void) {
  pthread_mutex_lock(&mu);
  Job *j = job;
  job = NULL;
  estado = M_PARADO;
  urlAtual[0] = 0;
  geracao++;
  int vivo = j && j->vivo;
  if (j) j->parar = 1;
  pthread_mutex_unlock(&mu);
  texLimpar();
  if (j && !vivo) {
    for (int k = 0; k < j->n; k++) free(j->tiles[k].jpg);
    free(j->tiles); free(j->tentado); free(j);
  }
}

static void pPedir(const SeekrFonte *f) {
  posMs = f->posMs; playheadMs = f->playheadMs; scrubbing = f->scrubbing;
  if (avisoPronto && avisoPronto == geracao) {          // the playing title's set is complete
    avisoPronto = 0;
    prontoDesde = SDL_GetTicks();
  }
  bufferando = video_bufferando_ms() > 0;
  { static int bAntes, eAntes;
    int e = video_falhou();
    if (bufferando && !bAntes) evRebuffer++;
    if (e && !eAntes) evErro++;
    bAntes = bufferando; eAntes = e; }
  if (!f->url || strncmp(f->url, "http", 4) || f->durMs < 60000) return;
  if (!strcmp(urlAtual, f->url)) return;                       // the same title: the worker is on it
  encerrar();
  motivoSem = 0;
  if (!jniPreparar()) { estado = M_FALHOU; snprintf(urlAtual, sizeof urlAtual, "%s", f->url); return; }
  Job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->dur = f->durMs;
  j->n = (int)(f->durMs / GRADE_MS) + 1;
  j->tiles = calloc((size_t)j->n, sizeof *j->tiles);
  j->tentado = calloc((size_t)j->n, 1);
  if (!j->tiles || !j->tentado) { free(j->tiles); free(j->tentado); free(j); return; }
  snprintf(j->urlA, sizeof j->urlA, "%s", f->url);
  chaveArquivo(f, j->arq, sizeof j->arq);
  snprintf(j->cabA, sizeof j->cabA, "%s", f->cabecalhos ? f->cabecalhos : "");
  const Stream *st = stream_item(stream_atual());
  j->alturaA = st ? st->altura : 0;
  j->nCopias = st ? escolherCopias(st, j->copias, f->durMs, j->motivos, sizeof j->motivos) : 0;
  if (f->imdb && strchr(f->imdb, ':')) snprintf(j->titulo, sizeof j->titulo, "%s", f->imdb);
  else snprintf(j->titulo, sizeof j->titulo, "%s%s%d:%d", f->imdb ? f->imdb : "", f->t ? " " : "", f->t, f->e);
  pthread_mutex_lock(&mu);
  j->g = ++geracao;
  j->vivo = 1;
  job = j;
  estado = M_BUSCANDO;
  snprintf(urlAtual, sizeof urlAtual, "%s", f->url);
  pthread_mutex_unlock(&mu);
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, trabalhar, j)) { pthread_mutex_lock(&mu); j->vivo = 0; pthread_mutex_unlock(&mu); encerrar(); }
  pthread_attr_destroy(&at);
}

static int pTemChave(void) { return 1; }
static int pEstado(void) {
  int e;
  pthread_mutex_lock(&mu); e = estado; pthread_mutex_unlock(&mu);
  return e == M_PRONTO ? SEEKR_PRONTO : e == M_BUSCANDO ? SEEKR_BUSCANDO : e == M_FALHOU ? SEEKR_SEM_PREVIA : SEEKR_DESLIGADO;
}
static const char *pRotulo(int est) {
  switch (est) {
    case SEEKR_BUSCANDO: return "Fazendo miniaturas neste aparelho";
    case SEEKR_PRONTO: return "Miniaturas disponíveis";
    case SEEKR_SEM_PREVIA: return motivoSem ? "Sem cópia menor para fazer as miniaturas" : "Este aparelho não conseguiu ler este vídeo";
    default: return "Fazendo miniaturas neste aparelho";
  }
}
static void pUso(SeekrUso *u) { memset(u, 0, sizeof *u); u->persistente = 1; u->chavePropria = 1; }
static void pTentar(void) { encerrar(); }
static void pDesligar(void) { encerrar(); }

static const SeekrProvedor PROVEDOR = {
  pTemChave, pEstado, pRotulo, pUso, pTentar, pPedir, pDesligar, pQuadros, pVelho, pOcioso
};
const SeekrProvedor *miniatura_provedor(void) { return &PROVEDOR; }
int miniatura_prontas_agora(void) {
  if (!prontoDesde) return 0;
  if (SDL_GetTicks() - prontoDesde < 4000u) return 1;
  prontoDesde = 0;
  return 0;
}
#endif
