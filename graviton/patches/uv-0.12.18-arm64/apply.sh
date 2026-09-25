#!/bin/bash
# Fix Errno.impl Error + rlim_t::from(u32) casts, resume build.
set -euo pipefail

exec >>/boot/home/uv-build.log 2>&1
echo "==== uv-port v5 (uv-unix shim fixes) $(date -u) ===="

. /boot/system/data/profile.d/rust-devel.sh
export PATH=/boot/system/bin:/boot/system/non-packaged/bin:$HOME/.cargo/bin:$PATH
export CARGO_INCREMENTAL=0
export CARGO_PROFILE_RELEASE_DEBUG=0
export CARGO_BUILD_RUSTFLAGS="$CARGO_BUILD_RUSTFLAGS -C codegen-units=1"
export CARGO_BUILD_JOBS=4

cat > ~/uv/crates/uv-unix/src/resource_limits.rs <<'EOF'
//! Helper for adjusting Unix resource limits.  See upstream for context.

#[cfg(not(target_os = "haiku"))]
use nix::errno::Errno;
#[cfg(not(target_os = "haiku"))]
use nix::sys::resource::{RLIM_INFINITY, Resource, getrlimit, rlim_t, setrlimit};
use thiserror::Error;

#[cfg(target_os = "haiku")]
mod haiku_shim {
    use std::{error, fmt, io};

    pub type rlim_t = libc::rlim_t;
    pub const RLIM_INFINITY: rlim_t = libc::RLIM_INFINITY;

    #[derive(Debug, Copy, Clone)]
    pub struct Errno(pub i32);
    impl Errno {
        pub fn desc(&self) -> String {
            let s = match self.0 {
                libc::EPERM => "Operation not permitted",
                libc::ENOSYS => "Function not implemented",
                libc::EINVAL => "Invalid argument",
                _ => "unknown",
            };
            format!("{} ({})", s, self.0)
        }
        pub fn last() -> Self {
            Errno(io::Error::last_os_error().raw_os_error().unwrap_or(0))
        }
    }
    impl fmt::Display for Errno {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.desc())
        }
    }
    impl error::Error for Errno {}

    #[derive(Debug, Copy, Clone)]
    pub enum Resource {
        RLIMIT_NOFILE,
    }
    impl Resource {
        fn to_libc(self) -> libc::c_int {
            match self {
                Self::RLIMIT_NOFILE => libc::RLIMIT_NOFILE as libc::c_int,
            }
        }
    }

    pub fn getrlimit(res: Resource) -> Result<(rlim_t, rlim_t), Errno> {
        let mut limit = libc::rlimit { rlim_cur: 0, rlim_max: 0 };
        let rc = unsafe { libc::getrlimit(res.to_libc(), &mut limit) };
        if rc == 0 { Ok((limit.rlim_cur, limit.rlim_max)) } else { Err(Errno::last()) }
    }

    pub fn setrlimit(res: Resource, soft: rlim_t, hard: rlim_t) -> Result<(), Errno> {
        let limit = libc::rlimit { rlim_cur: soft, rlim_max: hard };
        let rc = unsafe { libc::setrlimit(res.to_libc(), &limit) };
        if rc == 0 { Ok(()) } else { Err(Errno::last()) }
    }
}

#[cfg(target_os = "haiku")]
use haiku_shim::{RLIM_INFINITY, Resource, getrlimit, rlim_t, setrlimit, Errno};

#[derive(Debug, Error)]
pub enum OpenFileLimitError {
    #[error("failed to get open file limit: {0}")]
    GetLimitFailed(#[source] Errno),

    #[error("encountered unexpected negative soft limit: {value}")]
    NegativeSoftLimit { value: rlim_t },

    #[error("soft limit ({current}) already meets the target ({target})")]
    AlreadySufficient { current: u64, target: u64 },

    #[error("requested open file limit ({target}) exceeds the hard limit ({hard})")]
    ExceedsHardLimit { target: u64, hard: rlim_t },

    #[error("failed to set open file limit from {current} to {target}: {source}")]
    SetLimitFailed { current: u64, target: u64, #[source] source: Errno },
}

const MAX_NOFILE_LIMIT: rlim_t = 0x0010_0000;

pub fn adjust_open_file_limit() -> Result<u64, OpenFileLimitError> {
    let (soft, hard) =
        getrlimit(Resource::RLIMIT_NOFILE).map_err(OpenFileLimitError::GetLimitFailed)?;

    let Some(soft) = rlim_t_to_u64(soft) else {
        return Err(OpenFileLimitError::NegativeSoftLimit { value: soft });
    };

    #[expect(clippy::unnecessary_cast)]
    let target = rlim_t_to_u64(hard.min(MAX_NOFILE_LIMIT)).unwrap_or(MAX_NOFILE_LIMIT as u64);

    if soft >= target {
        return Err(OpenFileLimitError::AlreadySufficient { current: soft, target });
    }

    let target_rlim = target as rlim_t;
    set_open_file_limit_to(soft, target, target_rlim, hard)
}

pub fn set_open_file_limit(target: u32) -> Result<u64, OpenFileLimitError> {
    let (soft, hard) =
        getrlimit(Resource::RLIMIT_NOFILE).map_err(OpenFileLimitError::GetLimitFailed)?;
    let Some(soft) = rlim_t_to_u64(soft) else {
        return Err(OpenFileLimitError::NegativeSoftLimit { value: soft });
    };

    // `rlim_t::from(u32)` is not available on Haiku (rlim_t = usize), so use
    // an `as` cast which is a no-op on any concrete rlim_t.
    let target_rlim: rlim_t = target as rlim_t;
    let target = u64::from(target);
    if hard != RLIM_INFINITY && target_rlim > hard {
        return Err(OpenFileLimitError::ExceedsHardLimit { target, hard });
    }

    set_open_file_limit_to(soft, target, target_rlim, hard)
}

fn set_open_file_limit_to(
    current: u64,
    target: u64,
    target_rlim: rlim_t,
    hard: rlim_t,
) -> Result<u64, OpenFileLimitError> {
    setrlimit(Resource::RLIMIT_NOFILE, target_rlim, hard).map_err(|err| {
        OpenFileLimitError::SetLimitFailed { current, target, source: err }
    })?;
    Ok(target)
}

#[expect(clippy::useless_conversion)]
fn rlim_t_to_u64(value: rlim_t) -> Option<u64> {
    u64::try_from(value).ok()
}
EOF

echo "== uv-unix shim updated =="
grep -n 'as rlim_t\|impl error::Error' ~/uv/crates/uv-unix/src/resource_limits.rs | head -5

cd ~/uv
echo "== cargo build --release --no-default-features -p uv (resume) =="
date
cargo build --release --no-default-features -p uv 2>&1
BUILD_RC=$?
echo "cargo build rc=$BUILD_RC"
date

if [ $BUILD_RC -eq 0 ]; then
    echo "== resulting uv binary =="
    ls -la target/release/uv
    file target/release/uv || true
    ./target/release/uv --version || true
fi

exit $BUILD_RC
