package space.nuvio.nativelegacy

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build
import java.security.MessageDigest

/**
 * Confere um APK baixado ANTES de o instalador do sistema ser chamado. O Android ja recusa assinatura diferente e downgrade na troca
 * de pacote; isto repete a regra aqui para o app dizer o motivo e apagar o arquivo, e para nunca mostrar a tela de instalacao de
 * algo que nao e uma atualizacao nossa.
 *
 * Aceita SOMENTE: o mesmo applicationId, o MESMO conjunto de certificados de assinatura do app instalado (e nao vazio) e um
 * versionCode ESTRITAMENTE maior.
 */
object ApkVerifica {
    const val OK = 1
    const val RECUSADO_PACOTE_OU_ASSINATURA = 3
    const val RECUSADO_NAO_E_MAIS_NOVA = 4
    const val RECUSADO_ILEGIVEL = 5

    private fun flags(): Int =
        if (Build.VERSION.SDK_INT >= 28) PackageManager.GET_SIGNING_CERTIFICATES
        else @Suppress("DEPRECATION") PackageManager.GET_SIGNATURES

    private fun sha256Hex(b: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    /** SHA-256 de cada certificado que assina (o conjunto atual de assinantes, nao o historico de rotacao). */
    fun certificados(p: PackageInfo): Set<String> {
        if (Build.VERSION.SDK_INT >= 28) {
            val si = p.signingInfo ?: return emptySet()
            val arr = si.apkContentsSigners ?: return emptySet()
            return arr.map { sha256Hex(it.toByteArray()) }.toSet()
        }
        @Suppress("DEPRECATION")
        return (p.signatures ?: emptyArray()).map { sha256Hex(it.toByteArray()) }.toSet()
    }

    private fun codigo(p: PackageInfo): Long =
        if (Build.VERSION.SDK_INT >= 28) p.longVersionCode else @Suppress("DEPRECATION") p.versionCode.toLong()

    /** [OK] ou um dos RECUSADO_*. Nunca lanca. */
    fun verificar(pm: PackageManager, pacote: String, caminho: String): Int {
        return try {
            val novo = pm.getPackageArchiveInfo(caminho, flags()) ?: return RECUSADO_ILEGIVEL
            val atual = pm.getPackageInfo(pacote, flags())
            if (novo.packageName != pacote) return RECUSADO_PACOTE_OU_ASSINATURA
            val cNovo = certificados(novo)
            if (cNovo.isEmpty() || cNovo != certificados(atual)) return RECUSADO_PACOTE_OU_ASSINATURA
            if (codigo(novo) <= codigo(atual)) return RECUSADO_NAO_E_MAIS_NOVA
            OK
        } catch (e: Exception) {
            RECUSADO_ILEGIVEL
        }
    }
}
