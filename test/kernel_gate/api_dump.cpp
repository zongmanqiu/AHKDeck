// m004 kernel API surface dumper.
//
// Loads an AutoHotkey lib DLL, calls its exported Host(), optionally LoadFile()s a
// minimal script (some builds only populate the global script after LoadFile), then
// enumerates Lib.Funcs / Lib.Vars / Lib.Labels / Lib.Files through IDispatch and
// writes one TSV line per entry. No third-party code, no ATL.
//
// Usage: api_dump.exe <dll-path> <out-tsv> [script-path]
// ASCII only on purpose (MSVC /utf-8 is passed, but a GBK misparse once ate an #include).

#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <string>

typedef HRESULT(__stdcall *PFN_Host)(IDispatch **ppLib);

static FILE *g_out = nullptr;
static int g_fail = 0;

static void Emit(const char *tag, const char *a = "", const char *b = "",
                 const char *c = "", const char *d = "", const char *e = "") {
  fprintf(g_out, "%s\t%s\t%s\t%s\t%s\t%s\n", tag, a, b, c, d, e);
}

static void EmitMeta(const char *key, const char *value) {
  fprintf(g_out, "META\t%s\t%s\n", key, value);
}

// Converts a BSTR to a UTF-8 buffer. The result is owned by the caller.
static char *BstrToUtf8(BSTR s) {
  if (!s) return nullptr;
  int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return nullptr;
  char *buf = (char *)malloc((size_t)need);
  if (!buf) return nullptr;
  WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, nullptr, nullptr);
  return buf;
}

static VARIANT GetProp(IDispatch *obj, const wchar_t *name, HRESULT *hrOut = nullptr) {
  VARIANT v;
  VariantInit(&v);
  if (!obj || !name) {
    if (hrOut) *hrOut = E_POINTER;
    return v;
  }
  DISPID id = DISPID_UNKNOWN;
  wchar_t *n = const_cast<wchar_t *>(name);
  HRESULT hr = obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
  if (SUCCEEDED(hr)) {
    DISPPARAMS dp = {nullptr, nullptr, 0, 0};
    VARIANT res;
    VariantInit(&res);
    hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp,
                     &res, nullptr, nullptr);
    if (SUCCEEDED(hr)) v = res;
  }
  if (hrOut) *hrOut = hr;
  return v;
}

static BSTR GetStr(IDispatch *obj, const wchar_t *name) {
  VARIANT v = GetProp(obj, name);
  BSTR s = nullptr;
  if (v.vt == VT_BSTR) s = v.bstrVal;
  VariantClear(&v);
  return s;
}

static long long GetInt(IDispatch *obj, const wchar_t *name, HRESULT *hrOut = nullptr) {
  VARIANT v = GetProp(obj, name, hrOut);
  long long r = 0;
  switch (v.vt) {
    case VT_I4: r = v.lVal; break;
    case VT_I8: r = v.llVal; break;
    case VT_UI4: r = (long long)v.ulVal; break;
    case VT_UI8: r = (long long)v.ullVal; break;
    case VT_BOOL: r = (v.boolVal != VARIANT_FALSE) ? 1 : 0; break;
    default: break;
  }
  VariantClear(&v);
  return r;
}

static IEnumVARIANT *GetEnum(IDispatch *collection) {
  if (!collection) return nullptr;
  DISPPARAMS dp = {nullptr, nullptr, 0, 0};
  VARIANT v;
  VariantInit(&v);
  HRESULT hr = collection->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT,
                                  DISPATCH_PROPERTYGET | DISPATCH_METHOD, &dp, &v,
                                  nullptr, nullptr);
  if (FAILED(hr)) return nullptr;
  IEnumVARIANT *en = nullptr;
  if (v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) {
    IUnknown *u = (v.vt == VT_UNKNOWN) ? v.punkVal : v.pdispVal;
    if (u) u->QueryInterface(IID_IEnumVARIANT, (void **)&en);
  }
  VariantClear(&v);
  return en;
}

static void DumpFuncs(IDispatch *lib) {
  VARIANT v = GetProp(lib, L"Funcs");
  IDispatch *coll = (v.vt == VT_DISPATCH) ? v.pdispVal : nullptr;
  EmitMeta("funcs_collection_vt", coll ? "dispatch" : "missing");
  if (!coll) {
    VariantClear(&v);
    EmitMeta("funcs_error", "no collection");
    ++g_fail;
    return;
  }
  IEnumVARIANT *en = GetEnum(coll);
  int count = (int)GetInt(coll, L"Count");
  EmitMeta("funcs_count", std::to_string(count).c_str());
  if (en) {
    VARIANT item;
    ULONG fetched = 0;
    while (en->Next(1, &item, &fetched) == S_OK && fetched == 1) {
      IDispatch *f = (item.vt == VT_DISPATCH) ? item.pdispVal : nullptr;
      if (f) {
        BSTR name = GetStr(f, L"Name");
        char *n8 = BstrToUtf8(name);
        if (n8) {
          Emit("FUNC", n8, std::to_string(GetInt(f, L"IsBuiltIn")).c_str(),
               std::to_string(GetInt(f, L"MinParams")).c_str(),
               std::to_string(GetInt(f, L"MaxParams")).c_str(),
               std::to_string(GetInt(f, L"IsVariadic")).c_str());
          free(n8);
        }
        SysFreeString(name);
        f->Release();
      }
      VariantClear(&item);
    }
    en->Release();
  }
  coll->Release();
  VariantClear(&v);
}

static void DumpVars(IDispatch *lib) {
  VARIANT v = GetProp(lib, L"Vars");
  IDispatch *coll = (v.vt == VT_DISPATCH) ? v.pdispVal : nullptr;
  EmitMeta("vars_collection_vt", coll ? "dispatch" : "missing");
  if (!coll) {
    VariantClear(&v);
    EmitMeta("vars_error", "no collection");
    ++g_fail;
    return;
  }
  IEnumVARIANT *en = GetEnum(coll);
  int count = (int)GetInt(coll, L"Count");
  EmitMeta("vars_count", std::to_string(count).c_str());
  if (en) {
    VARIANT item;
    ULONG fetched = 0;
    while (en->Next(1, &item, &fetched) == S_OK && fetched == 1) {
      IDispatch *d = (item.vt == VT_DISPATCH) ? item.pdispVal : nullptr;
      if (d) {
        BSTR name = GetStr(d, L"Name");
        char *n8 = BstrToUtf8(name);
        if (n8) {
          Emit("VAR", n8, std::to_string(GetInt(d, L"IsReadOnly")).c_str(),
               std::to_string(GetInt(d, L"IsVirtual")).c_str(),
               std::to_string(GetInt(d, L"IsDeclared")).c_str());
          free(n8);
        }
        SysFreeString(name);
        d->Release();
      }
      VariantClear(&item);
    }
    en->Release();
  }
  coll->Release();
  VariantClear(&v);
}

static void DumpLabels(IDispatch *lib) {
  VARIANT v = GetProp(lib, L"Labels");
  IDispatch *coll = (v.vt == VT_DISPATCH) ? v.pdispVal : nullptr;
  EmitMeta("labels_collection_vt", coll ? "dispatch" : "missing");
  if (!coll) {
    VariantClear(&v);
    return; // Labels are per-script; absence is not a gate failure.
  }
  IEnumVARIANT *en = GetEnum(coll);
  EmitMeta("labels_count", std::to_string((int)GetInt(coll, L"Count")).c_str());
  if (en) {
    VARIANT item;
    ULONG fetched = 0;
    while (en->Next(1, &item, &fetched) == S_OK && fetched == 1) {
      IDispatch *d = (item.vt == VT_DISPATCH) ? item.pdispVal : nullptr;
      if (d) {
        BSTR name = GetStr(d, L"Name");
        char *n8 = BstrToUtf8(name);
        if (n8) {
          Emit("LABEL", n8, std::to_string(GetInt(d, L"Line")).c_str());
          free(n8);
        }
        SysFreeString(name);
        d->Release();
      }
      VariantClear(&item);
    }
    en->Release();
  }
  coll->Release();
  VariantClear(&v);
}

static void DumpFiles(IDispatch *lib) {
  VARIANT v = GetProp(lib, L"Files");
  if ((v.vt & VT_ARRAY) && (v.vt & VT_BSTR) && v.parray) {
    SAFEARRAY *sa = v.parray;
    long lb = 0, ub = -1;
    SafeArrayGetLBound(sa, 1, &lb);
    SafeArrayGetUBound(sa, 1, &ub);
    EmitMeta("files_count", std::to_string((int)(ub - lb + 1)).c_str());
    for (long i = lb; i <= ub; ++i) {
      BSTR s = nullptr;
      if (SUCCEEDED(SafeArrayGetElement(sa, &i, &s)) && s) {
        char *n8 = BstrToUtf8(s);
        if (n8) { Emit("FILE", n8); free(n8); }
        SysFreeString(s);
      }
    }
  } else {
    EmitMeta("files_count", "0");
  }
  VariantClear(&v);
}

static HRESULT CallLoadFile(IDispatch *lib, const wchar_t *script) {
  DISPID id = DISPID_UNKNOWN;
  wchar_t *n = const_cast<wchar_t *>(L"LoadFile");
  HRESULT hr = lib->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED(hr)) return hr;
  VARIANT arg;
  VariantInit(&arg);
  arg.vt = VT_BSTR;
  arg.bstrVal = SysAllocString(script);
  DISPPARAMS dp = {&arg, nullptr, 1, 0};
  VARIANT res;
  VariantInit(&res);
  hr = lib->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &res,
                   nullptr, nullptr);
  VariantClear(&arg);
  VariantClear(&res);
  return hr;
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 3) {
    wprintf(L"usage: api_dump.exe <dll-path> <out-tsv> [script-path]\n");
    return 2;
  }
  const wchar_t *dllPath = argv[1];
  const wchar_t *outPath = argv[2];
  const wchar_t *scriptPath = (argc >= 4) ? argv[3] : nullptr;

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    wprintf(L"CoInitializeEx failed\n");
    return 3;
  }
  HMODULE mod = LoadLibraryExW(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!mod) {
    wprintf(L"LoadLibraryExW failed: %lu\n", GetLastError());
    return 4;
  }
  PFN_Host host = (PFN_Host)GetProcAddress(mod, "Host");
  if (!host) {
    wprintf(L"Host export not found\n");
    return 5;
  }
  IDispatch *lib = nullptr;
  HRESULT hr = host(&lib);
  if (FAILED(hr) || !lib) {
    wprintf(L"Host() failed: 0x%08lX\n", (unsigned long)hr);
    return 6;
  }

  if (_wfopen_s(&g_out, outPath, L"wb") != 0 || !g_out) {
    wprintf(L"cannot open output: %s\n", outPath);
    return 7;
  }
  EmitMeta("dll", "loaded");
  EmitMeta("host", "ok");
  if (scriptPath) {
    hr = CallLoadFile(lib, scriptPath);
    EmitMeta("loadfile_hr", std::to_string((long)hr).c_str());
    EmitMeta("loadfile_ok", SUCCEEDED(hr) ? "1" : "0");
  } else {
    EmitMeta("loadfile_ok", "skipped");
  }

  DumpFuncs(lib);
  DumpVars(lib);
  DumpLabels(lib);
  DumpFiles(lib);

  fclose(g_out);
  g_out = nullptr;
  lib->Release();
  CoUninitialize();
  FreeLibrary(mod);
  wprintf(L"done fail=%d\n", g_fail);
  return 0;
}
