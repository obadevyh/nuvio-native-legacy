// OPENSEEK: como cada pedido e montado (SR-3/SR-4), os corpos JSON, a leitura
// das respostas, o mapa de estados para SEEKR_*, o endereco digitado e o LRU
// por chave que mantem as texturas quando o VTT e trocado.
//
//   bash tests/openseek_pedido.sh
#include "../src/openseekproto.h"
#include "../src/rede.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define B "https://nas.local:8787"
#define PINO "sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
#define TOKEN "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"
#define URL_VIDEO "https://cdn.example/aio/SEGREDO-123/video.mkv?token=abc&x=\"q\""

static int conta(const char *h, const char *a) {
  int n = 0; for (const char *p = h; (p = strstr(p, a)) != NULL; p++) n++; return n;
}

int main(void) {
  char b[16384], k[240];
  OpenSeekPedido o;
  size_t n;

  // --- endereco ---------------------------------------------------------------------
  assert(openseek_normalizar_endereco("nas.local", b, sizeof b) && !strcmp(b, "https://nas.local:8787"));
  assert(openseek_normalizar_endereco("192.168.1.5:9000", b, sizeof b) && !strcmp(b, "https://192.168.1.5:9000"));
  assert(openseek_normalizar_endereco(" HTTPS://NAS.Local:8787/ ", b, sizeof b) && !strcmp(b, "https://nas.local:8787"));
  assert(openseek_normalizar_endereco("[fe80::1]:8787", b, sizeof b) && !strcmp(b, "https://[fe80::1]:8787"));
  assert(!openseek_normalizar_endereco("http://nas.local", b, sizeof b));   // nunca http
  assert(!openseek_normalizar_endereco("ftp://nas", b, sizeof b));
  assert(!openseek_normalizar_endereco("user@nas", b, sizeof b));
  assert(!openseek_normalizar_endereco("nas/path", b, sizeof b));
  assert(!openseek_normalizar_endereco("nas:0", b, sizeof b));
  assert(!openseek_normalizar_endereco("nas:70000", b, sizeof b));
  assert(!openseek_normalizar_endereco("nas?x=1", b, sizeof b));
  assert(!openseek_normalizar_endereco("", b, sizeof b));
  assert(openseek_na_origem(B "/v1/x", B) && !openseek_na_origem(B "0/v1/x", B) &&
         !openseek_na_origem("https://nas.local:8788/v1", B) && !openseek_na_origem(B, B));

  // --- corpo do POST /v1/previews: a URL so aqui, escapada; cabecalhos filtrados --------
  OpenSeekFonte f = { URL_VIDEO,
                      "User-Agent: Nuvio/1.8\nCookie: sessao=SEGREDO\r\nreferer:  https://site/ \nOrigin: https://o\nAuthorization: Bearer X\n",
                      7234000, "tt0133093:1:2", 1, 2, 123456789ULL, "8e245d9679d31e12", "a\"b.mkv", 61000, -1 };
  n = openseek_json_previa(b, sizeof b, &f);
  assert(n > 0 && n == strlen(b) && n <= 16384);
  assert(strstr(b, "{\"source\":{\"url\":\"https://cdn.example/aio/SEGREDO-123/video.mkv?token=abc&x=\\\"q\\\"\""));
  assert(strstr(b, "\"headers\":{\"User-Agent\":\"Nuvio/1.8\",\"Referer\":\"https://site/\",\"Origin\":\"https://o\"}"));
  assert(!strstr(b, "Cookie") && !strstr(b, "sessao") && !strstr(b, "Authorization"));
  assert(strstr(b, "\"duration_ms\":7234000"));
  assert(strstr(b, "\"imdb_id\":\"tt0133093\",\"season\":1,\"episode\":2,\"video_size\":123456789"));
  assert(strstr(b, "\"filename\":\"a\\\"b.mkv\""));
  assert(strstr(b, "\"at_ms\":null,\"playhead_ms\":61000,\"layout\":\"hybrid\"}"));
  assert(conta(b, "SEGREDO-123") == 1);
  f.atMs = 4800000; f.cabecalhos = NULL; f.imdb = "kitsu:1";
  assert(openseek_json_previa(b, sizeof b, &f) && strstr(b, "\"headers\":{}") && strstr(b, "\"at_ms\":4800000") &&
         strstr(b, "\"imdb_id\":\"\""));
  { OpenSeekFonte g = f; g.url = "magnet:?xt=1"; assert(!openseek_json_previa(b, sizeof b, &g)); }
  { OpenSeekFonte g = f; g.url = "https://x/\x01"; assert(openseek_json_previa(b, sizeof b, &g) && strstr(b, "\\u0001")); }
  assert(!openseek_json_previa(b, 64, &f) && !b[0]);              // nao coube: nada
  assert(openseek_json_prioridade(b, sizeof b, -1, 1234000) && !strcmp(b, "{\"at_ms\":null,\"playhead_ms\":1234000}"));
  assert(openseek_json_prioridade(b, sizeof b, 4800000, 0) && !strcmp(b, "{\"at_ms\":4800000,\"playhead_ms\":0}"));
  assert(openseek_json_claim(b, sizeof b, "00112233445566778899aabbccddeeff", "Nuvio", "ab") &&
         !strcmp(b, "{\"pair_id\":\"00112233445566778899aabbccddeeff\",\"device_name\":\"Nuvio\",\"client_proof\":\"ab\"}"));

  // --- montagem do pedido: pino, sem redirect, URL do video so no corpo ----------------
  n = openseek_json_previa(b, sizeof b, &f);
  assert(openseek_pedido(&o, "POST", B "/v1/previews", TOKEN, PINO, 0, b, n, 0));
  assert(o.p.pino_publico && !strcmp(o.p.pino_publico, PINO));
  assert(o.p.seguir == 0 && o.p.tls_pessoal == 1 && o.p.prazo_ms == OS_PRAZO_API_MS);
  assert(!strcmp(o.p.url, B "/v1/previews") && !strstr(o.p.url, "cdn.example"));
  assert(o.p.corpo == b && o.p.n_corpo == n && strstr((const char *)o.p.corpo, "cdn.example"));
  assert(!strcmp(o.p.cabecalhos[0], "Authorization: Bearer " TOKEN));
  assert(!strcmp(o.p.cabecalhos[1], "Content-Type: application/json") && !o.p.cabecalhos[2]);
  for (int i = 0; o.p.cabecalhos[i]; i++) assert(!strstr(o.p.cabecalhos[i], "cdn.example"));
  // Sem pino: so o primeiro contato, sem token e sem corpo com segredo.
  assert(!openseek_pedido(&o, "POST", B "/v1/previews", TOKEN, NULL, 0, b, n, 0));
  assert(!openseek_pedido(&o, "POST", B "/v1/previews", TOKEN, "", 1, b, n, 0));
  assert(!openseek_pedido(&o, "POST", B "/v1/previews", NULL, NULL, 1, b, n, 0));
  assert(!openseek_pedido(&o, "POST", B "/v1/pair/claim", NULL, NULL, 1, "{\"pair_id\":1}", 13, 0));
  assert(openseek_pedido(&o, "POST", B "/v1/pair/start", NULL, NULL, 1, "{}", 2, 0) &&
         !o.p.pino_publico && o.p.cabecalhos && !strcmp(o.p.cabecalhos[0], "Content-Type: application/json") && o.p.seguir == 0);
  // Pino malformado (um caminho de arquivo, p.ex.) e http://: recusados.
  assert(!openseek_pedido(&o, NULL, B "/v1/health", NULL, "/sdcard/pin.der", 0, NULL, 0, 0));
  assert(!openseek_pedido(&o, NULL, "http://nas.local:8787/v1/health", NULL, PINO, 0, NULL, 0, 0));
  assert(!openseek_pedido(&o, NULL, B "/v1/x", "tok\r\nX: y", PINO, 0, NULL, 0, 0));
  assert(openseek_pedido(&o, NULL, B "/v1/c/cap/id/tiles/1.jpg?exp=1", NULL, PINO, 0, NULL, 0, OS_PRAZO_DADOS_MS) &&
         o.p.prazo_ms == 10000 && !o.p.cabecalhos && o.p.pino_publico);
  assert(rede_pino_valido(PINO) && rede_pino_valido(PINO ";" PINO));
  assert(!rede_pino_valido("sha256//AAAA") && !rede_pino_valido("/data/pin.pem") &&
         !rede_pino_valido(PINO ";") && !rede_pino_valido("sha1//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));

  // --- respostas --------------------------------------------------------------------
  {
    OpenSeekStart s; OpenSeekClaim c; OpenSeekPrevia p; long v; int e;
    assert(openseek_ler_start("{\"pair_id\":\"00112233445566778899aabbccddeeff\",\"salt\":\"0f1e2d3c4b5a69788796a5b4c3d2e1f0\","
                              "\"spki_pin\":\"" PINO "\",\"iterations\":600000}", &s));
    assert(s.iter == 600000 && !strcmp(s.pino, PINO));
    assert(!openseek_ler_start("{\"pair_id\":\"00\",\"salt\":\"0f1e2d3c4b5a69788796a5b4c3d2e1f0\",\"spki_pin\":\"" PINO "\",\"iterations\":1}", &s));
    assert(!openseek_ler_start("{\"pair_id\":\"00112233445566778899aabbccddeeff\",\"salt\":\"0f1e2d3c4b5a69788796a5b4c3d2e1f0\","
                               "\"spki_pin\":\"/etc/pin\",\"iterations\":600000}", &s));
    assert(openseek_ler_claim("{\"device_id\":\"0123456789abcdef\",\"token\":\"" TOKEN "\",\"server_proof\":\""
                              "ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01\"}", &c));
    assert(!openseek_ler_claim("{\"device_id\":\"0123456789abcdef\",\"token\":\"a b\",\"server_proof\":\"ab\"}", &c));
    const char *r = "{\"preview_id\":\"0123456789abcdef0123456789abcdef\",\"state\":\"preparing\",\"error\":null,"
      "\"vtt_url\":\"" B "/v1/c/CAP/0123456789abcdef0123456789abcdef/thumbs.vtt?exp=1999999999&layout=hybrid\","
      "\"vtt_version\":0,\"source_duration_ms\":null,\"tile_interval_ms\":10000,\"tolerance_ms\":5000,"
      "\"progress\":{\"tiles_done\":0,\"tiles_total\":null},\"refresh_after_ms\":1000}";
    assert(openseek_ler_previa(r, B, &p) && p.estado == OS_PREPARANDO && p.versao == 0 && p.exp == 1999999999LL);
    assert(!p.erro[0] && p.refreshMs == 1000);
    // vtt_url de outra origem: recusada (o pino nao cobre).
    assert(!openseek_ler_previa("{\"preview_id\":\"0123456789abcdef0123456789abcdef\",\"state\":\"ready\","
                                "\"vtt_url\":\"https://evil/x.vtt\",\"vtt_version\":3}", B, &p));
    assert(openseek_ler_previa("{\"preview_id\":\"0123456789abcdef0123456789abcdef\",\"state\":\"failed\","
                               "\"error\":\"range_unsupported\",\"vtt_url\":null}", B, &p) &&
           p.estado == OS_FALHOU && !strcmp(p.erro, "range_unsupported"));
    assert(openseek_ler_versao("{\"vtt_version\": 7,\"state\":\"ready\"}", &v, &e) && v == 7 && e == OS_PRONTO);
    assert(!openseek_ler_versao("{\"error\":\"unauthorized\"}", &v, &e));

    // --- estados (SEEKR_*) ---------------------------------------------------------
    int m;
    OpenSeekPrevia pr = p; pr.estado = OS_PREPARANDO;
    assert(openseek_mapear(202, REDE_OK, 0, &pr, 0, &m) == OS_SEEKR_BUSCANDO && m == OSM_PREPARANDO); // vazio = carregando
    assert(openseek_mapear(202, REDE_OK, 0, &pr, 3, &m) == OS_SEEKR_PRONTO);
    pr.estado = OS_PRONTO;     assert(openseek_mapear(200, REDE_OK, 0, &pr, 0, &m) == OS_SEEKR_PRONTO);
    pr.estado = OS_COMPLETO;   assert(openseek_mapear(200, REDE_OK, 0, &pr, 9, &m) == OS_SEEKR_PRONTO);
    pr.estado = OS_FALHOU;     assert(openseek_mapear(202, REDE_OK, 0, &pr, 9, &m) == OS_SEEKR_SEM_PREVIA && m == OSM_FONTE);
    assert(openseek_mapear(401, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_CHAVE_RECUSADA && m == OSM_RECUSADO);
    assert(openseek_mapear(429, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_LIMITE_PROVEDOR && m == OSM_OCUPADO);
    assert(openseek_mapear(503, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_LIMITE_PROVEDOR);
    assert(openseek_mapear(400, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_SEM_PREVIA && m == OSM_FONTE);
    assert(openseek_mapear(403, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_EXPIRADO);
    assert(openseek_mapear(500, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_REDE);
    assert(openseek_mapear(200, REDE_OK, 0, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL);   // ilegivel
    assert(openseek_mapear(0, REDE_TRANSPORTE, 7, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_REDE);
    assert(openseek_mapear(0, REDE_PRAZO, 28, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_REDE);
    assert(openseek_mapear(0, REDE_TRANSPORTE, REDE_CURL_PINO_DIFERENTE, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_PINO_MUDOU);
    assert(openseek_mapear(0, REDE_INDISPONIVEL, 4, NULL, 0, &m) == OS_SEEKR_REDE_INDISPONIVEL && m == OSM_SEM_PINO);
    assert(openseek_mapear(0, REDE_CANCELADO, 0, NULL, 0, &m) == -1);
    assert(openseek_mapear(0, REDE_GERACAO, 0, NULL, 0, &m) == -1);
  }

  // --- JPEG: dimensoes antes de decodificar -----------------------------------------------
  {
    static const unsigned char j[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46,
                                       0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0xB4, 0x01, 0x40, 0x01, 0x01, 0x11, 0x00 };
    int w = 0, h = 0;
    assert(openseek_jpeg_dimensoes(j, sizeof j, &w, &h) && w == 320 && h == 180);
    assert(!openseek_jpeg_dimensoes(j, 10, &w, &h));
    assert(!openseek_jpeg_dimensoes((const unsigned char *)"GIF89a....", 10, &w, &h));
  }

  // --- troca de VTT mantem as texturas (LRU por chave) ------------------------------------
  {
    OsLru l;
    const char *v1[] = {
      B "/v1/c/CAP1/0123456789abcdef0123456789abcdef/tiles/480.jpg?exp=100&t=4797960",
      B "/v1/c/CAP1/0123456789abcdef0123456789abcdef/tiles/481.jpg?exp=100&t=4810000",
      B "/v1/c/CAP1/0123456789abcdef0123456789abcdef/tiles/482.jpg?exp=100&t=4820000" };
    const char *v2[] = {   // versao nova: capacidade e exp novos, um quadro novo no meio
      B "/v1/c/CAP2/0123456789abcdef0123456789abcdef/tiles/479.jpg?exp=200&t=4790000",
      B "/v1/c/CAP2/0123456789abcdef0123456789abcdef/tiles/480.jpg?exp=200&t=4797960",
      B "/v1/c/CAP2/0123456789abcdef0123456789abcdef/tiles/481.jpg?exp=200&t=4810000",
      B "/v1/c/CAP2/0123456789abcdef0123456789abcdef/tiles/482.jpg?exp=200&t=4820000" };
    int sl[3];
    openseek_lru_ini(&l, 12);
    for (int i = 0; i < 3; i++) {
      assert(openseek_chave(v1[i], k, sizeof k));
      sl[i] = openseek_lru_lugar(&l, k, NULL, 0);
      assert(sl[i] >= 0);
    }
    // Os indices das cues andaram (+1), mas as tres texturas continuam achadas.
    for (int i = 1; i < 4; i++) {
      assert(openseek_chave(v2[i], k, sizeof k));
      assert(openseek_lru_achar(&l, k) == sl[i - 1]);
    }
    assert(openseek_chave(v2[0], k, sizeof k) && openseek_lru_achar(&l, k) < 0);  // o novo vem da rede
    // LRU: com 12 vagas, a 13a chave tira a menos usada, nunca uma protegida.
    {
      char ch[16][64]; const char *prot[1];
      openseek_lru_ini(&l, 12);
      for (int i = 0; i < 12; i++) { snprintf(ch[i], 64, "k%d", i); assert(openseek_lru_lugar(&l, ch[i], NULL, 0) == i); }
      openseek_lru_achar(&l, "k0");          // k0 recem usada; k1 e a mais velha
      prot[0] = "k1";                        // mas e a atual na tela: protegida
      assert(openseek_lru_lugar(&l, "k12", prot, 1) == 2);
      assert(openseek_lru_achar(&l, "k1") == 1 && openseek_lru_achar(&l, "k2") < 0);
    }
  }
  printf("openseek_pedido: ok\n");
  return 0;
}
