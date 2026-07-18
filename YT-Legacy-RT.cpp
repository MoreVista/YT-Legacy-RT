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
#include <string>
#include <vector>

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
	IDC_LIST,
	IDC_STATUS,
	IDC_VIDEO,
	IDC_BACK_BTN,
	IDC_PLAYER_TITLE,
	IDC_PLAYPAUSE_BTN,
	IDC_SEEKBAR,
	IDC_TIME_LABEL,
	IDC_FULLSCREEN_BTN,

	// 設定ウィンドウ
	IDC_SET_INSTANCE = 200,
	IDC_SET_QUALITY,
	IDC_SET_LOCAL,
	IDC_SET_IGNORECERT,
	IDC_SET_OK,
	IDC_SET_CANCEL,
};

#define WM_APP_SEARCHDONE  (WM_APP + 1)   // wParam: 検索世代
#define WM_APP_STATUS      (WM_APP + 2)   // lParam: heap確保した wchar_t* (受信側でfree)
#define WM_APP_THUMB       (WM_APP + 3)   // wParam: アイテムindex lParam: 検索世代

HWND g_hWnd;
HWND g_hSearchEdit;
HWND g_hSearchBtn;
HWND g_hSettingsBtn;
HWND g_hList;
HWND g_hStatus;
HWND g_hVideo;
HWND g_hBackBtn;
HWND g_hPlayerTitle;
HWND g_hPlayPauseBtn;
HWND g_hSeekBar;
HWND g_hTimeLabel;
HWND g_hFullscreenBtn;

HBRUSH g_hbrBlack;
bool g_fullscreen = false;
WINDOWPLACEMENT g_wpPrev = { sizeof(WINDOWPLACEMENT) };

HFONT g_hFontUI;      // 通常UI
HFONT g_hFontTitle;   // カードのタイトル
HFONT g_hFontSmall;   // カードの投稿者/時間

int  g_dpi = 96;
bool g_playerMode = false;   // false=一覧 true=プレイヤー

// 設定 (iniに保存)
std::wstring g_instanceUrl = L"http://";
int  g_qualityIdx = 0;       // 0=360p(itag18) 1=720p(itag22)
bool g_localProxy = true;
bool g_ignoreCert = false;

IMFMediaSession* g_pSession = nullptr;
IMFMediaSource*  g_pSource = nullptr;

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
std::vector<VideoItem> g_results;      // g_cs で保護
std::wstring g_searchError;            // g_cs で保護
LONG g_searchGen = 0;                  // 検索の世代 (古いスレッドの結果を破棄する)

WNDPROC g_oldEditProc = nullptr;
WNDPROC g_oldTrackProc = nullptr;

DWORD WINAPI PlayThread(LPVOID param);
DWORD WINAPI SearchThread(LPVOID param);
DWORD WINAPI ThumbThread(LPVOID param);
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
		err = L"URLの形式が不正です";
		return false;
	}
	bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

	HINTERNET hOpen = WinHttpOpen(
		L"Mozilla/5.0 (Windows NT 6.3; ARM) YT-Legacy-RT/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hOpen)
	{
		wchar_t buf[64];
		wsprintfW(buf, L"WinHttpOpen失敗 (code %u)", GetLastError());
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
		wchar_t buf[96];
		wsprintfW(buf, L"接続失敗 (WinHttpConnect, code %u, host=%s:%u)", GetLastError(), host, uc.nPort);
		err = buf;
	}
	else
	{
		hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			https ? WINHTTP_FLAG_SECURE : 0);
		if (!hRequest)
		{
			wchar_t buf[96];
			wsprintfW(buf, L"接続失敗 (WinHttpOpenRequest, code %u)", GetLastError());
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
				wchar_t buf[64];
				wsprintfW(buf, L"HTTPエラー %u", status);
				err = buf;
			}
		}
		else
		{
			wchar_t buf[64];
			wsprintfW(buf, L"通信失敗 (エラーコード %u)", GetLastError());
			err = buf;
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
};

// 検索結果のサムネイルを順次ダウンロードする
DWORD WINAPI ThumbThread(LPVOID param)
{
	ThumbParams* tp = (ThumbParams*)param;

	for (int i = 0;; i++)
	{
		std::wstring videoId;
		EnterCriticalSection(&g_cs);
		bool stale = (tp->gen != g_searchGen);
		if (!stale && i < (int)g_results.size())
			videoId = g_results[i].videoId;
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
		if (tp->gen == g_searchGen && i < (int)g_results.size() && !g_results[i].thumb)
		{
			g_results[i].thumb = bmp;
			stored = true;
		}
		LeaveCriticalSection(&g_cs);

		if (stored)
			PostMessageW(g_hWnd, WM_APP_THUMB, (WPARAM)i, (LPARAM)tp->gen);
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

DWORD WINAPI SearchThread(LPVOID param)
{
	SearchParams* sp = (SearchParams*)param;

	std::wstring url = sp->instanceUrl + L"/api/v1/search?type=video&q=" + UrlEncode(sp->query);
	PostStatus(L"検索中: " + sp->query);

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
			g_searchError = L"結果が0件でした";
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
// Media Foundation 再生コア
// ---------------------------------------------------------------------------
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
		g_pSource->Shutdown();
		g_pSource->Release();
		g_pSource = nullptr;
	}
	InterlockedExchange64(&g_duration100ns, 0);
}

HRESULT CreateSession()
{
	Cleanup();
	return MFCreateMediaSession(nullptr, &g_pSession);
}

HRESULT CreateMediaSource(const wchar_t* url)
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
		(IUnknown**)&g_pSource
	);

	resolver->Release();
	return hr;
}

HRESULT CreateTopology()
{
	IMFTopology* topology = nullptr;
	IMFPresentationDescriptor* pd = nullptr;

	HRESULT hr = MFCreateTopology(&topology);
	if (FAILED(hr)) return hr;

	hr = g_pSource->CreatePresentationDescriptor(&pd);
	if (FAILED(hr))
	{
		topology->Release();
		return hr;
	}

	UINT64 duration = 0;
	pd->GetUINT64(MF_PD_DURATION, &duration);
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

		srcNode->SetUnknown(MF_TOPONODE_SOURCE, g_pSource);
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

	hr = g_pSession->SetTopology(0, topology);

	pd->Release();
	topology->Release();
	return hr;
}

void Play(const wchar_t* url)
{
	HRESULT hr;
	if (FAILED(hr = CreateSession()))
	{
		PostStatus(L"再生失敗: セッション作成エラー");
		return;
	}
	if (FAILED(hr = CreateMediaSource(url)))
	{
		wchar_t buf[128];
		wsprintfW(buf, L"再生失敗: ソースを開けません (hr=0x%08X)", hr);
		PostStatus(buf);
		return;
	}
	if (FAILED(hr = CreateTopology()))
	{
		wchar_t buf[128];
		wsprintfW(buf, L"再生失敗: トポロジ構築エラー (hr=0x%08X)", hr);
		PostStatus(buf);
		return;
	}

	PROPVARIANT var;
	PropVariantInit(&var);
	g_pSession->Start(&GUID_NULL, &var);
	PostStatus(L"再生中");
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
		SetWindowTextW(g_hPlayPauseBtn, L"一時停止");
		SetStatus(L"再生中");
	}
	else
	{
		g_pSession->Pause();
		g_isPaused = true;
		SetWindowTextW(g_hPlayPauseBtn, L"再生");
		SetStatus(L"一時停止中");
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

DWORD WINAPI PlayThread(LPVOID param)
{
	std::wstring* input = (std::wstring*)param;

	wchar_t finalUrl[2048] = {};

	if (IsLocalPath(input->c_str()))
	{
		ToFileUrl(input->c_str(), finalUrl, 2048);
	}
	else
	{
		lstrcpynW(finalUrl, input->c_str(), 2048);
	}

	delete input;
	Play(finalUrl);
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

	g_qualityIdx = GetPrivateProfileIntW(L"Settings", L"Quality", 0, ini.c_str()) ? 1 : 0;
	g_localProxy = GetPrivateProfileIntW(L"Settings", L"LocalProxy", 1, ini.c_str()) != 0;
	g_ignoreCert = GetPrivateProfileIntW(L"Settings", L"IgnoreCert", 0, ini.c_str()) != 0;
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
	SetWindowTextW(g_hPlayPauseBtn, L"一時停止");
	SetWindowTextW(g_hTimeLabel, L"--:-- / --:--");
	SendMessageW(g_hSeekBar, TBM_SETPOS, TRUE, 0);
	Layout(g_hWnd);
	RefreshAll();
}

static void LeavePlayerView()
{
	if (g_fullscreen)
		ToggleFullscreen();
	g_playerMode = false;
	Cleanup();
	Layout(g_hWnd);
	RefreshAll();
}

static void PlayVideoId(const std::wstring& videoId, const std::wstring& title)
{
	std::wstring instanceUrl = TrimTrailingSlash(g_instanceUrl);
	if (instanceUrl.empty() || instanceUrl == L"http://" || instanceUrl == L"https://")
	{
		SetStatus(L"設定でインスタンスURLを入力してください");
		return;
	}

	// 画質: itag 18 = 360p / itag 22 = 720p (どちらもH.264+AACのmuxed MP4)
	const wchar_t* itag = (g_qualityIdx == 1) ? L"22" : L"18";

	std::wstring url = instanceUrl + L"/latest_version?id=" + videoId + L"&itag=" + itag;
	if (g_localProxy)
		url += L"&local=true";

	EnterPlayerView(title);
	SetStatus(L"読み込み中...");
	CreateThread(nullptr, 0, PlayThread, new std::wstring(url), 0, nullptr);
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

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static HWND hInst, hQuality, hLocal, hCert;

	switch (msg)
	{
	case WM_CREATE:
	{
		int M = S(12);
		int y = M;

		CreateLabel(hWnd, L"InvidiousインスタンスURL:", M, y, S(300));
		y += S(22);
		hInst = CreateWindowW(L"EDIT", g_instanceUrl.c_str(),
			WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
			M, y, S(360), S(26), hWnd, (HMENU)IDC_SET_INSTANCE, nullptr, nullptr);
		y += S(36);

		CreateLabel(hWnd, L"画質:", M, y + S(3), S(60));
		hQuality = CreateWindowW(L"COMBOBOX", nullptr,
			WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
			M + S(66), y, S(100), S(200), hWnd, (HMENU)IDC_SET_QUALITY, nullptr, nullptr);
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"360p");
		SendMessageW(hQuality, CB_ADDSTRING, 0, (LPARAM)L"720p");
		SendMessageW(hQuality, CB_SETCURSEL, g_qualityIdx, 0);
		y += S(36);

		hLocal = CreateWindowW(L"BUTTON", L"インスタンス経由で再生 (local=true)",
			WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			M, y, S(360), S(22), hWnd, (HMENU)IDC_SET_LOCAL, nullptr, nullptr);
		SendMessageW(hLocal, BM_SETCHECK, g_localProxy ? BST_CHECKED : BST_UNCHECKED, 0);
		y += S(28);

		hCert = CreateWindowW(L"BUTTON", L"証明書エラーを無視",
			WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			M, y, S(360), S(22), hWnd, (HMENU)IDC_SET_IGNORECERT, nullptr, nullptr);
		SendMessageW(hCert, BM_SETCHECK, g_ignoreCert ? BST_CHECKED : BST_UNCHECKED, 0);
		y += S(38);

		CreateWindowW(L"BUTTON", L"OK",
			WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
			M + S(170), y, S(90), S(28), hWnd, (HMENU)IDC_SET_OK, nullptr, nullptr);
		CreateWindowW(L"BUTTON", L"キャンセル",
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
			g_qualityIdx = (int)SendMessageW(hQuality, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
			g_localProxy = SendMessageW(hLocal, BM_GETCHECK, 0, 0) == BST_CHECKED;
			g_ignoreCert = SendMessageW(hCert, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
	int w = S(410), h = S(260);
	int x = rc.left + ((rc.right - rc.left) - w) / 2;
	int y = rc.top + ((rc.bottom - rc.top) - h) / 2;

	EnableWindow(g_hWnd, FALSE);
	CreateWindowW(L"YTLegacyRTSettings", L"設定",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		x, y, w, h, g_hWnd, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// フルスクリーン切り替え
// ---------------------------------------------------------------------------
static void ToggleFullscreen()
{
	DWORD style = (DWORD)GetWindowLongPtrW(g_hWnd, GWL_STYLE);
	if (!g_fullscreen)
	{
		MONITORINFO mi = { sizeof(mi) };
		if (GetWindowPlacement(g_hWnd, &g_wpPrev) &&
			GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTOPRIMARY), &mi))
		{
			SetWindowLongPtrW(g_hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(g_hWnd, HWND_TOP,
				mi.rcMonitor.left, mi.rcMonitor.top,
				mi.rcMonitor.right - mi.rcMonitor.left,
				mi.rcMonitor.bottom - mi.rcMonitor.top,
				SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			g_fullscreen = true;
			SetWindowTextW(g_hFullscreenBtn, L"全画面解除");
		}
	}
	else
	{
		SetWindowLongPtrW(g_hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPlacement(g_hWnd, &g_wpPrev);
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		g_fullscreen = false;
		SetWindowTextW(g_hFullscreenBtn, L"全画面");
	}
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

	std::wstring title, author;
	long long len = -1;
	HBITMAP thumb = nullptr;
	EnterCriticalSection(&g_cs);
	if (idx < (int)g_results.size())
	{
		title = g_results[idx].title;
		author = g_results[idx].author;
		len = g_results[idx].lengthSeconds;
		thumb = g_results[idx].thumb;
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
			wsprintfW(badge, L"%lld:%02lld:%02lld", len / 3600, (len / 60) % 60, len % 60);
		else
			wsprintfW(badge, L"%lld:%02lld", len / 60, len % 60);

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
	ShowWindow(g_hList, listVis);
	ShowWindow(g_hBackBtn, playerVis);
	ShowWindow(g_hPlayerTitle, playerVis);
	ShowWindow(g_hVideo, playerVis);
	ShowWindow(g_hPlayPauseBtn, playerVis);
	ShowWindow(g_hSeekBar, playerVis);
	ShowWindow(g_hTimeLabel, playerVis);
	ShowWindow(g_hFullscreenBtn, playerVis);

	if (!g_playerMode)
	{
		// 上段: 検索ボックス + 検索 + 設定
		int topH = S(32);
		int btnW = S(44);
		MoveWindow(g_hSearchEdit, M, M, w - M * 2 - btnW * 2 - S(12), topH, TRUE);
		MoveWindow(g_hSearchBtn, w - M - btnW * 2 - S(6), M, btnW, topH, TRUE);
		MoveWindow(g_hSettingsBtn, w - M - btnW, M, btnW, topH, TRUE);

		// リスト
		int y = M + topH + S(6);
		MoveWindow(g_hList, M, y, w - M * 2, h - y - statusH - M, TRUE);
	}
	else
	{
		// 上段: 戻る + タイトル
		int topH = S(32);
		MoveWindow(g_hBackBtn, M, M, S(70), topH, TRUE);
		MoveWindow(g_hPlayerTitle, M + S(78), M + S(6), w - M * 2 - S(78), topH - S(6), TRUE);

		// 下段: 再生/一時停止 + シークバー + 時間 + 全画面
		int ctrlH = S(32);
		int ctrlY = h - statusH - ctrlH - S(4);
		int btnW = S(80);
		int timeW = S(110);
		int fsW = S(80);
		MoveWindow(g_hPlayPauseBtn, M, ctrlY, btnW, ctrlH, TRUE);
		MoveWindow(g_hSeekBar, M + btnW + S(8), ctrlY, w - M * 2 - btnW - timeW - fsW - S(24), ctrlH, TRUE);
		MoveWindow(g_hTimeLabel, w - M - fsW - S(8) - timeW, ctrlY + S(8), timeW, ctrlH - S(8), TRUE);
		MoveWindow(g_hFullscreenBtn, w - M - fsW, ctrlY, fsW, ctrlH, TRUE);

		// 映像
		int y = M + topH + S(6);
		MoveWindow(g_hVideo, 0, y, w, ctrlY - y - S(4), TRUE);
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
		wchar_t buf[64];
		wsprintfW(buf, L"%d件の動画が見つかりました", count);
		status = buf;
	}
	SetStatus(status);
}

static void StartSearch()
{
	std::wstring query = GetWindowTextStr(g_hSearchEdit);
	std::wstring instanceUrl = TrimTrailingSlash(g_instanceUrl);
	if (query.empty())
	{
		SetStatus(L"検索ワードを入力してください");
		return;
	}
	if (instanceUrl.empty() || instanceUrl == L"http://" || instanceUrl == L"https://")
	{
		SetStatus(L"⚙ボタンからインスタンスURLを設定してください");
		return;
	}

	// URLやローカルパスを直接入力した場合はそのまま再生
	if (query.find(L"http://") == 0 || query.find(L"https://") == 0 || IsLocalPath(query.c_str()))
	{
		EnterPlayerView(query);
		SetStatus(L"読み込み中...");
		CreateThread(nullptr, 0, PlayThread, new std::wstring(query), 0, nullptr);
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

		g_hSearchBtn = CreateWindowW(L"BUTTON", L"検索",
			WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SEARCH_BTN, nullptr, nullptr);

		g_hSettingsBtn = CreateWindowW(L"BUTTON", L"設定",
			WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SETTINGS_BTN, nullptr, nullptr);

		g_hList = CreateWindowW(L"LISTBOX", nullptr,
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
			LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
			0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, nullptr, nullptr);
		SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, ItemHeight());

		g_hStatus = CreateWindowW(L"STATIC", L"準備完了",
			WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
			0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS, nullptr, nullptr);

		g_hBackBtn = CreateWindowW(L"BUTTON", L"← 戻る",
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_BACK_BTN, nullptr, nullptr);

		g_hPlayerTitle = CreateWindowW(L"STATIC", L"",
			WS_CHILD | SS_ENDELLIPSIS,
			0, 0, 0, 0, hWnd, (HMENU)IDC_PLAYER_TITLE, nullptr, nullptr);

		g_hPlayPauseBtn = CreateWindowW(L"BUTTON", L"一時停止",
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_PLAYPAUSE_BTN, nullptr, nullptr);

		g_hSeekBar = CreateWindowW(TRACKBAR_CLASSW, nullptr,
			WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
			0, 0, 0, 0, hWnd, (HMENU)IDC_SEEKBAR, nullptr, nullptr);
		g_oldTrackProc = (WNDPROC)SetWindowLongPtrW(g_hSeekBar, GWLP_WNDPROC, (LONG_PTR)SeekBarProc);

		g_hTimeLabel = CreateWindowW(L"STATIC", L"--:-- / --:--",
			WS_CHILD | SS_RIGHT,
			0, 0, 0, 0, hWnd, (HMENU)IDC_TIME_LABEL, nullptr, nullptr);

		g_hFullscreenBtn = CreateWindowW(L"BUTTON", L"全画面",
			WS_CHILD,
			0, 0, 0, 0, hWnd, (HMENU)IDC_FULLSCREEN_BTN, nullptr, nullptr);

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
		if (mis->CtlID == IDC_LIST)
		{
			mis->itemHeight = ItemHeight();
			return TRUE;
		}
		break;
	}

	case WM_DRAWITEM:
	{
		DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
		if (dis->CtlID == IDC_LIST)
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

		case IDC_BACK_BTN:
			LeavePlayerView();
			SetStatus(L"準備完了");
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
				if (!videoId.empty())
					PlayVideoId(videoId, title);
			}
			break;
		}
		break;

	case WM_APP_SEARCHDONE:
		if ((LONG)wp == g_searchGen)
		{
			FillResultList();

			// サムネイル取得開始
			ThumbParams* tp = new ThumbParams;
			tp->gen = (LONG)wp;
			tp->instanceUrl = TrimTrailingSlash(g_instanceUrl);
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

	case WM_APP_STATUS:
	{
		wchar_t* text = (wchar_t*)lp;
		if (text)
		{
			SetStatus(text);
			free(text);
		}
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
