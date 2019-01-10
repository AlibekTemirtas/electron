// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/api/atom_api_protocol.h"

#include <unordered_set>

#include "atom/app/atom_main_delegate.h"
#include "atom/browser/atom_browser_client.h"
#include "atom/browser/atom_browser_main_parts.h"
#include "atom/browser/browser.h"
#include "atom/browser/net/url_request_async_asar_job.h"
#include "atom/browser/net/url_request_buffer_job.h"
#include "atom/browser/net/url_request_fetch_job.h"
#include "atom/browser/net/url_request_stream_job.h"
#include "atom/browser/net/url_request_string_job.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "atom/common/node_includes.h"
#include "atom/common/options_switches.h"
#include "base/command_line.h"
#include "base/strings/string_util.h"
#include "base/threading/platform_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/common/content_client.h"
#include "native_mate/dictionary.h"
#include "url/url_util.h"

using content::BrowserThread;

namespace atom {

namespace api {

namespace {

// List of registered custom standard schemes.
std::vector<std::string> g_standard_schemes;

}  // namespace

std::vector<std::string> GetStandardSchemes() {
  return g_standard_schemes;
}

void RegisterSchemesAsPrivileged(const std::vector<std::string>& schemes,
                                 mate::Arguments* args) {
  bool standard = true;
  bool secure = true;
  bool bypassCSP = true;
  bool allowServiceWorkers = true;
  bool supportFetchAPI = true;
  bool corsEnabled = true;
  if (args->Length() == 2) {
    mate::Dictionary options;
    if (args->GetNext(&options)) {
      options.Get("standard", &standard);
      options.Get("secure", &secure);
      options.Get("bypassCSP", &bypassCSP);
      options.Get("allowServiceWorkers", &allowServiceWorkers);
      options.Get("supportFetchAPI", &supportFetchAPI);
      options.Get("corsEnabled", &corsEnabled);
    }
  }

  std::unordered_set<std::string> switches;
  auto* policy = content::ChildProcessSecurityPolicy::GetInstance();
  for (const auto& scheme : schemes) {
    // Register scheme to privileged list (https, wss, data, chrome-extension)
    if (standard) {
      g_standard_schemes = schemes;
      url::AddStandardScheme(scheme.c_str(), url::SCHEME_WITH_HOST);
      switches.insert(atom::switches::kStandardSchemes);
      policy->RegisterWebSafeScheme(scheme);
    }
    if (secure) {
      url::AddSecureScheme(scheme.c_str());
      switches.insert(atom::switches::kSecureSchemes);
    }
    if (bypassCSP) {
      url::AddCSPBypassingScheme(scheme.c_str());
      switches.insert(atom::switches::kBypassCSPSchemes);
    }
    if (corsEnabled) {
      url::AddCORSEnabledScheme(scheme.c_str());
      switches.insert(atom::switches::kCORSSchemes);
    }
    if (supportFetchAPI) {
      // NYI
      switches.insert(atom::switches::kFetchSchemes);
    }
  }

  if (allowServiceWorkers) {
    atom::AtomBrowserClient::SetCustomServiceWorkerSchemes({schemes});
    switches.insert(atom::switches::kServiceWorkerSchemes);
  }

  // Add the schemes to command line switches, so child processes can also
  // register them.
  for (const auto& _switch : switches)
    base::CommandLine::ForCurrentProcess()->AppendSwitchASCII(
        _switch, base::JoinString(schemes, ","));
}

Protocol::Protocol(v8::Isolate* isolate, AtomBrowserContext* browser_context)
    : browser_context_(browser_context), weak_factory_(this) {
  Init(isolate);
}

Protocol::~Protocol() {}
void Protocol::UnregisterProtocol(const std::string& scheme,
                                  mate::Arguments* args) {
  CompletionCallback callback;
  args->GetNext(&callback);
  auto* getter = static_cast<URLRequestContextGetter*>(
      browser_context_->GetRequestContext());
  content::BrowserThread::PostTaskAndReplyWithResult(
      content::BrowserThread::IO, FROM_HERE,
      base::BindOnce(&Protocol::UnregisterProtocolInIO,
                     base::RetainedRef(getter), scheme),
      base::BindOnce(&Protocol::OnIOCompleted, GetWeakPtr(), callback));
}

// static
Protocol::ProtocolError Protocol::UnregisterProtocolInIO(
    scoped_refptr<URLRequestContextGetter> request_context_getter,
    const std::string& scheme) {
  auto* job_factory = request_context_getter->job_factory();
  if (!job_factory->HasProtocolHandler(scheme))
    return PROTOCOL_NOT_REGISTERED;
  job_factory->SetProtocolHandler(scheme, nullptr);
  return PROTOCOL_OK;
}

void Protocol::IsProtocolHandled(const std::string& scheme,
                                 const BooleanCallback& callback) {
  auto* getter = static_cast<URLRequestContextGetter*>(
      browser_context_->GetRequestContext());
  content::BrowserThread::PostTaskAndReplyWithResult(
      content::BrowserThread::IO, FROM_HERE,
      base::Bind(&Protocol::IsProtocolHandledInIO, base::RetainedRef(getter),
                 scheme),
      callback);
}

// static
bool Protocol::IsProtocolHandledInIO(
    scoped_refptr<URLRequestContextGetter> request_context_getter,
    const std::string& scheme) {
  return request_context_getter->job_factory()->IsHandledProtocol(scheme);
}

void Protocol::UninterceptProtocol(const std::string& scheme,
                                   mate::Arguments* args) {
  CompletionCallback callback;
  args->GetNext(&callback);
  auto* getter = static_cast<URLRequestContextGetter*>(
      browser_context_->GetRequestContext());
  content::BrowserThread::PostTaskAndReplyWithResult(
      content::BrowserThread::IO, FROM_HERE,
      base::BindOnce(&Protocol::UninterceptProtocolInIO,
                     base::RetainedRef(getter), scheme),
      base::BindOnce(&Protocol::OnIOCompleted, GetWeakPtr(), callback));
}

// static
Protocol::ProtocolError Protocol::UninterceptProtocolInIO(
    scoped_refptr<URLRequestContextGetter> request_context_getter,
    const std::string& scheme) {
  return request_context_getter->job_factory()->UninterceptProtocol(scheme)
             ? PROTOCOL_OK
             : PROTOCOL_NOT_INTERCEPTED;
}

void Protocol::OnIOCompleted(const CompletionCallback& callback,
                             ProtocolError error) {
  // The completion callback is optional.
  if (callback.is_null())
    return;

  v8::Locker locker(isolate());
  v8::HandleScope handle_scope(isolate());

  if (error == PROTOCOL_OK) {
    callback.Run(v8::Null(isolate()));
  } else {
    std::string str = ErrorCodeToString(error);
    callback.Run(v8::Exception::Error(mate::StringToV8(isolate(), str)));
  }
}

std::string Protocol::ErrorCodeToString(ProtocolError error) {
  switch (error) {
    case PROTOCOL_FAIL:
      return "Failed to manipulate protocol factory";
    case PROTOCOL_REGISTERED:
      return "The scheme has been registered";
    case PROTOCOL_NOT_REGISTERED:
      return "The scheme has not been registered";
    case PROTOCOL_INTERCEPTED:
      return "The scheme has been intercepted";
    case PROTOCOL_NOT_INTERCEPTED:
      return "The scheme has not been intercepted";
    default:
      return "Unexpected error";
  }
}

// static
mate::Handle<Protocol> Protocol::Create(v8::Isolate* isolate,
                                        AtomBrowserContext* browser_context) {
  return mate::CreateHandle(isolate, new Protocol(isolate, browser_context));
}

// static
void Protocol::BuildPrototype(v8::Isolate* isolate,
                              v8::Local<v8::FunctionTemplate> prototype) {
  prototype->SetClassName(mate::StringToV8(isolate, "Protocol"));
  mate::ObjectTemplateBuilder(isolate, prototype->PrototypeTemplate())
      .SetMethod("registerStringProtocol",
                 &Protocol::RegisterProtocol<URLRequestStringJob>)
      .SetMethod("registerBufferProtocol",
                 &Protocol::RegisterProtocol<URLRequestBufferJob>)
      .SetMethod("registerFileProtocol",
                 &Protocol::RegisterProtocol<URLRequestAsyncAsarJob>)
      .SetMethod("registerHttpProtocol",
                 &Protocol::RegisterProtocol<URLRequestFetchJob>)
      .SetMethod("registerStreamProtocol",
                 &Protocol::RegisterProtocol<URLRequestStreamJob>)
      .SetMethod("unregisterProtocol", &Protocol::UnregisterProtocol)
      .SetMethod("isProtocolHandled", &Protocol::IsProtocolHandled)
      .SetMethod("interceptStringProtocol",
                 &Protocol::InterceptProtocol<URLRequestStringJob>)
      .SetMethod("interceptBufferProtocol",
                 &Protocol::InterceptProtocol<URLRequestBufferJob>)
      .SetMethod("interceptFileProtocol",
                 &Protocol::InterceptProtocol<URLRequestAsyncAsarJob>)
      .SetMethod("interceptHttpProtocol",
                 &Protocol::InterceptProtocol<URLRequestFetchJob>)
      .SetMethod("interceptStreamProtocol",
                 &Protocol::InterceptProtocol<URLRequestStreamJob>)
      .SetMethod("uninterceptProtocol", &Protocol::UninterceptProtocol);
}

}  // namespace api

}  // namespace atom

namespace {

void RegisterSchemesAsPrivileged(const std::vector<std::string>& schemes,
                                 mate::Arguments* args) {
  if (atom::Browser::Get()->is_ready()) {
    args->ThrowError(
        "protocol.registerSchemesAsPrivileged should be called before "
        "app is ready");
    return;
  }

  atom::api::RegisterSchemesAsPrivileged(schemes, args);
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* isolate = context->GetIsolate();
  mate::Dictionary dict(isolate, exports);
  dict.SetMethod("registerSchemesAsPrivileged", &RegisterSchemesAsPrivileged);
  dict.SetMethod("getStandardSchemes", &atom::api::GetStandardSchemes);
}

}  // namespace

NODE_BUILTIN_MODULE_CONTEXT_AWARE(atom_browser_protocol, Initialize)
