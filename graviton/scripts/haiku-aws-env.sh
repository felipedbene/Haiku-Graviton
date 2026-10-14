# haiku-aws-env.sh -- resolve this account's AWS credentials, once, the same way
# in every tool here. SOURCE it, do not execute it:
#
#     . "$HERE/haiku-aws-env.sh"
#
# POSIX sh on purpose: haiku-publish-driver is #!/bin/sh. No bashisms.
#
# WHY THIS EXISTS. The account's credentials do not live where plain `aws` looks
# by default. They arrive either in the project profile (a self-refreshing
# `credential_process`, or whatever `ada credentials update --profile
# haiku-graviton` last wrote) or, after an `ada credentials update` with no
# --profile, in the ambient `default` profile. Three tools (haiku-status,
# haiku-launch, haiku-ssh) grew their own copy of a fix for the first case;
# seven others had no fix at all and reported "no AWS credentials" while valid
# credentials sat one variable away. On 2026-10-14 that cost a maintainer four
# failed commands in a row -- `haiku-bake-lock release` insisting there were no
# credentials when `aws --profile haiku-graviton sts get-caller-identity`
# answered immediately.
#
# The ordering below is the part that matters, and it is NOT what the three
# inline copies did. They adopted the project profile whenever AWS_PROFILE and
# AWS_ACCESS_KEY_ID were both unset -- which overrides a *working* ambient
# default with a project profile that may be stale, i.e. it breaks the case
# where someone just ran `ada credentials update` without --profile. So:
#
#   1. caller already chose (AWS_PROFILE or AWS_ACCESS_KEY_ID set) -> obey it;
#   2. ambient credentials already work                            -> leave them;
#   3. the project profile resolves                                -> adopt it;
#   4. nothing resolves -> say nothing. The caller's own credential check is
#      what reports the failure, in the caller's own words.
#
# Never fatal, never prints on success, and makes at most two STS calls.
# Override the profile name with HAIKU_GRAVITON_PROFILE.

haiku_aws_env() {
	if [ -n "${AWS_PROFILE:-}" ] || [ -n "${AWS_ACCESS_KEY_ID:-}" ]; then
		return 0
	fi

	if timeout 12 aws sts get-caller-identity --query Account --output text \
			>/dev/null 2>&1; then
		return 0
	fi

	_hae_profile="${HAIKU_GRAVITON_PROFILE:-haiku-graviton}"
	if timeout 12 aws --profile "$_hae_profile" sts get-caller-identity \
			--query Account --output text >/dev/null 2>&1; then
		AWS_PROFILE="$_hae_profile"
		export AWS_PROFILE
	fi
	unset _hae_profile
	return 0
}

haiku_aws_env
