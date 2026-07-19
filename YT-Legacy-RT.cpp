// YT-Legacy-RT
// 改造Windows RT (jailbreak済み) 向け Invidious API YouTubeクライアント
// Win32 + Media Foundation + WinHTTP + GDI+ のみ使用 (外部ライブラリなし)

#include <windows.h>
#include <winhttp.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <evr.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

// ---------------------------------------------------------------------------
// コントロールID / メッセージ
// ---------------------------------------------------------------------------
enum
{
	IDC_SEARCH_EDIT = 100,
	IDC_SEARCH_BTN,
	IDC_SETTINGS_BTN,
	IDC_SUBS_BTN,
	IDC_LIST,
	IDC_STATUS,
	IDC_VIDEO,
	IDC_BACK_BTN,
	IDC_PLAYER_TITLE,
	IDC_PLAYPAUSE_BTN,
	IDC_SEEKBAR,
	IDC_TIME_LABEL,
	IDC_FULLSCREEN_BTN,
	IDC_REC_LIST,
	IDC_STATS_LABEL,
	IDC_CHAN_ICON,
	IDC_CHAN_NAME,
	IDC_SUB_BTN,
	IDC_MINI_THUMB,
	IDC_MINI_TITLE,
	IDC_MINI_CLOSE,

	// 設定ウィンドウ
	IDC_SET_INSTANCE = 200,
	IDC_SET_QUALITY,
	IDC_SET_LANGUAGE,
	IDC_SET_LOCAL,
	IDC_SET_IGNORECERT,
	IDC_SET_OK,
	IDC_SET_CANCEL,
};

#define WM_APP_SEARCHDONE  (WM_APP + 1)   // wParam: 検索世代
#define WM_APP_STATUS      (WM_APP + 2)   // lParam: heap確保した wchar_t* (受信側でfree)
#define WM_APP_THUMB       (WM_APP + 3)   // wParam: アイテムindex lParam: 検索世代
#define WM_APP_WATCHINFO   (WM_APP + 4)   // wParam: 動画情報世代
#define WM_APP_RECTHUMB    (WM_APP + 5)   // wParam: アイテムindex lParam: 動画情報世代
#define WM_APP_CHANICON    (WM_APP + 6)   // wParam: 動画情報世代 lParam: HBITMAP
#define WM_APP_MINITHUMB   (WM_APP + 7)   // wParam: 動画情報世代 lParam: HBITMAP

HWND g_hWnd;
HWND g_hSearchEdit;
HWND g_hSearchBtn;
HWND g_hSettingsBtn;
HWND g_hSubsBtn;
HWND g_hList;
HWND g_hStatus;
HWND g_hVideo;
HWND g_hBackBtn;
HWND g_hPlayerTitle;
HWND g_hPlayPauseBtn;
HWND g_hSeekBar;
HWND g_hTimeLabel;
HWND g_hFullscreenBtn;
HWND g_hRecList;
HWND g_hStatsLabel;
HWND g_hChanIcon;
HWND g_hChanName;
HWND g_hSubBtn;
HBITMAP g_chanIconBmp = nullptr;   // チャンネルアイコン (STATICに設定中のもの)

// ミニプレイヤー (一覧画面で再生継続中に左下に表示)
HWND g_hMiniThumb;
HWND g_hMiniTitle;
HWND g_hMiniClose;
HBITMAP g_miniThumbBmp = nullptr;

HBRUSH g_hbrBlack;
bool g_fullscreen = false;
WINDOWPLACEMENT g_wpPrev = { sizeof(WINDOWPLACEMENT) };

HFONT g_hFontUI;      // 通常UI
HFONT g_hFontTitle;   // カードのタイトル
HFONT g_hFontSmall;   // カードの投稿者/時間

int  g_dpi = 96;
bool g_playerMode = false;   // false=一覧 true=プレイヤー
int  g_listMode = 0;         // ホームのリスト内容: 0=動画 1=登録チャンネル

// 設定 (iniに保存)
std::wstring g_instanceUrl = L"http://";
int  g_qualityIdx = 0;       // 0=360p(itag18) 1=720p(itag22)
bool g_localProxy = true;
bool g_ignoreCert = false;

IMFMediaSession* g_pSession = nullptr;
IMFMediaSource*  g_pSource = nullptr;   // muxed単体、またはDASH時は映像+音声の合成ソース

volatile LONGLONG g_duration100ns = 0;  // 動画の長さ (100ns単位、0=不明)
bool g_isPaused = false;
bool g_userSeeking = false;             // トラックバーをドラッグ中
int  g_seekBarMax = -1;                 // 設定済みのトラックバー最大値 (秒)

#define TIMER_POSITION 1

struct VideoItem
{
	std::wstring videoId;
	std::wstring title;
	std::wstring author;
	long long lengthSeconds;
	HBITMAP thumb;
};

CRITICAL_SECTION g_cs;
std::vector<VideoItem> g_results;      // 検索結果 (g_cs で保護)
std::wstring g_searchError;            // g_cs で保護
LONG g_searchGen = 0;                  // 検索の世代 (古いスレッドの結果を破棄する)

std::vector<VideoItem> g_recResults;   // おすすめ動画 (g_cs で保護)
std::wstring g_watchStats;             // 再生数・高評価などの表示文字列 (g_cs で保護)
std::wstring g_nowPlaying;             // 「再生中 (720p DASH)」等 (g_cs で保護)
std::wstring g_chanId;                 // 再生中動画のチャンネルID (g_cs で保護)
std::wstring g_chanTitle;              // 同チャンネル名 (g_cs で保護)
LONG g_recGen = 0;                     // 動画情報の世代

WNDPROC g_oldEditProc = nullptr;
WNDPROC g_oldTrackProc = nullptr;

DWORD WINAPI PlayThread(LPVOID param);
DWORD WINAPI SearchThread(LPVOID param);
DWORD WINAPI ThumbThread(LPVOID param);
DWORD WINAPI WatchThread(LPVOID param);
static void StartSearch();
static void ToggleFullscreen();

static int S(int px) { return MulDiv(px, g_dpi, 96); }

// ---------------------------------------------------------------------------
// ユーティリティ
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// UTF-8バイト列をパーセントエンコード
static std::wstring UrlEncode(const std::wstring& text)
{
	std::string utf8 = WideToUtf8(text);
	std::wstring out;
	const wchar_t* hex = L"0123456789ABCDEF";
	for (size_t i = 0; i < utf8.size(); i++)
	{
		unsigned char c = (unsigned char)utf8[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			out += (wchar_t)c;
		}
		else
		{
			out += L'%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

static std::wstring TrimTrailingSlash(std::wstring s)
{
	while (!s.empty() && (s[s.size() - 1] == L'/' || s[s.size() - 1] == L' '))
		s.erase(s.size() - 1);
	return s;
}

static std::wstring GetWindowTextStr(HWND h)
{
	int len = GetWindowTextLengthW(h);
	std::wstring s(len + 1, L'\0');
	GetWindowTextW(h, &s[0], len + 1);
	s.resize(wcslen(s.c_str()));
	return s;
}

// ---------------------------------------------------------------------------
// 多言語対応
// 内蔵: 日本語/英語。exeフォルダの lang\<code>.ini (UTF-16LE) があれば上書き。
// 言語コードは ISO 639-1 (ja, en, fr, ...)。設定が空なら Windows の言語設定に従う。
// ---------------------------------------------------------------------------
std::map<std::wstring, std::wstring> g_langMap;   // g_cs で保護
std::wstring g_langSetting;                        // ""=自動 / "ja" / "en" / 外部ファイルコード

struct LangChoice { std::wstring code; std::wstring display; };
std::vector<LangChoice> g_langChoices;

static const wchar_t* LANG_EN[][2] = {
	{ L"lang_auto",       L"Auto (Windows language)" },
	{ L"search_btn",      L"Search" },
	{ L"settings_btn",    L"Settings" },
	{ L"back_btn",        L"\x2190 Back" },
	{ L"pause_btn",       L"Pause" },
	{ L"play_btn",        L"Play" },
	{ L"fullscreen",      L"Fullscreen" },
	{ L"fullscreen_exit", L"Exit full screen" },
	{ L"ready",           L"Ready" },
	{ L"settings_title",  L"Settings" },
	{ L"instance_label",  L"Invidious instance URL:" },
	{ L"quality_label",   L"Quality:" },
	{ L"language_label",  L"Language:" },
	{ L"local_chk",       L"Play via instance proxy (local=true)" },
	{ L"cert_chk",        L"Ignore certificate errors" },
	{ L"ok_btn",          L"OK" },
	{ L"cancel_btn",      L"Cancel" },
	{ L"searching",       L"Searching: " },
	{ L"results_found",   L"%d videos found" },
	{ L"no_results",      L"No results" },
	{ L"enter_query",     L"Enter a search term" },
	{ L"set_instance",    L"Set the instance URL in Settings" },
	{ L"loading",         L"Loading..." },
	{ L"loading_n",       L"Loading... (%d/%d)" },
	{ L"fetching_info",   L"Fetching video info..." },
	{ L"playing",         L"Playing" },
	{ L"paused",          L"Paused" },
	{ L"ended",           L"Playback finished" },
	{ L"play_failed",     L"Playback failed: " },
	{ L"fallback_note",   L" *first choice failed: " },
	{ L"no_dash",         L"no DASH streams" },
	{ L"info_fail",       L"video info fetch failed" },
	{ L"unreachable",     L"unreachable" },
	{ L"audio_suffix",    L" (audio)" },
	{ L"start_timeout",   L"start timeout" },
	{ L"direct_suffix",   L"direct" },
	{ L"cant_fetch",      L" (the instance could not fetch this video)" },
	{ L"http_error",      L"HTTP error %u" },
	{ L"net_fail",        L"network failure (error %u)" },
	{ L"bad_url",         L"invalid URL" },
	{ L"open_fail",       L"WinHttpOpen failed (code %u)" },
	{ L"conn_fail",       L"connection failed (WinHttpConnect, code %u, host=%s:%u)" },
	{ L"req_fail",        L"connection failed (WinHttpOpenRequest, code %u)" },
	{ L"views_fmt",       L"%s views" },
	{ L"likes_fmt",       L"%s likes" },
	{ L"subscribe_btn",   L"Subscribe" },
	{ L"subscribed_btn",  L"Subscribed" },
	{ L"subs_btn",        L"Subscriptions" },
	{ L"channels_found",  L"%d channels" },
	{ L"no_subs",         L"No subscribed channels" },
	{ L"num_style",       L"west" },
};

static const wchar_t* LANG_JA[][2] = {
	{ L"lang_auto",       L"自動 (Windowsの言語設定)" },
	{ L"search_btn",      L"検索" },
	{ L"settings_btn",    L"設定" },
	{ L"back_btn",        L"\x2190 戻る" },
	{ L"pause_btn",       L"一時停止" },
	{ L"play_btn",        L"再生" },
	{ L"fullscreen",      L"全画面" },
	{ L"fullscreen_exit", L"全画面解除" },
	{ L"ready",           L"準備完了" },
	{ L"settings_title",  L"設定" },
	{ L"instance_label",  L"InvidiousインスタンスURL:" },
	{ L"quality_label",   L"画質:" },
	{ L"language_label",  L"言語:" },
	{ L"local_chk",       L"インスタンス経由で再生 (local=true)" },
	{ L"cert_chk",        L"証明書エラーを無視" },
	{ L"ok_btn",          L"OK" },
	{ L"cancel_btn",      L"キャンセル" },
	{ L"searching",       L"検索中: " },
	{ L"results_found",   L"%d件の動画が見つかりました" },
	{ L"no_results",      L"結果が0件でした" },
	{ L"enter_query",     L"検索ワードを入力してください" },
	{ L"set_instance",    L"設定でインスタンスURLを入力してください" },
	{ L"loading",         L"読み込み中..." },
	{ L"loading_n",       L"読み込み中... (%d/%d)" },
	{ L"fetching_info",   L"動画情報を取得中..." },
	{ L"playing",         L"再生中" },
	{ L"paused",          L"一時停止中" },
	{ L"ended",           L"再生終了" },
	{ L"play_failed",     L"再生失敗: " },
	{ L"fallback_note",   L" ※上位候補失敗: " },
	{ L"no_dash",         L"DASHストリームなし" },
	{ L"info_fail",       L"動画情報取得失敗" },
	{ L"unreachable",     L"接続不可" },
	{ L"audio_suffix",    L" (音声)" },
	{ L"start_timeout",   L"開始タイムアウト" },
	{ L"direct_suffix",   L"直接" },
	{ L"cant_fetch",      L" (インスタンス側でこの動画を取得できていません)" },
	{ L"http_error",      L"HTTPエラー %u" },
	{ L"net_fail",        L"通信失敗 (エラーコード %u)" },
	{ L"bad_url",         L"URLの形式が不正です" },
	{ L"open_fail",       L"WinHttpOpen失敗 (code %u)" },
	{ L"conn_fail",       L"接続失敗 (WinHttpConnect, code %u, host=%s:%u)" },
	{ L"req_fail",        L"接続失敗 (WinHttpOpenRequest, code %u)" },
	{ L"views_fmt",       L"%s回視聴" },
	{ L"likes_fmt",       L"高評価 %s" },
	{ L"subscribe_btn",   L"チャンネル登録" },
	{ L"subscribed_btn",  L"登録済み" },
	{ L"subs_btn",        L"登録チャンネル" },
	{ L"channels_found",  L"%d件のチャンネル" },
	{ L"no_subs",         L"登録チャンネルはありません" },
	{ L"num_style",       L"jp" },
};

static std::wstring GetExeDir()
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash) *(slash + 1) = L'\0';
	return path;
}

// 翻訳文字列の取得 (キーが無ければキー自身を返す)
static std::wstring Tr(const wchar_t* key)
{
	EnterCriticalSection(&g_cs);
	std::map<std::wstring, std::wstring>::const_iterator it = g_langMap.find(key);
	std::wstring result = (it != g_langMap.end()) ? it->second : key;
	LeaveCriticalSection(&g_cs);
	return result;
}

static void LoadLangTableLocked(const wchar_t* (*table)[2], int count)
{
	for (int i = 0; i < count; i++)
		g_langMap[table[i][0]] = table[i][1];
}

// 外部言語ファイル (UTF-16LE BOMのini) の [Strings] で上書きする
static void LoadLangFileLocked(const std::wstring& code)
{
	std::wstring file = GetExeDir() + L"lang\\" + code + L".ini";
	if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
		return;
	std::vector<wchar_t> buf(32768, L'\0');
	GetPrivateProfileSectionW(L"Strings", buf.data(), 32768, file.c_str());
	const wchar_t* p = buf.data();
	while (*p)
	{
		std::wstring line = p;
		p += line.size() + 1;
		size_t eq = line.find(L'=');
		if (eq != std::wstring::npos && eq > 0)
		{
			std::wstring value = line.substr(eq + 1);
			// 前後の空白が必要な値は引用符で囲めるようにする
			if (value.size() >= 2 && value[0] == L'"' && value[value.size() - 1] == L'"')
				value = value.substr(1, value.size() - 2);
			g_langMap[line.substr(0, eq)] = value;
		}
	}
}

// Windowsの言語設定から言語コードを得る (例: "ja-JP" → "ja")
static std::wstring AutoLangCode()
{
	wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
	{
		std::wstring code = name;
		size_t dash = code.find(L'-');
		if (dash != std::wstring::npos)
			code = code.substr(0, dash);
		return code;
	}
	return L"en";
}

static void ReloadLanguage()
{
	std::wstring code = g_langSetting.empty() ? AutoLangCode() : g_langSetting;

	EnterCriticalSection(&g_cs);
	g_langMap.clear();
	// 英語をベースに読み、対象言語で上書き (訳抜けキーは英語表示になる)
	LoadLangTableLocked(LANG_EN, ARRAYSIZE(LANG_EN));
	if (code == L"ja")
		LoadLangTableLocked(LANG_JA, ARRAYSIZE(LANG_JA));
	LoadLangFileLocked(code);
	LeaveCriticalSection(&g_cs);
}

// 設定画面用: 選択可能な言語一覧 (内蔵 + lang\*.ini)
static void BuildLangChoices()
{
	g_langChoices.clear();
	LangChoice c;
	c.code = L"";   c.display = Tr(L"lang_auto"); g_langChoices.push_back(c);
	c.code = L"ja"; c.display = L"日本語";        g_langChoices.push_back(c);
	c.code = L"en"; c.display = L"English";       g_langChoices.push_back(c);

	WIN32_FIND_DATAW fd;
	std::wstring pattern = GetExeDir() + L"lang\\*.ini";
	HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			std::wstring name = fd.cFileName;
			size_t dot = name.rfind(L'.');
			std::wstring code = name.substr(0, dot);
			if (code == L"ja" || code == L"en" || code.empty())
				continue;
			wchar_t disp[128] = {};
			GetPrivateProfileStringW(L"Meta", L"Name", code.c_str(), disp, 128,
				(GetExeDir() + L"lang\\" + name).c_str());
			c.code = code;
			c.display = disp;
			g_langChoices.push_back(c);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
}

// ワーカースレッドからステータス欄を更新
static void PostStatus(const std::wstring& text)
{
	wchar_t* copy = _wcsdup(text.c_str());
	if (!PostMessageW(g_hWnd, WM_APP_STATUS, 0, (LPARAM)copy))
		free(copy);
}

static void SetStatus(const std::wstring& text)
{
	SetWindowTextW(g_hStatus, text.c_str());
}

// ---------------------------------------------------------------------------
// WinHTTPによるGET (Invidious API / サムネイル取得用)
// ---------------------------------------------------------------------------
static bool HttpGet(const std::wstring& url, std::string& out, std::wstring& err)
{
	out.clear();
	err.clear();

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;

	if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
	{
		err = Tr(L"bad_url");
		return false;
	}
	bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

	HINTERNET hOpen = WinHttpOpen(
		L"Mozilla/5.0 (Windows NT 6.3; ARM) YT-Legacy-RT/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hOpen)
	{
		wchar_t buf[128];
		swprintf_s(buf, Tr(L"open_fail").c_str(), GetLastError());
		err = buf;
		return false;
	}

	// Windows RT 8.1 では既定でTLS1.2が有効でない場合があるため明示する
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
	                  WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
	                  WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
	WinHttpSetOption(hOpen, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

	bool ok = false;
	HINTERNET hConnect = WinHttpConnect(hOpen, host, uc.nPort, 0);
	HINTERNET hRequest = nullptr;

	if (!hConnect)
	{
		wchar_t buf[192];
		swprintf_s(buf, Tr(L"conn_fail").c_str(), GetLastError(), host, uc.nPort);
		err = buf;
	}
	else
	{
		hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			https ? WINHTTP_FLAG_SECURE : 0);
		if (!hRequest)
		{
			wchar_t buf[128];
			swprintf_s(buf, Tr(L"req_fail").c_str(), GetLastError());
			err = buf;
		}
	}

	if (hRequest)
	{
		if (g_ignoreCert)
		{
			// 古いRT端末はルート証明書ストアが更新されていないことがある
			DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
			              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
			              SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
			              SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
			WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
		}

		if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			WinHttpReceiveResponse(hRequest, nullptr))
		{
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

			DWORD avail = 0;
			do
			{
				avail = 0;
				if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
				if (avail == 0) break;
				size_t offset = out.size();
				out.resize(offset + avail);
				DWORD read = 0;
				if (!WinHttpReadData(hRequest, &out[offset], avail, &read)) break;
				out.resize(offset + read);
			} while (avail > 0);

			if (status == 200)
			{
				ok = true;
			}
			else
			{
				wchar_t buf[128];
				swprintf_s(buf, Tr(L"http_error").c_str(), status);
				err = buf;
			}
		}
		else
		{
			wchar_t buf[128];
			swprintf_s(buf, Tr(L"net_fail").c_str(), GetLastError());
			err = buf;
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hOpen);
	return ok;
}

// ストリームURLを事前検証し、リダイレクト解決済みの最終URLを得る。
// 古いMedia Foundationは相対パスのLocationヘッダを追えないことがあるため、
// WinHTTPで解決した絶対URLをMFに渡す。失敗時はstatusにHTTPコードが入る (0=接続不可)。
static bool ProbeStreamUrl(const std::wstring& url, std::wstring& finalUrl, DWORD& status)
{
	finalUrl = url;
	status = 0;

	// googlevideoのDASH URLは2000文字を超えるため大きめに取る
	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[512] = {};
	std::vector<wchar_t> path(8192, L'\0');
	uc.lpszHostName = host;
	uc.dwHostNameLength = 512;
	uc.lpszUrlPath = path.data();
	uc.dwUrlPathLength = 8192;

	if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
		return false;
	bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

	HINTERNET hOpen = WinHttpOpen(
		L"Mozilla/5.0 (Windows NT 6.3; ARM) YT-Legacy-RT/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hOpen) return false;

	// フォールバックを速くするため短めのタイムアウト
	WinHttpSetTimeouts(hOpen, 10000, 10000, 30000, 30000);

	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
	                  WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
	                  WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
	WinHttpSetOption(hOpen, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

	bool ok = false;
	HINTERNET hConnect = WinHttpConnect(hOpen, host, uc.nPort, 0);
	HINTERNET hRequest = nullptr;
	if (hConnect)
	{
		hRequest = WinHttpOpenRequest(hConnect, L"GET", path.data(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			https ? WINHTTP_FLAG_SECURE : 0);
	}
	if (hRequest)
	{
		if (g_ignoreCert)
		{
			DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
			              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
			              SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
			              SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
			WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
		}

		if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			WinHttpReceiveResponse(hRequest, nullptr))
		{
			DWORD size = sizeof(status);
			WinHttpQueryHeaders(hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

			if (status == 200 || status == 206)
			{
				// リダイレクト解決後の最終URL
				std::vector<wchar_t> urlBuf(8192, L'\0');
				DWORD urlLen = (DWORD)(urlBuf.size() * sizeof(wchar_t));
				if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, urlBuf.data(), &urlLen))
					finalUrl = urlBuf.data();
				ok = true;
			}
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hOpen);
	return ok;
}

// ---------------------------------------------------------------------------
// 最小限のJSONパース (Invidiousの検索結果用)
// ---------------------------------------------------------------------------
static std::wstring JsonUnescape(const std::wstring& s)
{
	std::wstring out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] != L'\\' || i + 1 >= s.size())
		{
			out += s[i];
			continue;
		}
		wchar_t c = s[++i];
		switch (c)
		{
		case L'"':  out += L'"';  break;
		case L'\\': out += L'\\'; break;
		case L'/':  out += L'/';  break;
		case L'n':  out += L'\n'; break;
		case L't':  out += L'\t'; break;
		case L'r':  break;
		case L'b':  break;
		case L'f':  break;
		case L'u':
			if (i + 4 < s.size())
			{
				wchar_t hex[5] = { s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0 };
				out += (wchar_t)wcstoul(hex, nullptr, 16);
				i += 4;
			}
			break;
		default: out += c; break;
		}
	}
	return out;
}

// obj内の "key":"value" を取り出す
static std::wstring JsonGetString(const std::wstring& obj, const wchar_t* key)
{
	std::wstring pat = L"\"";
	pat += key;
	pat += L"\"";
	size_t p = obj.find(pat);
	if (p == std::wstring::npos) return std::wstring();
	p = obj.find(L':', p + pat.size());
	if (p == std::wstring::npos) return std::wstring();
	p++;
	while (p < obj.size() && (obj[p] == L' ' || obj[p] == L'\t' || obj[p] == L'\n' || obj[p] == L'\r')) p++;
	if (p >= obj.size() || obj[p] != L'"') return std::wstring();
	p++;
	size_t start = p;
	while (p < obj.size())
	{
		if (obj[p] == L'\\') { p += 2; continue; }
		if (obj[p] == L'"') break;
		p++;
	}
	return JsonUnescape(obj.substr(start, p - start));
}

static long long JsonGetNumber(const std::wstring& obj, const wchar_t* key)
{
	std::wstring pat = L"\"";
	pat += key;
	pat += L"\"";
	size_t p = obj.find(pat);
	if (p == std::wstring::npos) return -1;
	p = obj.find(L':', p + pat.size());
	if (p == std::wstring::npos) return -1;
	p++;
	while (p < obj.size() && (obj[p] == L' ' || obj[p] == L'\t')) p++;
	return _wtoi64(obj.c_str() + p);
}

// トップレベル配列 [ {..}, {..} ] を各オブジェクト文字列に分割
// (文字列リテラル内の括弧は無視する)
static void SplitTopLevelObjects(const std::wstring& json, std::vector<std::wstring>& objs)
{
	int depth = 0;
	bool inStr = false;
	size_t objStart = 0;
	for (size_t i = 0; i < json.size(); i++)
	{
		wchar_t c = json[i];
		if (inStr)
		{
			if (c == L'\\') i++;
			else if (c == L'"') inStr = false;
			continue;
		}
		if (c == L'"') { inStr = true; continue; }
		if (c == L'{')
		{
			if (depth == 0) objStart = i;
			depth++;
		}
		else if (c == L'}')
		{
			depth--;
			if (depth == 0)
				objs.push_back(json.substr(objStart, i - objStart + 1));
		}
	}
}

// ---------------------------------------------------------------------------
// サムネイル
// ---------------------------------------------------------------------------
static int ThumbW() { return S(160); }
static int ThumbH() { return S(90); }

// JPEGバイト列をデコードして指定サイズのHBITMAPを作る
static HBITMAP DecodeImageToBitmap(const std::string& data, int w, int h)
{
	if (data.empty()) return nullptr;

	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
	if (!hMem) return nullptr;
	void* p = GlobalLock(hMem);
	memcpy(p, data.data(), data.size());
	GlobalUnlock(hMem);

	IStream* stream = nullptr;
	if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream)))
	{
		GlobalFree(hMem);
		return nullptr;
	}

	HBITMAP result = nullptr;
	Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromStream(stream);
	if (src && src->GetLastStatus() == Gdiplus::Ok)
	{
		Gdiplus::Bitmap dst(w, h, PixelFormat32bppRGB);
		Gdiplus::Graphics g(&dst);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
		g.DrawImage(src, Gdiplus::Rect(0, 0, w, h));
		dst.GetHBITMAP(Gdiplus::Color(255, 255, 255), &result);
	}
	delete src;
	stream->Release();
	return result;
}

struct ThumbParams
{
	LONG gen;
	std::wstring instanceUrl;
	bool rec;   // true = おすすめリスト用
};

// リストのサムネイルを順次ダウンロードする (検索結果 / おすすめ共用)
DWORD WINAPI ThumbThread(LPVOID param)
{
	ThumbParams* tp = (ThumbParams*)param;
	std::vector<VideoItem>& items = tp->rec ? g_recResults : g_results;
	volatile LONG& curGen = tp->rec ? g_recGen : g_searchGen;
	UINT doneMsg = tp->rec ? WM_APP_RECTHUMB : WM_APP_THUMB;

	for (int i = 0;; i++)
	{
		std::wstring videoId;
		EnterCriticalSection(&g_cs);
		bool stale = (tp->gen != curGen);
		if (!stale && i < (int)items.size())
			videoId = items[i].videoId;
		LeaveCriticalSection(&g_cs);

		if (stale || videoId.empty()) break;

		std::wstring url = tp->instanceUrl + L"/vi/" + videoId + L"/mqdefault.jpg";
		std::string data;
		std::wstring err;
		HBITMAP bmp = nullptr;
		if (HttpGet(url, data, err))
			bmp = DecodeImageToBitmap(data, ThumbW(), ThumbH());
		if (!bmp) continue;

		bool stored = false;
		EnterCriticalSection(&g_cs);
		if (tp->gen == curGen && i < (int)items.size() && !items[i].thumb)
		{
			items[i].thumb = bmp;
			stored = true;
		}
		LeaveCriticalSection(&g_cs);

		if (stored)
			PostMessageW(g_hWnd, doneMsg, (WPARAM)i, (LPARAM)tp->gen);
		else
			DeleteObject(bmp);
	}

	delete tp;
	return 0;
}

// ---------------------------------------------------------------------------
// 検索
// ---------------------------------------------------------------------------
struct SearchParams
{
	LONG gen;
	std::wstring instanceUrl;
	std::wstring query;
};

// g_cs保持中に呼ぶこと
static void ClearResultsLocked()
{
	for (size_t i = 0; i < g_results.size(); i++)
		if (g_results[i].thumb) DeleteObject(g_results[i].thumb);
	g_results.clear();
}

// g_cs保持中に呼ぶこと
static void ClearRecLocked()
{
	for (size_t i = 0; i < g_recResults.size(); i++)
		if (g_recResults[i].thumb) DeleteObject(g_recResults[i].thumb);
	g_recResults.clear();
}

DWORD WINAPI SearchThread(LPVOID param)
{
	SearchParams* sp = (SearchParams*)param;

	std::wstring url = sp->instanceUrl + L"/api/v1/search?type=video&q=" + UrlEncode(sp->query);
	PostStatus(Tr(L"searching") + sp->query);

	std::string body;
	std::wstring err;
	bool ok = HttpGet(url, body, err);

	EnterCriticalSection(&g_cs);
	if (sp->gen != g_searchGen)
	{
		// 新しい検索が始まっているので破棄
		LeaveCriticalSection(&g_cs);
		delete sp;
		return 0;
	}
	ClearResultsLocked();
	g_searchError.clear();
	if (ok)
	{
		std::wstring json = Utf8ToWide(body);
		std::vector<std::wstring> objs;
		SplitTopLevelObjects(json, objs);
		for (size_t i = 0; i < objs.size(); i++)
		{
			if (JsonGetString(objs[i], L"type") != L"video") continue;
			VideoItem item;
			item.videoId = JsonGetString(objs[i], L"videoId");
			item.title = JsonGetString(objs[i], L"title");
			item.author = JsonGetString(objs[i], L"author");
			item.lengthSeconds = JsonGetNumber(objs[i], L"lengthSeconds");
			item.thumb = nullptr;
			if (!item.videoId.empty())
				g_results.push_back(item);
		}
		if (g_results.empty())
			g_searchError = Tr(L"no_results");
	}
	else
	{
		g_searchError = err;
	}
	LeaveCriticalSection(&g_cs);

	PostMessageW(g_hWnd, WM_APP_SEARCHDONE, (WPARAM)sp->gen, 0);
	delete sp;
	return 0;
}

// ---------------------------------------------------------------------------
// チャンネル登録 (exeフォルダの subscriptions.ini にローカル保存)
// ---------------------------------------------------------------------------
static std::wstring GetSubsPath()
{
	return GetExeDir() + L"subscriptions.ini";
}

// BOM付きUTF-16LEで空のiniを作る
// (ANSIのままだと日本語チャンネル名が ? に潰れて保存されるため)
static void CreateSubsFileWithBom(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	const wchar_t header[] = L"\xFEFF[Subscriptions]\r\n";
	DWORD written = 0;
	WriteFile(h, header, (DWORD)(wcslen(header) * sizeof(wchar_t)), &written, nullptr);
	CloseHandle(h);
}

// subscriptions.ini がUTF-16LEであることを保証する (無ければ作成、ANSIなら移行)
static void EnsureSubsFileUnicode()
{
	std::wstring path = GetSubsPath();

	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		CreateSubsFileWithBom(path);
		return;
	}
	BYTE bom[2] = {};
	DWORD read = 0;
	ReadFile(h, bom, 2, &read, nullptr);
	CloseHandle(h);
	if (read >= 2 && bom[0] == 0xFF && bom[1] == 0xFE)
		return;   // 既にUTF-16LE

	// ANSIで作られた既存ファイルをUTF-16LEへ移行 (チャンネルIDは保持される)
	std::vector<wchar_t> buf(32768, L'\0');
	GetPrivateProfileSectionW(L"Subscriptions", buf.data(), 32768, path.c_str());
	WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());   // キャッシュ書き出し
	CreateSubsFileWithBom(path);
	const wchar_t* p = buf.data();
	while (*p)
	{
		std::wstring line = p;
		p += line.size() + 1;
		size_t eq = line.find(L'=');
		if (eq != std::wstring::npos && eq > 0)
			WritePrivateProfileStringW(L"Subscriptions",
				line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), path.c_str());
	}
}

static bool IsSubscribed(const std::wstring& channelId)
{
	if (channelId.empty()) return false;
	wchar_t buf[8] = {};
	GetPrivateProfileStringW(L"Subscriptions", channelId.c_str(), L"", buf, 8, GetSubsPath().c_str());
	return buf[0] != L'\0';
}

static void SetSubscribed(const std::wstring& channelId, const std::wstring& name, bool subscribe)
{
	if (channelId.empty()) return;
	EnsureSubsFileUnicode();
	WritePrivateProfileStringW(L"Subscriptions", channelId.c_str(),
		subscribe ? (name.empty() ? L"1" : name.c_str()) : nullptr,
		GetSubsPath().c_str());
}

// ---------------------------------------------------------------------------
// 動画情報 (再生数・高評価・おすすめ) の取得
// ---------------------------------------------------------------------------

// local=true時のAPIは相対パス (/videoplayback?...) を返すため絶対URL化する
static std::wstring AbsolutizeUrl(const std::wstring& u, const std::wstring& instanceUrl)
{
	if (u.find(L"http://") == 0 || u.find(L"https://") == 0)
		return u;
	if (u.find(L"//") == 0)
	{
		// プロトコル相対 → インスタンスのスキームを補完
		size_t p = instanceUrl.find(L"://");
		if (p != std::wstring::npos)
			return instanceUrl.substr(0, p + 1) + u;
		return L"http:" + u;
	}
	if (!u.empty() && u[0] == L'/')
		return instanceUrl + u;
	return u;
}

// 12345 → "1.2万" / "12.3K" のような略記 (言語の num_style に従う)
// 注意: wsprintfW は %lld 非対応のため swprintf_s を使うこと
static std::wstring FormatCompact(long long n)
{
	wchar_t buf[64];
	if (n < 0) return std::wstring();

	if (Tr(L"num_style") == L"jp")
	{
		if (n >= 100000000)
		{
			long long x10 = n * 10 / 100000000;  // 0.1億単位
			if (x10 % 10)
				swprintf_s(buf, L"%lld.%lld億", x10 / 10, x10 % 10);
			else
				swprintf_s(buf, L"%lld億", x10 / 10);
		}
		else if (n >= 10000)
		{
			long long x10 = n * 10 / 10000;      // 0.1万単位
			if (n < 100000 && x10 % 10)
				swprintf_s(buf, L"%lld.%lld万", x10 / 10, x10 % 10);
			else
				swprintf_s(buf, L"%lld万", x10 / 10);
		}
		else
		{
			swprintf_s(buf, L"%lld", n);
		}
	}
	else
	{
		if (n >= 1000000000)
		{
			long long x10 = n * 10 / 1000000000;
			if (x10 % 10)
				swprintf_s(buf, L"%lld.%lldB", x10 / 10, x10 % 10);
			else
				swprintf_s(buf, L"%lldB", x10 / 10);
		}
		else if (n >= 1000000)
		{
			long long x10 = n * 10 / 1000000;
			if (n < 10000000 && x10 % 10)
				swprintf_s(buf, L"%lld.%lldM", x10 / 10, x10 % 10);
			else
				swprintf_s(buf, L"%lldM", x10 / 10);
		}
		else if (n >= 1000)
		{
			long long x10 = n * 10 / 1000;
			if (n < 10000 && x10 % 10)
				swprintf_s(buf, L"%lld.%lldK", x10 / 10, x10 % 10);
			else
				swprintf_s(buf, L"%lldK", x10 / 10);
		}
		else
		{
			swprintf_s(buf, L"%lld", n);
		}
	}
	return buf;
}

// json中の "key":[ ... ] の中身を取り出す (文字列リテラル対応)
static std::wstring JsonGetArray(const std::wstring& json, const wchar_t* key)
{
	std::wstring pat = L"\"";
	pat += key;
	pat += L"\"";
	size_t p = json.find(pat);
	if (p == std::wstring::npos) return std::wstring();
	p = json.find(L'[', p + pat.size());
	if (p == std::wstring::npos) return std::wstring();

	int depth = 0;
	bool inStr = false;
	for (size_t i = p; i < json.size(); i++)
	{
		wchar_t c = json[i];
		if (inStr)
		{
			if (c == L'\\') i++;
			else if (c == L'"') inStr = false;
			continue;
		}
		if (c == L'"') { inStr = true; continue; }
		if (c == L'[') depth++;
		else if (c == L']')
		{
			depth--;
			if (depth == 0)
				return json.substr(p, i - p + 1);
		}
	}
	return std::wstring();
}

struct WatchParams
{
	LONG gen;
	std::wstring instanceUrl;
	std::wstring videoId;
};

DWORD WINAPI WatchThread(LPVOID param)
{
	WatchParams* wp = (WatchParams*)param;

	std::wstring url = wp->instanceUrl + L"/api/v1/videos/" + wp->videoId;
	std::string body;
	std::wstring err;
	bool ok = HttpGet(url, body, err);

	std::wstring stats;
	std::vector<VideoItem> recs;
	std::wstring chanId, chanTitle, iconUrl;
	if (ok)
	{
		std::wstring json = Utf8ToWide(body);

		// おすすめ動画
		std::wstring recArray = JsonGetArray(json, L"recommendedVideos");
		std::vector<std::wstring> objs;
		SplitTopLevelObjects(recArray, objs);
		for (size_t i = 0; i < objs.size(); i++)
		{
			VideoItem item;
			item.videoId = JsonGetString(objs[i], L"videoId");
			item.title = JsonGetString(objs[i], L"title");
			item.author = JsonGetString(objs[i], L"author");
			long long views = JsonGetNumber(objs[i], L"viewCount");
			if (views >= 0)
			{
				wchar_t tmp[96];
				swprintf_s(tmp, Tr(L"views_fmt").c_str(), FormatCompact(views).c_str());
				item.author += std::wstring(L"・") + tmp;
			}
			item.lengthSeconds = JsonGetNumber(objs[i], L"lengthSeconds");
			item.thumb = nullptr;
			if (!item.videoId.empty())
				recs.push_back(item);
		}

		// 統計 (recommendedVideosより手前のトップレベル部分から取る)
		size_t recPos = json.find(L"\"recommendedVideos\"");
		std::wstring top = (recPos != std::wstring::npos) ? json.substr(0, recPos) : json;
		long long views = JsonGetNumber(top, L"viewCount");
		long long likes = JsonGetNumber(top, L"likeCount");
		std::wstring author = JsonGetString(top, L"author");
		std::wstring published = JsonGetString(top, L"publishedText");

		// チャンネル情報 (アイコンは76px以上の最小サイズを選ぶ)
		chanId = JsonGetString(top, L"authorId");
		chanTitle = author;
		{
			std::wstring thumbArr = JsonGetArray(top, L"authorThumbnails");
			std::vector<std::wstring> tobjs;
			SplitTopLevelObjects(thumbArr, tobjs);
			for (size_t t = 0; t < tobjs.size(); t++)
			{
				std::wstring u = JsonGetString(tobjs[t], L"url");
				if (u.empty()) continue;
				iconUrl = u;
				if (JsonGetNumber(tobjs[t], L"width") >= 76)
					break;
			}
			iconUrl = AbsolutizeUrl(iconUrl, wp->instanceUrl);
			// プロキシ有効時はggpht直アクセスを避けインスタンス経由にする
			size_t gp = iconUrl.find(L"ggpht.com/");
			if (g_localProxy && gp != std::wstring::npos)
				iconUrl = wp->instanceUrl + L"/ggpht/" + iconUrl.substr(gp + 10);
		}

		wchar_t tmp[128];
		if (views >= 0)
		{
			swprintf_s(tmp, Tr(L"views_fmt").c_str(), FormatCompact(views).c_str());
			stats += tmp;
		}
		if (likes >= 0)
		{
			swprintf_s(tmp, Tr(L"likes_fmt").c_str(), FormatCompact(likes).c_str());
			stats += (stats.empty() ? L"" : L"　") + std::wstring(tmp);
		}
		if (!author.empty())
			stats += (stats.empty() ? L"" : L"　") + author;
		if (!published.empty())
			stats += (stats.empty() ? L"" : L"　") + published;
	}

	EnterCriticalSection(&g_cs);
	if (wp->gen == g_recGen)
	{
		ClearRecLocked();
		g_recResults.swap(recs);
		g_watchStats = stats;
		g_chanId = chanId;
		g_chanTitle = chanTitle;
		LeaveCriticalSection(&g_cs);
		PostMessageW(g_hWnd, WM_APP_WATCHINFO, (WPARAM)wp->gen, 0);

		// チャンネルアイコンを取得 (失敗しても無視)
		if (!iconUrl.empty())
		{
			std::string img;
			std::wstring ierr;
			if (HttpGet(iconUrl, img, ierr))
			{
				HBITMAP bmp = DecodeImageToBitmap(img, S(36), S(36));
				if (bmp && !PostMessageW(g_hWnd, WM_APP_CHANICON, (WPARAM)wp->gen, (LPARAM)bmp))
					DeleteObject(bmp);
			}
		}

		// ミニプレイヤー用サムネイルを取得 (失敗しても無視)
		{
			std::wstring turl = wp->instanceUrl + L"/vi/" + wp->videoId + L"/mqdefault.jpg";
			std::string img;
			std::wstring terr;
			if (HttpGet(turl, img, terr))
			{
				HBITMAP bmp = DecodeImageToBitmap(img, S(72), S(40));
				if (bmp && !PostMessageW(g_hWnd, WM_APP_MINITHUMB, (WPARAM)wp->gen, (LPARAM)bmp))
					DeleteObject(bmp);
			}
		}
	}
	else
	{
		LeaveCriticalSection(&g_cs);
	}

	delete wp;
	return 0;
}

// ---------------------------------------------------------------------------
// チャンネルの動画一覧 (検索結果リストに表示する)
// ---------------------------------------------------------------------------
struct ChannelParams
{
	LONG gen;
	std::wstring instanceUrl;
	std::wstring channelId;
};

DWORD WINAPI ChannelThread(LPVOID param)
{
	ChannelParams* cp = (ChannelParams*)param;

	std::wstring url = cp->instanceUrl + L"/api/v1/channels/" + cp->channelId + L"/videos";
	std::string body;
	std::wstring err;
	bool ok = HttpGet(url, body, err);

	EnterCriticalSection(&g_cs);
	if (cp->gen != g_searchGen)
	{
		LeaveCriticalSection(&g_cs);
		delete cp;
		return 0;
	}
	ClearResultsLocked();
	g_searchError.clear();
	if (ok)
	{
		std::wstring json = Utf8ToWide(body);
		// 新APIは {"videos":[...]} 、旧APIはトップレベル配列
		std::wstring arr = JsonGetArray(json, L"videos");
		if (arr.empty())
			arr = json;
		std::vector<std::wstring> objs;
		SplitTopLevelObjects(arr, objs);
		for (size_t i = 0; i < objs.size(); i++)
		{
			VideoItem item;
			item.videoId = JsonGetString(objs[i], L"videoId");
			item.title = JsonGetString(objs[i], L"title");
			item.author = JsonGetString(objs[i], L"author");
			item.lengthSeconds = JsonGetNumber(objs[i], L"lengthSeconds");
			item.thumb = nullptr;
			if (!item.videoId.empty())
				g_results.push_back(item);
		}
		if (g_results.empty())
			g_searchError = Tr(L"no_results");
	}
	else
	{
		g_searchError = err;
	}
	LeaveCriticalSection(&g_cs);

	PostMessageW(g_hWnd, WM_APP_SEARCHDONE, (WPARAM)cp->gen, 0);
	delete cp;
	return 0;
}

// ---------------------------------------------------------------------------
// Media Foundation 再生コア
// ---------------------------------------------------------------------------
volatile LONG g_sessionErrorHr = 0;   // セッションの非同期エラー (0=なし)

// セッションイベントを監視して非同期エラーを捕捉する
class SessionCallback : public IMFAsyncCallback
{
	LONG m_ref;
	IMFMediaSession* m_session;

	~SessionCallback()
	{
		if (m_session) m_session->Release();
	}

public:
	SessionCallback(IMFMediaSession* s) : m_ref(1), m_session(s)
	{
		m_session->AddRef();
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback))
		{
			*ppv = static_cast<IMFAsyncCallback*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release()
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	STDMETHODIMP GetParameters(DWORD*, DWORD*) { return E_NOTIMPL; }

	STDMETHODIMP Invoke(IMFAsyncResult* result)
	{
		IMFMediaEvent* ev = nullptr;
		if (FAILED(m_session->EndGetEvent(result, &ev)))
		{
			// セッションは既にシャットダウン済み → 監視終了
			m_session->Release();
			m_session = nullptr;
			Release();
			return S_OK;
		}

		MediaEventType met = MEUnknown;
		ev->GetType(&met);
		HRESULT status = S_OK;
		ev->GetStatus(&status);

		if (FAILED(status))
			InterlockedExchange(&g_sessionErrorHr, (LONG)status);

		if (met == MESessionEnded)
			PostStatus(Tr(L"ended"));

		ev->Release();

		if (met == MESessionClosed || FAILED(m_session->BeginGetEvent(this, nullptr)))
		{
			m_session->Release();
			m_session = nullptr;
			Release();
		}
		return S_OK;
	}
};

void Cleanup()
{
	if (g_pSession)
	{
		g_pSession->Close();
		g_pSession->Shutdown();
		g_pSession->Release();
		g_pSession = nullptr;
	}
	if (g_pSource)
	{
		// 合成ソースの場合はShutdownで内包する映像/音声ソースも停止する
		g_pSource->Shutdown();
		g_pSource->Release();
		g_pSource = nullptr;
	}
	InterlockedExchange64(&g_duration100ns, 0);
}

HRESULT CreateSession()
{
	Cleanup();
	InterlockedExchange(&g_sessionErrorHr, 0);
	HRESULT hr = MFCreateMediaSession(nullptr, &g_pSession);
	if (SUCCEEDED(hr))
	{
		SessionCallback* cb = new SessionCallback(g_pSession);
		if (FAILED(g_pSession->BeginGetEvent(cb, nullptr)))
			cb->Release();
	}
	return hr;
}

HRESULT CreateMediaSource(const wchar_t* url, IMFMediaSource** ppSource)
{
	IMFSourceResolver* resolver = nullptr;
	MF_OBJECT_TYPE type = MF_OBJECT_INVALID;

	HRESULT hr = MFCreateSourceResolver(&resolver);
	if (FAILED(hr)) return hr;

	hr = resolver->CreateObjectFromURL(
		url,
		MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
		nullptr,
		&type,
		(IUnknown**)ppSource
	);

	resolver->Release();
	return hr;
}

// 1つのメディアソースの全ストリームをトポロジへ接続する
// (muxedなら映像+音声、DASHの単独ストリームなら片方のみが接続される)
static HRESULT AddSourceToTopology(IMFTopology* topology, IMFMediaSource* source)
{
	IMFPresentationDescriptor* pd = nullptr;

	HRESULT hr = source->CreatePresentationDescriptor(&pd);
	if (FAILED(hr)) return hr;

	UINT64 duration = 0;
	pd->GetUINT64(MF_PD_DURATION, &duration);
	if ((LONGLONG)duration > g_duration100ns)
		InterlockedExchange64(&g_duration100ns, (LONGLONG)duration);

	DWORD streamCount = 0;
	pd->GetStreamDescriptorCount(&streamCount);

	for (DWORD i = 0; i < streamCount; i++)
	{
		BOOL selected = FALSE;
		IMFStreamDescriptor* sd = nullptr;

		hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
		if (FAILED(hr) || !selected)
		{
			if (sd) sd->Release();
			continue;
		}

		IMFMediaTypeHandler* handler = nullptr;
		GUID major = GUID_NULL;

		hr = sd->GetMediaTypeHandler(&handler);
		if (FAILED(hr))
		{
			sd->Release();
			continue;
		}

		handler->GetMajorType(&major);

		IMFTopologyNode* srcNode = nullptr;
		IMFTopologyNode* outNode = nullptr;
		IMFActivate* renderer = nullptr;

		// ソースノード作成
		hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &srcNode);
		if (FAILED(hr)) goto NEXT;

		srcNode->SetUnknown(MF_TOPONODE_SOURCE, source);
		srcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd);
		srcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd);

		// 出力ノード作成
		hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outNode);
		if (FAILED(hr)) goto NEXT;

		// 動画 / 音声で分岐
		if (major == MFMediaType_Video)
		{
			hr = MFCreateVideoRendererActivate(g_hVideo, &renderer);
		}
		else if (major == MFMediaType_Audio)
		{
			hr = MFCreateAudioRendererActivate(&renderer);
		}
		else
		{
			// その他のストリームは無視
			goto NEXT;
		}

		if (FAILED(hr)) goto NEXT;

		outNode->SetObject(renderer);
		outNode->SetUINT32(MF_TOPONODE_STREAMID, 0);
		outNode->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);

		topology->AddNode(srcNode);
		topology->AddNode(outNode);
		srcNode->ConnectOutput(0, outNode, 0);

	NEXT:
		if (renderer) renderer->Release();
		if (outNode) outNode->Release();
		if (srcNode) srcNode->Release();
		handler->Release();
		sd->Release();
	}

	pd->Release();
	return S_OK;
}

HRESULT CreateTopology()
{
	IMFTopology* topology = nullptr;

	HRESULT hr = MFCreateTopology(&topology);
	if (FAILED(hr)) return hr;

	InterlockedExchange64(&g_duration100ns, 0);

	hr = AddSourceToTopology(topology, g_pSource);

	if (SUCCEEDED(hr))
		hr = g_pSession->SetTopology(0, topology);

	topology->Release();
	return hr;
}

// audioUrl が空でなければDASH: 映像・音声ソースをMFCreateAggregateSourceで
// 1つの合成ソースにまとめてから通常のトポロジに載せる
// (Media Sessionは複数ソースを直接置いたトポロジをサポートしないため)
HRESULT Play(const wchar_t* url, const wchar_t* audioUrl)
{
	HRESULT hr;
	if (FAILED(hr = CreateSession())) return hr;

	if (audioUrl && *audioUrl)
	{
		IMFMediaSource* video = nullptr;
		IMFMediaSource* audio = nullptr;
		IMFCollection* col = nullptr;

		hr = CreateMediaSource(url, &video);
		if (SUCCEEDED(hr))
			hr = CreateMediaSource(audioUrl, &audio);
		if (SUCCEEDED(hr))
			hr = MFCreateCollection(&col);
		if (SUCCEEDED(hr))
		{
			col->AddElement(video);
			col->AddElement(audio);
			hr = MFCreateAggregateSource(col, &g_pSource);
		}

		if (col) col->Release();
		if (FAILED(hr))
		{
			if (video) { video->Shutdown(); video->Release(); }
			if (audio) { audio->Shutdown(); audio->Release(); }
			return hr;
		}
		// 合成ソースが参照を保持するためローカル参照は解放してよい
		video->Release();
		audio->Release();
	}
	else
	{
		if (FAILED(hr = CreateMediaSource(url, &g_pSource))) return hr;
	}

	if (FAILED(hr = CreateTopology())) return hr;

	PROPVARIANT var;
	PropVariantInit(&var);
	return g_pSession->Start(&GUID_NULL, &var);
}

// 現在の再生位置を秒で返す (-1 = 取得不可)
static int GetPositionSec()
{
	if (!g_pSession) return -1;
	int result = -1;
	IMFClock* clock = nullptr;
	if (SUCCEEDED(g_pSession->GetClock(&clock)))
	{
		IMFPresentationClock* pc = nullptr;
		if (SUCCEEDED(clock->QueryInterface(IID_PPV_ARGS(&pc))))
		{
			MFTIME t = 0;
			if (SUCCEEDED(pc->GetTime(&t)))
				result = (int)(t / 10000000);
			pc->Release();
		}
		clock->Release();
	}
	return result;
}

// 指定秒へシーク (一時停止中は停止状態を維持)
static void SeekToSec(int sec)
{
	if (!g_pSession) return;
	PROPVARIANT var;
	PropVariantInit(&var);
	var.vt = VT_I8;
	var.hVal.QuadPart = (LONGLONG)sec * 10000000;
	g_pSession->Start(&GUID_NULL, &var);
	if (g_isPaused)
		g_pSession->Pause();
	PropVariantClear(&var);
}

// 再生 ⇔ 一時停止の切り替え
static void TogglePause()
{
	if (!g_pSession) return;
	if (g_isPaused)
	{
		PROPVARIANT var;
		PropVariantInit(&var);  // VT_EMPTY = 現在位置から再開
		g_pSession->Start(&GUID_NULL, &var);
		g_isPaused = false;
		SetWindowTextW(g_hPlayPauseBtn, Tr(L"pause_btn").c_str());
		EnterCriticalSection(&g_cs);
		std::wstring s = g_nowPlaying.empty() ? Tr(L"playing") : g_nowPlaying;
		LeaveCriticalSection(&g_cs);
		SetStatus(s);
	}
	else
	{
		g_pSession->Pause();
		g_isPaused = true;
		SetWindowTextW(g_hPlayPauseBtn, Tr(L"play_btn").c_str());
		SetStatus(Tr(L"paused"));
	}
}

// ウィンドウサイズ変更時に映像領域を追従させる
static void UpdateVideoPosition()
{
	if (!g_pSession) return;
	IMFVideoDisplayControl* disp = nullptr;
	if (SUCCEEDED(MFGetService(g_pSession, MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&disp))))
	{
		RECT rc;
		GetClientRect(g_hVideo, &rc);
		disp->SetVideoPosition(nullptr, &rc);
		disp->Release();
	}
}

// ---------------------------------------------------------------------------
// 再生スレッド
// ---------------------------------------------------------------------------
bool IsLocalPath(const wchar_t* s)
{
	// C:\xxx や D:\xxx を判定
	return (wcslen(s) > 2 && s[1] == L':' && s[2] == L'\\');
}

void ToFileUrl(const wchar_t* path, wchar_t* out, size_t outSize)
{
	// file:///C:/xxx 形式にする
	wsprintfW(out, L"file:///%c:%s", path[0], path + 2);
}

// 再生候補: audioが空ならmuxed/単一ファイル、非空ならDASH (映像+音声)
struct PlayCandidate
{
	std::wstring video;
	std::wstring audio;
	std::wstring label;   // 表示用: "720p DASH" / "360p" 等
};

struct PlayParams
{
	std::vector<PlayCandidate> urls;   // 前から順に試す
	// DASH用 (videoIdが非空かつquality>0ならworker内でAPIから候補を先頭に追加)
	std::wstring instanceUrl;
	std::wstring videoId;
	int quality;        // 目標の高さ (480/720/1080)。0ならDASHを使わない
	bool localProxy;
};

// "bitrate":"123456" (文字列) と "bitrate":123456 (数値) の両方に対応
static long long JsonGetNumberFlexible(const std::wstring& obj, const wchar_t* key)
{
	long long n = JsonGetNumber(obj, key);
	if (n > 0) return n;
	std::wstring s = JsonGetString(obj, key);
	if (!s.empty()) return _wtoi64(s.c_str());
	return n;
}

// adaptiveFormatsからH.264映像とAAC音声の最適ペアを選ぶ
static bool PickDashStreams(const std::wstring& json, int targetHeight,
                            std::wstring& videoUrl, std::wstring& audioUrl,
                            std::wstring& label)
{
	std::wstring arr = JsonGetArray(json, L"adaptiveFormats");
	if (arr.empty()) return false;

	std::vector<std::wstring> objs;
	SplitTopLevelObjects(arr, objs);

	int bestH = 0;
	bool best60 = false;
	long long bestABr = -1;

	for (size_t i = 0; i < objs.size(); i++)
	{
		std::wstring type = JsonGetString(objs[i], L"type");
		std::wstring u = JsonGetString(objs[i], L"url");
		if (u.empty()) continue;

		if (type.find(L"video/mp4") == 0 && type.find(L"avc1") != std::wstring::npos)
		{
			// RTのハードウェアデコーダはH.264のみ (VP9/AV1不可)
			std::wstring ql = JsonGetString(objs[i], L"qualityLabel");
			int hgt = _wtoi(ql.c_str());
			bool is60 = ql.find(L"p60") != std::wstring::npos;
			if (hgt <= 0 || hgt > targetHeight) continue;
			// 高解像度優先、同解像度ならRTの負荷が軽い30fpsを優先
			if (hgt > bestH || (hgt == bestH && best60 && !is60))
			{
				bestH = hgt;
				best60 = is60;
				videoUrl = u;
			}
		}
		else if (type.find(L"audio/mp4") == 0)
		{
			long long br = JsonGetNumberFlexible(objs[i], L"bitrate");
			if (br > bestABr)
			{
				bestABr = br;
				audioUrl = u;
			}
		}
	}

	if (bestH > 0)
	{
		wchar_t buf[32];
		swprintf_s(buf, L"%dp%s DASH", bestH, best60 ? L"60" : L"");
		label = buf;
	}
	return !videoUrl.empty() && !audioUrl.empty();
}

// Start後に再生クロックが実際に動き出すまで待つ。
// 非同期エラーやタイムアウト時はfalseを返す (outHr: E_PENDING=タイムアウト)
static bool WaitForPlaybackStart(HRESULT* outHr, int timeoutMs)
{
	for (int waited = 0; waited < timeoutMs; waited += 200)
	{
		LONG err = g_sessionErrorHr;
		if (err != 0)
		{
			*outHr = (HRESULT)err;
			return false;
		}
		if (!g_pSession)
		{
			*outHr = E_FAIL;
			return false;
		}

		IMFClock* clock = nullptr;
		if (SUCCEEDED(g_pSession->GetClock(&clock)))
		{
			MFCLOCK_STATE state = MFCLOCK_STATE_INVALID;
			clock->GetState(0, &state);
			clock->Release();
			if (state == MFCLOCK_STATE_RUNNING)
			{
				*outHr = S_OK;
				return true;
			}
		}
		Sleep(200);
	}
	*outHr = E_PENDING;
	return false;
}

DWORD WINAPI PlayThread(LPVOID param)
{
	PlayParams* pp = (PlayParams*)param;
	bool played = false;
	std::wstring lastErr;

	// DASH候補の構築 (480p以上が選択されている場合)
	// プロキシ経由 (local=true、相対URLで返るため絶対化) と直接URLの両方を用意
	if (pp->quality > 0 && !pp->videoId.empty())
	{
		PostStatus(Tr(L"fetching_info"));
		std::wstring apiBase = pp->instanceUrl + L"/api/v1/videos/" + pp->videoId;

		std::vector<PlayCandidate> dashCands;
		for (int pass = 0; pass < 2; pass++)
		{
			bool useLocal = (pass == 0);
			if (useLocal && !pp->localProxy)
				continue;

			std::string body;
			std::wstring err;
			if (!HttpGet(apiBase + (useLocal ? L"?local=true" : L""), body, err))
			{
				lastErr = Tr(L"info_fail") + L" (" + err + L")";
				continue;
			}

			std::wstring json = Utf8ToWide(body);
			PlayCandidate dash;
			if (PickDashStreams(json, pp->quality, dash.video, dash.audio, dash.label))
			{
				dash.video = AbsolutizeUrl(dash.video, pp->instanceUrl);
				dash.audio = AbsolutizeUrl(dash.audio, pp->instanceUrl);
				if (!useLocal)
					dash.label += L" " + Tr(L"direct_suffix");
				dashCands.push_back(dash);
			}
			else
			{
				lastErr = Tr(L"no_dash");
			}
		}
		pp->urls.insert(pp->urls.begin(), dashCands.begin(), dashCands.end());
	}

	for (size_t i = 0; i < pp->urls.size(); i++)
	{
		if (pp->urls.size() > 1)
		{
			wchar_t buf[128];
			swprintf_s(buf, Tr(L"loading_n").c_str(), (int)i + 1, (int)pp->urls.size());
			PostStatus(buf);
		}

		const PlayCandidate& c = pp->urls[i];

		if (IsLocalPath(c.video.c_str()))
		{
			wchar_t fileUrl[2048] = {};
			ToFileUrl(c.video.c_str(), fileUrl, 2048);
			HRESULT hr = Play(fileUrl, nullptr);
			if (SUCCEEDED(hr))
			{
				std::wstring playing = Tr(L"playing");
				EnterCriticalSection(&g_cs);
				g_nowPlaying = playing;
				LeaveCriticalSection(&g_cs);
				PostStatus(playing);
				played = true;
				break;
			}
			wchar_t buf[64];
			swprintf_s(buf, L"hr=0x%08X", (unsigned)hr);
			lastErr = buf;
			continue;
		}

		// 事前にWinHTTPで検証し、リダイレクト解決済みURLをMFに渡す
		std::wstring resolvedVideo, resolvedAudio;
		DWORD status = 0;
		if (!ProbeStreamUrl(c.video, resolvedVideo, status))
		{
			if (status != 0)
			{
				wchar_t buf[64];
				swprintf_s(buf, L"HTTP %u", status);
				lastErr = buf;
			}
			else
			{
				lastErr = Tr(L"unreachable");
			}
			continue;
		}
		if (!c.audio.empty())
		{
			if (!ProbeStreamUrl(c.audio, resolvedAudio, status))
			{
				if (status != 0)
				{
					wchar_t buf[64];
					swprintf_s(buf, L"HTTP %u", status);
					lastErr = buf;
				}
				else
				{
					lastErr = Tr(L"unreachable");
				}
				lastErr += Tr(L"audio_suffix");
				continue;
			}
		}

		HRESULT hr = Play(resolvedVideo.c_str(),
			resolvedAudio.empty() ? nullptr : resolvedAudio.c_str());
		if (SUCCEEDED(hr))
		{
			// クロックが実際に動き出したことを確認してから成功扱いにする
			HRESULT startErr = S_OK;
			if (WaitForPlaybackStart(&startErr, 20000))
			{
				// 再生中の解像度と、フォールバックした場合はその理由を表示
				std::wstring msg = Tr(L"playing");
				if (!c.label.empty())
					msg += L" (" + c.label + L")";
				if (i > 0 && !lastErr.empty())
					msg += Tr(L"fallback_note") + lastErr;

				EnterCriticalSection(&g_cs);
				g_nowPlaying = msg;
				LeaveCriticalSection(&g_cs);
				PostStatus(msg);

				played = true;
				break;
			}
			hr = startErr;
			Cleanup();
		}

		if (hr == E_PENDING)
		{
			lastErr = Tr(L"start_timeout");
		}
		else
		{
			wchar_t buf[64];
			swprintf_s(buf, L"hr=0x%08X", (unsigned)hr);
			lastErr = buf;
		}
		if (!c.label.empty())
			lastErr += L" (" + c.label + L")";
	}

	if (!played)
	{
		Cleanup();
		std::wstring msg = Tr(L"play_failed") + lastErr;
		if (lastErr.find(L"HTTP 4") == 0 || lastErr.find(L"HTTP 5") == 0)
			msg += Tr(L"cant_fetch");
		PostStatus(msg);
	}

	delete pp;
	return 0;
}

// ---------------------------------------------------------------------------
// 設定の保存/読み込み (exeと同じフォルダの YT-Legacy-RT.ini)
// ---------------------------------------------------------------------------
static std::wstring GetIniPath()
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash) *(slash + 1) = L'\0';
	std::wstring ini = path;
	ini += L"YT-Legacy-RT.ini";
	return ini;
}

static void LoadSettings()
{
	std::wstring ini = GetIniPath();
	wchar_t buf[1024] = {};

	GetPrivateProfileStringW(L"Settings", L"InstanceUrl", L"http://", buf, 1024, ini.c_str());
	g_instanceUrl = buf;

	g_qualityIdx = GetPrivateProfileIntW(L"Settings", L"Quality", 0, ini.c_str());
	if (g_qualityIdx < 0 || g_qualityIdx > 3) g_qualityIdx = 0;
	g_localProxy = GetPrivateProfileIntW(L"Settings", L"LocalProxy", 1, ini.c_str()) != 0;
	g_ignoreCert = GetPrivateProfileIntW(L"Settings", L"IgnoreCert", 0, ini.c_str()) != 0;

	GetPrivateProfileStringW(L"Settings", L"Language", L"", buf, 1024, ini.c_str());
	g_langSetting = buf;
}

static void SaveSettings()
{
	std::wstring ini = GetIniPath();
	wchar_t num[16];

	WritePrivateProfileStringW(L"Settings", L"InstanceUrl", g_instanceUrl.c_str(), ini.c_str());
	wsprintfW(num, L"%d", g_qualityIdx);
	WritePrivateProfileStringW(L"Settings", L"Quality", num, ini.c_str());
	wsprintfW(num, L"%d", g_localProxy ? 1 : 0);
	WritePrivateProfileStringW(L"Settings", L"LocalProxy", num, ini.c_str());
	wsprintfW(num, L"%d", g_ignoreCert ? 1 : 0);
	WritePrivateProfileStringW(L"Settings", L"IgnoreCert", num, ini.c_str());
	WritePrivateProfileStringW(L"Settings", L"Language", g_langSetting.c_str(), ini.c_str());
}

// ---------------------------------------------------------------------------
// 再生開始 (プレイヤービューへ切り替え)
// ---------------------------------------------------------------------------
static void Layout(HWND hWnd);

// ビュー切り替えで残った旧背景を子コントロールごと消して描き直す
static void RefreshAll()
{
	RedrawWindow(g_hWnd, nullptr, nullptr,
		RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void EnterPlayerView(const std::wstring& title)
{
	g_playerMode = true;
	g_isPaused = false;
	g_userSeeking = false;
	g_seekBarMax = -1;
	SetWindowTextW(g_hPlayerTitle, title.c_str());
	SetWindowTextW(g_hPlayPauseBtn, Tr(L"pause_btn").c_str());
	SetWindowTextW(g_hTimeLabel, L"--:-- / --:--");
	SetWindowTextW(g_hStatsLabel, L"");
	SendMessageW(g_hSeekBar, TBM_SETPOS, TRUE, 0);
	SendMessageW(g_hRecList, LB_RESETCONTENT, 0, 0);

	// チャンネルバーを初期化
	SetWindowTextW(g_hChanName, L"");
	SetWindowTextW(g_hSubBtn, Tr(L"subscribe_btn").c_str());
	EnableWindow(g_hSubBtn, FALSE);
	{
		HBITMAP old = (HBITMAP)SendMessageW(g_hChanIcon, STM_SETIMAGE, IMAGE_BITMAP, 0);
		if (old) DeleteObject(old);
		g_chanIconBmp = nullptr;
	}

	// ミニプレイヤーを初期化 (新しい動画のタイトル/サムネイルに差し替え)
	SetWindowTextW(g_hMiniTitle, title.c_str());
	{
		HBITMAP old = (HBITMAP)SendMessageW(g_hMiniThumb, STM_SETIMAGE, IMAGE_BITMAP, 0);
		if (old) DeleteObject(old);
		g_miniThumbBmp = nullptr;
	}

	Layout(g_hWnd);
	RefreshAll();
}

static void LeavePlayerView()
{
	if (g_fullscreen)
		ToggleFullscreen();
	g_playerMode = false;
	// 再生は停止せず継続する (一覧画面ではミニプレイヤーが表示される)
	Layout(g_hWnd);
	RefreshAll();
}

// ミニプレイヤーのクリック → プレイヤービューへ復帰
static void ReturnToPlayer()
{
	if (g_playerMode || !g_pSession)
		return;
	g_playerMode = true;
	EnterCriticalSection(&g_cs);
	std::wstring s = g_nowPlaying;
	LeaveCriticalSection(&g_cs);
	if (!s.empty() && !g_isPaused)
		SetStatus(s);
	Layout(g_hWnd);
	RefreshAll();
}

static void PlayVideoId(const std::wstring& videoId, const std::wstring& title)
{
	std::wstring instanceUrl = TrimTrailingSlash(g_instanceUrl);
	if (instanceUrl.empty() || instanceUrl == L"http://" || instanceUrl == L"https://")
	{
		SetStatus(Tr(L"set_instance"));
		return;
	}

	// 480p以上はDASH (adaptiveFormatsの映像+音声を合成)。
	// 失敗時やDASHが無い場合は360p muxed (itag 18) へフォールバック。
	static const int qmap[4] = { 0, 480, 720, 1080 };
	PlayParams* pp = new PlayParams;
	pp->instanceUrl = instanceUrl;
	pp->videoId = videoId;
	pp->quality = qmap[(g_qualityIdx >= 0 && g_qualityIdx <= 3) ? g_qualityIdx : 0];
	pp->localProxy = g_localProxy;

	// muxedフォールバック (プロキシ経由 → 直接)
	std::wstring base = instanceUrl + L"/latest_version?id=" + videoId + L"&itag=18";
	PlayCandidate c;
	c.label = L"360p";
	if (g_localProxy)
	{
		c.video = base + L"&local=true";
		pp->urls.push_back(c);
	}
	c.video = base;
	c.label = L"360p " + Tr(L"direct_suffix");
	pp->urls.push_back(c);

	EnterPlayerView(title);
	SetStatus(Tr(L"loading"));
	CreateThread(nullptr, 0, PlayThread, pp, 0, nullptr);

	// 再生数・高評価・おすすめ動画をバックグラウンドで取得
	WatchParams* wpr = new WatchParams;
	wpr->gen = InterlockedIncrement(&g_recGen);
	wpr->instanceUrl = instanceUrl;
	wpr->videoId = videoId;
	CreateThread(nullptr, 0, WatchThread, wpr, 0, nullptr);
}

// 指定チャンネルの動画一覧の取得を開始する (結果はホームのリストへ)
static void StartChannelVideos(const std::wstring& channelId)
{
	if (channelId.empty())
		return;

	ChannelParams* cp = new ChannelParams;
	cp->gen = InterlockedIncrement(&g_searchGen);
	cp->instanceUrl = TrimTrailingSlash(g_instanceUrl);
	cp->channelId = channelId;

	SetStatus(Tr(L"loading"));
	CreateThread(nullptr, 0, ChannelThread, cp, 0, nullptr);
}

// チャンネル名/アイコンのクリック → そのチャンネルの動画一覧を表示
static void OpenChannelVideos()
{
	std::wstring channelId;
	EnterCriticalSection(&g_cs);
	channelId = g_chanId;
	LeaveCriticalSection(&g_cs);
	if (channelId.empty())
		return;

	LeavePlayerView();
	StartChannelVideos(channelId);
}

// チャンネル登録の切り替え
static void ToggleSubscribe()
{
	std::wstring channelId, name;
	EnterCriticalSection(&g_cs);
	channelId = g_chanId;
	name = g_chanTitle;
	LeaveCriticalSection(&g_cs);
	if (channelId.empty())
		return;

	bool nowSub = !IsSubscribed(channelId);
	SetSubscribed(channelId, name, nowSub);
	SetWindowTextW(g_hSubBtn, Tr(nowSub ? L"subscribed_btn" : L"subscribe_btn").c_str());
}

// ---------------------------------------------------------------------------
// 設定ウィンドウ
// ---------------------------------------------------------------------------
static HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w)
{
	HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
		x, y, w, S(20), parent, nullptr, nullptr, nullptr);
	SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
	return h;
}

static void ApplyUiTexts();

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static HWND hInst, hQuality, hLang, hLocal, hCert;

	switch (msg)
	{
	case WM_CREATE:
	{
		int M = S(12);
		int y = M;

		CreateLabel(hWnd, Tr(L"instance_label").c_str(), M, y, S(300));
		y += S(22);
		hInst = CreateWindowW(L"EDIT", g_instanceUrl.c_str(),
			WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
			M, y, S(360), S(26), hWnd, (HMENU)IDC_SET_INSTANCE, nullptr, nullptr);
		y += S(36);

		CreateLabel(hWnd, Tr(L"quality_label").c_str(), M, y + S(3), S(80));
		hQuality = CreateWindowW(L"COMBOBOX", nullptr,
			WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
			M + S(86), y, S(130), S(200), hWnd, (HMENU)IDC_SET_QUALITY, nullptr, nullptr);
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"360p");
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"480p (DASH)");
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"720p (DASH)");
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"1080p (DASH)");
		SendMessageW(hQuality, CB_SETCURSEL, g_qualityIdx, 0);
		y += S(36);

		CreateLabel(hWnd, Tr(L"language_label").c_str(), M, y + S(3), S(80));
		hLang = CreateWindowW(L"COMBOBOX", nullptr,
			WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
			M + S(86), y, S(200), S(240), hWnd, (HMENU)IDC_SET_LANGUAGE, nullptr, nullptr);
		BuildLangChoices();
		{
			int sel = 0;
			for (size_t i = 0; i < g_langChoices.size(); i++)
			{
				SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)g_langChoices[i].display.c_str());
				if (g_langChoices[i].code == g_langSetting)
					sel = (int)i;
			}
			SendMessageW(hLang, CB_SETCURSEL, sel, 0);
		}
		y += S(36);

		hLocal = CreateWindowW(L"BUTTON", Tr(L"local_chk").c_str(),
			WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			M, y, S(360), S(22), hWnd, (HMENU)IDC_SET_LOCAL, nullptr, nullptr);
		SendMessageW(hLocal, BM_SETCHECK, g_localProxy ? BST_CHECKED : BST_UNCHECKED, 0);
		y += S(28);

		hCert = CreateWindowW(L"BUTTON", Tr(L"cert_chk").c_str(),
			WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			M, y, S(360), S(22), hWnd, (HMENU)IDC_SET_IGNORECERT, nullptr, nullptr);
		SendMessageW(hCert, BM_SETCHECK, g_ignoreCert ? BST_CHECKED : BST_UNCHECKED, 0);
		y += S(38);

		CreateWindowW(L"BUTTON", Tr(L"ok_btn").c_str(),
			WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
			M + S(170), y, S(90), S(28), hWnd, (HMENU)IDC_SET_OK, nullptr, nullptr);
		CreateWindowW(L"BUTTON", Tr(L"cancel_btn").c_str(),
			WS_CHILD | WS_VISIBLE,
			M + S(270), y, S(90), S(28), hWnd, (HMENU)IDC_SET_CANCEL, nullptr, nullptr);

		HWND child = GetWindow(hWnd, GW_CHILD);
		while (child)
		{
			SendMessageW(child, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
			child = GetWindow(child, GW_HWNDNEXT);
		}
		break;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDC_SET_OK)
		{
			g_instanceUrl = TrimTrailingSlash(GetWindowTextStr(hInst));
			g_qualityIdx = (int)SendMessageW(hQuality, CB_GETCURSEL, 0, 0);
			if (g_qualityIdx < 0 || g_qualityIdx > 3) g_qualityIdx = 0;
			g_localProxy = SendMessageW(hLocal, BM_GETCHECK, 0, 0) == BST_CHECKED;
			g_ignoreCert = SendMessageW(hCert, BM_GETCHECK, 0, 0) == BST_CHECKED;

			int langSel = (int)SendMessageW(hLang, CB_GETCURSEL, 0, 0);
			if (langSel >= 0 && langSel < (int)g_langChoices.size() &&
				g_langChoices[langSel].code != g_langSetting)
			{
				g_langSetting = g_langChoices[langSel].code;
				ReloadLanguage();
				ApplyUiTexts();
			}

			SaveSettings();
			DestroyWindow(hWnd);
		}
		else if (LOWORD(wp) == IDC_SET_CANCEL)
		{
			DestroyWindow(hWnd);
		}
		break;

	case WM_CLOSE:
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		EnableWindow(g_hWnd, TRUE);
		SetForegroundWindow(g_hWnd);
		break;
	}
	return DefWindowProc(hWnd, msg, wp, lp);
}

static void OpenSettings()
{
	RECT rc;
	GetWindowRect(g_hWnd, &rc);
	int w = S(410), h = S(300);
	int x = rc.left + ((rc.right - rc.left) - w) / 2;
	int y = rc.top + ((rc.bottom - rc.top) - h) / 2;

	EnableWindow(g_hWnd, FALSE);
	CreateWindowW(L"YTLegacyRTSettings", Tr(L"settings_title").c_str(),
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		x, y, w, h, g_hWnd, nullptr, nullptr, nullptr);
}

// 言語変更をメインウィンドウの各コントロールへ反映する
static void ApplyUiTexts()
{
	SetWindowTextW(g_hSearchBtn, Tr(L"search_btn").c_str());
	SetWindowTextW(g_hSettingsBtn, Tr(L"settings_btn").c_str());
	SetWindowTextW(g_hSubsBtn, Tr(L"subs_btn").c_str());
	SetWindowTextW(g_hBackBtn, Tr(L"back_btn").c_str());
	SetWindowTextW(g_hPlayPauseBtn, Tr(g_isPaused ? L"play_btn" : L"pause_btn").c_str());
	SetWindowTextW(g_hFullscreenBtn, Tr(g_fullscreen ? L"fullscreen_exit" : L"fullscreen").c_str());
	if (!g_playerMode)
		SetStatus(Tr(L"ready"));
	// ラベル長が変わるのでボタン幅を再計算
	Layout(g_hWnd);
	RefreshAll();
}

// ---------------------------------------------------------------------------
// フルスクリーン切り替え
// ---------------------------------------------------------------------------
static void RefreshAll();

static void ToggleFullscreen()
{
	DWORD style = (DWORD)GetWindowLongPtrW(g_hWnd, GWL_STYLE);
	if (!g_fullscreen)
	{
		MONITORINFO mi = { sizeof(mi) };
		if (GetWindowPlacement(g_hWnd, &g_wpPrev) &&
			GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi))
		{
			// SetWindowPos中に同期的にWM_SIZE→Layout()が走るため、
			// フラグとボタン表記は必ず先に更新しておく
			g_fullscreen = true;
			SetWindowTextW(g_hFullscreenBtn, Tr(L"fullscreen_exit").c_str());
			SetWindowLongPtrW(g_hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(g_hWnd, HWND_TOP,
				mi.rcMonitor.left, mi.rcMonitor.top,
				mi.rcMonitor.right - mi.rcMonitor.left,
				mi.rcMonitor.bottom - mi.rcMonitor.top,
				SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}
	else
	{
		g_fullscreen = false;
		SetWindowTextW(g_hFullscreenBtn, Tr(L"fullscreen").c_str());
		SetWindowLongPtrW(g_hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPlacement(g_hWnd, &g_wpPrev);
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}

	// 最大化⇔ボーダーレスなどサイズが変わらずWM_SIZEが来ないケースに備え、
	// 切り替え後の状態で必ず再レイアウトする
	Layout(g_hWnd);
	RefreshAll();
}

// ---------------------------------------------------------------------------
// シークバー / 時間表示の更新
// ---------------------------------------------------------------------------
static void FormatTime(int sec, wchar_t* out)
{
	if (sec < 0)
		lstrcpyW(out, L"--:--");
	else if (sec >= 3600)
		wsprintfW(out, L"%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
	else
		wsprintfW(out, L"%d:%02d", sec / 60, sec % 60);
}

static void UpdateTimeLabel(int pos, int dur)
{
	wchar_t p[32], d[32], buf[80];
	FormatTime(pos, p);
	FormatTime(dur, d);
	wsprintfW(buf, L"%s / %s", p, d);
	SetWindowTextW(g_hTimeLabel, buf);
}

static void UpdateSeekUI()
{
	if (!g_playerMode || !g_pSession) return;

	int dur = (int)(g_duration100ns / 10000000);
	if (dur > 0 && g_seekBarMax != dur)
	{
		g_seekBarMax = dur;
		SendMessageW(g_hSeekBar, TBM_SETRANGEMIN, FALSE, 0);
		SendMessageW(g_hSeekBar, TBM_SETRANGEMAX, TRUE, dur);
		SendMessageW(g_hSeekBar, TBM_SETLINESIZE, 0, 5);
		SendMessageW(g_hSeekBar, TBM_SETPAGESIZE, 0, 10);
		EnableWindow(g_hSeekBar, TRUE);
	}
	if (dur <= 0)
		EnableWindow(g_hSeekBar, FALSE);

	int pos = GetPositionSec();

	// ドラッグ中はユーザーのつまみ位置を優先する
	if (g_userSeeking)
		pos = (int)SendMessageW(g_hSeekBar, TBM_GETPOS, 0, 0);
	else if (pos >= 0)
		SendMessageW(g_hSeekBar, TBM_SETPOS, TRUE, pos);

	UpdateTimeLabel(pos, dur > 0 ? dur : -1);
}

// ---------------------------------------------------------------------------
// カード型リストの描画
// ---------------------------------------------------------------------------
static int ItemHeight() { return ThumbH() + S(16); }

static void DrawListItem(DRAWITEMSTRUCT* dis)
{
	HDC dc = dis->hDC;
	RECT rc = dis->rcItem;

	// 背景
	FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

	if (dis->itemID == (UINT)-1) return;

	// カード領域
	RECT card = rc;
	card.left += S(4);
	card.right -= S(4);
	card.top += S(3);
	card.bottom -= S(3);

	bool selected = (dis->itemState & ODS_SELECTED) != 0;
	HBRUSH bg = CreateSolidBrush(selected ? RGB(229, 241, 251) : RGB(255, 255, 255));
	FillRect(dc, &card, bg);
	DeleteObject(bg);
	HBRUSH frame = CreateSolidBrush(selected ? RGB(0, 120, 215) : RGB(210, 210, 210));
	FrameRect(dc, &card, frame);
	DeleteObject(frame);

	int pad = S(5);
	int idx = (int)dis->itemID;
	bool channelMode = (g_listMode == 1 && dis->CtlID == IDC_LIST);

	std::wstring title, author;
	long long len = -1;
	HBITMAP thumb = nullptr;
	EnterCriticalSection(&g_cs);
	std::vector<VideoItem>& items = (dis->CtlID == IDC_REC_LIST) ? g_recResults : g_results;
	if (idx < (int)items.size())
	{
		title = items[idx].title;
		author = items[idx].author;
		len = items[idx].lengthSeconds;
		thumb = items[idx].thumb;
	}

	// チャンネル一覧モードはチャンネル名のみのシンプルなカード
	if (channelMode)
	{
		LeaveCriticalSection(&g_cs);
		RECT trc = { card.left + S(12), card.top, card.right - S(8), card.bottom };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(20, 20, 20));
		SelectObject(dc, g_hFontTitle);
		DrawTextW(dc, title.c_str(), -1, &trc,
			DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		return;
	}

	// サムネイル
	RECT thumbRc = { card.left + pad, card.top + pad,
	                 card.left + pad + ThumbW(), card.top + pad + ThumbH() };
	if (thumb)
	{
		HDC mem = CreateCompatibleDC(dc);
		HGDIOBJ old = SelectObject(mem, thumb);
		BitBlt(dc, thumbRc.left, thumbRc.top, ThumbW(), ThumbH(), mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteDC(mem);
	}
	else
	{
		HBRUSH ph = CreateSolidBrush(RGB(225, 225, 225));
		FillRect(dc, &thumbRc, ph);
		DeleteObject(ph);
	}
	LeaveCriticalSection(&g_cs);

	// 再生時間バッジ (サムネイル右下)
	if (len >= 0)
	{
		wchar_t badge[32];
		if (len >= 3600)
			swprintf_s(badge, L"%lld:%02lld:%02lld", len / 3600, (len / 60) % 60, len % 60);
		else
			swprintf_s(badge, L"%lld:%02lld", len / 60, len % 60);

		SelectObject(dc, g_hFontSmall);
		SIZE sz;
		GetTextExtentPoint32W(dc, badge, lstrlenW(badge), &sz);
		RECT brc = { thumbRc.right - sz.cx - S(8), thumbRc.bottom - sz.cy - S(4),
		             thumbRc.right - S(2), thumbRc.bottom - S(2) };
		HBRUSH bb = CreateSolidBrush(RGB(0, 0, 0));
		FillRect(dc, &brc, bb);
		DeleteObject(bb);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		DrawTextW(dc, badge, -1, &brc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}

	// タイトル (折り返し最大3行)
	RECT textRc = { thumbRc.right + S(10), card.top + pad,
	                card.right - pad, card.bottom - pad - S(20) };
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(20, 20, 20));
	SelectObject(dc, g_hFontTitle);
	DrawTextW(dc, title.c_str(), -1, &textRc, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);

	// 投稿者 (カード下部)
	RECT authorRc = { textRc.left, card.bottom - pad - S(18), textRc.right, card.bottom - pad };
	SetTextColor(dc, RGB(110, 110, 110));
	SelectObject(dc, g_hFontSmall);
	DrawTextW(dc, author.c_str(), -1, &authorRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// UIレイアウト
// ---------------------------------------------------------------------------
// ラベルの実測幅からボタン幅を決める (言語によって文字数が大きく変わるため)
static int ButtonWidth(const std::wstring& text, int minW)
{
	HDC dc = GetDC(g_hWnd);
	HGDIOBJ old = SelectObject(dc, g_hFontUI);
	SIZE sz = {};
	GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
	SelectObject(dc, old);
	ReleaseDC(g_hWnd, dc);
	int wid = sz.cx + S(20);
	return wid > minW ? wid : minW;
}

static void Layout(HWND hWnd)
{
	RECT rc;
	GetClientRect(hWnd, &rc);
	int w = rc.right;
	int h = rc.bottom;
	const int M = S(8);
	int statusH = S(20);

	int listVis = g_playerMode ? SW_HIDE : SW_SHOW;
	int playerVis = g_playerMode ? SW_SHOW : SW_HIDE;

	ShowWindow(g_hSearchEdit, listVis);
	ShowWindow(g_hSearchBtn, listVis);
	ShowWindow(g_hSettingsBtn, listVis);
	ShowWindow(g_hSubsBtn, listVis);
	ShowWindow(g_hList, listVis);
	ShowWindow(g_hBackBtn, playerVis);
	ShowWindow(g_hPlayerTitle, playerVis);
	ShowWindow(g_hVideo, playerVis);
	ShowWindow(g_hPlayPauseBtn, playerVis);
	ShowWindow(g_hSeekBar, playerVis);
	ShowWindow(g_hTimeLabel, playerVis);
	ShowWindow(g_hFullscreenBtn, playerVis);
	ShowWindow(g_hRecList, g_playerMode && !g_fullscreen ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hStatsLabel, g_playerMode && !g_fullscreen ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hChanIcon, g_playerMode && !g_fullscreen ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hChanName, g_playerMode && !g_fullscreen ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hSubBtn, g_playerMode && !g_fullscreen ? SW_SHOW : SW_HIDE);

	// ミニプレイヤー (一覧画面で再生セッションが残っている間だけ表示)
	bool miniVisible = !g_playerMode && g_pSession != nullptr;
	ShowWindow(g_hMiniThumb, miniVisible ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hMiniTitle, miniVisible ? SW_SHOW : SW_HIDE);
	ShowWindow(g_hMiniClose, miniVisible ? SW_SHOW : SW_HIDE);

	if (!g_playerMode)
	{
		// 上段: 検索ボックス + 検索 + 登録チャンネル + 設定
		int topH = S(32);
		int searchW = ButtonWidth(Tr(L"search_btn"), S(44));
		int subsW = ButtonWidth(Tr(L"subs_btn"), S(44));
		int settingsW = ButtonWidth(Tr(L"settings_btn"), S(44));
		MoveWindow(g_hSearchEdit, M, M,
			w - M * 2 - searchW - subsW - settingsW - S(18), topH, TRUE);
		MoveWindow(g_hSearchBtn, w - M - settingsW - S(6) - subsW - S(6) - searchW, M, searchW, topH, TRUE);
		MoveWindow(g_hSubsBtn, w - M - settingsW - S(6) - subsW, M, subsW, topH, TRUE);
		MoveWindow(g_hSettingsBtn, w - M - settingsW, M, settingsW, topH, TRUE);

		// リスト (ミニプレイヤー表示中はその分だけ短くする)
		int y = M + topH + S(6);
		int listBottom = h - statusH - M;
		if (miniVisible)
		{
			int miniH = S(44);
			int miniY = h - statusH - miniH - S(2);
			int thumbW = S(72);
			int closeW = S(28);
			int barW = w - M * 2;
			if (barW > S(420)) barW = S(420);   // 左下にコンパクトに置く
			MoveWindow(g_hMiniThumb, M, miniY + S(2), thumbW, S(40), TRUE);
			MoveWindow(g_hMiniClose, M + barW - closeW, miniY + S(8), closeW, S(28), TRUE);
			MoveWindow(g_hMiniTitle, M + thumbW + S(6), miniY + S(12),
				barW - thumbW - closeW - S(14), S(20), TRUE);
			listBottom = miniY - S(4);
		}
		MoveWindow(g_hList, M, y, w - M * 2, listBottom - y, TRUE);
	}
	else
	{
		// 上段: 戻るボタン
		int topH = S(32);
		int backW = ButtonWidth(Tr(L"back_btn"), S(70));
		MoveWindow(g_hBackBtn, M, M, backW, topH, TRUE);

		// 右列: チャンネルバー + おすすめ動画 (フルスクリーン中は非表示で映像を広く使う)
		int recW = g_fullscreen ? 0 : S(300);
		int y = M + topH + S(6);
		if (recW > w / 2) recW = w / 2;
		if (recW > 0)
		{
			int rx = w - M - recW;
			int chH = S(40);
			int iconSz = S(36);
			int subW = ButtonWidth(Tr(L"subscribe_btn"), S(70));
			int subW2 = ButtonWidth(Tr(L"subscribed_btn"), S(70));
			if (subW2 > subW) subW = subW2;

			MoveWindow(g_hChanIcon, rx, y, iconSz, iconSz, TRUE);
			MoveWindow(g_hChanName, rx + iconSz + S(6), y + S(9),
				recW - iconSz - subW - S(14), S(20), TRUE);
			MoveWindow(g_hSubBtn, rx + recW - subW, y + S(3), subW, S(30), TRUE);

			MoveWindow(g_hRecList, rx, y + chH + S(4), recW,
				h - y - chH - S(4) - statusH - S(4), TRUE);
		}

		// 左列の幅
		int lw = w - M * 2 - (recW > 0 ? recW + S(8) : 0);

		// 下段: 再生/一時停止 + シークバー + 時間 + 全画面 (左列の幅に収める)
		// ボタン幅は両状態のラベルの長い方に合わせ、切り替えで幅が変わらないようにする
		int ctrlH = S(32);
		int ctrlY = h - statusH - ctrlH - S(4);
		int btnW = ButtonWidth(Tr(L"pause_btn"), S(80));
		int playW = ButtonWidth(Tr(L"play_btn"), S(80));
		if (playW > btnW) btnW = playW;
		int timeW = S(110);
		int fsW = ButtonWidth(Tr(L"fullscreen"), S(80));
		int fsExitW = ButtonWidth(Tr(L"fullscreen_exit"), S(80));
		if (fsExitW > fsW) fsW = fsExitW;
		int cr = M + lw;   // 左列右端
		MoveWindow(g_hPlayPauseBtn, M, ctrlY, btnW, ctrlH, TRUE);
		MoveWindow(g_hSeekBar, M + btnW + S(8), ctrlY, lw - btnW - timeW - fsW - S(24), ctrlH, TRUE);
		MoveWindow(g_hTimeLabel, cr - fsW - S(8) - timeW, ctrlY + S(8), timeW, ctrlH - S(8), TRUE);
		MoveWindow(g_hFullscreenBtn, cr - fsW, ctrlY, fsW, ctrlH, TRUE);

		// タイトル + 統計 (映像の下)
		int infoH = g_fullscreen ? 0 : S(64);
		if (infoH > 0)
		{
			int infoY = ctrlY - infoH;
			MoveWindow(g_hPlayerTitle, M, infoY, lw, S(40), TRUE);
			MoveWindow(g_hStatsLabel, M, infoY + S(44), lw, S(18), TRUE);
		}
		else
		{
			MoveWindow(g_hPlayerTitle, 0, 0, 0, 0, TRUE);
		}

		// 映像
		MoveWindow(g_hVideo, M, y, lw, ctrlY - infoH - y - S(4), TRUE);
	}

	MoveWindow(g_hStatus, M, h - statusH, w - M * 2, statusH, TRUE);
	UpdateVideoPosition();
}

static void FillResultList()
{
	SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);

	int count;
	std::wstring status;
	EnterCriticalSection(&g_cs);
	count = (int)g_results.size();
	status = g_searchError;
	LeaveCriticalSection(&g_cs);

	for (int i = 0; i < count; i++)
		SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)i);

	if (status.empty())
	{
		wchar_t buf[128];
		swprintf_s(buf, Tr(L"results_found").c_str(), count);
		status = buf;
	}
	SetStatus(status);
}

// 登録チャンネルの一覧をホームのリストに表示する
static void ShowSubscriptions()
{
	EnsureSubsFileUnicode();
	std::vector<wchar_t> buf(32768, L'\0');
	GetPrivateProfileSectionW(L"Subscriptions", buf.data(), 32768, GetSubsPath().c_str());

	EnterCriticalSection(&g_cs);
	InterlockedIncrement(&g_searchGen);   // 進行中の検索/サムネイル取得を無効化
	ClearResultsLocked();
	g_searchError.clear();
	const wchar_t* p = buf.data();
	while (*p)
	{
		std::wstring line = p;
		p += line.size() + 1;
		size_t eq = line.find(L'=');
		if (eq != std::wstring::npos && eq > 0)
		{
			VideoItem item;
			item.videoId = line.substr(0, eq);   // チャンネルID
			item.title = line.substr(eq + 1);    // チャンネル名
			item.lengthSeconds = -1;
			item.thumb = nullptr;
			g_results.push_back(item);
		}
	}
	int count = (int)g_results.size();
	LeaveCriticalSection(&g_cs);

	g_listMode = 1;
	SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, S(44));
	SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);
	for (int i = 0; i < count; i++)
		SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)i);
	InvalidateRect(g_hList, nullptr, TRUE);

	if (count > 0)
	{
		wchar_t sbuf[128];
		swprintf_s(sbuf, Tr(L"channels_found").c_str(), count);
		SetStatus(sbuf);
	}
	else
	{
		SetStatus(Tr(L"no_subs"));
	}
}

static void StartSearch()
{
	std::wstring query = GetWindowTextStr(g_hSearchEdit);
	std::wstring instanceUrl = TrimTrailingSlash(g_instanceUrl);
	if (query.empty())
	{
		SetStatus(Tr(L"enter_query"));
		return;
	}
	if (instanceUrl.empty() || instanceUrl == L"http://" || instanceUrl == L"https://")
	{
		SetStatus(Tr(L"set_instance"));
		return;
	}

	// URLやローカルパスを直接入力した場合はそのまま再生
	if (query.find(L"http://") == 0 || query.find(L"https://") == 0 || IsLocalPath(query.c_str()))
	{
		PlayParams* pp = new PlayParams;
		pp->quality = 0;
		pp->localProxy = false;
		PlayCandidate c;
		c.video = query;
		pp->urls.push_back(c);
		EnterPlayerView(query);
		SetStatus(Tr(L"loading"));
		CreateThread(nullptr, 0, PlayThread, pp, 0, nullptr);
		return;
	}

	SearchParams* sp = new SearchParams;
	sp->gen = InterlockedIncrement(&g_searchGen);
	sp->instanceUrl = instanceUrl;
	sp->query = query;
	CreateThread(nullptr, 0, SearchThread, sp, 0, nullptr);
}

// シークバー: クリック/タップした位置へつまみを直接移動させる
// (既定はページ単位ジャンプのため細かくシークできない)
LRESULT CALLBACK SeekBarProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_LBUTTONDOWN)
	{
		RECT ch = {};
		SendMessageW(hWnd, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
		int mn = (int)SendMessageW(hWnd, TBM_GETRANGEMIN, 0, 0);
		int mx = (int)SendMessageW(hWnd, TBM_GETRANGEMAX, 0, 0);
		int x = GET_X_LPARAM(lp);
		if (mx > mn && ch.right > ch.left)
		{
			int pos = mn + MulDiv(x - ch.left, mx - mn, ch.right - ch.left);
			if (pos < mn) pos = mn;
			if (pos > mx) pos = mx;
			SendMessageW(hWnd, TBM_SETPOS, TRUE, pos);
			// この後既定の処理がつまみ (クリック位置に移動済み) のドラッグを開始し、
			// 離した時点で TB_ENDTRACK が飛んでシークされる
		}
	}
	return CallWindowProcW(g_oldTrackProc, hWnd, msg, wp, lp);
}

// 検索欄でEnterキーを押したら検索
LRESULT CALLBACK SearchEditProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_KEYDOWN && wp == VK_RETURN)
	{
		StartSearch();
		return 0;
	}
	if (msg == WM_CHAR && wp == VK_RETURN)
		return 0;  // ビープ音防止
	return CallWindowProcW(g_oldEditProc, hWnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// メインウィンドウ
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
		g_hWnd = hWnd;

		g_hSearchEdit = CreateWindowW(L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SEARCH_EDIT, nullptr, nullptr);
		g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);

		g_hSearchBtn = CreateWindowW(L"BUTTON", Tr(L"search_btn").c_str(),
			WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SEARCH_BTN, nullptr, nullptr);

		g_hSettingsBtn = CreateWindowW(L"BUTTON", Tr(L"settings_btn").c_str(),
			WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SETTINGS_BTN, nullptr, nullptr);

		g_hSubsBtn = CreateWindowW(L"BUTTON", Tr(L"subs_btn").c_str(),
			WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SUBS_BTN, nullptr, nullptr);

		g_hList = CreateWindowW(L"LISTBOX", nullptr,
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
			LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
			0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, nullptr, nullptr);
		SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, ItemHeight());

		g_hStatus = CreateWindowW(L"STATIC", Tr(L"ready").c_str(),
			WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
			0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS, nullptr, nullptr);

		g_hBackBtn = CreateWindowW(L"BUTTON", Tr(L"back_btn").c_str(),
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_BACK_BTN, nullptr, nullptr);

		g_hPlayerTitle = CreateWindowW(L"STATIC", L"",
			WS_CHILD | SS_ENDELLIPSIS,
			0, 0, 0, 0, hWnd, (HMENU)IDC_PLAYER_TITLE, nullptr, nullptr);

		g_hPlayPauseBtn = CreateWindowW(L"BUTTON", Tr(L"pause_btn").c_str(),
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_PLAYPAUSE_BTN, nullptr, nullptr);

		g_hSeekBar = CreateWindowW(TRACKBAR_CLASSW, nullptr,
			WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SEEKBAR, nullptr, nullptr);
		g_oldTrackProc = (WNDPROC)SetWindowLongPtrW(g_hSeekBar, GWLP_WNDPROC, (LONG_PTR)SeekBarProc);

		g_hTimeLabel = CreateWindowW(L"STATIC", L"--:-- / --:--",
			WS_CHILD | SS_RIGHT,
			0, 0, 0, 0, hWnd, (HMENU)IDC_TIME_LABEL, nullptr, nullptr);

		g_hFullscreenBtn = CreateWindowW(L"BUTTON", Tr(L"fullscreen").c_str(),
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_FULLSCREEN_BTN, nullptr, nullptr);

		g_hRecList = CreateWindowW(L"LISTBOX", nullptr,
			WS_CHILD | WS_VSCROLL | LBS_NOTIFY |
			LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
			0, 0, 0, 0, hWnd, (HMENU)IDC_REC_LIST, nullptr, nullptr);
		SendMessageW(g_hRecList, LB_SETITEMHEIGHT, 0, ItemHeight());

		g_hStatsLabel = CreateWindowW(L"STATIC", L"",
			WS_CHILD | SS_ENDELLIPSIS,
			0, 0, 0, 0, hWnd, (HMENU)IDC_STATS_LABEL, nullptr, nullptr);

		g_hChanIcon = CreateWindowW(L"STATIC", nullptr,
			WS_CHILD | SS_BITMAP | SS_CENTERIMAGE | SS_NOTIFY,
			0, 0, 0, 0, hWnd, (HMENU)IDC_CHAN_ICON, nullptr, nullptr);

		g_hChanName = CreateWindowW(L"STATIC", L"",
			WS_CHILD | SS_ENDELLIPSIS | SS_NOTIFY,
			0, 0, 0, 0, hWnd, (HMENU)IDC_CHAN_NAME, nullptr, nullptr);

		g_hSubBtn = CreateWindowW(L"BUTTON", Tr(L"subscribe_btn").c_str(),
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SUB_BTN, nullptr, nullptr);

		g_hMiniThumb = CreateWindowW(L"STATIC", nullptr,
			WS_CHILD | SS_BITMAP | SS_CENTERIMAGE | SS_NOTIFY | WS_BORDER,
			0, 0, 0, 0, hWnd, (HMENU)IDC_MINI_THUMB, nullptr, nullptr);

		g_hMiniTitle = CreateWindowW(L"STATIC", L"",
			WS_CHILD | SS_ENDELLIPSIS | SS_NOTIFY,
			0, 0, 0, 0, hWnd, (HMENU)IDC_MINI_TITLE, nullptr, nullptr);

		g_hMiniClose = CreateWindowW(L"BUTTON", L"\x2715",
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_MINI_CLOSE, nullptr, nullptr);

		SetTimer(hWnd, TIMER_POSITION, 500, nullptr);

		g_hVideo = CreateWindowW(L"STATIC", nullptr,
			WS_CHILD | SS_NOTIFY,
			0, 0, 0, 0, hWnd, (HMENU)IDC_VIDEO, nullptr, nullptr);

		{
			HWND child = GetWindow(hWnd, GW_CHILD);
			while (child)
			{
				SendMessageW(child, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
				child = GetWindow(child, GW_HWNDNEXT);
			}
		}
		SendMessageW(g_hPlayerTitle, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

		Layout(hWnd);
		break;

	case WM_SIZE:
		Layout(hWnd);
		break;

	case WM_TIMER:
		if (wp == TIMER_POSITION)
			UpdateSeekUI();
		break;

	case WM_KEYDOWN:
		if (wp == VK_ESCAPE && g_fullscreen)
			ToggleFullscreen();
		break;

	case WM_ERASEBKGND:
		// プレイヤービューは黒背景
		if (g_playerMode)
		{
			RECT rc;
			GetClientRect(hWnd, &rc);
			FillRect((HDC)wp, &rc, g_hbrBlack);
			return 1;
		}
		break;

	case WM_CTLCOLORSTATIC:
		if (g_playerMode)
		{
			HDC dc = (HDC)wp;
			SetTextColor(dc, RGB(230, 230, 230));
			SetBkMode(dc, TRANSPARENT);
			return (LRESULT)g_hbrBlack;
		}
		break;

	case WM_HSCROLL:
		if ((HWND)lp == g_hSeekBar)
		{
			int code = LOWORD(wp);
			if (code == TB_THUMBTRACK)
			{
				// ドラッグ中: 時間表示だけ追従させる
				g_userSeeking = true;
				int pos = (int)SendMessageW(g_hSeekBar, TBM_GETPOS, 0, 0);
				UpdateTimeLabel(pos, g_seekBarMax);
			}
			else if (code == TB_ENDTRACK)
			{
				int pos = (int)SendMessageW(g_hSeekBar, TBM_GETPOS, 0, 0);
				SeekToSec(pos);
				g_userSeeking = false;
			}
		}
		break;

	case WM_MEASUREITEM:
	{
		MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
		if (mis->CtlID == IDC_LIST || mis->CtlID == IDC_REC_LIST)
		{
			mis->itemHeight = ItemHeight();
			return TRUE;
		}
		break;
	}

	case WM_DRAWITEM:
	{
		DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
		if (dis->CtlID == IDC_LIST || dis->CtlID == IDC_REC_LIST)
		{
			DrawListItem(dis);
			return TRUE;
		}
		break;
	}

	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDC_SEARCH_BTN:
			StartSearch();
			break;

		case IDC_SETTINGS_BTN:
			OpenSettings();
			break;

		case IDC_SUBS_BTN:
			ShowSubscriptions();
			break;

		case IDC_BACK_BTN:
			LeavePlayerView();
			SetStatus(Tr(L"ready"));
			break;

		case IDC_PLAYPAUSE_BTN:
			TogglePause();
			break;

		case IDC_FULLSCREEN_BTN:
			ToggleFullscreen();
			break;

		case IDC_VIDEO:
			// 映像のダブルクリック/ダブルタップで全画面切り替え
			if (HIWORD(wp) == STN_DBLCLK && g_playerMode)
				ToggleFullscreen();
			break;

		case IDC_SUB_BTN:
			ToggleSubscribe();
			break;

		case IDC_CHAN_ICON:
		case IDC_CHAN_NAME:
			// チャンネル登録ボタン以外の部分 → チャンネルの動画一覧へ
			if (HIWORD(wp) == STN_CLICKED && g_playerMode)
				OpenChannelVideos();
			break;

		case IDC_MINI_THUMB:
		case IDC_MINI_TITLE:
			// ミニプレイヤーのクリック → 再生中の動画に戻る
			if (HIWORD(wp) == STN_CLICKED)
				ReturnToPlayer();
			break;

		case IDC_MINI_CLOSE:
			// 再生を停止してミニプレイヤーを閉じる
			Cleanup();
			Layout(hWnd);
			SetStatus(Tr(L"ready"));
			break;

		case IDC_LIST:
			if (HIWORD(wp) == LBN_DBLCLK)
			{
				int sel = (int)SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
				std::wstring videoId, title;
				EnterCriticalSection(&g_cs);
				if (sel >= 0 && sel < (int)g_results.size())
				{
					videoId = g_results[sel].videoId;
					title = g_results[sel].title;
				}
				LeaveCriticalSection(&g_cs);
				if (videoId.empty())
					break;
				if (g_listMode == 1)
					StartChannelVideos(videoId);   // videoId = チャンネルID
				else
					PlayVideoId(videoId, title);
			}
			break;

		case IDC_REC_LIST:
			if (HIWORD(wp) == LBN_DBLCLK)
			{
				int sel = (int)SendMessageW(g_hRecList, LB_GETCURSEL, 0, 0);
				std::wstring videoId, title;
				EnterCriticalSection(&g_cs);
				if (sel >= 0 && sel < (int)g_recResults.size())
				{
					videoId = g_recResults[sel].videoId;
					title = g_recResults[sel].title;
				}
				LeaveCriticalSection(&g_cs);
				if (!videoId.empty())
					PlayVideoId(videoId, title);
			}
			break;
		}
		break;

	case WM_APP_SEARCHDONE:
		if ((LONG)wp == g_searchGen)
		{
			// 動画一覧モードに戻す (チャンネル一覧から遷移した場合)
			if (g_listMode != 0)
			{
				g_listMode = 0;
				SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, ItemHeight());
			}
			FillResultList();

			// サムネイル取得開始
			ThumbParams* tp = new ThumbParams;
			tp->gen = (LONG)wp;
			tp->instanceUrl = TrimTrailingSlash(g_instanceUrl);
			tp->rec = false;
			CreateThread(nullptr, 0, ThumbThread, tp, 0, nullptr);
		}
		break;

	case WM_APP_THUMB:
		if ((LONG)lp == g_searchGen)
		{
			RECT rc;
			if (SendMessageW(g_hList, LB_GETITEMRECT, wp, (LPARAM)&rc) != LB_ERR)
				InvalidateRect(g_hList, &rc, TRUE);
		}
		break;

	case WM_APP_WATCHINFO:
		if ((LONG)wp == g_recGen)
		{
			int count;
			std::wstring stats, chanId, chanTitle;
			EnterCriticalSection(&g_cs);
			count = (int)g_recResults.size();
			stats = g_watchStats;
			chanId = g_chanId;
			chanTitle = g_chanTitle;
			LeaveCriticalSection(&g_cs);

			SetWindowTextW(g_hStatsLabel, stats.c_str());

			// チャンネルバー更新
			SetWindowTextW(g_hChanName, chanTitle.c_str());
			SetWindowTextW(g_hSubBtn,
				Tr(IsSubscribed(chanId) ? L"subscribed_btn" : L"subscribe_btn").c_str());
			EnableWindow(g_hSubBtn, !chanId.empty());

			SendMessageW(g_hRecList, LB_RESETCONTENT, 0, 0);
			for (int i = 0; i < count; i++)
				SendMessageW(g_hRecList, LB_ADDSTRING, 0, (LPARAM)i);

			if (count > 0)
			{
				ThumbParams* tp = new ThumbParams;
				tp->gen = (LONG)wp;
				tp->instanceUrl = TrimTrailingSlash(g_instanceUrl);
				tp->rec = true;
				CreateThread(nullptr, 0, ThumbThread, tp, 0, nullptr);
			}
		}
		break;

	case WM_APP_RECTHUMB:
		if ((LONG)lp == g_recGen)
		{
			RECT rc;
			if (SendMessageW(g_hRecList, LB_GETITEMRECT, wp, (LPARAM)&rc) != LB_ERR)
				InvalidateRect(g_hRecList, &rc, TRUE);
		}
		break;

	case WM_APP_CHANICON:
	{
		HBITMAP bmp = (HBITMAP)lp;
		if ((LONG)wp == g_recGen && bmp)
		{
			HBITMAP old = (HBITMAP)SendMessageW(g_hChanIcon, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bmp);
			if (old) DeleteObject(old);
			g_chanIconBmp = bmp;
		}
		else if (bmp)
		{
			DeleteObject(bmp);
		}
		break;
	}

	case WM_APP_MINITHUMB:
	{
		HBITMAP bmp = (HBITMAP)lp;
		if ((LONG)wp == g_recGen && bmp)
		{
			HBITMAP old = (HBITMAP)SendMessageW(g_hMiniThumb, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bmp);
			if (old) DeleteObject(old);
			g_miniThumbBmp = bmp;
		}
		else if (bmp)
		{
			DeleteObject(bmp);
		}
		break;
	}

	case WM_APP_STATUS:
	{
		wchar_t* text = (wchar_t*)lp;
		if (text)
		{
			SetStatus(text);
			free(text);
		}
		// 再生失敗などでセッションが消えた場合にミニプレイヤーの表示を追従させる
		if (!g_playerMode)
			Layout(hWnd);
		break;
	}

	case WM_DESTROY:
		SaveSettings();
		Cleanup();
		MFShutdown();
		PostQuitMessage(0);
		break;
	}
	return DefWindowProc(hWnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd)
{
	InitializeCriticalSection(&g_cs);
	MFStartup(MF_VERSION);

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
	InitCommonControlsEx(&icc);

	ULONG_PTR gdipToken = 0;
	Gdiplus::GdiplusStartupInput gdipInput;
	Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

	HDC screen = GetDC(nullptr);
	g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
	ReleaseDC(nullptr, screen);

	g_hbrBlack = CreateSolidBrush(RGB(16, 16, 20));

	g_hFontUI = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
	g_hFontTitle = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
	g_hFontSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");

	LoadSettings();
	ReloadLanguage();

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"YTLegacyRT";
	RegisterClassW(&wc);

	WNDCLASSW wcs = {};
	wcs.lpfnWndProc = SettingsWndProc;
	wcs.hInstance = hInst;
	wcs.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcs.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcs.lpszClassName = L"YTLegacyRTSettings";
	RegisterClassW(&wcs);

	HWND hWnd = CreateWindowW(
		wc.lpszClassName,
		L"YT-Legacy-RT",
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, S(700), S(600),
		nullptr, nullptr, hInst, nullptr);

	ShowWindow(hWnd, nCmd);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	Gdiplus::GdiplusShutdown(gdipToken);
	return 0;
}
