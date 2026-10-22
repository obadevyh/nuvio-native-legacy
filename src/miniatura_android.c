// SEEK-BAR THUMBNAILS MADE ON THIS DEVICE (Android, test): see miniatura_android.h.
#include "miniatura_android.h"

#ifndef NV_ANDROID
const SeekrProvedor *miniatura_provedor(void) { return NULL; }
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
// What the playing file showed at each check anchor: decoded ONCE per title and reused for every copy checked
// (the 4K decode is what the player feels; a second copy must not pay it again).
typedef struct { int estado; long pA; int ok; float sig[SIG_N]; } Ancora;   // estado: 0 not yet, 1 read, -1 failed


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
  Ancora anc[N_ANCORAS];
  char arq[640];             // the saved set on this device ("" = cannot save)
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
static jmethodID jAbrir, jInfo, jQuadro, jExato, jSoltar, jFechar, jLidos, jRitmo, jFalhaRede;

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
    jExato = (*env)->GetStaticMethodID(env, c, "quadroExato", "(JJ)[B");
    jSoltar = (*env)->GetStaticMethodID(env, c, "soltarDecoder", "(J)V");
    jFechar = (*env)->GetStaticMethodID(env, c, "fechar", "(J)V");
    jLidos = (*env)->GetStaticMethodID(env, c, "lidos", "(J)J");
    jRitmo = (*env)->GetStaticMethodID(env, c, "ritmo", "()J");
    jFalhaRede = (*env)->GetStaticMethodID(env, c, "falhaRede", "(J)I");
    if ((*env)->ExceptionCheck(env) || !jAbrir || !jInfo || !jQuadro || !jExato || !jSoltar || !jFechar || !jLidos || !jRitmo || !jFalhaRede) {
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
static int mExato(long long h, long tMs, long *pts, float sig[SIG_N], int *temSig) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, jExato, (jlong)h, (jlong)tMs);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
  return desempacotar(env, a, pts, sig, temSig, NULL, NULL);
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

static int parou(const Job *j) { return j->parar; }
static void dormir(const Job *j, int ms) { for (int t = 0; t < ms && !parou(j); t += 50) usleep(50000); }

// The player first: nothing is read while it rebuffers, reconnects or has failed. Measured 22/10 (Dune 2, 4K
// through a StremThru proxy): a burst of thumbnail reads on the playing link and the player's reconnects were
// refused together (ConnectException) until it gave up.
static void esperarPlayer(const Job *j) {
  int avisou = 0;
  while (!parou(j) && (bufferando || video_reconectando() || video_falhou())) {
    if (!avisou) { printf("[miniatura] waiting: the player is buffering/reconnecting\n"); fflush(stdout); avisou = 1; }
    dormir(j, 1000);
  }
}
// A failed frame: when a READ failed (the engine says so), back off 5 s, 10 s, 20 s and end the job on the 4th in a
// row (the link is refusing us). A frame that only would not DECODE is skipped at once; 8 of those in a row end it.
static __thread int falhasSeguidas, decodSeguidas;   // per lane
static int mFalhaRede(long long h);
static int falhou(const Job *j, int ok, long long h) {
  if (ok) { falhasSeguidas = 0; decodSeguidas = 0; return 0; }
  if (!mFalhaRede(h)) {
    if (++decodSeguidas >= 8) { printf("[miniatura] stopping: %d frames in a row would not decode\n", decodSeguidas); fflush(stdout); return 1; }
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

// ---- the timeline check (OpenSeek timeline.verify_alignment) ----
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
                   double *b2, double *erro, int *nref) {
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
      int lido = mExato(hB, t, &p, sb, &ok);
      if (falhou(j, lido, hB)) return 0;
      if (!lido || !ok) continue;
      double c = 0;
      for (int k = 0; k < SIG_N; k++) c += sa[k] * sb[k];
      cs[nc] = c; ps[nc] = p; nc++;
    }
    if (nc < 3) continue;
    int m = 0;
    for (int k = 1; k < nc; k++) if (cs[k] > cs[m]) m = k;
    if (cs[m] < MIN_CORR) continue;
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

static int conferir(Job *j, long long hA, long long hB, long durA, long durB, Ancora *anc, Mapa *mapa) {
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
    int casou = nc && melhor >= MIN_CORR && melhor - segundo >= MIN_MARGEM;
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
    if (refinar(j, hB, anc, in, ni, slopes[h], bh, durA, &b2, &erro, &nref) && fabs(b2 - bh) <= MAX_RESIDUO_MS && erro < melhorErro) {
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
// <data>/miniaturas/<fnv64 of the file identity>.nvm : "NVM1", n, dur, complete, then (k, pts, len, jpeg)...
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
  if (fread(mag, 1, 4, f) != 4 || memcmp(mag, "NVM1", 4) || !lerI32(f, &n) || !lerI32(f, &dur) || !lerI32(f, &completo) ||
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
  if (!j->arq[0] || !j->feitos) return;
  char dir[512], tmp[620];
  snprintf(dir, sizeof dir, "%s", j->arq);
  char *barra = strrchr(dir, '/');
  if (barra) { *barra = 0; mkdir(dir, 0700); }
  snprintf(tmp, sizeof tmp, "%s.tmp", j->arq);
  FILE *f = fopen(tmp, "wb");
  if (!f) return;
  fwrite("NVM1", 1, 4, f);
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

// One lane of thumbnail making (see trabalhar). Slots are claimed under the lock, so two lanes never make the same one.
typedef struct { Job *j; long long h; int usaB; Mapa mapa; int id; int feitos; unsigned long somaMs; long long lido; } Faixa;
static void *faixa(void *u) {
  Faixa *f = u;
  Job *j = f->j;
  long long lido0 = mLidos(f->h);
  falhasSeguidas = 0; decodSeguidas = 0;
  while (!parou(j)) {
    if (bufferando || video_reconectando() || video_falhou()) { mSoltar(f->h); esperarPlayer(j); continue; }
    long disp = memDisponivelKB();
    if (disp >= 0 && disp < PISO_KB) { mSoltar(f->h); dormir(j, 1500); continue; }
    pthread_mutex_lock(&mu);
    int k = proximo(j);
    if (k >= 0) j->tentado[k] = 1;
    pthread_mutex_unlock(&mu);
    if (k < 0) break;
    long alvo = k * GRADE_MS, p; int okS;
    float sig[SIG_N]; unsigned char *jpg = NULL; int nj = 0;
    Uint32 s = SDL_GetTicks();
    long tB = f->usaB ? (long)(f->mapa.a * alvo + f->mapa.b) : alvo;
    if (tB < 0) tB = 0;
    int ok = mQuadro(f->h, tB, 1, 1, &p, sig, &okS, &jpg, &nj);
    if (falhou(j, ok, f->h)) break;
    if (ok && f->usaB) p = (long)((p - f->mapa.b) / f->mapa.a);       // back on the playing file's timeline
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
    f->feitos++; f->somaMs += SDL_GetTicks() - s;
    f->lido = mLidos(f->h) - lido0;
    if (f->id == 1 && f->feitos % 100 == 0) salvar(j, 0);
    if (f->id == 1 && f->feitos % 30 == 0) {
      printf("[miniatura] %d/%d tiles from %s, lane avg %lu ms, lane read %.1f MB, pace %lld/s, free %ld MB\n", j->feitos, j->n,
             origem, f->somaMs / (unsigned long)f->feitos, f->lido / 1048576.0, mRitmo(), disp / 1024);
      fflush(stdout);
    }
    if (!f->usaB) usleep(20000);
  }
  f->lido = mLidos(f->h) - lido0;
  return NULL;
}

static void *trabalhar(void *u) {
  Job *j = u;
  long long hA = 0, hB = 0, info[5];
  Mapa mapa = {1.0, 0.0, 0.0, 0};
  int usaB = 0, copiaUsada = 0;
  Uint32 t0 = SDL_GetTicks();
  falhasSeguidas = 0; decodSeguidas = 0;
  if (j->arq[0]) limparCache(j->arq);           // other titles' sets older than 2 h go now
  if (carregar(j)) goto fim;                    // a complete saved set: no network at all
  esperarPlayer(j);
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
    int aceita = ib[0] > 0 && conferir(j, hA, hB, j->dur, (long)ib[0], j->anc, &mapa);
    long long lidosB = mLidos(hB);
    if (aceita) {
      usaB = 1;
      copiaUsada = c;
      snprintf(origem, sizeof origem, "%dp copy", j->copias[c].altura);
    } else { mFechar(hB); hB = 0; }
    printf("[miniatura] check took %.1f s, read play %.1f MB + copy %.1f MB\n", (SDL_GetTicks() - c0) / 1000.0,
           (mLidos(hA) - lA0) / 1048576.0, lidosB / 1048576.0);
    fflush(stdout);
  }
  if (!usaB) snprintf(origem, sizeof origem, "%dp (playing file)", j->alturaA);
  {
    // From the small copy: two lanes, each with its own reader and decoder, so a request is always in flight while
    // the other lane decodes (one request takes ~0.3 s through an add-on proxy; the pacing in MiniaturaMotor is
    // shared and adaptive). From the playing 4K file: one lane only (the decode is what the player feels).
    Faixa f1 = { j, usaB ? hB : hA, usaB, mapa, 1, 0, 0, 0 }, f2 = { j, 0, usaB, mapa, 2, 0, 0, 0 };
    pthread_t t2;
    int duas = 0;
    if (usaB && !parou(j)) {
      f2.h = mAbrir(j->copias[copiaUsada].url, j->copias[copiaUsada].cab);
      if (f2.h) duas = pthread_create(&t2, NULL, faixa, &f2) == 0;
      if (!duas) { mFechar(f2.h); f2.h = 0; }
    }
    faixa(&f1);
    if (duas) pthread_join(t2, NULL);
    mFechar(f2.h);
    pthread_mutex_lock(&mu);
    int completo = proximo(j) < 0;
    pthread_mutex_unlock(&mu);
    salvar(j, completo && !parou(j));
    printf("[miniatura] done: %d/%d tiles from %s in %.0f s (%d lane%s), read %.1f MB, pace now %lld/s\n", j->feitos, j->n,
           origem, (SDL_GetTicks() - t0) / 1000.0, duas ? 2 : 1, duas ? "s" : "", (f1.lido + f2.lido) / 1048576.0, mRitmo());
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
  chaveArquivo(f, j->arq, sizeof j->arq);
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
