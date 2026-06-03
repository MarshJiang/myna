use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MynaMode {
    Latin = 0,
    Pinyin = 1,
}

#[repr(C)]
pub struct MynaView {
    pub mode_label: *mut c_char,
    pub preedit: *mut c_char,
    pub candidate_count: usize,
    pub candidates: *mut *mut c_char,
}

#[repr(C)]
pub struct MynaAction {
    pub commit: *mut c_char,
    pub request_refresh: bool,
}

unsafe extern "C" {
    fn myna_rime_create() -> *mut c_void;
    fn myna_rime_destroy(handle: *mut c_void);
    fn myna_rime_process_key(handle: *mut c_void, key: c_int) -> bool;
    fn myna_rime_select_candidate(handle: *mut c_void, index: usize) -> bool;
    fn myna_rime_clear(handle: *mut c_void);
    fn myna_rime_take_commit(handle: *mut c_void) -> *mut c_char;
    fn myna_rime_get_preedit(handle: *mut c_void) -> *mut c_char;
    fn myna_rime_get_candidate_count(handle: *mut c_void) -> usize;
    fn myna_rime_get_candidate(handle: *mut c_void, index: usize) -> *mut c_char;
    fn myna_rime_is_composing(handle: *mut c_void) -> bool;
    fn myna_rime_string_free(value: *mut c_char);
}

pub struct Keyboard {
    mode: MynaMode,
    shifted: bool,
    rime: *mut c_void,
}

impl Drop for Keyboard {
    fn drop(&mut self) {
        if !self.rime.is_null() {
            unsafe { myna_rime_destroy(self.rime) };
        }
    }
}

fn cstring(text: &str) -> *mut c_char {
    CString::new(text).unwrap_or_default().into_raw()
}

unsafe fn take_optional_string(ptr: *mut c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }

    let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    myna_rime_string_free(ptr);
    Some(text)
}

fn action(commit: Option<String>, request_refresh: bool) -> *mut MynaAction {
    Box::into_raw(Box::new(MynaAction {
        commit: commit.map_or(ptr::null_mut(), |text| cstring(&text)),
        request_refresh,
    }))
}

#[no_mangle]
pub extern "C" fn myna_keyboard_new() -> *mut Keyboard {
    Box::into_raw(Box::new(Keyboard {
        mode: MynaMode::Pinyin,
        shifted: false,
        rime: unsafe { myna_rime_create() },
    }))
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_free(kbd: *mut Keyboard) {
    if !kbd.is_null() {
        drop(Box::from_raw(kbd));
    }
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_mode(kbd: *const Keyboard) -> MynaMode {
    kbd.as_ref().map(|kbd| kbd.mode).unwrap_or(MynaMode::Latin)
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_set_mode(kbd: *mut Keyboard, mode: MynaMode) -> *mut MynaAction {
    let Some(kbd) = kbd.as_mut() else {
        return action(None, false);
    };

    if kbd.mode != mode {
        if !kbd.rime.is_null() {
            myna_rime_clear(kbd.rime);
        }
        kbd.mode = mode;
    }

    action(None, true)
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_toggle_mode(kbd: *mut Keyboard) -> *mut MynaAction {
    let Some(kbd) = kbd.as_mut() else {
        return action(None, false);
    };

    let next = match kbd.mode {
        MynaMode::Latin => MynaMode::Pinyin,
        MynaMode::Pinyin => MynaMode::Latin,
    };

    myna_keyboard_set_mode(kbd, next)
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_press_label(
    kbd: *mut Keyboard,
    label: *const c_char,
) -> *mut MynaAction {
    let Some(kbd) = kbd.as_mut() else {
        return action(None, false);
    };
    if label.is_null() {
        return action(None, false);
    }

    let label = CStr::from_ptr(label).to_string_lossy();
    match label.as_ref() {
        "Shift" => {
            kbd.shifted = !kbd.shifted;
            return action(None, true);
        }
        "Space" => return handle_space(kbd),
        "Backspace" => return handle_backspace(kbd),
        "Enter" => return handle_enter(kbd),
        "Mode" => return myna_keyboard_toggle_mode(kbd),
        _ => {}
    }

    if label.chars().count() != 1 {
        return action(None, false);
    }

    let mut ch = label.chars().next().unwrap();
    if kbd.shifted {
        ch = ch.to_ascii_uppercase();
        kbd.shifted = false;
    }

    match kbd.mode {
        MynaMode::Latin => action(Some(ch.to_string()), true),
        MynaMode::Pinyin => {
            if ch.is_ascii_alphabetic() && !kbd.rime.is_null() {
                myna_rime_process_key(kbd.rime, ch.to_ascii_lowercase() as c_int);
                let commit = take_optional_string(myna_rime_take_commit(kbd.rime));
                action(commit, true)
            } else {
                action(Some(ch.to_string()), true)
            }
        }
    }
}

unsafe fn handle_space(kbd: &mut Keyboard) -> *mut MynaAction {
    match kbd.mode {
        MynaMode::Latin => action(Some(" ".to_string()), true),
        MynaMode::Pinyin if !kbd.rime.is_null() && myna_rime_is_composing(kbd.rime) => {
            myna_rime_process_key(kbd.rime, b' ' as c_int);
            let commit = take_optional_string(myna_rime_take_commit(kbd.rime));
            action(commit, true)
        }
        MynaMode::Pinyin => action(Some(" ".to_string()), true),
    }
}

unsafe fn handle_backspace(kbd: &mut Keyboard) -> *mut MynaAction {
    if kbd.mode == MynaMode::Pinyin && !kbd.rime.is_null() && myna_rime_is_composing(kbd.rime) {
        myna_rime_process_key(kbd.rime, 0xff08);
        let commit = take_optional_string(myna_rime_take_commit(kbd.rime));
        return action(commit, true);
    }

    action(Some("\u{8}".to_string()), true)
}

unsafe fn handle_enter(kbd: &mut Keyboard) -> *mut MynaAction {
    if kbd.mode == MynaMode::Pinyin && !kbd.rime.is_null() && myna_rime_is_composing(kbd.rime) {
        myna_rime_process_key(kbd.rime, b' ' as c_int);
        let commit = take_optional_string(myna_rime_take_commit(kbd.rime));
        return action(commit, true);
    }

    action(Some("\n".to_string()), true)
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_select_candidate(
    kbd: *mut Keyboard,
    index: usize,
) -> *mut MynaAction {
    let Some(kbd) = kbd.as_mut() else {
        return action(None, false);
    };

    if kbd.mode == MynaMode::Pinyin && !kbd.rime.is_null() {
        myna_rime_select_candidate(kbd.rime, index);
        let commit = take_optional_string(myna_rime_take_commit(kbd.rime));
        return action(commit, true);
    }

    action(None, false)
}

#[no_mangle]
pub unsafe extern "C" fn myna_keyboard_view(kbd: *mut Keyboard) -> *mut MynaView {
    let Some(kbd) = kbd.as_ref() else {
        return ptr::null_mut();
    };

    let mode_label = match kbd.mode {
        MynaMode::Latin => "EN",
        MynaMode::Pinyin => "拼",
    };

    let preedit = if kbd.mode == MynaMode::Pinyin && !kbd.rime.is_null() {
        myna_rime_get_preedit(kbd.rime)
    } else {
        ptr::null_mut()
    };

    let mut candidates = Vec::new();
    if kbd.mode == MynaMode::Pinyin && !kbd.rime.is_null() {
        let count = myna_rime_get_candidate_count(kbd.rime).min(8);
        for index in 0..count {
            let candidate = myna_rime_get_candidate(kbd.rime, index);
            if !candidate.is_null() {
                candidates.push(candidate);
            }
        }
    }

    let candidate_count = candidates.len();
    let candidates = if candidates.is_empty() {
        ptr::null_mut()
    } else {
        let mut boxed = candidates.into_boxed_slice();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
        ptr
    };

    Box::into_raw(Box::new(MynaView {
        mode_label: cstring(mode_label),
        preedit,
        candidate_count,
        candidates,
    }))
}

#[no_mangle]
pub unsafe extern "C" fn myna_view_free(view: *mut MynaView) {
    if view.is_null() {
        return;
    }

    let view = Box::from_raw(view);
    myna_string_free(view.mode_label);
    myna_rime_string_free(view.preedit);

    if !view.candidates.is_null() {
        let candidates = Vec::from_raw_parts(view.candidates, view.candidate_count, view.candidate_count);
        for candidate in candidates {
            myna_rime_string_free(candidate);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn myna_action_free(action: *mut MynaAction) {
    if action.is_null() {
        return;
    }

    let action = Box::from_raw(action);
    myna_string_free(action.commit);
}

#[no_mangle]
pub unsafe extern "C" fn myna_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}
