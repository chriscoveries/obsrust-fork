//! Safe core for `os_strtod` and `os_dtostr` (`libobs/util/platform-number.c`):
//! locale-independent conversion between `double` and its text form. The
//! config-file port reads and writes doubles through the same functions.

use crate::platform::{DoubleFormat, snprintf_double};

/// `os_strtod`: the value of the longest valid prefix of `s`, as `strtod`
/// reads it with `.` as the decimal point; 0 when nothing converts.
pub fn strtod(s: &[u8]) -> f64 {
    os_strtod(s)
}

/// `os_dtostr`: `value` as text that reads back to the same `double`, or
/// `None` when it does not fit a buffer of `size` bytes (NUL included).
///
/// The size checks follow C exactly, including their quirks: the `%.17g`
/// text is measured before the exponent is shortened, and appending `.0`
/// needs one byte more than it uses (`length + 3 >= size` fails).
pub fn dtostr(value: f64, size: usize) -> Option<Vec<u8>> {
    let printed = g_format(value, DoubleFormat::G17);
    if printed.len() >= size {
        return None;
    }
    if !printed.contains(&b'.') && !printed.contains(&b'e') && printed.len() + 3 >= size {
        return None;
    }
    Some(dtostr_text(value))
}

/* --------------------------------------------------------------------- */
/* strtod                                                                  */

fn is_c_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\x0b' | b'\x0c' | b'\r')
}

pub(crate) fn skip_space(b: &[u8], mut i: usize) -> usize {
    while i < b.len() && is_c_space(b[i]) {
        i += 1;
    }
    i
}

fn starts_ci(b: &[u8], pat: &[u8]) -> bool {
    b.len() >= pat.len() && b[..pat.len()].eq_ignore_ascii_case(pat)
}

/// `strtod`: parse the valid prefix like the C library. Covers leading
/// space, sign, `inf`/`infinity`, `nan(...)`, `0x` hex floats with `p`
/// exponent, and decimal floats with `e` exponent. No conversion -> 0.
fn parse_double(b: &[u8]) -> f64 {
    let mut i = skip_space(b, 0);
    let neg = match b.get(i) {
        Some(b'-') => {
            i += 1;
            true
        }
        Some(b'+') => {
            i += 1;
            false
        }
        _ => false,
    };
    let rest = &b[i..];
    let sign = if neg { -1.0 } else { 1.0 };
    if starts_ci(rest, b"infinity") {
        return sign * f64::INFINITY;
    }
    if starts_ci(rest, b"inf") {
        return sign * f64::INFINITY;
    }
    if starts_ci(rest, b"nan") {
        return if neg { -f64::NAN } else { f64::NAN };
    }
    if rest.len() >= 2 && rest[0] == b'0' && (rest[1] == b'x' || rest[1] == b'X') {
        // The previous bounded mantissa lost sticky bits beyond 128 bits,
        // and an ASCII-only separator check misses locale decimal points.
        // The host C runtime handles exact rounding and C's valid-prefix
        // rules.
        return crate::platform::os_strtod(b);
    }
    // decimal: int digits [. frac digits] [eE [sign] exp digits]
    let mut j = 0;
    let mut int_digits = 0usize;
    while j < rest.len() && rest[j].is_ascii_digit() {
        j += 1;
        int_digits += 1;
    }
    let mut frac_digits = 0usize;
    if rest.get(j) == Some(&b'.') {
        j += 1;
        while j < rest.len() && rest[j].is_ascii_digit() {
            j += 1;
            frac_digits += 1;
        }
    }
    if int_digits == 0 && frac_digits == 0 {
        return 0.0;
    }
    let mant_end = j;
    if matches!(rest.get(j), Some(b'e') | Some(b'E')) {
        let mut k = j + 1;
        if matches!(rest.get(k), Some(b'+') | Some(b'-')) {
            k += 1;
        }
        let exp_start = k;
        while k < rest.len() && rest[k].is_ascii_digit() {
            k += 1;
        }
        if k > exp_start {
            j = k;
        } else {
            j = mant_end;
        }
    }
    let text = std::str::from_utf8(&rest[..j]).unwrap_or("");
    text.parse::<f64>()
        .map_or(0.0, |v| if neg { -v } else { v })
}

/// Size of the local buffer in C's `os_strtod` (`libobs/util/platform-number.c`,
/// `char buf[64]`). It copies at most `sizeof(buf) - 1` bytes of the string.
const OS_STRTOD_BUF: usize = 64;

/// `os_strtod`: C copies at most `OS_STRTOD_BUF - 1` (63) bytes into its
/// buffer before calling `strtod`, so everything past byte 63 is ignored.
/// The cut comes first, on the whole stored string: leading whitespace and
/// the sign count against the window, and it applies to hex floats as well
/// as decimals, so it must stay ahead of `parse_double`'s own dispatch.
fn os_strtod(b: &[u8]) -> f64 {
    parse_double(&b[..b.len().min(OS_STRTOD_BUF - 1)])
}

/* --------------------------------------------------------------------- */
/* printf-format helpers                                                   */

/// `snprintf("%.*g", prec, v)` pieces: the `%.{prec-1}e` mantissa/exponent,
/// deciding `%e` vs `%f` by C's rule (exp < -4 or >= prec), then stripped
/// of trailing zeros.
fn g_parts(v: f64, prec: usize) -> (String, i64, bool) {
    let p = prec.max(1);
    let sci = format!("{:.*e}", p - 1, v);
    let epos = sci.find('e').expect("e-format always has e");
    let exp: i64 = sci[epos + 1..].parse().expect("e-format exponent");
    if exp < -4 || exp >= p as i64 {
        (strip_zeros(&sci[..epos]).to_owned(), exp, true)
    } else {
        let decimals = (p as i64 - 1 - exp).max(0) as usize;
        (
            strip_zeros(&format!("{:.*}", decimals, v)).to_owned(),
            exp,
            false,
        )
    }
}

/// `snprintf(format, v)` for `format` (`%.17g` or `%g`): `%e`/`%f` chosen
/// by the C rule, trailing zeros and a bare `.` stripped, exponent signed
/// with at least two digits (`1e+06`, `1e-05`). `inf` and `nan` come from
/// the C library itself, because their spelling is the libc's choice: the
/// sign of a NaN (glibc `-nan`, Apple libc `nan`) and UCRT's
/// `-nan(ind)` and `nan(snan)`.
pub(crate) fn g_format(v: f64, format: DoubleFormat) -> Vec<u8> {
    if !v.is_finite() {
        return snprintf_double(format, v);
    }
    let (mant, exp, e_form) = g_parts(v, format.precision());
    if e_form {
        format!(
            "{}e{}{:02}",
            mant,
            if exp < 0 { '-' } else { '+' },
            exp.abs()
        )
        .into_bytes()
    } else {
        mant.into_bytes()
    }
}

/// `%g` zero stripping: drop trailing zeros after a `.`, then a bare `.`.
fn strip_zeros(s: &str) -> &str {
    if !s.contains('.') {
        return s;
    }
    let s = s.trim_end_matches('0');
    s.strip_suffix('.').unwrap_or(s)
}

/// `os_dtostr`: `%.17g`, `.0` appended when no `.`/`e` (so `inf.0`, `-inf.0`
/// and `nan.0` too), then the C exponent normalization: the `+` and
/// leading zeros are removed from the exponent (`1e+18` -> `1e18`,
/// `1.5e-05` -> `1.5e-5`), so the value reads back unchanged (#754).
fn dtostr_text(v: f64) -> Vec<u8> {
    let mut s = g_format(v, DoubleFormat::G17);
    if !s.contains(&b'.') && !s.contains(&b'e') {
        s.extend_from_slice(b".0");
    }
    let Some(ei) = s.iter().position(|&b| b == b'e') else {
        return s;
    };
    let mut start = ei + 1;
    let mut end = start + 1;
    if s[start] == b'-' {
        start += 1;
    }
    while end < s.len() && s[end] == b'0' {
        end += 1;
    }
    if end != start {
        s.drain(start..end);
    }
    s
}
