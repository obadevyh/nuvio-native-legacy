#!/usr/bin/env python3
"""Resolves the merge conflicts that a daily upstream sync is expected to hit,
and nothing else (tools/sync_upstream.ps1).

Run in the repository root after `git merge origin/master` stopped with
conflicts (conflictStyle=merge: <<<<<<< ours / ======= / >>>>>>> theirs).
"ours" is the sharing branch, "theirs" is upstream.

Known kinds, each resolved the one way that is always right for it:
  src/idioma_*.h            both sides added translation lines next to each
                            other: keep both, sorted by the strcmp of the
                            decoded key bytes (idioma.c binary-searches the
                            table; tools/idiomas.py checks it afterwards).
                            Same key on both sides: upstream's line wins.
  src/ajustes.c             both sides appended settings to the positional
  src/ajustes_ux_padrao.inc lists (enum, CHAVE[], defaults): upstream's
                            entries first, then ours. Settings are saved by
                            key name, so the order never reaches a saved file.
  deploy/app/appinfo.json   the higher "version" wins.

Any other conflicted file, or a hunk that doesn't look like its kind, is left
alone and listed; the exit status is then 1 and the caller aborts the merge.
"""
import re
import subprocess
import sys

LISTAS = {"src/ajustes.c", "src/ajustes_ux_padrao.inc"}
IDIOMA = re.compile(r"^src/idioma_[a-z]+\.h$")
CHAVE = re.compile(r'^\s*(?:T\(|\{\s*)"((?:[^"\\]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "r": "\r", "0": "\0"}


def conflitados():
    out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=U"],
                         capture_output=True, text=True, check=True).stdout
    return [l for l in out.splitlines() if l]


def chave(linha):
    m = CHAVE.match(linha)
    if not m:
        return None
    s = re.sub(r"\\(.)", lambda e: ESCAPES.get(e.group(1), e.group(1)), m.group(1))
    return s.encode("utf-8")


def resolver_idioma(nossos, deles):
    porChave = {}
    for l in nossos + deles:          # deles por ultimo: vence na mesma chave
        k = chave(l)
        if k is None:
            if l.strip():
                raise ValueError("linha que nao e de traducao: " + l.strip()[:80])
            continue
        porChave[k] = l
    return [porChave[k] for k in sorted(porChave)]


def resolver_versao(nossos, deles):
    def v(ls):
        m = re.search(r'"version":\s*"(\d+)\.(\d+)\.(\d+)"', "".join(ls))
        if not m:
            raise ValueError("bloco sem version")
        return tuple(int(x) for x in m.groups())
    return nossos if v(nossos) >= v(deles) else deles


# A line of a positional list: a comment, enumerators, a string key, a menu
# item macro or a row of numeric defaults. A hunk with anything else (an `if`,
# a `case`, a brace) is code, and stacking both sides of code is not a merge.
LINHA_DE_LISTA = re.compile(
    r"^\s*$"
    r"|^\s*//.*$"
    r"|^\s*/\*.*\*/\s*$"
    r"|^\s*AJ_[A-Z0-9_]+\s*(?:,\s*AJ_[A-Z0-9_]+\s*)*,?\s*(?://.*)?$"
    r'|^\s*(?:"(?:[^"\\]|\\.)*"\s*,\s*)+(?://.*)?$'
    r"|^\s*[A-Z][A-Z0-9_]*\(.*\)\s*,\s*(?://.*)?$"
    r"|^\s*(?:-?\d+\s*,\s*)+(?:/\*.*\*/)?\s*$")


def empilhar_listas(nossos, deles):
    for l in nossos + deles:
        if not LINHA_DE_LISTA.match(l.rstrip("\r\n")):
            raise ValueError("bloco de codigo, nao de lista: " + l.strip()[:80])
    return deles + nossos


def resolver(caminho):
    if IDIOMA.match(caminho):
        regra = resolver_idioma
    elif caminho in LISTAS:
        regra = empilhar_listas
    elif caminho == "deploy/app/appinfo.json":
        regra = resolver_versao
    else:
        raise ValueError("conflito fora dos tipos conhecidos")
    with open(caminho, encoding="utf-8", newline="") as f:
        linhas = f.readlines()
    saida, i, blocos = [], 0, 0
    while i < len(linhas):
        l = linhas[i]
        if not l.startswith("<<<<<<< "):
            saida.append(l)
            i += 1
            continue
        nossos, deles, j = [], [], i + 1
        while j < len(linhas) and not linhas[j].startswith("======="):
            if linhas[j].startswith(("|||||||", "<<<<<<< ")):
                raise ValueError("conflito aninhado ou estilo diff3")
            nossos.append(linhas[j])
            j += 1
        j += 1
        while j < len(linhas) and not linhas[j].startswith(">>>>>>> "):
            deles.append(linhas[j])
            j += 1
        if j >= len(linhas):
            raise ValueError("marcador de conflito sem fim")
        saida.extend(regra(nossos, deles))
        blocos += 1
        i = j + 1
    texto = "".join(saida)
    if re.search(r"^(<<<<<<< |=======$|>>>>>>> )", texto, re.M):
        raise ValueError("marcador restante")
    with open(caminho, "w", encoding="utf-8", newline="") as f:
        f.write(texto)
    subprocess.run(["git", "add", "--", caminho], check=True)
    return blocos


def main():
    falhas = []
    for c in conflitados():
        try:
            n = resolver(c)
            print(f"resolvido: {c} ({n} bloco(s))")
        except (ValueError, UnicodeDecodeError) as e:
            falhas.append(c)
            print(f"NAO resolvido: {c}: {e}")
    if falhas:
        print(f"{len(falhas)} arquivo(s) precisam de uma pessoa")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
