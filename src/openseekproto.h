// OPENSEEK (servidor proprio de miniaturas): a parte PURA do cliente. Sem rede,
// sem SDL, sem GL, sem fios — para os testes rodarem com `cc` sozinhos
// (tests/openseek*.sh). O contrato e docs/api/CONTRACT-v1.md do OpenSeek;
// o comportamento na tela, docs/design/SCRUBBING.md. O resto mora em openseek.c.
//
// SEGURANCA (SR-3/SR-4): a URL do video e os cabecalhos so saem no CORPO JSON
// de um POST com TLS fixado (pino). Nada daqui imprime coisa alguma; quem chama
// tambem nao pode imprimir URL, token, pino, codigo nem URL de capacidade.
#ifndef NV_OPENSEEKPROTO_H
#define NV_OPENSEEKPROTO_H
#include <stddef.h>
#include "seekrvtt.h"
#include "rede.h"

#define OS_PORTA_PADRAO 8787
#define OS_BASE_MAX 200
#define OS_TOKEN_MAX 64
#define OS_PINO_MAX 64            // "sha256//" + 44
#define OS_TILE_BYTES_MAX (4u * 1024u * 1024u)
#define OS_TILE_LADO_MAX 3200
#define OS_TILE_ALTO_MAX 1800
#define OS_PRAZO_API_MS 5000u     // status / prioridade / criar / parear
#define OS_PRAZO_DADOS_MS 10000u  // VTT, quadros, folhas

// Os mesmos numeros de SEEKR_* (seekr.h); openseek.c confere com _Static_assert.
enum {
  OS_SEEKR_DESLIGADO = 0, OS_SEEKR_BUSCANDO, OS_SEEKR_PRONTO, OS_SEEKR_SEM_PREVIA,
  OS_SEEKR_CHAVE_RECUSADA, OS_SEEKR_LIMITE_LOCAL, OS_SEEKR_LIMITE_PROVEDOR,
  OS_SEEKR_REDE_INDISPONIVEL
};
// Por que (so para o rotulo; o estado de cima decide o desenho).
enum {
  OSM_NENHUM = 0, OSM_REDE, OSM_PINO_MUDOU, OSM_SEM_PINO, OSM_RECUSADO,
  OSM_OCUPADO, OSM_FONTE, OSM_EXPIRADO, OSM_PREPARANDO, OSM_NAO_PAREADO
};
enum { OS_PREPARANDO = 1, OS_PRONTO, OS_COMPLETO, OS_FALHOU };

// "nas.local", "192.168.1.5:8787", "https://nas:8787/", "[fe80::1]:9000" ->
// "https://host:porta" (porta padrao 8787). http://, usuario@, caminho, query
// ou caractere fora de [A-Za-z0-9.-] no host: 0.
int  openseek_normalizar_endereco(const char *entrada, char *base, size_t n);
// 1 quando `url` e "<base>/..." (mesmo esquema, host e porta).
int  openseek_na_origem(const char *url, const char *base);
// Chave de cache de quadro/folha: a URL sem t=, sem exp= e sem fragmento. A
// mesma imagem com capacidade renovada (exp novo) ou pts diferente (t) tem a
// mesma chave: a troca de VTT nao perde texturas (SCRUBBING §4).
int  openseek_chave(const char *url, char *dst, size_t n);
// exp= (Unix s) da URL, 0 sem.
long long openseek_url_exp(const char *url);
// Largura/altura do SOF de um JPEG, ANTES de decodificar. 1 se achou.
int  openseek_jpeg_dimensoes(const unsigned char *b, size_t n, int *w, int *h);

typedef struct {
  const char *url;          // SEGREDO: a URL do video (video_url_atual)
  const char *cabecalhos;   // "Nome: valor\n" — so User-Agent/Referer/Origin saem
  long durMs;
  const char *imdb; int temporada, episodio;
  unsigned long long tamanho;
  const char *videoHash, *arquivo;
  long playheadMs, atMs;    // atMs < 0 = null
} OpenSeekFonte;
// Corpos JSON. Devolvem o tamanho (0 = nao coube ou entrada invalida).
size_t openseek_json_previa(char *dst, size_t n, const OpenSeekFonte *f);
size_t openseek_json_prioridade(char *dst, size_t n, long atMs, long playheadMs);
size_t openseek_json_claim(char *dst, size_t n, const char *pairId,
                           const char *device, const char *prova);

typedef struct { char pairId[33], salt[33], pino[OS_PINO_MAX]; long iter; } OpenSeekStart;
// pair_id/salt 32 hex, spki_pin bem formado (rede_pino_valido), iterations.
int  openseek_ler_start(const char *json, OpenSeekStart *s);
typedef struct { char deviceId[17], token[OS_TOKEN_MAX], prova[65]; } OpenSeekClaim;
int  openseek_ler_claim(const char *json, OpenSeekClaim *c);

typedef struct {
  char id[33];
  int  estado;              // OS_PREPARANDO..OS_FALHOU
  char erro[40];
  char vtt[2300];           // so na origem pareada
  long versao, refreshMs;
  long long exp;            // do vtt_url
} OpenSeekPrevia;
// Resposta 200/202 do POST /v1/previews ou do GET /v1/previews/{id}.
int  openseek_ler_previa(const char *json, const char *base, OpenSeekPrevia *p);
// {"vtt_version":N,"state":"..."} do /priority. 1 se leu a versao.
int  openseek_ler_versao(const char *json, long *versao, int *estado);

// Resultado de um pedido de API -> estado SEEKR_* (OS_SEEKR_*) e motivo.
// -1 quando nao muda nada (cancelado/obsoleto). `p` so com 200/202 lido.
int  openseek_mapear(int http, int redeErro, int curlErro, const OpenSeekPrevia *p,
                     int nCues, int *motivo);

// --- Pedido de rede ----------------------------------------------------------
// Monta TODO pedido do OpenSeek num lugar so (testado): https, pino
// obrigatorio (exceto o primeiro contato /v1/pair/start, sem segredo nenhum),
// seguir = 0, tls_pessoal (certificado autoassinado; o pino continua valendo),
// prazo curto. `cab` recebe "Authorization: Bearer ..." quando ha token.
typedef struct {
  char url[2400];
  char auth[96];
  const char *cabs[3];
  RedePedido p;
} OpenSeekPedido;
// metodo NULL = GET. pino NULL/"" so com semPino = 1 (pair/start). 0 = recusado.
int  openseek_pedido(OpenSeekPedido *o, const char *metodo, const char *url,
                     const char *token, const char *pino, int semPino,
                     const void *corpo, size_t nCorpo, unsigned prazoMs);

// --- Cache LRU por chave (texturas e JPEGs) ----------------------------------
#define OS_LRU_MAX 16
typedef struct { char chave[240]; unsigned uso; int ocupado; } OsLruItem;
typedef struct { OsLruItem it[OS_LRU_MAX]; int n; unsigned relogio; } OsLru;
void openseek_lru_ini(OsLru *l, int n);           // n <= OS_LRU_MAX
int  openseek_lru_achar(OsLru *l, const char *chave);   // indice ou -1 (marca uso)
// Indice para guardar `chave` (o livre ou o menos usado fora de `proteger`).
int  openseek_lru_lugar(OsLru *l, const char *chave, const char *const *proteger, int np);
#endif
