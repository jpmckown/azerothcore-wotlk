$root = $PWD
$modules = Get-ChildItem -Path .\modules -Directory -ErrorAction SilentlyContinue

foreach ($module in $modules) {
    Set-Location -Path ([string]::Format("{0}\modules\{1}", $root, $module.Name))
    git.exe stash --include-untracked
    git.exe pull
    git.exe stash pop
}

Set-Location $root
