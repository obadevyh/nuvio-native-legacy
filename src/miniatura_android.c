// SEEK-BAR THUMBNAILS MADE ON THIS DEVICE (Android, test): see miniatura_android.h.
#include "miniatura_android.h"

#ifndef NV_ANDROID
const SeekrProvedor *miniatura_provedor(void) { return NULL; }
#else
#include "streams.h"
#include "video.h"
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
#define TENTATIVAS 2                 // smaller copies tried per title

enum { M_PARADO = 0, M_BUSCANDO, M_PRONTO, M_FALHOU };

typedef struct { unsigned char *jpg; int n; long pts; } Tile;

typedef struct {
  char url[4096], cab[512];
  int altura;
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
  int feitos;
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

// ---- JNI (MiniaturaMotor.kt) ----
static jclass cls;
static jmethodID jAbrir, jInfo, jQuadro, jSoltar, jFechar, jLidos;

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
    jAbrir = (*env)->GetStaticMethodID(env, c, "abrir", "(Ljava/lang/String;Ljava/lang/String;)J");
    jInfo = (*env)->GetStaticMethodID(env, c, "info", "(J)[J");
    jQuadro = (*env)->GetStaticMethodID(env, c, "quadro", "(JJZZ)[B");
    jSoltar = (*env)->GetStaticMethodID(env, c, "soltarDecoder", "(J)V");
    jFechar = (*env)->GetStaticMethodID(env, c, "fechar", "(J)V");
    jLidos = (*env)->GetStaticMethodID(env, c, "lidos", "(J)J");
    if ((*env)->ExceptionCheck(env) || !jAbrir || !jInfo || !jQuadro || !jSoltar || !jFechar || !jLidos) {
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

static long long mAbrir(const char *url, const char *cab) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jstring u = (*env)->NewStringUTF(env, url), c = (*env)->NewStringUTF(env, cab ? cab : "");
  jlong h = (*env)->CallStaticLongMethod(env, cls, jAbrir, u, c);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); h = 0; }
  (*env)->DeleteLocalRef(env, u); (*env)->DeleteLocalRef(env, c);
  return h;
}
static void mInfo(long long h, long long out[5]) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  memset(out, 0, 5 * sizeof *out);
  jlongArray a = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, jInfo, (jlong)h);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
  if (a) { (*env)->GetLongArrayRegion(env, a, 0, 5, (jlong *)out); (*env)->DeleteLocalRef(env, a); }
}
// One keyframe: pts (ms), signature (unit-norm, mean-removed; 0 when flat) and optionally the JPEG (malloc'd).
static int mQuadro(long long h, long tMs, int closest, int querJpeg, long *pts, float sig[SIG_N], int *temSig,
                   unsigned char **jpg, int *nJpg) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, jQuadro, (jlong)h, (jlong)tMs,
                                                            closest ? JNI_TRUE : JNI_FALSE, querJpeg ? JNI_TRUE : JNI_FALSE);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
  if (!a) return 0;
  jsize n = (*env)->GetArrayLength(env, a);
  unsigned char *b = n > CAB + SIG_N ? malloc((size_t)n) : NULL;
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

static long memDisponivelKB(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  char l[128]; long v = -1;
  if (!f) return -1;
  while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %ld kB", &v) == 1) break;
  fclose(f);
  return v;
}

static int parou(const Job *j) { return j->parar; }
static void dormir(const Job *j, int ms) { for (int t = 0; t < ms && !parou(j); t += 50) usleep(50000); }

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
static int escolherCopias(const Stream *play, Copia *out) {
  int idx[TENTATIVAS], n = 0;
  unsigned edA = edicoes(play);
  for (int i = 0; i < stream_n(); i++) {
    const Stream *c = stream_item(i);
    if (!c || i == stream_atual() || c->naoVideo || !c->url[0]) continue;
    if (!(c->emCache || c->cacheNativo)) continue;                       // never start a download for thumbnails
    if (c->altura < 480 || c->altura > 1080 || (play->altura > 0 && c->altura >= play->altura)) continue;
    if (edicoes(c) != edA) continue;
    if (!strcmp(c->url, play->url)) continue;
    // keep the best TENTATIVAS: closest to 720p, then smallest
    int pos = n;
    for (int k = 0; k < n; k++) {
      const Stream *o = stream_item(idx[k]);
      int dc = abs(c->altura - 720), dk = abs(o->altura - 720);
      if (dc < dk || (dc == dk && tamanho(c) < tamanho(o))) { pos = k; break; }
    }
    if (pos >= TENTATIVAS) continue;
    if (n < TENTATIVAS) n++;
    for (int k = n - 1; k > pos; k--) idx[k] = idx[k - 1];
    idx[pos] = i;
  }
  for (int k = 0; k < n; k++) {
    const Stream *c = stream_item(idx[k]);
    snprintf(out[k].url, sizeof out[k].url, "%s", c->url);
    snprintf(out[k].cab, sizeof out[k].cab, "%s", c->cabecalhos);
    out[k].altura = c->altura;
    snprintf(out[k].nome, sizeof out[k].nome, "%s", c->arquivo[0] ? c->arquivo : c->rotulo);
  }
  return n;
}

// ---- the timeline check (OpenSeek timeline.verify_alignment, without the fine refine pass) ----
typedef struct { double a, b, erro; int inliers; } Mapa;
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

static int conferir(Job *j, long long hA, long long hB, long durA, long durB, Mapa *mapa) {
  double razao = (double)durB / (double)durA;
  Par matches[N_ANCORAS];
  int nm = 0, chamadasB = 0;
  float sa[SIG_N], sb[SIG_N];
  if (fabs(razao - 1.0) > MAX_RAZAO_ERRO) { printf("[miniatura] check: runtime differs %.1f%%\n", (razao - 1) * 100); return 0; }
  for (int v = 0; v < N_ANCORAS && !parou(j); v++) {
    int i = (v * 7) % N_ANCORAS;
    double frac = (i + 0.2 + 0.6 * fmod(i * 0.6180339887, 1.0)) / N_ANCORAS;
    long tA = (long)(durA * frac);
    tA = (tA + GRADE_MS / 2) / GRADE_MS * GRADE_MS;
    long pA; int okA;
    unsigned char *jpg = NULL; int nj = 0;
    if (!mQuadro(hA, tA, 1, 1, &pA, sa, &okA, &jpg, &nj)) continue;
    // the anchor is also a finished tile when it lands on its slot
    { int k = (int)((pA + GRADE_MS / 2) / GRADE_MS);
      pthread_mutex_lock(&mu);
      if (jpg && k >= 0 && k < j->n && labs(pA - k * GRADE_MS) <= TOL_MS && !j->tiles[k].jpg) {
        j->tiles[k].jpg = jpg; j->tiles[k].n = nj; j->tiles[k].pts = pA; j->tentado[k] = 1;
        j->feitos++; estado = M_PRONTO; jpg = NULL;
      }
      pthread_mutex_unlock(&mu); }
    free(jpg);
    if (!okA) continue;                                      // flat frame: no information
    double a, b, centro;
    long janela, passo;
    if (hipotese(matches, nm, razao, &a, &b)) { centro = a * pA + b; janela = JANELA_ESTREITA_MS; passo = PASSO_ESTREITO_MS; }
    else { centro = razao * pA; janela = JANELA_LARGA_MS; passo = PASSO_LARGO_MS; }
    double melhor = -2, pMelhor = 0;
    double cs[64], ps[64]; int nc = 0;
    for (long t = (long)centro - janela; t <= (long)centro + janela && nc < 64 && !parou(j); t += passo) {
      long tb = t < 0 ? 0 : t >= durB ? durB - 1 : t, pB; int okB;
      if (!mQuadro(hB, tb, 1, 0, &pB, sb, &okB, NULL, NULL)) continue;
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
    if (nc && melhor >= MIN_CORR && melhor - segundo >= MIN_MARGEM) { matches[nm].pa = pA; matches[nm].pb = pMelhor; nm++; }
    printf("[miniatura] check anchor %d/%d at %lds: best r=%.2f next r=%.2f %s\n", v + 1, N_ANCORAS, pA / 1000, melhor,
           segundo, nc && melhor >= MIN_CORR && melhor - segundo >= MIN_MARGEM ? "match" : "no");
    fflush(stdout);
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
  double res = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, s2 = 0;
  for (int k = 0; k < ni; k++) {
    double r = in[k].pb - (a * in[k].pa + b);
    if (fabs(r) > res) res = fabs(r);
    sx += in[k].pa; sy += r; sxx += in[k].pa * in[k].pa; sxy += in[k].pa * r; s2 += r * r;
  }
  if (res > MAX_RESIDUO_MS || fabs(a - 1.0) > MAX_RAZAO_ERRO) { printf("[miniatura] check: rejected (residual %.0f ms)\n", res); return 0; }
  double media = sy / ni, sigma = sqrt(fmax(0, s2 / ni - media * media));
  double den = ni * sxx - sx * sx, tend = den > 0 ? fabs((ni * sxy - sx * sy) / den) * durA / 2 : 0;
  double erro = 3 * sigma / sqrt(ni) + tend + 0.5 * 4000.0 + 250.0;
  if (erro > MAX_ERRO_MS) { printf("[miniatura] check: mapping uncertain (%.0f ms)\n", erro); return 0; }
  mapa->a = a; mapa->b = b; mapa->erro = erro; mapa->inliers = ni;
  printf("[miniatura] check: ACCEPTED t_copy = %.5f x t_play %+.1fs (%d anchors, +-%.1fs, %d copy reads)\n", a, b / 1000, ni,
         erro / 1000, chamadasB);
  fflush(stdout);
  return 1;
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

static void *trabalhar(void *u) {
  Job *j = u;
  long long hA = 0, hB = 0, info[5];
  Mapa mapa = {1.0, 0.0, 0.0, 0};
  int usaB = 0;
  Uint32 t0 = SDL_GetTicks();
  hA = mAbrir(j->urlA, j->cabA);
  if (!hA) {
    printf("[miniatura] could not open the playing source\n"); fflush(stdout);
    pthread_mutex_lock(&mu); if (job == j) estado = M_FALHOU; pthread_mutex_unlock(&mu);
    goto fim;
  }
  mInfo(hA, info);
  if (info[0] > 0) j->dur = (long)info[0];
  printf("[miniatura] playing %lldx%lld hdr=%lld %.1f min; %d smaller cop%s in the list\n", info[1], info[2], info[3],
         j->dur / 60000.0, j->nCopias, j->nCopias == 1 ? "y" : "ies");
  fflush(stdout);
  for (int c = 0; c < j->nCopias && !usaB && !parou(j); c++) {
    hB = mAbrir(j->copias[c].url, j->copias[c].cab);
    if (!hB) { printf("[miniatura] copy %d (%dp) would not open\n", c + 1, j->copias[c].altura); continue; }
    long long ib[5];
    mInfo(hB, ib);
    printf("[miniatura] checking copy %d: %lldx%lld %.1f min\n", c + 1, ib[1], ib[2], ib[0] / 60000.0); fflush(stdout);
    Uint32 c0 = SDL_GetTicks();
    long long lA0 = mLidos(hA);
    if (ib[0] > 0 && conferir(j, hA, hB, j->dur, (long)ib[0], &mapa)) {
      usaB = 1;
      snprintf(origem, sizeof origem, "%dp copy", j->copias[c].altura);
    } else { mFechar(hB); hB = 0; }
    printf("[miniatura] check took %.1f s, read play %.1f MB + copy %.1f MB\n", (SDL_GetTicks() - c0) / 1000.0,
           (mLidos(hA) - lA0) / 1048576.0, hB ? mLidos(hB) / 1048576.0 : 0.0);
    fflush(stdout);
  }
  if (!usaB) snprintf(origem, sizeof origem, "%dp (playing file)", j->alturaA);
  {
    long long hT = usaB ? hB : hA, lido0 = mLidos(hT);
    int feitosAqui = 0; unsigned long somaMs = 0;
    while (!parou(j)) {
      if (bufferando) { mSoltar(hT); dormir(j, 1000); continue; }       // the player comes first
      long disp = memDisponivelKB();
      if (disp >= 0 && disp < PISO_KB) { mSoltar(hT); dormir(j, 1500); continue; }
      pthread_mutex_lock(&mu);
      int k = proximo(j);
      if (k >= 0) j->tentado[k] = 1;
      pthread_mutex_unlock(&mu);
      if (k < 0) break;
      long alvo = k * GRADE_MS, p; int okS;
      float sig[SIG_N]; unsigned char *jpg = NULL; int nj = 0;
      Uint32 s = SDL_GetTicks();
      long tB = usaB ? (long)(mapa.a * alvo + mapa.b) : alvo;
      if (tB < 0) tB = 0;
      int ok = mQuadro(hT, tB, 1, 1, &p, sig, &okS, &jpg, &nj);
      if (ok && usaB) p = (long)((p - mapa.b) / mapa.a);              // back on the playing file's timeline
      if (ok && jpg && labs(p - alvo) <= TOL_MS) {
        int dest = (int)((p + GRADE_MS / 2) / GRADE_MS);
        pthread_mutex_lock(&mu);
        if (dest < 0 || dest >= j->n) dest = k;
        if (!j->tiles[dest].jpg) { j->tiles[dest].jpg = jpg; j->tiles[dest].n = nj; j->tiles[dest].pts = p; jpg = NULL; j->feitos++; }
        j->tentado[dest] = 1;
        if (job == j) estado = M_PRONTO;
        pthread_mutex_unlock(&mu);
      }
      free(jpg);
      feitosAqui++; somaMs += SDL_GetTicks() - s;
      if (feitosAqui % 30 == 0) {
        printf("[miniatura] %d/%d tiles from %s, avg %lu ms, read %.1f MB, free %ld MB\n", j->feitos, j->n, origem,
               somaMs / (unsigned long)feitosAqui, (mLidos(hT) - lido0) / 1048576.0, disp / 1024);
        fflush(stdout);
      }
      usleep(20000);
    }
    printf("[miniatura] done: %d/%d tiles from %s in %.0f s, read %.1f MB\n", j->feitos, j->n, origem,
           (SDL_GetTicks() - t0) / 1000.0, (mLidos(hT) - lido0) / 1048576.0);
    fflush(stdout);
  }
fim:
  mFechar(hB);
  mFechar(hA);
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
      velho = atual >= 0;
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
  bufferando = video_bufferando_ms() > 0;
  if (!f->url || strncmp(f->url, "http", 4) || f->durMs < 60000) return;
  if (!strcmp(urlAtual, f->url)) return;                       // the same title: the worker is on it
  encerrar();
  if (!jniPreparar()) { estado = M_FALHOU; snprintf(urlAtual, sizeof urlAtual, "%s", f->url); return; }
  Job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->dur = f->durMs;
  j->n = (int)(f->durMs / GRADE_MS) + 1;
  j->tiles = calloc((size_t)j->n, sizeof *j->tiles);
  j->tentado = calloc((size_t)j->n, 1);
  if (!j->tiles || !j->tentado) { free(j->tiles); free(j->tentado); free(j); return; }
  snprintf(j->urlA, sizeof j->urlA, "%s", f->url);
  snprintf(j->cabA, sizeof j->cabA, "%s", f->cabecalhos ? f->cabecalhos : "");
  const Stream *st = stream_item(stream_atual());
  j->alturaA = st ? st->altura : 0;
  j->nCopias = st ? escolherCopias(st, j->copias) : 0;
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
    case SEEKR_SEM_PREVIA: return "Este aparelho não conseguiu ler este vídeo";
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
#endif
