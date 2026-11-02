# haiku-bake-fetch.sh -- presign() and REMOTE_FETCH_PRELUDE, shared by
# haiku-bake-builder and haiku-builder-s3-client. SOURCE it after REGION is set:
#
#     . "$HERE/haiku-bake-fetch.sh"
#
# Moved out of haiku-bake-builder unchanged (issue 867) so the S3-client step the
# bake runs can also be run, and verified, on its own against any instance.
# --- getting files onto the box (issue 728) ----------------------------------
# Every staging step below used to be `/boot/system/bin/debeos-ssm-agent s3 cp`.
# Issue #302 replaced that interim custom agent (AgentVersion 0.4.0, which carried
# an `s3 cp` subcommand) with the real upstream amazon-ssm-agent, which has no S3
# verbs -- and the bake's hpkg pool stages `amazon_ssm_agent` only
# (pipeline/buildspecs/cross-build.yml). So a current canonical image has NO S3
# client of any kind, and every fetch here died with
#   /boot/system/bin/debeos-ssm-agent: No such file or directory
#   failed to run commands: exit status 127
# on line 2 of the first generated script.
#
# The fix presigns on the WORKSTATION, where the credentials already are, and
# curls on the box. Nothing on the box needs S3 access, no credential ever leaves
# this machine, and it works on the leanest possible image.
#
# NOT `pkgman install debeos_ssm_agent`, even though that 0.4.0 hpkg is still
# sitting in the green pool and would be the one-line fix: the package ships
# data/launch/, so baking it would start the retired agent on every subsequent
# boot of this AMI, where it would race the real agent for the instance's
# managed-node registration -- re-creating precisely the condition #302 removed.
# The long-term answer, packaging go-arm64/debeos-aws into the image, is what
# haiku-builder-s3-client now does on every builder bake (issue 867). Staging still
# goes through presigned curl, because the client is built from files staged here.
presign() {
  aws s3 presign "$1" --region "$REGION" --expires-in "${2:-10800}"
}

# Emitted at the top of every remote script that fetches. `fetch <url> <dest>`
# writes to a .part and moves it into place only on success AND non-empty, so a
# failed or truncated download never masquerades as a staged file -- the same
# rule haiku-srccache-fetch applies to source tarballs. Loud and fatal: the
# whole point of 728 is that a silent transfer failure is worse than a dead one.
#
# curl is NOT on the lean canonical image -- haiku-provision-native-builder
# installs it as part of its TOOLS set -- so bootstrap it here if absent. `feed`
# is the same non-interactive pkgman shape the provisioner uses: the lean base
# ships bootstrap "Haiku Project" libs that the DeBeOS repo supersedes, so
# pulling curl (-> lib:libz>=1.3.2) raises a numbered "allow vendor change"
# prompt that stays interactive even under `pkgman -y`.
REMOTE_FETCH_PRELUDE='
_feed() { i=0; while [ $i -lt 80 ]; do echo 1; i=$((i+1)); done; echo yes; }
CURL=/boot/system/bin/curl
if [ ! -x "$CURL" ]; then
  CURL=$(command -v curl 2>/dev/null || true)
fi
if [ -z "$CURL" ]; then
  echo "stage: no curl on this image; installing it from the repo (issue 728)" >&2
  _feed | pkgman refresh >/dev/null 2>&1 || true
  _feed | pkgman install curl >/dev/null 2>&1 || true
  CURL=/boot/system/bin/curl
fi
if [ ! -x "$CURL" ]; then
  echo "stage: FATAL: this image has no S3 client and no curl, so nothing can be" >&2
  echo "  staged onto it. See issue 728: the image carries amazon-ssm-agent," >&2
  echo "  which has no S3 verbs, and debeos-ssm-agent is gone." >&2
  exit 1
fi
fetch() {
  "$CURL" -fsSL --retry 3 --retry-delay 5 -o "$2.part" "$1" || {
    echo "stage: FATAL: could not fetch $2" >&2; rm -f "$2.part"; exit 1; }
  if [ ! -s "$2.part" ]; then
    echo "stage: FATAL: fetched 0 bytes for $2" >&2; rm -f "$2.part"; exit 1
  fi
  mv "$2.part" "$2"
}
'
