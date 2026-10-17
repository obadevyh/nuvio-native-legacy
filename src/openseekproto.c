#include "openseekproto.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static int hostCar(char c) { return isalnum((unsigned char)c) || c == '.' || c == '-'; }

int openseek_normalizar_endereco(const char *in, char *base, size_t n) {
  char host[128];
  const char *p, *h, *hf;
  long porta = OS_PORTA_PADRAO;
  size_t nh;
  int v6 = 0;
  if (!base || n < 16) return 0;
  base[0] = 0;
  if (!in) return 0;
  while (*in == ' ' || *in == '\t') in++;
  p = in;
  if (!strncasecmp(p, "https://", 8)) p += 8;
  else if (strstr(p, "://")) return 0;           // http:// e o resto: nunca
  h = p;
  if (*h == '[') {                               // IPv6 entre colchetes
    hf = strchr(h, ']');
    if (!hf) return 0;
    for (const char *q = h + 1; q < hf; q++)
      if (!isxdigit((unsigned char)*q) && *q != ':' && *q != '.') return 0;
    hf++; v6 = 1;
  } else {
    hf = h;
    while (hostCar(*hf)) hf++;
  }
  nh = (size_t)(hf - h);
  if (nh == 0 || nh >= sizeof host || (v6 && nh < 4)) return 0;
  memcpy(host, h, nh); host[nh] = 0;
  if (!v6 && (host[0] == '-' || host[0] == '.' || host[nh - 1] == '-')) return 0;
  p = hf;
  if (*p == ':') {
    char *f;
    porta = strtol(p + 1, &f, 10);
    if (f == p + 1 || porta < 1 || porta > 65535) return 0;
    p = f;
  }
  if (*p == '/') p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p) return 0;                              // caminho, query, @, espaco...
  for (size_t i = 0; i < nh; i++) host[i] = (char)tolower((unsigned char)host[i]);
  return (size_t)snprintf(base, n, "https://%s:%ld", host, porta) < n;
}

int openseek_na_origem(const char *url, const char *base) {
  size_t k;
  if (!url || !base || !*base) return 0;
  k = strlen(base);
  return !strncmp(url, base, k) && url[k] == '/';
}

// Copia a URL sem os parametros `tira` (lista "t\0exp\0\0") e sem fragmento.
static int semParams(const char *url, char *dst, size_t n) {
  const char *q = strchr(url, '?'), *fim = url + strcspn(url, "#");
  size_t k;
  if (!q || q > fim) q = fim;
  k = (size_t)(q - url);
  if (k >= n) return 0;
  memcpy(dst, url, k);
  if (q < fim) {
    const char *i = q + 1;
    int primeiro = 1;
    while (i < fim) {
      const char *f = i;
      size_t len;
      while (f < fim && *f != '&') f++;
      len = (size_t)(f - i);
      if (!(len >= 2 && !strncmp(i, "t=", 2)) && !(len >= 4 && !strncmp(i, "exp=", 4)) && len) {
        if (k + len + 2 >= n) return 0;
        dst[k++] = primeiro ? '?' : '&';
        memcpy(dst + k, i, len); k += len; primeiro = 0;
      }
      i = f < fim ? f + 1 : f;
    }
  }
  if (k >= n) return 0;
  dst[k] = 0;
  return 1;
}
int openseek_chave(const char *url, char *dst, size_t n) {
  char *c;
  if (!url || !dst || !n) return 0;
  dst[0] = 0;
  if (!semParams(url, dst, n)) { dst[0] = 0; return 0; }
  // /v1/c/<cap>/: a capacidade depende do exp (CONTRACT-v1) e muda a cada
  // renovacao; o conteudo e o mesmo (preview_id + indice, imutavel).
  c = strstr(dst, "/v1/c/");
  if (c) {
    char *seg = c + 6, *fimSeg = strchr(seg, '/');
    if (fimSeg && fimSeg > seg) { *seg = '_'; memmove(seg + 1, fimSeg, strlen(fimSeg) + 1); }
  }
  return 1;
}

long long openseek_url_exp(const char *url) {
  const char *p;
  if (!url) return 0;
  p = strstr(url, "?exp=");
  if (!p) p = strstr(url, "&exp=");
  if (p) {
    char *fim;
    long long e = strtoll(p + 5, &fim, 10);
    if (fim > p + 5 && (*fim == 0 || *fim == '&' || *fim == '#') && e > 0) return e;
  }
  return 0;
}

int openseek_jpeg_dimensoes(const unsigned char *b, size_t n, int *w, int *h) {
  size_t i = 2;
  if (!b || n < 4 || b[0] != 0xFF || b[1] != 0xD8) return 0;
  while (i + 4 <= n) {
    unsigned m, len;
    if (b[i] != 0xFF) return 0;
    while (i < n && b[i] == 0xFF) i++;
    if (i >= n) return 0;
    m = b[i++];
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
    if (m == 0xD9 || m == 0xDA) return 0;        // sem SOF antes dos dados
    if (i + 2 > n) return 0;
    len = (unsigned)b[i] << 8 | b[i + 1];
    if (len < 2 || i + len > n) return 0;
    if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (len < 7) return 0;
      if (h) *h = (int)((unsigned)b[i + 3] << 8 | b[i + 4]);
      if (w) *w = (int)((unsigned)b[i + 5] << 8 | b[i + 6]);
      return 1;
    }
    i += len;
  }
  return 0;
}

// --- JSON --------------------------------------------------------------------
typedef struct { char *d; size_t n, k; int ok; } Buf;
static void bAdd(Buf *b, const char *s, size_t len) {
  if (!b->ok || len >= b->n - b->k) { b->ok = 0; return; }
  memcpy(b->d + b->k, s, len); b->k += len; b->d[b->k] = 0;
}
static void bTxt(Buf *b, const char *s) { bAdd(b, s, strlen(s)); }
static void bFmt(Buf *b, const char *fmt, long long v) {
  char t[32]; int k = snprintf(t, sizeof t, fmt, v); if (k > 0) bAdd(b, t, (size_t)k);
}
// Cadeia JSON com aspas. Controle -> \u00XX; o resto (UTF-8) passa.
static void bStr(Buf *b, const char *s, size_t len) {
  size_t i;
  bAdd(b, "\"", 1);
  for (i = 0; i < len && b->ok; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; bAdd(b, e, 2); }
    else if (c < 0x20 || c == 0x7f) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); bAdd(b, e, 6); }
    else bAdd(b, (const char *)&c, 1);
  }
  bAdd(b, "\"", 1);
}

static const char *CABS_OK[] = { "User-Agent", "Referer", "Origin" };

size_t openseek_json_previa(char *dst, size_t n, const OpenSeekFonte *f) {
  Buf b = { dst, n, 0, 1 };
  int primeiro = 1;
  if (!dst || n < 2 || !f || !f->url || strncmp(f->url, "http", 4)) return 0;
  dst[0] = 0;
  bTxt(&b, "{\"source\":{\"url\":");
  bStr(&b, f->url, strlen(f->url));
  bTxt(&b, ",\"headers\":{");
  // Allowlist (CONTRACT-v1): User-Agent, Referer, Origin. Um de cada.
  for (int k = 0; k < 3; k++) {
    const char *l = f->cabecalhos;
    size_t nome = strlen(CABS_OK[k]);
    while (l && *l) {
      const char *fim = l + strcspn(l, "\n");
      if ((size_t)(fim - l) > nome && !strncasecmp(l, CABS_OK[k], nome) && l[nome] == ':') {
        const char *v = l + nome + 1, *vf = fim;
        while (v < vf && (*v == ' ' || *v == '\t')) v++;
        while (vf > v && (vf[-1] == '\r' || vf[-1] == ' ' || vf[-1] == '\t')) vf--;
        if (vf > v) {
          if (!primeiro) bTxt(&b, ",");
          bStr(&b, CABS_OK[k], nome); bTxt(&b, ":"); bStr(&b, v, (size_t)(vf - v));
          primeiro = 0;
          break;
        }
      }
      l = *fim ? fim + 1 : fim;
    }
  }
  bTxt(&b, "}");
  if (f->durMs > 0) bFmt(&b, ",\"duration_ms\":%lld", f->durMs);
  bTxt(&b, "},\"hints\":{");
  {
    const char *im = f->imdb && !strncmp(f->imdb, "tt", 2) ? f->imdb : "";
    bTxt(&b, "\"imdb_id\":"); bStr(&b, im, strcspn(im, ":"));
  }
  bFmt(&b, ",\"season\":%lld", f->temporada > 0 ? f->temporada : 0);
  bFmt(&b, ",\"episode\":%lld", f->episodio > 0 ? f->episodio : 0);
  bFmt(&b, ",\"video_size\":%lld", (long long)(f->tamanho > 0x7fffffffffffffffULL ? 0 : f->tamanho));
  bTxt(&b, ",\"video_hash\":"); bStr(&b, f->videoHash ? f->videoHash : "", f->videoHash ? strlen(f->videoHash) : 0);
  bTxt(&b, ",\"filename\":"); bStr(&b, f->arquivo ? f->arquivo : "", f->arquivo ? strlen(f->arquivo) : 0);
  bTxt(&b, "},\"at_ms\":");
  if (f->atMs >= 0) bFmt(&b, "%lld", f->atMs); else bTxt(&b, "null");
  bFmt(&b, ",\"playhead_ms\":%lld", f->playheadMs > 0 ? f->playheadMs : 0);
  bTxt(&b, ",\"layout\":\"hybrid\"}");
  if (!b.ok || b.k > 16384) { dst[0] = 0; return 0; }   // corpo <= 16 KiB
  return b.k;
}

size_t openseek_json_prioridade(char *dst, size_t n, long atMs, long playheadMs) {
  Buf b = { dst, n, 0, 1 };
  if (!dst || n < 2) return 0;
  dst[0] = 0;
  bTxt(&b, "{\"at_ms\":");
  if (atMs >= 0) bFmt(&b, "%lld", atMs); else bTxt(&b, "null");
  bFmt(&b, ",\"playhead_ms\":%lld}", playheadMs > 0 ? playheadMs : 0);
  return b.ok ? b.k : 0;
}

size_t openseek_json_claim(char *dst, size_t n, const char *pairId,
                           const char *device, const char *prova) {
  Buf b = { dst, n, 0, 1 };
  if (!dst || n < 2 || !pairId || !device || !prova) return 0;
  dst[0] = 0;
  bTxt(&b, "{\"pair_id\":"); bStr(&b, pairId, strlen(pairId));
  bTxt(&b, ",\"device_name\":"); bStr(&b, device, strlen(device));
  bTxt(&b, ",\"client_proof\":"); bStr(&b, prova, strlen(prova));
  bTxt(&b, "}");
  return b.ok ? b.k : 0;
}

static int soHex(const char *s, size_t n) {
  size_t i;
  if (strlen(s) != n) return 0;
  for (i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return 0;
  return 1;
}

int openseek_ler_start(const char *json, OpenSeekStart *s) {
  double it;
  if (!json || !s) return 0;
  memset(s, 0, sizeof *s);
  if (!js_texto_raiz(json, "pair_id", s->pairId, sizeof s->pairId) || !soHex(s->pairId, 32) ||
      !js_texto_raiz(json, "salt", s->salt, sizeof s->salt) || !soHex(s->salt, 32) ||
      !js_texto_raiz(json, "spki_pin", s->pino, sizeof s->pino) || !rede_pino_valido(s->pino) ||
      strchr(s->pino, ';')) { memset(s, 0, sizeof *s); return 0; }
  it = js_num(json, NULL, "iterations", -1);
  if (it < 1 || it > 1e9) { memset(s, 0, sizeof *s); return 0; }
  s->iter = (long)it;
  return 1;
}

int openseek_ler_claim(const char *json, OpenSeekClaim *c) {
  size_t i, n;
  if (!json || !c) return 0;
  memset(c, 0, sizeof *c);
  if (!js_texto_raiz(json, "device_id", c->deviceId, sizeof c->deviceId) || !soHex(c->deviceId, 16) ||
      !js_texto_raiz(json, "token", c->token, sizeof c->token) ||
      !js_texto_raiz(json, "server_proof", c->prova, sizeof c->prova) || !soHex(c->prova, 64)) {
    memset(c, 0, sizeof *c); return 0;
  }
  n = strlen(c->token);
  if (n < 32 || n > 60) { memset(c, 0, sizeof *c); return 0; }
  for (i = 0; i < n; i++)
    if (!isalnum((unsigned char)c->token[i]) && c->token[i] != '-' && c->token[i] != '_') {
      memset(c, 0, sizeof *c); return 0;
    }
  return 1;
}

static int estadoDe(const char *s) {
  if (!strcmp(s, "preparing")) return OS_PREPARANDO;
  if (!strcmp(s, "ready")) return OS_PRONTO;
  if (!strcmp(s, "complete")) return OS_COMPLETO;
  if (!strcmp(s, "failed")) return OS_FALHOU;
  return 0;
}

int openseek_ler_previa(const char *json, const char *base, OpenSeekPrevia *p) {
  char st[24] = "";
  double v;
  if (!json || !p || !base) return 0;
  memset(p, 0, sizeof *p);
  if (!js_texto_raiz(json, "preview_id", p->id, sizeof p->id) || !soHex(p->id, 32) ||
      !js_texto_raiz(json, "state", st, sizeof st) || !(p->estado = estadoDe(st))) {
    memset(p, 0, sizeof *p); return 0;
  }
  js_texto_raiz(json, "error", p->erro, sizeof p->erro);
  // vtt_url absoluta e NA ORIGEM PAREADA: o pino so cobre ela (NUVIO-REVIEW 4).
  if (!seekr_ler_lookup_base(json, base, p->vtt, sizeof p->vtt) || !openseek_na_origem(p->vtt, base))
    p->vtt[0] = 0;
  if (!p->vtt[0] && p->estado != OS_FALHOU) { memset(p, 0, sizeof *p); return 0; }
  p->exp = openseek_url_exp(p->vtt);
  v = js_num(json, NULL, "vtt_version", 0);
  p->versao = v >= 0 && v < 2e9 ? (long)v : 0;
  v = js_num(json, NULL, "refresh_after_ms", 1000);
  p->refreshMs = v >= 100 && v <= 600000 ? (long)v : 1000;
  return 1;
}

int openseek_ler_versao(const char *json, long *versao, int *estado) {
  char st[24] = "";
  double v;
  if (!json || !js_tem(json, NULL, "vtt_version")) return 0;
  v = js_num(json, NULL, "vtt_version", -1);
  if (v < 0 || v >= 2e9) return 0;
  if (versao) *versao = (long)v;
  if (estado) *estado = js_texto_raiz(json, "state", st, sizeof st) ? estadoDe(st) : 0;
  return 1;
}

int openseek_mapear(int http, int redeErro, int curlErro, const OpenSeekPrevia *p,
                    int nCues, int *motivo) {
  int m = OSM_NENHUM, e;
  if (redeErro == REDE_CANCELADO || redeErro == REDE_GERACAO) return -1;
  if (redeErro == REDE_INDISPONIVEL || redeErro == REDE_ENTRADA) {
    // Sem libcurl ou sem CURLOPT_PINNEDPUBLICKEY neste aparelho: nada sai.
    m = OSM_SEM_PINO; e = OS_SEEKR_REDE_INDISPONIVEL;
  } else if (redeErro != REDE_OK && curlErro == REDE_CURL_PINO_DIFERENTE) {
    m = OSM_PINO_MUDOU; e = OS_SEEKR_REDE_INDISPONIVEL;
  } else if (redeErro != REDE_OK || http == 0) {
    m = OSM_REDE; e = OS_SEEKR_REDE_INDISPONIVEL;
  } else if (http == 401) {
    m = OSM_RECUSADO; e = OS_SEEKR_CHAVE_RECUSADA;
  } else if (http == 403) {
    m = OSM_EXPIRADO; e = OS_SEEKR_REDE_INDISPONIVEL;   // capacidade vencida: renovar
  } else if (http == 429 || http == 503) {
    m = OSM_OCUPADO; e = OS_SEEKR_LIMITE_PROVEDOR;
  } else if (http == 400 || http == 413 || http == 404) {
    m = OSM_FONTE; e = OS_SEEKR_SEM_PREVIA;
  } else if (http >= 500 || http < 200 || http >= 300) {
    m = OSM_REDE; e = OS_SEEKR_REDE_INDISPONIVEL;
  } else if (!p) {
    m = OSM_REDE; e = OS_SEEKR_REDE_INDISPONIVEL;          // 2xx ilegivel
  } else if (p->estado == OS_FALHOU) {
    m = OSM_FONTE; e = OS_SEEKR_SEM_PREVIA;
  } else if (p->estado == OS_PREPARANDO && nCues <= 0) {
    // VTT vazio enquanto prepara = carregando, nunca "sem previa".
    m = OSM_PREPARANDO; e = OS_SEEKR_BUSCANDO;
  } else e = OS_SEEKR_PRONTO;
  if (motivo) *motivo = m;
  return e;
}

int openseek_pedido(OpenSeekPedido *o, const char *metodo, const char *url,
                    const char *token, const char *pino, int semPino,
                    const void *corpo, size_t nCorpo, unsigned prazoMs) {
  int k = 0;
  if (!o) return 0;
  memset(o, 0, sizeof *o);
  if (!url || strncmp(url, "https://", 8) || strlen(url) >= sizeof o->url) return 0;
  // SR-3: sem pino so o primeiro contato, que nao leva segredo algum.
  if (pino && *pino) { if (!rede_pino_valido(pino)) return 0; }
  else if (!semPino || (token && *token) || (corpo && nCorpo > 2)) return 0;
  // SR-4: nada de URL de video em query/caminho; o unico caminho com query e
  // o long-poll de status, que so leva numeros.
  strcpy(o->url, url);
  if (token && *token) {
    if (strlen(token) > 60 || strpbrk(token, "\r\n ")) return 0;
    snprintf(o->auth, sizeof o->auth, "Authorization: Bearer %s", token);
    o->cabs[k++] = o->auth;
  }
  if (corpo && nCorpo) o->cabs[k++] = "Content-Type: application/json";
  o->cabs[k] = NULL;
  o->p.metodo = metodo;
  o->p.url = o->url;
  o->p.cabecalhos = k ? o->cabs : NULL;
  o->p.corpo = corpo;
  o->p.n_corpo = nCorpo;
  o->p.prazo_ms = prazoMs ? prazoMs : OS_PRAZO_API_MS;
  o->p.max_bytes = OS_TILE_BYTES_MAX;
  o->p.seguir = 0;
  o->p.tls_pessoal = 1;
  o->p.pino_publico = pino && *pino ? pino : NULL;
  return 1;
}

// --- LRU -----------------------------------------------------------------------
void openseek_lru_ini(OsLru *l, int n) {
  memset(l, 0, sizeof *l);
  l->n = n < 1 ? 1 : n > OS_LRU_MAX ? OS_LRU_MAX : n;
}
int openseek_lru_achar(OsLru *l, const char *chave) {
  int i;
  if (!chave || !*chave) return -1;
  for (i = 0; i < l->n; i++)
    if (l->it[i].ocupado && !strcmp(l->it[i].chave, chave)) { l->it[i].uso = ++l->relogio; return i; }
  return -1;
}
int openseek_lru_lugar(OsLru *l, const char *chave, const char *const *prot, int np) {
  int i, k, melhor = -1;
  if (!chave || strlen(chave) >= sizeof l->it[0].chave) return -1;
  i = openseek_lru_achar(l, chave);
  if (i >= 0) return i;
  for (i = 0; i < l->n; i++) {
    int protegido = 0;
    if (!l->it[i].ocupado) { melhor = i; break; }
    for (k = 0; k < np; k++) if (prot[k] && !strcmp(prot[k], l->it[i].chave)) protegido = 1;
    if (protegido) continue;
    if (melhor < 0 || l->it[i].uso < l->it[melhor].uso) melhor = i;
  }
  if (melhor < 0) melhor = 0;
  snprintf(l->it[melhor].chave, sizeof l->it[melhor].chave, "%s", chave);
  l->it[melhor].ocupado = 1;
  l->it[melhor].uso = ++l->relogio;
  return melhor;
}
