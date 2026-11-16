# Builds the portable Windows package: dist/OpenSourcePedal-win64.zip
# Run after `cmake --build --preset release` (from a VS 2022 developer shell), at the repository root.
# The checkout needs the Git LFS files (`git lfs pull`), because the app needs the real IRs and DI clip.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$art = "$root\build\release\app\OpenSourcePedal_artefacts\Release"
$dist = "$root\dist"
$pkg = "$dist\OpenSourcePedal"
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$pkg\docs", "$pkg\presets" | Out-Null
Copy-Item "$art\Standalone\OpenSourcePedal.exe" $pkg
Copy-Item "$root\build\release\tools\osp-tonematch.exe" $pkg
Copy-Item "$root\assets" "$pkg\assets" -Recurse
Copy-Item "$root\presets\factory" "$pkg\presets\factory" -Recurse
Copy-Item "$art\VST3" "$pkg\VST3" -Recurse
Get-ChildItem "$pkg\VST3" -File | Where-Object Extension -in '.lib', '.exp', '.pdb' | Remove-Item
Copy-Item "$root\docs\guides\recording-a-di.md" "$pkg\docs"
Copy-Item "$root\docs\guides\quickstart.md" "$pkg\QUICKSTART.md"
Compress-Archive -Path $pkg -DestinationPath "$dist\OpenSourcePedal-win64.zip" -Force
"$dist\OpenSourcePedal-win64.zip"
