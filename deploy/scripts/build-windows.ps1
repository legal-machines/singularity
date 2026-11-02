$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
python (Join-Path $ScriptDirectory "singularity.py") release --platform windows @args
exit $LASTEXITCODE
