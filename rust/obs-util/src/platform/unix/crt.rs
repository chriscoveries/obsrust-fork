//! C locale and numeric conversion calls used by the platform boundary.

#![allow(unsafe_code)]

use std::ffi::CStr;

/// First byte of `localeconv()->decimal_point`.
pub fn decimal_point() -> u8 {
    // SAFETY: localeconv returns a non-null pointer to a struct whose
    // decimal_point member is a non-null NUL-terminated string.
    unsafe { *(*libc::localeconv()).decimal_point as u8 }
}

/// `strtod(s, NULL)`.
pub fn strtod(s: &CStr) -> f64 {
    // SAFETY: `s` is NUL-terminated for the duration of the call; a null
    // endptr is permitted by strtod.
    unsafe { libc::strtod(s.as_ptr(), std::ptr::null_mut()) }
}
