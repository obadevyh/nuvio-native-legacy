// Casamento do anexo "-android.apk" da release (auto-atualizacao no Android).
//   bash tests/atualizacao_android.sh
// Inclui atualizacao.c com NV_ANDROID definido aqui (acharApk e estatica); o resto do
// nucleo compila sem a flag, e a ponte JNI e substituida pelo stub abaixo.
#define NV_VERSAO "1.0.53"
#define NV_ANDROID 1
#include "../src/atualizacao.c"

int android_instalar_apk(const char *c) { (void)c; return 0; }

static int falhas = 0;
#define CONFERE(c, ...) do { if (!(c)) { falhas++; printf("FALHA: " __VA_ARGS__); printf("\n"); } } while (0)

#define REPO "https://github.com/obadevyh/nuvio-native-legacy/releases/download/v1.7.0/"
#define ASSET(nome, dig) "{\"name\":\"" nome "\",\"size\":1,\"digest\":\"sha256:" dig "\",\"download_count\":0," \
  "\"browser_download_url\":\"" REPO nome "\"}"

int main(void) {
  char url[512], hash[80];
  const char *rel = "{\"assets\":["
    ASSET("Nuvio-1.7.0-android.apk", "aaaa") ","
    ASSET("Nuvio-1.7.0-sharing-debug.apk", "bbbb") ","
    ASSET("Nuvio-1.7.0_arm.ipk", "cccc") ","
    ASSET("Nuvio-1.7.0-sharing.apk", "dddd") ","
    ASSET("Nuvio-1.7.0-tpk-arm.so", "eeee") "]}";
  CONFERE(acharApk(rel, url, sizeof url, hash, sizeof hash), "achou o -sharing.apk");
  CONFERE(!strcmp(url, REPO "Nuvio-1.7.0-sharing.apk"), "url certa: [%s]", url);
  CONFERE(!strcmp(hash, "dddd"), "digest do MESMO anexo: [%s]", hash);
  CONFERE(!acharApk("{\"assets\":[" ASSET("Nuvio-1-android.apk", "aa") "," ASSET("Nuvio-1-sharing-debug.apk", "bb") "]}",
                    url, sizeof url, hash, sizeof hash), "outro flavour e debug nao casam");
  CONFERE(url[0] == 0 && hash[0] == 0, "e saem vazios");
  CONFERE(!acharApk("{\"assets\":[{\"browser_download_url\":\"" REPO "Nuvio-1-sharing.apk\"}]}",
                    url, sizeof url, hash, sizeof hash), "sem digest nao casa");
  CONFERE(!acharApk("{\"assets\":[{\"digest\":\"sha256:ab\",\"browser_download_url\":\"https://evil.example/Nuvio-1-sharing.apk\"}]}",
                    url, sizeof url, hash, sizeof hash), "origem fora do repositorio nao casa");
  CONFERE(!acharApk("{\"assets\":[]}", url, sizeof url, hash, sizeof hash), "sem anexo devolve 0");
  printf(falhas ? "atualizacao_android: %d FALHA(S)\n" : "atualizacao_android: ok\n", falhas);
  return falhas != 0;
}
