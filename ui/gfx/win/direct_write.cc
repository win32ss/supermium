// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/win/direct_write.h"

#include <wrl/client.h>

#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/debug/alias.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/trace_event/trace_event.h"
#include "base/win/windows_version.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"

namespace gfx {
namespace win {

bool ShouldUseDirectWrite() {
  // Considering that there are seemingly some decent DirectWrite implementations
  // out there for XP, we will no longer discriminate by OS version.
  if (!::LoadLibraryA("dwrite.dll")) {
    return false;
  }
  // If forced off, don't use it.
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  return !command_line.HasSwitch("disable-direct-write");;
}

namespace {

static bool dwrite_enabled = false;

// Pointer to the global IDWriteFactory interface.
IDWriteFactory* g_direct_write_factory = nullptr;

void SetDirectWriteFactory(IDWriteFactory* factory) {
  DCHECK(!g_direct_write_factory);
  // We grab a reference on the DirectWrite factory. This reference is
  // leaked, which is ok because skia leaks it as well.
  factory->AddRef();
  g_direct_write_factory = factory;
}

}  // anonymous namespace

void CreateDWriteFactory(IDWriteFactory** factory) {
  if (!gfx::win::ShouldUseDirectWrite())
    return;

  using DWriteCreateFactoryProc = decltype(DWriteCreateFactory)*;
  HMODULE dwrite_dll = LoadLibraryW(L"dwrite.dll");
  if (!dwrite_dll)
    return;

  DWriteCreateFactoryProc dwrite_create_factory_proc =
      reinterpret_cast<DWriteCreateFactoryProc>(
          GetProcAddress(dwrite_dll, "DWriteCreateFactory"));
  if (!dwrite_create_factory_proc)
    return;

  Microsoft::WRL::ComPtr<IUnknown> factory_unknown;
  
  HRESULT hr =
      dwrite_create_factory_proc(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                          &factory_unknown);
  if (FAILED(hr)) {
    base::debug::Alias(&hr);
    CHECK(false);
    return;
  }
  factory_unknown.CopyTo(factory);
}

void InitializeDirectWrite() {
  static bool tried_dwrite_initialize = false;
  DCHECK(!tried_dwrite_initialize);
  tried_dwrite_initialize = true;

  TRACE_EVENT0("fonts", "gfx::InitializeDirectWrite");

  Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  CreateDWriteFactory(&factory);
  if (factory == nullptr) {
	  sk_sp<SkFontMgr> direct_write_font_mgr = SkFontMgr_New_GDI();
	  skia::OverrideDefaultSkFontMgr(std::move(direct_write_font_mgr));
	  return;
  }
  SetDirectWriteFactory(factory.Get());

// The skia call to create a new DirectWrite font manager instance can fail
  // if we are unable to get the system font collection from the DirectWrite
  // factory. The GetSystemFontCollection method in the IDWriteFactory
  // interface fails with E_INVALIDARG on certain Windows 7 gold versions
  // (6.1.7600.*).
  sk_sp<SkFontMgr> direct_write_font_mgr =
      SkFontMgr_New_DirectWrite(factory.Get());

  int iteration = 0;
  if (!direct_write_font_mgr &&
      base::win::GetVersion() == base::win::Version::WIN7) {
    // Windows (win7_rtm) may fail to map the service sections
    // (crbug.com/956064).
    constexpr int kMaxRetries = 5;
    constexpr base::TimeDelta kRetrySleepTime = base::Microseconds(500);
    while (iteration < kMaxRetries) {
      base::PlatformThread::Sleep(kRetrySleepTime);
      direct_write_font_mgr = SkFontMgr_New_DirectWrite(factory.Get());
      if (direct_write_font_mgr)
        break;
      ++iteration;
    }
  }
  if (!direct_write_font_mgr)
    iteration = -1;
  DCHECK(!!direct_write_font_mgr);
  if (!direct_write_font_mgr)
    direct_write_font_mgr = SkFontMgr_New_GDI();
  else
	dwrite_enabled = true;

  // Override the default skia font manager. This must be called before any
  // use of the skia font manager is done (e.g. before any call to
  // skia::DefaultFontMgr()).
  skia::OverrideDefaultSkFontMgr(std::move(direct_write_font_mgr));
}

bool IsDirectWriteEnabled() {
  return dwrite_enabled;
}

IDWriteFactory* GetDirectWriteFactory() {
  // Some unittests may access this accessor without any previous call to
  // |InitializeDirectWrite|. A call to |InitializeDirectWrite| after this
  // function being called is still invalid.
  if (!g_direct_write_factory)
    InitializeDirectWrite();
  return g_direct_write_factory;
}

std::optional<std::string> RetrieveLocalizedString(
    IDWriteLocalizedStrings* names,
    const std::string& locale) {
  std::wstring locale_wide = base::UTF8ToWide(locale);

  // If locale is empty, index 0 will be used. Otherwise, the locale name must
  // be found and must exist.
  UINT32 index = 0;
  BOOL exists = false;
  if (!locale.empty() &&
      (FAILED(names->FindLocaleName(locale_wide.c_str(), &index, &exists)) ||
       !exists)) {
    return std::nullopt;
  }

  // Get the string length.
  UINT32 length = 0;
  if (FAILED(names->GetStringLength(index, &length)))
    return std::nullopt;

  // The output buffer length needs to be one larger to receive the NUL
  // character.
  std::wstring buffer;
  buffer.resize(length + 1);
  if (FAILED(names->GetString(index, &buffer[0], buffer.size())))
    return std::nullopt;

  // Shrink the string to fit the actual length.
  buffer.resize(length);

  return base::WideToUTF8(buffer);
}

std::optional<std::string> RetrieveLocalizedFontName(
    std::string_view font_name,
    const std::string& locale) {
  Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  CreateDWriteFactory(&factory);

  Microsoft::WRL::ComPtr<IDWriteFontCollection> font_collection;
  if (FAILED(factory->GetSystemFontCollection(&font_collection))) {
    return std::nullopt;
  }

  UINT32 index = 0;
  BOOL exists;
  std::wstring font_name_wide = base::UTF8ToWide(font_name);
  if (FAILED(font_collection->FindFamilyName(font_name_wide.c_str(), &index,
                                             &exists)) ||
      !exists) {
    return std::nullopt;
  }

  Microsoft::WRL::ComPtr<IDWriteFontFamily> font_family;
  Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> family_names;
  if (FAILED(font_collection->GetFontFamily(index, &font_family)) ||
      FAILED(font_family->GetFamilyNames(&family_names))) {
    return std::nullopt;
  }

  return RetrieveLocalizedString(family_names.Get(), locale);
}

}  // namespace win
}  // namespace gfx