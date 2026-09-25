//! Helper for adjusting Unix resource limits.
//!
//! Linux has a historically low default limit of 1024 open file descriptors per process.
//! macOS also defaults to a low soft limit (typically 256), though its hard limit is much
//! higher. On modern multi-core machines, these low defaults can cause "too many open files"
//! errors because uv infers concurrency limits from CPU count and may schedule more concurrent
//! work than the default file descriptor limit allows.
//!
//! This module attempts to raise the soft limit to the hard limit at startup to avoid these
//! errors without requiring users to manually configure their shell's `ulimit` settings.
//! The raised limit is inherited by child processes, which is important for commands like
//! `uv run` that spawn Python interpreters.
//!
//! See: <https://github.com/astral-sh/uv/issues/16999>

#[cfg(not(target_os = "haiku"))]
use nix::errno::Errno;
#[cfg(not(target_os = "haiku"))]
use nix::sys::resource::{RLIM_INFINITY, Resource, getrlimit, rlim_t, setrlimit};
use thiserror::Error;

// Haiku shim: nix::sys::resource is not implemented for target_os = "haiku",
// so route the calls through libc directly.  Haiku's rlim_t is `usize`
// (unsigned, at least pointer-width).  Matches nix's public API surface.
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
    // thiserror derives `#[source]` support via std::error::Error.
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
        // SAFETY: `libc::rlimit` is POD and getrlimit writes both fields.
        let rc = unsafe { libc::getrlimit(res.to_libc(), &mut limit) };
        if rc == 0 { Ok((limit.rlim_cur, limit.rlim_max)) } else { Err(Errno::last()) }
    }

    pub fn setrlimit(res: Resource, soft: rlim_t, hard: rlim_t) -> Result<(), Errno> {
        let limit = libc::rlimit { rlim_cur: soft, rlim_max: hard };
        // SAFETY: `libc::rlimit` is POD.
        let rc = unsafe { libc::setrlimit(res.to_libc(), &limit) };
        if rc == 0 { Ok(()) } else { Err(Errno::last()) }
    }
}

#[cfg(target_os = "haiku")]
use haiku_shim::{RLIM_INFINITY, Resource, getrlimit, rlim_t, setrlimit, Errno};

/// Errors that can occur when adjusting resource limits.
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

/// Maximum file descriptor limit to request.
///
/// We cap at 0x100000 (1,048,576) to match the typical Linux default (`/proc/sys/fs/nr_open`)
/// and to avoid issues with extremely high limits.
///
/// Note: `rlim_t` is platform-specific (`u64` on Linux/macOS, `i64` on FreeBSD,
/// `usize` on Haiku).
const MAX_NOFILE_LIMIT: rlim_t = 0x0010_0000;

/// Attempt to raise the open file descriptor limit to the maximum allowed.
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

/// Set the soft open-file descriptor limit while preserving the hard limit.
pub fn set_open_file_limit(target: u32) -> Result<u64, OpenFileLimitError> {
    let (soft, hard) =
        getrlimit(Resource::RLIMIT_NOFILE).map_err(OpenFileLimitError::GetLimitFailed)?;
    let Some(soft) = rlim_t_to_u64(soft) else {
        return Err(OpenFileLimitError::NegativeSoftLimit { value: soft });
    };

    // `rlim_t::from(u32)` is not available on Haiku (rlim_t = usize); an `as`
    // cast is a no-op on the concrete rlim_t on every supported target.
    let target_rlim: rlim_t = target as rlim_t;
    let target = u64::from(target);
    if hard != RLIM_INFINITY && target_rlim > hard {
        return Err(OpenFileLimitError::ExceedsHardLimit { target, hard });
    }

    set_open_file_limit_to(soft, target, target_rlim, hard)
}

/// Update the soft open-file descriptor limit while preserving the hard limit.
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

/// Convert `rlim_t` to `u64`, returning `None` if negative.
#[expect(clippy::useless_conversion)]
fn rlim_t_to_u64(value: rlim_t) -> Option<u64> {
    u64::try_from(value).ok()
}
