<#
  Repeatable release of the Nuvio Legacy SHARING build (appId space.nuvio.nativelegacy.sharing) through GitHub Releases, which is what the
  in-app updater reads (src/atualizacao.c). A release only becomes visible to the app when EVERYTHING below succeeded (draft -> verified -> published).

    tools\release_sharing.ps1 -Version 2.0.9 -Notes release-notes.md            # the real thing
    tools\release_sharing.ps1 -Version 2.0.9 -Notes release-notes.md -DryRun    # everything except creating/publishing the release

  Steps: refuse if the tree is dirty or the version is not higher than the latest PUBLISHED one (versionCode compare) -> bump deploy/app/appinfo.json
  and commit -> C unit tests (WSL) -> assembleSharing signed with the dedicated key -> verify package, versionCode, signing certificate (pinned SHA-256),
  v2 signature, not debuggable -> SHA-256 -> DRAFT release + asset "Nuvio-<version>-sharing.apk" -> compare GitHub's own digest and a re-download with the
  local hash -> publish -> read the public releases/latest like the app does.
  The signing password is read from the key folder and never printed. The keystore never leaves C:\Users\Samue\.nuvio-sharing.
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$Notes,
  [switch]$DryRun,
  [string]$Repo = "Tolu-Walop-E/nuvio-native-legacy",
  [string]$KeyDir = "C:\Users\Samue\.nuvio-sharing",
  [string]$SdkDir = "C:\Users\Samue\AppData\Local\Android\Sdk"
)
$ErrorActionPreference = "Stop"
$PinnedCert = "5f8dc63e966d9e5f48c74038ea877bf89363dbde93a58507c83740cfdec7e611"   # the certificate of the APKs people already have
$AppId = "space.nuvio.nativelegacy.sharing"
$root = Split-Path -Parent $PSScriptRoot
$info = Join-Path $root "deploy\app\appinfo.json"
$bt = Join-Path $SdkDir "build-tools\36.0.0"
function Step($t) { Write-Host "`n== $t" -ForegroundColor Cyan }
function Fail($t) { throw "RELEASE STOPPED: $t" }
function Code($v) {
  if ($v -notmatch '^v?(\d+)\.(\d+)\.(\d+)$') { return -1 }
  if ([int]$Matches[2] -gt 99 -or [int]$Matches[3] -gt 99) { return -1 }
  return [int]$Matches[1] * 10000 + [int]$Matches[2] * 100 + [int]$Matches[3]
}
$code = Code $Version
if ($code -lt 0) { Fail "version must be x.y.z (y and z below 100)" }
if (-not (Test-Path $Notes) -or (Get-Item $Notes).Length -lt 10) { Fail "release notes file missing or empty: $Notes" }
$Notes = (Resolve-Path $Notes).Path
Set-Location $root

Step "1/8 repository state"
if (git status --porcelain --untracked-files=no) { Fail "uncommitted changes; commit or stash first" }
$latest = $null
try { $latest = (gh release view --repo $Repo --json tagName,isDraft,isPrerelease | ConvertFrom-Json) } catch { }
if ($latest) {
  $lc = Code $latest.tagName
  Write-Host "latest published: $($latest.tagName) (versionCode $lc)"
  if ($code -le $lc) { Fail "versionCode $code is not higher than the published $lc (Android refuses same/lower; clients would never see it)" }
} else { Write-Host "no published release yet" }
$bumped = $false
$text = [IO.File]::ReadAllText($info)
if ($text -notmatch '"version":\s*"' + [regex]::Escape($Version) + '"') {
  $new = [regex]::Replace($text, '("version":\s*")[^"]+(")', '${1}' + $Version + '${2}')
  [IO.File]::WriteAllText($info, $new, (New-Object Text.UTF8Encoding $false))
  $bumped = $true
  Write-Host "appinfo.json -> $Version"
}

Step "2/8 unit tests (WSL)"
$wslRoot = (wsl wslpath -a ($root -replace '\\', '/')).Trim()
$tests = @("atualizacao_ota", "openseek_guest", "openseek_pedido", "openseek_alt", "openseek_vtt", "openseek_osp1")
foreach ($t in $tests) {
  $o = wsl -e bash -c "cd '$wslRoot' && bash tests/$t.sh 2>&1 | tail -3"
  $o | ForEach-Object { Write-Host "  $_" }
  if (-not ($o -match "ok")) { if ($bumped) { git checkout deploy/app/appinfo.json }; Fail "test $t failed" }
}

Step "3/8 build (assembleSharing, dedicated key)"
$env:ANDROID_HOME = $SdkDir
$env:NUVIO_SHARING_KEYSTORE = Join-Path $KeyDir "sharing.jks"
$env:NUVIO_SHARING_PASS = (Get-Content (Join-Path $KeyDir "sharing.pass") -Raw).Trim()
Push-Location (Join-Path $root "android")
try {
  .\gradlew.bat assembleSharing --console=plain 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
  if ($LASTEXITCODE -ne 0) { Fail "gradle build failed" }
} finally { Pop-Location; Remove-Item Env:NUVIO_SHARING_PASS -ErrorAction SilentlyContinue }
$built = Join-Path $root "android\app\build\outputs\apk\sharing\app-sharing.apk"
if (-not (Test-Path $built)) { Fail "no APK produced" }

Step "4/8 verify the APK"
$outDir = Join-Path $root "build\release"
New-Item -ItemType Directory -Force $outDir | Out-Null
$asset = Join-Path $outDir "Nuvio-$Version-sharing.apk"
Copy-Item $built $asset -Force
$badging = & (Join-Path $bt "aapt2.exe") dump badging $asset | Out-String
if ($badging -notmatch "package: name='$([regex]::Escape($AppId))' versionCode='$code' versionName='$([regex]::Escape($Version))-sharing'") { Fail "package/versionCode mismatch:`n$(($badging -split "`n")[0])" }
if ($badging -match "application-debuggable") { Fail "the APK is debuggable" }
$sig = & (Join-Path $bt "apksigner.bat") verify --print-certs -v $asset 2>&1 | Out-String
if ($sig -notmatch "Verified using v2 scheme \(APK Signature Scheme v2\): true") { Fail "v2 signature not verified" }
if ($sig -notmatch "Signer #1 certificate SHA-256 digest: $PinnedCert") { Fail "signing certificate is NOT the pinned one (would not update installed copies)" }
if (([regex]::Matches($sig, "Signer #\d+ certificate DN")).Count -ne 1) { Fail "unexpected number of signers" }
$sha = (Get-FileHash $asset -Algorithm SHA256).Hash.ToLower()
Write-Host "  package $AppId, versionCode $code, v2 signature OK, certificate = pinned, not debuggable"
Write-Host "  SHA-256 $sha ($([math]::Round((Get-Item $asset).Length / 1MB, 1)) MB)"
if ($bumped) { git add deploy/app/appinfo.json; git commit -q -m "release: $Version (versionCode $code)"; Write-Host "  committed the version bump" }

if ($DryRun) {
  Step "DRY RUN: stopping before any GitHub change"
  Write-Host "would: gh release create v$Version --draft (asset $([IO.Path]::GetFileName($asset))), compare digest, publish, read releases/latest"
  if ($bumped) { git reset -q --hard HEAD~1; Write-Host "  (version bump commit undone)" }
  return
}

Step "5/8 draft release + asset (invisible to the app)"
$tag = "v$Version"
gh release create $tag $asset --repo $Repo --draft --title "Nuvio Legacy $Version" --notes-file $Notes | Out-Null
$rel = (gh api "repos/$Repo/releases" --paginate | ConvertFrom-Json) | Where-Object { $_.tag_name -eq $tag -and $_.draft } | Select-Object -First 1
if (-not $rel) { Fail "draft not found after creation" }
$a = $rel.assets | Where-Object { $_.name -eq "Nuvio-$Version-sharing.apk" } | Select-Object -First 1
if (-not $a) { gh release delete $tag --repo $Repo --yes | Out-Null; Fail "asset missing from the draft; draft deleted" }

Step "6/8 verify what GitHub holds"
$remote = ($a.digest -replace '^sha256:', '').ToLower()
if ($remote -ne $sha) { gh release delete $tag --repo $Repo --yes | Out-Null; Fail "GitHub's digest ($remote) differs from the local file ($sha); draft deleted" }
$tmp = Join-Path $env:TEMP "nuvio-release-check.apk"
gh api -H "Accept: application/octet-stream" "repos/$Repo/releases/assets/$($a.id)" > $tmp
if ((Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower() -ne $sha) { gh release delete $tag --repo $Repo --yes | Out-Null; Fail "re-downloaded asset differs; draft deleted" }
Remove-Item $tmp -Force
Write-Host "  asset digest and a fresh download both equal the local SHA-256"

Step "7/8 publish"
gh release edit $tag --repo $Repo --draft=false --latest | Out-Null

Step "8/8 read it like the app does (anonymous releases/latest)"
$pub = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ "User-Agent" = "nuvio-release-check" }
$pa = $pub.assets | Where-Object { $_.name -eq "Nuvio-$Version-sharing.apk" }
$okPub = ($pub.tag_name -eq $tag) -and $pa -and ($pa.browser_download_url -like "https://github.com/$Repo/releases/download/*") -and (($pa.digest -replace '^sha256:', '') -eq $sha)
if (-not $okPub) { gh release edit $tag --repo $Repo --draft=true | Out-Null; Fail "the public JSON did not match; reverted to draft" }
Write-Host "RELEASED $tag : $($pa.browser_download_url)" -ForegroundColor Green
