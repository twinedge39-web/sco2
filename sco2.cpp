// SCO2 (すこすこ) - New Scouter GUI & Self-Test
// Preserves the experimental measurement routine from scouter_repro.cpp.
// Build (MSVC x64): cl /nologo /O2 /EHsc /utf-8 /std:c++17 /MT sco2.cpp user32.lib gdi32.lib shell32.lib /link /SUBSYSTEM:WINDOWS /Fe:sco2.exe

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <limits>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>

// ============================================================================
// 1. Measurement Core (Preserved verbatim from NewSCO/scouter_repro.cpp)
// ============================================================================

static volatile uint32_t sink = 0;
static void original_workload(int target) {
    uint32_t ebx=1, r8=10, r11=4, edx=6, ecx=7, r9=1, r10=0;
    while (r10 < (uint32_t)target) {
        if (r9 >= ebx && ((uint32_t)(r9+1)==edx || (uint32_t)(edx+1)==r9)) {
            ++r10;
            if (r10 >= (uint32_t)target) break;
        }
        ecx += 2; ++r11; edx += ecx;
        if (edx >= r8) {
            edx -= r8; ++r9;
            while (edx >= r8) {edx -= r8; ++r9;}
        }
        if (r11 == r8) {
            r9 = ebx; ebx = r8; r8 *= 10;
        }
    }
    sink = r10;
}

struct MeasurementResult {
    bool valid = false;
    double elapsed = 0.0;
    double seconds = 0.0;
    double score = 0.0;
    std::wstring error_message;
};

// Evaluates score from elapsed time using the exact formula from scouter_repro.cpp,
// while safely rejecting <= 0 intervals, NaN/Infinity, and abnormal huge numbers.
static MeasurementResult evaluate_elapsed_time(double elapsed) {
    MeasurementResult res;
    res.elapsed = elapsed;
    if (!std::isfinite(elapsed)) {
        res.valid = false;
        res.error_message = L"測定不能：経過時間が不正（NaN/Infinity）です。再計測してください。";
        return res;
    }

    const double seconds = elapsed - 1.0 + 0.0000037;
    res.seconds = seconds;
    if (seconds <= 0.0 || !std::isfinite(seconds)) {
        res.valid = false;
        res.error_message = L"測定不能：補正後計測時間が 0 以下または不正です。再計測してください。";
        return res;
    }

    const double x = std::fabs(7.0 / seconds + 10.0);
    const double base = std::pow(x, 5.0) / 53.0;
    res.score = base;

    if (!std::isfinite(base) || base <= 0.0) {
        res.valid = false;
        res.error_message = L"測定不能：計算結果が数値範囲外（オーバーフロー等）です。再計測してください。";
        return res;
    }

    // In scouter_repro.cpp: base > 1000000000.0 is flagged as inconsistent timing/loop behavior.
    // Also if elapsed <= 1.0 (sleep returned early / zero workload time), base explodes (~3.29e32).
    if (elapsed <= 1.0 || base > 1000000000.0) {
        res.valid = false;
        res.error_message = L"測定不能：計測時間の異常による異常な巨大値が検出されました。再計測してください。";
        return res;
    }

    res.valid = true;
    return res;
}

static MeasurementResult run_single_measurement() {
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    original_workload(21);
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();
    return evaluate_elapsed_time(elapsed);
}

// ============================================================================
// 2. Rank Data (sco2.txt) Parser and Threshold Judgment
// ============================================================================

struct RankEntry {
    int64_t threshold = 0;
    std::wstring name;
};

struct RankTable {
    bool ok = false;
    std::vector<RankEntry> entries;
    std::wstring error_message;
};

static std::wstring get_exe_dir_sco2_txt_path() {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    while (true) {
        len = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return L"sco2.txt";
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    size_t pos = buf.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"sco2.txt";
    }
    return buf.substr(0, pos + 1) + L"sco2.txt";
}

static bool utf8_to_wstring_strict(const std::string& bytes, std::wstring& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    int wlen = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()),
        nullptr, 0);
    if (wlen <= 0) {
        return false;
    }
    out.resize(wlen);
    int res = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()),
        &out[0], wlen);
    return res == wlen;
}

static RankTable parse_rank_table_from_bytes(const std::string& raw_bytes) {
    RankTable table;

    // Strip UTF-8 BOM if present
    size_t offset = 0;
    if (raw_bytes.size() >= 3 &&
        static_cast<unsigned char>(raw_bytes[0]) == 0xEF &&
        static_cast<unsigned char>(raw_bytes[1]) == 0xBB &&
        static_cast<unsigned char>(raw_bytes[2]) == 0xBF) {
        offset = 3;
    }

    std::string content = raw_bytes.substr(offset);
    std::wstring wcontent;
    if (!utf8_to_wstring_strict(content, wcontent)) {
        table.ok = false;
        table.error_message = L"sco2.txt の文字コードが不正です（UTF-8 で保存してください）。";
        return table;
    }

    size_t line_start = 0;
    int line_num = 0;
    while (line_start <= wcontent.size()) {
        size_t line_end = wcontent.find(L'\n', line_start);
        std::wstring line;
        if (line_end == std::wstring::npos) {
            line = wcontent.substr(line_start);
            line_start = wcontent.size() + 1;
        } else {
            line = wcontent.substr(line_start, line_end - line_start);
            line_start = line_end + 1;
        }
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        ++line_num;

        // Check blank or comment line
        size_t first_non_space = line.find_first_not_of(L" \t");
        if (first_non_space == std::wstring::npos) {
            continue; // Blank line ignored
        }
        if (line[first_non_space] == L'#') {
            continue; // Comment line ignored
        }

        // Must contain at least one TAB
        size_t tab_pos = line.find(L'\t');
        if (tab_pos == std::wstring::npos) {
            table.ok = false;
            table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目にタブ区切りがありません。";
            return table;
        }

        std::wstring num_part = line.substr(0, tab_pos);
        // Trim ASCII spaces around number
        size_t num_start = num_part.find_first_not_of(L' ');
        size_t num_end = num_part.find_last_not_of(L' ');
        if (num_start == std::wstring::npos) {
            table.ok = false;
            table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の下限値が空です。";
            return table;
        }
        num_part = num_part.substr(num_start, num_end - num_start + 1);

        // Validate integer digits
        int64_t threshold = 0;
        for (wchar_t ch : num_part) {
            if (ch < L'0' || ch > L'9') {
                table.ok = false;
                table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の下限値が正の整数ではありません。";
                return table;
            }
            int digit = ch - L'0';
            if (threshold > (std::numeric_limits<int64_t>::max() - digit) / 10) {
                table.ok = false;
                table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の下限値が大きすぎます。";
                return table;
            }
            threshold = threshold * 10 + digit;
        }

        // Skip one or more tabs
        size_t name_start = line.find_first_not_of(L'\t', tab_pos);
        if (name_start == std::wstring::npos) {
            table.ok = false;
            table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の階級名が空です。";
            return table;
        }
        std::wstring name_part = line.substr(name_start);
        // Trim leading/trailing spaces or tabs from rank name
        size_t ns = name_part.find_first_not_of(L" \t");
        size_t ne = name_part.find_last_not_of(L" \t");
        if (ns == std::wstring::npos) {
            table.ok = false;
            table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の階級名が空です。";
            return table;
        }
        name_part = name_part.substr(ns, ne - ns + 1);

        // Enforce strict ascending order
        if (!table.entries.empty()) {
            int64_t prev = table.entries.back().threshold;
            if (threshold == prev) {
                table.ok = false;
                table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の下限値（" + std::to_wstring(threshold) + L"）が重複しています。";
                return table;
            }
            if (threshold < prev) {
                table.ok = false;
                table.error_message = L"sco2.txt の " + std::to_wstring(line_num) + L" 行目の下限値（" + std::to_wstring(threshold) + L"）が昇順になっていません。";
                return table;
            }
        }

        table.entries.push_back(RankEntry{threshold, name_part});
    }

    if (table.entries.empty()) {
        table.ok = false;
        table.error_message = L"sco2.txt に有効な階級データが含まれていません。";
        return table;
    }

    table.ok = true;
    return table;
}

static RankTable load_rank_table_from_file(const std::wstring& path) {
    RankTable table;
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        table.ok = false;
        table.error_message = L"sco2.txt が見つからないか開けません（sco2.exe と同じフォルダに配置してください）。";
        return table;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(hFile, &file_size) || file_size.QuadPart < 0 || file_size.QuadPart > 10 * 1024 * 1024) {
        CloseHandle(hFile);
        table.ok = false;
        table.error_message = L"sco2.txt のファイルサイズが不正です。";
        return table;
    }

    std::string raw_bytes(static_cast<size_t>(file_size.QuadPart), '\0');
    DWORD bytes_read = 0;
    BOOL read_ok = TRUE;
    if (!raw_bytes.empty()) {
        read_ok = ReadFile(hFile, &raw_bytes[0], static_cast<DWORD>(raw_bytes.size()), &bytes_read, nullptr);
    }
    CloseHandle(hFile);

    if (!read_ok || bytes_read != raw_bytes.size()) {
        table.ok = false;
        table.error_message = L"sco2.txt の読み込みに失敗しました。";
        return table;
    }

    return parse_rank_table_from_bytes(raw_bytes);
}

static std::wstring append_kyu_once(const std::wstring& rank_name) {
    if (!rank_name.empty() && rank_name.back() == L'級') {
        return rank_name;
    }
    return rank_name + L" 級";
}

// Returns the raw matched rank name (or "判定対象外") for testing/inspection
static std::wstring judge_rank_raw_name(const RankTable& table, double score, bool* out_exceeded_max = nullptr) {
    if (out_exceeded_max) *out_exceeded_max = false;
    if (!table.ok || table.entries.empty() || !std::isfinite(score)) {
        return L"";
    }
    if (score < static_cast<double>(table.entries.front().threshold)) {
        return L"判定対象外";
    }
    // Find largest threshold <= score
    auto it = std::upper_bound(
        table.entries.begin(), table.entries.end(), score,
        [](double val, const RankEntry& entry) {
            return val < static_cast<double>(entry.threshold);
        });
    if (it == table.entries.begin()) {
        return L"判定対象外";
    }
    --it;
    if (it == table.entries.end() - 1 && score > static_cast<double>(it->threshold)) {
        if (out_exceeded_max) *out_exceeded_max = true;
    }
    return it->name;
}

// Formats the rank line for GUI display: e.g. 《 ナメック星を揺るがす強者 級 》
static std::wstring format_rank_display(const RankTable& table, double score) {
    if (!table.ok) {
        return L"※ " + table.error_message;
    }
    if (score < static_cast<double>(table.entries.front().threshold)) {
        return L"《 判定対象外 》";
    }
    bool exceeded_max = false;
    std::wstring name = judge_rank_raw_name(table, score, &exceeded_max);
    std::wstring with_kyu = append_kyu_once(name);
    if (exceeded_max) {
        return L"《 " + with_kyu + L" 》（測定限界超過）";
    }
    return L"《 " + with_kyu + L" 》";
}

// Formats score with comma separators and 2 decimal places (e.g. 397,449.49)
static std::wstring format_score_number(double score) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2) << score;
    std::wstring s = oss.str();
    size_t dot = s.find(L'.');
    std::wstring int_part = (dot == std::wstring::npos) ? s : s.substr(0, dot);
    std::wstring frac_part = (dot == std::wstring::npos) ? L"" : s.substr(dot);

    std::wstring formatted_int;
    int count = 0;
    for (auto it = int_part.rbegin(); it != int_part.rend(); ++it) {
        if (*it == L'-') {
            formatted_int.push_back(*it);
            continue;
        }
        if (count > 0 && count % 3 == 0) {
            formatted_int.push_back(L',');
        }
        formatted_int.push_back(*it);
        ++count;
    }
    std::reverse(formatted_int.begin(), formatted_int.end());
    return formatted_int + frac_part;
}

// ============================================================================
// 3. Win32 Lightweight GUI
// ============================================================================

static constexpr UINT WM_APP_MEASURE_DONE = WM_APP + 1;
static constexpr int IDC_MEASURE_BUTTON = 1001;

struct AppState {
    HWND hwnd = nullptr;
    HWND hButton = nullptr;
    HFONT hFontMain = nullptr;
    HFONT hFontRank = nullptr;
    HFONT hFontButton = nullptr;
    bool measuring = false;
    bool has_result = false;
    int completed_runs = 0;
    std::wstring line1;
    std::wstring line2;
    RankTable rank_table;
    std::mutex result_mutex;
    MeasurementResult latest_result;
};

static AppState g_app;

static int scale_dpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

static UINT get_window_dpi(HWND hwnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(hUser32, "GetDpiForWindow"));
        if (fn && hwnd) {
            UINT dpi = fn(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = 96;
    if (hdc) {
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hwnd, hdc);
    }
    return dpi > 0 ? dpi : 96;
}

static void enable_dpi_awareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
        using SetProcessDPIAwareFn = BOOL(WINAPI*)();
        auto fnLegacy = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(hUser32, "SetProcessDPIAware"));
        if (fnLegacy) {
            fnLegacy();
        }
    }
}

static void recreate_fonts(HWND hwnd) {
    if (g_app.hFontMain) DeleteObject(g_app.hFontMain);
    if (g_app.hFontRank) DeleteObject(g_app.hFontRank);
    if (g_app.hFontButton) DeleteObject(g_app.hFontButton);

    UINT dpi = get_window_dpi(hwnd);
    g_app.hFontMain = CreateFontW(
        -scale_dpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
    g_app.hFontRank = CreateFontW(
        -scale_dpi(15, dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
    g_app.hFontButton = CreateFontW(
        -scale_dpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");

    if (g_app.hButton && g_app.hFontButton) {
        SendMessageW(g_app.hButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontButton), TRUE);
    }
}

static void layout_controls(HWND hwnd) {
    if (!g_app.hButton) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    UINT dpi = get_window_dpi(hwnd);

    int btn_w = scale_dpi(130, dpi);
    int btn_h = scale_dpi(34, dpi);
    int btn_x = (cw - btn_w) / 2;
    int btn_y = g_app.has_result || !g_app.rank_table.ok || g_app.measuring
        ? ch - btn_h - scale_dpi(22, dpi)
        : (ch - btn_h) / 2;

    MoveWindow(g_app.hButton, btn_x, btn_y, btn_w, btn_h, TRUE);
}

static void start_measurement(HWND hwnd) {
    if (g_app.measuring) return; // Prevent double execution

    g_app.measuring = true;
    g_app.has_result = true;
    g_app.line1 = L"測定中…";
    g_app.line2 = L"しばらくお待ちください";

    EnableWindow(g_app.hButton, FALSE);
    SetWindowTextW(g_app.hButton, L"測定中…");
    layout_controls(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);

    std::thread([hwnd]() {
        MeasurementResult res = run_single_measurement();
        {
            std::lock_guard<std::mutex> lock(g_app.result_mutex);
            g_app.latest_result = res;
        }
        if (IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_APP_MEASURE_DONE, 0, 0);
        }
    }).detach();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;
        g_app.rank_table = load_rank_table_from_file(get_exe_dir_sco2_txt_path());
        if (!g_app.rank_table.ok) {
            g_app.line1 = L"※ sco2.txt の読み込みエラー";
            g_app.line2 = g_app.rank_table.error_message;
        }

        g_app.hButton = CreateWindowExW(
            0, L"BUTTON", L"計測する",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 120, 32,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MEASURE_BUTTON)),
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance, nullptr);

        recreate_fonts(hwnd);
        layout_controls(hwnd);
        SetFocus(g_app.hButton);
        return 0;
    }

    case WM_SIZE:
        layout_controls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_DPICHANGED: {
        recreate_fonts(hwnd);
        const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
        if (prcNewWindow) {
            SetWindowPos(
                hwnd, nullptr,
                prcNewWindow->left, prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout_controls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_MEASURE_BUTTON && HIWORD(wParam) == BN_CLICKED) {
            start_measurement(hwnd);
            return 0;
        }
        break;

    case WM_APP_MEASURE_DONE: {
        MeasurementResult res;
        {
            std::lock_guard<std::mutex> lock(g_app.result_mutex);
            res = g_app.latest_result;
        }
        // Reload sco2.txt from EXE directory on completion so any fix is reflected
        g_app.rank_table = load_rank_table_from_file(get_exe_dir_sco2_txt_path());

        g_app.measuring = false;
        g_app.has_result = true;
        ++g_app.completed_runs;

        if (!res.valid) {
            g_app.line1 = L"測定不能";
            g_app.line2 = res.error_message;
        } else {
            g_app.line1 = L"このPCの戦闘力は " + format_score_number(res.score) + L" です。";
            g_app.line2 = format_rank_display(g_app.rank_table, res.score);
        }

        EnableWindow(g_app.hButton, TRUE);
        SetWindowTextW(g_app.hButton, L"計測する");
        layout_controls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        SetFocus(g_app.hButton);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));

        if (g_app.has_result || !g_app.rank_table.ok || g_app.measuring) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

            UINT dpi = get_window_dpi(hwnd);
            int pad_x = scale_dpi(14, dpi);

            RECT rcLine1 = rc;
            rcLine1.left += pad_x;
            rcLine1.right -= pad_x;
            rcLine1.top = scale_dpi(26, dpi);
            rcLine1.bottom = scale_dpi(54, dpi);

            HGDIOBJ oldFont = SelectObject(hdc, g_app.hFontMain);
            DrawTextW(hdc, g_app.line1.c_str(), -1, &rcLine1,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT rcLine2 = rc;
            rcLine2.left += pad_x;
            rcLine2.right -= pad_x;
            rcLine2.top = scale_dpi(56, dpi);
            rcLine2.bottom = scale_dpi(104, dpi);

            SelectObject(hdc, g_app.hFontRank);
            DrawTextW(hdc, g_app.line2.c_str(), -1, &rcLine2,
                      DT_CENTER | DT_TOP | DT_WORDBREAK);

            SelectObject(hdc, oldFont);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (g_app.hFontMain) { DeleteObject(g_app.hFontMain); g_app.hFontMain = nullptr; }
        if (g_app.hFontRank) { DeleteObject(g_app.hFontRank); g_app.hFontRank = nullptr; }
        if (g_app.hFontButton) { DeleteObject(g_app.hFontButton); g_app.hFontButton = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// 4. Built-in Self-Test & CLI Mode (--self-test / --cli)
// ============================================================================

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &out[0], len, nullptr, nullptr);
    return out;
}

static void write_stdout_utf8(const std::wstring& msg) {
    std::string u8 = wstring_to_utf8(msg);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
    }
}

static HWND create_main_window(HINSTANCE hInstance, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"SCO2_MainWindowClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wc);

    UINT dpi = get_window_dpi(nullptr);
    int client_w = scale_dpi(340, dpi);
    int client_h = scale_dpi(165, dpi);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, client_w, client_h};
    AdjustWindowRect(&rc, style, FALSE);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - win_w) / 2;
    int pos_y = (screen_h - win_h) / 2;

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"SCO2 - すこすこ",
        style,
        pos_x, pos_y, win_w, win_h,
        nullptr, nullptr, hInstance, nullptr);

    if (hwnd) {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

static int run_self_test(HINSTANCE hInstance) {
    bool all_passed = true;
    auto check = [&](bool cond, const std::wstring& desc) {
        if (cond) {
            write_stdout_utf8(L"[PASS] " + desc + L"\n");
        } else {
            write_stdout_utf8(L"[FAIL] " + desc + L"\n");
            all_passed = false;
        }
    };

    // 1. Load sco2.txt next to sco2.exe
    std::wstring path = get_exe_dir_sco2_txt_path();
    RankTable table = load_rank_table_from_file(path);
    check(table.ok, L"Load sco2.txt from EXE directory (" + path + L"), entries=" + std::to_wstring(table.entries.size()));
    if (!table.ok) {
        write_stdout_utf8(L"  Error: " + table.error_message + L"\n");
        return 1;
    }

    // 2. Boundary value checks specified in section 7
    struct BoundaryCase {
        double score;
        const wchar_t* expected_raw;
        const wchar_t* expected_display;
    };
    const BoundaryCase cases[] = {
        { 4.0,         L"判定対象外",                   L"《 判定対象外 》" },
        { 5.0,         L"一般人（農夫）",               L"《 一般人（農夫） 級 》" },
        { 299999.0,    L"ギニュー特戦隊を超えた戦士",   L"《 ギニュー特戦隊を超えた戦士 級 》" },
        { 300000.0,    L"フリーザ軍の想定外",           L"《 フリーザ軍の想定外 級 》" },
        { 309296.0,    L"フリーザ軍の想定外",           L"《 フリーザ軍の想定外 級 》" },
        { 349999.0,    L"フリーザ軍の想定外",           L"《 フリーザ軍の想定外 級 》" },
        { 350000.0,    L"ナメック星を揺るがす強者",     L"《 ナメック星を揺るがす強者 級 》" },
        { 397449.0,    L"ナメック星を揺るがす強者",     L"《 ナメック星を揺るがす強者 級 》" },
        { 399999.0,    L"ナメック星を揺るがす強者",     L"《 ナメック星を揺るがす強者 級 》" },
        { 400000.0,    L"フリーザが警戒する存在",       L"《 フリーザが警戒する存在 級 》" },
        { 529999.0,    L"フリーザ第1形態・射程圏内",    L"《 フリーザ第1形態・射程圏内 級 》" },
        { 530000.0,    L"フリーザ第1形態",             L"《 フリーザ第1形態 級 》" },
        { 530001.0,    L"フリーザ第1形態",             L"《 フリーザ第1形態 級 》" },
        { 149999999.0, L"フリーザ最終形態100%",         L"《 フリーザ最終形態100% 級 》" },
        { 150000000.0, L"超サイヤ人孫悟空（フリーザ編）", L"《 超サイヤ人孫悟空（フリーザ編） 級 》" },
        { 150000001.0, L"超サイヤ人孫悟空（フリーザ編）", L"《 超サイヤ人孫悟空（フリーザ編） 級 》（測定限界超過）" },
    };

    for (const auto& c : cases) {
        std::wstring actual_raw = judge_rank_raw_name(table, c.score);
        std::wstring actual_disp = format_rank_display(table, c.score);
        bool ok = (actual_raw == c.expected_raw) && (actual_disp == c.expected_display);
        check(ok, L"Boundary " + format_score_number(c.score) + L" -> " + actual_disp);
    }

    // 3. UTF-8 BOM & Comment / Multiple TAB / Duplicate / Corrupted line tests
    {
        std::string bom_data = "\xEF\xBB\xBF# comment line\r\n\r\n10\t\tテスト戦士\r\n100\t上級戦士級\r\n";
        RankTable t_bom = parse_rank_table_from_bytes(bom_data);
        check(t_bom.ok && t_bom.entries.size() == 2, L"UTF-8 with BOM, comments, blank lines, and multi-TAB");
        check(format_rank_display(t_bom, 15.0) == L"《 テスト戦士 級 》", L"Rank display appends 級 once");
        check(format_rank_display(t_bom, 100.0) == L"《 上級戦士級 》", L"Rank display avoids duplicate 級");
    }
    {
        std::string dup_data = "10\tA\n10\tB\n";
        RankTable t_dup = parse_rank_table_from_bytes(dup_data);
        check(!t_dup.ok, L"Rejects duplicate threshold: " + t_dup.error_message);
    }
    {
        std::string desc_data = "100\tA\n50\tB\n";
        RankTable t_desc = parse_rank_table_from_bytes(desc_data);
        check(!t_desc.ok, L"Rejects non-ascending threshold: " + t_desc.error_message);
    }
    {
        std::string bad_data = "100 NoTabHere\n";
        RankTable t_bad = parse_rank_table_from_bytes(bad_data);
        check(!t_bad.ok, L"Rejects missing TAB line: " + t_bad.error_message);
    }
    {
        RankTable t_missing = load_rank_table_from_file(L"nonexistent_sco2_file.txt");
        check(!t_missing.ok, L"Handles missing file safely: " + t_missing.error_message);
    }

    // 4. Abnormal timing / NaN / Infinity / Huge value safety checks
    {
        check(!evaluate_elapsed_time(0.5).valid, L"Rejects elapsed <= 1.0 (seconds <= 0)");
        check(!evaluate_elapsed_time(1.0).valid, L"Rejects elapsed == 1.0 (huge value ~3.29e32)");
        check(!evaluate_elapsed_time(std::numeric_limits<double>::quiet_NaN()).valid, L"Rejects NaN");
        check(!evaluate_elapsed_time(std::numeric_limits<double>::infinity()).valid, L"Rejects Infinity");
        check(evaluate_elapsed_time(1.35).valid, L"Accepts normal elapsed time (1.35s)");
    }

    // 5. GUI Window + Non-blocking Measurement + Re-measurement test
    {
        enable_dpi_awareness();
        HWND hwnd = create_main_window(hInstance, SW_SHOWNOACTIVATE);
        check(hwnd != nullptr, L"GUI window created (Title: SCO2 - すこすこ)");
        if (hwnd) {
            // Trigger first measurement via button click
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_MEASURE_BUTTON, BN_CLICKED), reinterpret_cast<LPARAM>(g_app.hButton));
            check(g_app.measuring && !IsWindowEnabled(g_app.hButton) && g_app.line1 == L"測定中…",
                  L"GUI enters 測定中… state and disables button to prevent double execution");

            // Attempt second click while measuring -> should be ignored
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_MEASURE_BUTTON, BN_CLICKED), reinterpret_cast<LPARAM>(g_app.hButton));

            // Pump message loop until first measurement completes
            MSG msg{};
            while (g_app.completed_runs < 1 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            check(!g_app.measuring && IsWindowEnabled(g_app.hButton) && g_app.completed_runs == 1,
                  L"GUI first measurement completed: " + g_app.line1 + L" / " + g_app.line2);

            // Trigger re-measurement
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_MEASURE_BUTTON, BN_CLICKED), reinterpret_cast<LPARAM>(g_app.hButton));
            while (g_app.completed_runs < 2 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            check(!g_app.measuring && IsWindowEnabled(g_app.hButton) && g_app.completed_runs == 2,
                  L"GUI re-measurement completed: " + g_app.line1 + L" / " + g_app.line2);

            DestroyWindow(hwnd);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    return all_passed ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        std::wstring arg1 = argv[1];
        LocalFree(argv);
        if (arg1 == L"--self-test") {
            return run_self_test(hInstance);
        }
        if (arg1 == L"--cli") {
            RankTable table = load_rank_table_from_file(get_exe_dir_sco2_txt_path());
            MeasurementResult res = run_single_measurement();
            if (!res.valid) {
                write_stdout_utf8(res.error_message + L"\n");
                return 1;
            }
            write_stdout_utf8(L"このPCの戦闘力は " + format_score_number(res.score) + L" です。\n");
            write_stdout_utf8(format_rank_display(table, res.score) + L"\n");
            return 0;
        }
    } else if (argv) {
        LocalFree(argv);
    }

    enable_dpi_awareness();
    HWND hwnd = create_main_window(hInstance, nCmdShow);
    if (!hwnd) {
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}
