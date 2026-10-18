//! C locale and numeric conversion calls used by the platform boundary.
//! These resolve against the UCRT or MinGW CRT used by libobs.

use core::ffi::{CStr, c_char};

/// The leading member of the CRT's `struct lconv`.
#[repr(C)]
struct LconvPrefix {
    decimal_point: *mut c_char,
}

unsafe extern "C" {
    fn localeconv() -> *mut LconvPrefix;
}

/// First byte of `localeconv()->decimal_point`.
pub fn decimal_point() -> u8 {
    // SAFETY: localeconv returns a non-null pointer to a struct whose first
    // member is a non-null NUL-terminated decimal point string.
    unsafe { *(*localeconv()).decimal_point as u8 }
}

/// `strtod(s, NULL)`.
pub fn strtod(s: &CStr) -> f64 {
    // SAFETY: `s` is NUL-terminated for the duration of the call; a null
    // endptr is permitted by strtod.
    unsafe { libc::strtod(s.as_ptr(), core::ptr::null_mut()) }
}
