// Pecas PURAS da leitura da release do GitHub (sem SDL): o digest do MESMO anexo, a escolha do anexo pelo sufixo E pela origem fixada,
// e a versao como numero (versionCode). Separadas de atualizacao.c para os testes (tests/atualizacao_ota.c) rodarem com um cc comum.
#ifndef NV_ATUALIZACAO_PURA_H
#define NV_ATUALIZACAO_PURA_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__)
#define AT_PURA __attribute__((unused)) static
#else
#define AT_PURA static
#endif

// "x.y.z" -> x*10000 + y*100 + z (a MESMA conta do build.gradle.kts), com ou sem "v". -1 se nao for exatamente tres numeros (y e z < 100):
// "2.0.8-beta", "2.0", "2.0.8.1" e "latest" nao sao versao oferecivel. So uma versao de codigo MAIOR e nova (nada de downgrade nem de igual).
AT_PURA long at_versao_codigo(const char *v) {
  long n[3] = { 0, 0, 0 };
  int i;
  char *f;
  if (!v) return -1;
  if (*v == 'v') v++;
  for (i = 0; i < 3; i++) {
    if (*v < '0' || *v > '9') return -1;
    n[i] = strtol(v, &f, 10);
    if (n[i] > 99999 || (i > 0 && n[i] > 99)) return -1;
    v = f;
    if (i < 2) { if (*v != '.') return -1; v++; }
  }
  if (*v) return -1;
  return n[0] * 10000 + n[1] * 100 + n[2];
}

// O sha256 hex do anexo cuja browser_download_url comeca em `ini`. O campo
// `digest` vem ANTES do browser_download_url dentro do mesmo anexo (ordem do
// JSON do GitHub: ... size, digest, download_count, ..., browser_download_url),
// entao a busca e PARA TRAS a partir da url — ir para frente pegaria o digest do
// anexo SEGUINTE. Escreve "" quando o anexo nao tem digest.
AT_PURA void hashAntesDe(const char *corpo, const char *ini, char *hash, size_t tamHash) {
  const char *d = NULL, *q = corpo;
  if (!hash || !tamHash) return;
  hash[0] = 0;
  while (q < ini) {
    const char *r = strstr(q, "\"digest\":");
    if (!r || r > ini) break;
    d = r; q = r + 8;
  }
  if (!d) return;
  { const char *outro = d;                               // duas chaves de url entre o digest e esta url: o digest e do anexo ANTERIOR
    int urls = 0;                                        // (a primeira e a PROPRIA chave desta url, que vem depois do digest dela)
    while ((outro = strstr(outro, "\"browser_download_url\":")) != NULL && outro < ini) {
      if (++urls > 1) return;
      outro += 23;
    } }
  d = strchr(d + 8, '"');
  if (!d) return;
  d++;
  if (!strncmp(d, "sha256:", 7)) d += 7;   // so o hex interessa
  { const char *e = strchr(d, '"');
    if (e && (size_t)(e - d) < tamHash) { memcpy(hash, d, (size_t)(e - d)); hash[e - d] = 0; } }
}


// Anexo cujo nome termina em `sufixo` (casamento EXATO, sem o AT_SUFIXO2 do
// .ipk) e o sha256 do MESMO anexo (hashAntesDe). Sem digest ignora o anexo: sem
// hash nao ha como confiar em codigo remoto. Serve ao .so do .tpk e ao .apk.
AT_PURA int acharAnexo(const char *corpo, char *dst, size_t tam, char *hash, size_t tamHash,
                      const char *sufixo, const char *prefixo) {
  const char *p = corpo;
  const char *chave = "\"browser_download_url\":";
  size_t k = strlen(sufixo);
  dst[0] = 0;
  if (hash && tamHash) hash[0] = 0;
  while ((p = strstr(p, chave)) != NULL) {
    const char *ini;
    size_t n;
    p += strlen(chave);
    while (*p == ' ') p++;
    if (*p != '"') continue;
    ini = ++p;
    while (*p && *p != '"') p++;
    n = (size_t)(p - ini);
    if (n > k && n < tam && !strncmp(ini + n - k, sufixo, k) &&
        (!prefixo || (n > strlen(prefixo) && !strncmp(ini, prefixo, strlen(prefixo))))) {   // origem FIXA: so a release do nosso repositorio
      char h[80] = "";
      if (prefixo) {                       // origem fixada: nada de "..", query, fragmento, usuario@ ou barra invertida na url
        size_t q;
        int ruim = 0;
        for (q = 0; q < n; q++) if (ini[q] == '?' || ini[q] == '#' || ini[q] == '@' || ini[q] == '\\' || ini[q] == '%' ||
                                    (q + 1 < n && ini[q] == '.' && ini[q + 1] == '.')) ruim = 1;
        if (ruim) continue;
      }
      hashAntesDe(corpo, ini, h, sizeof h);
      if (!h[0]) continue;                 // sem digest: nao confiar
      memcpy(dst, ini, n); dst[n] = 0;
      snprintf(hash, tamHash, "%s", h);
      return 1;
    }
  }
  return 0;
}

#endif
