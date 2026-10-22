// A escolha do anexo e da versao da atualizacao do fork, so as pecas puras (sem SDL):
//   bash tests/atualizacao_ota.sh
#include "../src/atualizacao_pura.h"

static int falhas = 0;
#define CONFERE(c, ...) do { if (!(c)) { falhas++; printf("FALHA: " __VA_ARGS__); printf("\n"); } } while (0)

#define REPO "https://github.com/obadevyh/nuvio-native-legacy/releases/download/"
#define SUF "-sharing.apk"
#define ASSET(nome, dig, url) "{\"name\":\"" nome "\",\"size\":1,\"digest\":\"sha256:" dig "\",\"download_count\":0," \
  "\"browser_download_url\":\"" url "\"}"
#define SEMDIG(nome, url) "{\"name\":\"" nome "\",\"size\":1,\"download_count\":0,\"browser_download_url\":\"" url "\"}"
#define REL(...) "{\"tag_name\":\"v2.0.8\",\"assets\":[" __VA_ARGS__ "]}"

static int acha(const char *rel, char *url, char *hash) { return acharAnexo(rel, url, 512, hash, 80, SUF, REPO); }

int main(void) {
  char url[512], hash[80];
  // ---- versao como versionCode
  CONFERE(at_versao_codigo("2.0.8") == 20008 && at_versao_codigo("v2.0.8") == 20008 && at_versao_codigo("2.0.7") == 20007, "codigos");
  CONFERE(at_versao_codigo("2.0.8") > at_versao_codigo("2.0.7") && at_versao_codigo("2.1.0") > at_versao_codigo("2.0.99"), "ordem");
  CONFERE(!(at_versao_codigo("2.0.7") > at_versao_codigo("2.0.7")), "igual nao e novo");
  CONFERE(!(at_versao_codigo("2.0.6") > at_versao_codigo("2.0.7")), "downgrade nao e novo");
  CONFERE(at_versao_codigo("2.0.8-beta") == -1 && at_versao_codigo("2.0") == -1 && at_versao_codigo("2.0.8.1") == -1 &&
          at_versao_codigo("latest") == -1 && at_versao_codigo("") == -1 && at_versao_codigo(NULL) == -1 &&
          at_versao_codigo("2.100.0") == -1 && at_versao_codigo("2.0.100") == -1 && at_versao_codigo("v") == -1 &&
          at_versao_codigo("2.0.-1") == -1 && at_versao_codigo("2..8") == -1, "versoes recusadas");
  CONFERE(at_versao_codigo("10.0.0") == 100000, "10.0.0");
  // ---- o anexo bom
  CONFERE(acha(REL(ASSET("Nuvio-2.0.8-sharing.apk", "abcd", REPO "v2.0.8/Nuvio-2.0.8-sharing.apk")), url, hash), "achou");
  CONFERE(!strcmp(url, REPO "v2.0.8/Nuvio-2.0.8-sharing.apk") && !strcmp(hash, "abcd"), "url/hash certos [%s] [%s]", url, hash);
  // ---- origem errada
  CONFERE(!acha(REL(ASSET("Nuvio-2.0.8-sharing.apk", "abcd", "https://evil.example/Nuvio-2.0.8-sharing.apk")), url, hash), "outro host");
  CONFERE(url[0] == 0 && hash[0] == 0, "saidas vazias");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", "https://github.com/obadevyh/nuvio-native-legacy.evil/releases/download/v1/x-sharing.apk")), url, hash), "repo parecido");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", "https://github.com.evil.example/obadevyh/nuvio-native-legacy/releases/download/v1/x-sharing.apk")), url, hash), "host parecido");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", "http://github.com/obadevyh/nuvio-native-legacy/releases/download/v1/x-sharing.apk")), url, hash), "http");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", REPO "../../../evil/repo/releases/download/v1/x-sharing.apk")), url, hash), "..");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", REPO "v1/x-sharing.apk?redirect=https://evil")), url, hash), "query nao termina no sufixo");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", REPO "v1/%2e%2e/x-sharing.apk")), url, hash), "percent-2e");
  CONFERE(!acha(REL(ASSET("x-sharing.apk", "abcd", "https://user@github.com/obadevyh/nuvio-native-legacy/releases/download/v1/x-sharing.apk")), url, hash), "usuario@");
  // ---- sufixo
  CONFERE(!acha(REL(ASSET("a", "abcd", REPO "v1/Nuvio-2.0.8-sharing.apk.exe")), url, hash), "sufixo + extensao");
  CONFERE(!acha(REL(ASSET("a", "abcd", REPO "v1/Nuvio-2.0.8-sharing.apkx")), url, hash), "sufixo colado");
  CONFERE(!acha(REL(ASSET("a", "abcd", REPO "v1/Nuvio-2.0.8-android.apk")), url, hash), "outro flavour");
  CONFERE(!acha(REL(ASSET("a", "abcd", REPO "v1/Nuvio-2.0.8-sharing-debug.apk")), url, hash), "debug");
  // ---- digest obrigatorio e do MESMO anexo
  CONFERE(!acha(REL(SEMDIG("a", REPO "v1/Nuvio-2.0.8-sharing.apk")), url, hash), "sem digest");
  CONFERE(!acha(REL(ASSET("outro.zip", "aaaa", REPO "v1/outro.zip") "," SEMDIG("a", REPO "v1/Nuvio-2.0.8-sharing.apk")), url, hash),
          "nao empresta o digest do anexo ANTERIOR");
  CONFERE(acha(REL(ASSET("outro.zip", "aaaa", REPO "v1/outro.zip") "," ASSET("a", "bbbb", REPO "v1/Nuvio-2.0.8-sharing.apk")), url, hash) && !strcmp(hash, "bbbb"),
          "digest do proprio anexo [%s]", hash);
  CONFERE(acha(REL(ASSET("a", "bbbb", REPO "v1/Nuvio-2.0.8-sharing.apk") "," ASSET("outro.zip", "cccc", REPO "v1/outro.zip")), url, hash) && !strcmp(hash, "bbbb"),
          "nem o do SEGUINTE [%s]", hash);
  // o falso (outro host) vem ANTES do verdadeiro: so o verdadeiro vale
  CONFERE(acha(REL(ASSET("a", "ffff", "https://evil.example/Nuvio-2.0.8-sharing.apk") "," ASSET("a", "bbbb", REPO "v1/Nuvio-2.0.8-sharing.apk")), url, hash) &&
          !strcmp(hash, "bbbb") && !strncmp(url, REPO, strlen(REPO)), "o falso nao ofusca o verdadeiro");
  CONFERE(!acha("{\"assets\":[]}", url, hash) && !acha("", url, hash), "vazio");
  // sem prefixo (o .tpk) o comportamento antigo continua
  CONFERE(acharAnexo(REL(ASSET("a", "dddd", "https://x/libnuvio.so")), url, 512, hash, 80, "libnuvio.so", NULL) && !strcmp(hash, "dddd"), "sem prefixo");
  printf(falhas ? "atualizacao_ota: %d FALHA(S)\n" : "atualizacao_ota: ok\n", falhas);
  return falhas != 0;
}
