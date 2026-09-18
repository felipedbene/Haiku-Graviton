# Environment for the amazon-ssm-agent launch_daemon job (sourced via
# `env { from_script ... }`). launch_daemon parses `export VAR=...` lines.
#
# PATH must include the shell directory so ssm-document-worker can resolve `sh`
# for AWS-RunShellScript (Haiku's /bin/sh is a symlink to bash under
# /boot/system/bin). /boot/system/non-packaged/bin is included so any
# operator-dropped helper is reachable too.
export PATH=/boot/system/bin:/bin:/boot/system/non-packaged/bin
