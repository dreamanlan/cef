// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/custom_scheme.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_browser.h"
#include "include/cef_callback.h"
#include "include/cef_frame.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/cef_resource_handler.h"
#include "include/cef_response.h"
#include "include/cef_scheme.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "myapp/cefclient/common/custom_scheme_common.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/cefclient/hostclr/native_callbacks.h"
#include "myapp/shared/browser/main_message_loop.h"

namespace client::custom_scheme {

namespace {

// Parses the managed response JSON delivered through complete_native_callback.
// Shape: { "status": 200, "mime": "text/html", "body": "...", "base64": false }
// A payload that is not a JSON object is treated as a raw text/html body.
void ParseManagedResponse(const std::string& json,
                          int& status,
                          std::string& mime,
                          std::string& body,
                          bool& base64) {
  status = 200;
  mime = "text/html";
  body.clear();
  base64 = false;

  CefRefPtr<CefValue> v = CefParseJSON(json, JSON_PARSER_RFC);
  if (v.get() && v->GetType() == VTYPE_DICTIONARY) {
    CefRefPtr<CefDictionaryValue> d = v->GetDictionary();
    if (d->HasKey("status") &&
        (d->GetType("status") == VTYPE_INT ||
         d->GetType("status") == VTYPE_DOUBLE)) {
      status = d->GetInt("status");
    }
    if (d->HasKey("mime") && d->GetType("mime") == VTYPE_STRING) {
      mime = d->GetString("mime").ToString();
    }
    if (d->HasKey("base64") && d->GetType("base64") == VTYPE_BOOL) {
      base64 = d->GetBool("base64");
    }
    if (d->HasKey("body") && d->GetType("body") == VTYPE_STRING) {
      body = d->GetString("body").ToString();
    }
  } else {
    body = json;
  }
}

// Decodes a base64 string using CEF's helper. Returns empty on failure.
std::string DecodeBase64(const std::string& b64) {
  CefRefPtr<CefBinaryValue> bin = CefBase64Decode(b64);
  if (!bin.get()) {
    return std::string();
  }
  std::string out;
  out.resize(bin->GetSize());
  if (!out.empty()) {
    bin->GetData(&out[0], out.size(), 0);
  }
  return out;
}

// Extracts the host component of |url| (empty on parse failure).
std::string GetUrlHost(const std::string& url) {
  CefURLParts parts;
  if (CefParseURL(url, parts)) {
    return CefString(&parts.host).ToString();
  }
  return std::string();
}

// Built-in HTML tab bar served for <scheme>://tabbar/ when managed code does
// not take over the request. It is a self-contained functional skeleton: it
// renders a tab strip, supports new/close/select entirely on the front end,
// keeps draggable regions (-webkit-app-region) so the frameless window can be
// moved, reserves space on the button side for the native window-control
// overlay, and reports every command through window.cefQuery
// ({channel:'tabbar', action:...}). A reverse channel (window.__tabbarApi) lets
// C++ push address / loading state back (see ViewsWindow reverse push). Managed
// code (C#/DSL) may still fully replace this page via on_custom_scheme.
std::string BuildBuiltinTabbarPage() {
  return R"TABBAR(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  html,body{margin:0;padding:0;height:100%;overflow:hidden;
    font-family:'Segoe UI',Arial,sans-serif;font-size:13px;
    background:#dee1e6;color:#3c4043;user-select:none;}
  .root{display:flex;flex-direction:column;box-sizing:border-box;}
  .tabrow{display:flex;align-items:flex-end;height:34px;box-sizing:border-box;
    -webkit-app-region:drag;padding:0 140px 0 8px;}
  .tabrow.mac{padding:0 8px 0 92px;}
  .tabs{display:flex;align-items:flex-end;height:100%;overflow:hidden;
    flex:0 1 auto;}
  .tab{-webkit-app-region:no-drag;display:flex;align-items:center;
    max-width:200px;min-width:90px;height:30px;margin:0 1px;padding:0 4px 0 10px;
    background:#f1f3f4;border-radius:8px 8px 0 0;cursor:default;
    white-space:nowrap;overflow:hidden;}
  .tab.active{background:#fff;}
  .tab .title{flex:1;overflow:hidden;text-overflow:ellipsis;}
  .tab .close{-webkit-app-region:no-drag;margin-left:6px;width:16px;height:16px;
    line-height:16px;text-align:center;border-radius:50%;cursor:pointer;
    flex:none;}
  .tab .close:hover{background:#d0d3d6;}
  .newtab{-webkit-app-region:no-drag;width:28px;height:28px;line-height:26px;
    text-align:center;margin:0 4px;border-radius:50%;cursor:pointer;
    font-size:18px;flex:none;}
  .newtab:hover{background:#c8ccd0;}
  .navrow{display:flex;align-items:center;height:42px;box-sizing:border-box;
    -webkit-app-region:no-drag;padding:0 8px;background:#fff;flex:none;}
  .navrow.hidden{display:none;}
  .navbtn{width:28px;height:28px;margin-right:2px;padding:0;border:none;
    background:transparent;border-radius:4px;cursor:pointer;color:#5f6368;
    font-size:16px;line-height:28px;text-align:center;flex:none;}
  .navbtn:hover{background:#e8eaed;}
  .navbtn:disabled{color:#c0c3c7;cursor:default;background:transparent;}
  #stop{display:none;}
  .urlbar{flex:1;height:28px;margin-left:6px;box-sizing:border-box;
    border:1px solid #cfd3d7;border-radius:14px;padding:0 14px;outline:none;
    font-size:13px;color:#3c4043;background:#f1f3f4;}
  .urlbar:focus{background:#fff;border-color:#4a90e2;}
</style>
</head>
<body>
<div class="root">
  <div class="tabrow" id="tabrow">
    <div class="tabs" id="tabs"></div>
    <div class="newtab" id="newtab" title="New tab">+</div>
  </div>
  <div class="navrow" id="navrow">
    <button class="navbtn" id="back" title="Back" disabled>&#8592;</button>
    <button class="navbtn" id="forward" title="Forward" disabled>&#8594;</button>
    <button class="navbtn" id="reload" title="Reload">&#8635;</button>
    <button class="navbtn" id="stop" title="Stop">&#215;</button>
    <input class="urlbar" id="urlbar" type="text" spellcheck="false"
      placeholder="Search or enter address">
  </div>
</div>
<script>
(function(){
  var tabs=[];
  var nextId=1;
  var activeId=0;
  // Navigation row visibility follows the ?nav flag set by C++ at creation:
  // nav=0 (Chrome-style, Chrome draws its own toolbar) hides it; otherwise
  // (Alloy-style) the HTML owns the address/nav row.
  var navOn=!/[?&]nav=0(&|$)/.test(location.search);
  if(!navOn){
    var nr=document.getElementById('navrow');
    if(nr){nr.classList.add('hidden');}
  }
  if(/Mac/i.test(navigator.platform)){
    document.getElementById('tabrow').classList.add('mac');
  }
  function send(action,extra){
    if(!window.cefQuery){return;}
    var msg={channel:'tabbar',action:action};
    if(extra){for(var k in extra){msg[k]=extra[k];}}
    window.cefQuery({request:JSON.stringify(msg),
      onSuccess:function(){},onFailure:function(){}});
  }
  // Report the strip's intrinsic content height to C++ so the docked view can
  // resize to match (the view height is pinned by C++, so we measure .root,
  // whose rows are fixed-height, giving the true desired height). See §6.5/§8.
  var lastH=0;
  function reportHeight(){
    var root=document.querySelector('.root');
    if(!root){return;}
    var h=Math.ceil(root.getBoundingClientRect().height);
    if(h>0&&h!==lastH){lastH=h;send('resize',{height:h});}
  }
  if(window.ResizeObserver){
    try{new ResizeObserver(reportHeight).observe(document.querySelector('.root'));}
    catch(e){}
  }
  window.addEventListener('load',reportHeight);
  function render(){
    var c=document.getElementById('tabs');
    c.innerHTML='';
    tabs.forEach(function(t){
      var el=document.createElement('div');
      el.className='tab'+(t.id===activeId?' active':'');
      el.setAttribute('data-id',t.id);
      var ti=document.createElement('span');
      ti.className='title';
      ti.textContent=t.title||'New Tab';
      el.appendChild(ti);
      var cl=document.createElement('span');
      cl.className='close';
      cl.textContent='\u00d7';
      cl.setAttribute('data-close',t.id);
      el.appendChild(cl);
      c.appendChild(el);
    });
  }
  function addTab(title,activate){
    var id=nextId++;
    tabs.push({id:id,title:title||'New Tab',url:''});
    if(activate!==false){activeId=id;}
    render();
    return id;
  }
  function closeTab(id){
    var i=tabs.findIndex(function(t){return t.id===id;});
    if(i<0){return;}
    tabs.splice(i,1);
    if(activeId===id){activeId=tabs.length?tabs[Math.max(0,i-1)].id:0;}
    render();
  }
  function selectTab(id){activeId=id;render();}
  document.getElementById('newtab').addEventListener('click',function(){
    var id=addTab('New Tab',true);send('newtab',{id:id});
  });
  document.getElementById('tabs').addEventListener('click',function(e){
    var closeId=e.target.getAttribute&&e.target.getAttribute('data-close');
    if(closeId){var cid=parseInt(closeId,10);closeTab(cid);
      send('closetab',{id:cid});return;}
    var el=e.target;
    while(el&&el!==this&&!(el.getAttribute&&el.getAttribute('data-id'))){
      el=el.parentNode;
    }
    if(el&&el.getAttribute){
      var sid=parseInt(el.getAttribute('data-id'),10);
      if(sid){selectTab(sid);send('selecttab',{id:sid});}
    }
  });
  // Navigation toolbar wiring. Each control reports through cefQuery; the C++
  // side (tabbar message handler, added in a later stage) drives the actual
  // content browser and pushes state back via window.__tabbarApi.
  var urlbar=document.getElementById('urlbar');
  function navigate(u){
    u=(u||'').trim();
    if(!u){return;}
    send('navigate',{url:u});
  }
  document.getElementById('back').addEventListener('click',function(){
    send('back');
  });
  document.getElementById('forward').addEventListener('click',function(){
    send('forward');
  });
  document.getElementById('reload').addEventListener('click',function(){
    send('reload');
  });
  document.getElementById('stop').addEventListener('click',function(){
    send('stop');
  });
  urlbar.addEventListener('keydown',function(e){
    if(e.key==='Enter'||e.keyCode===13){navigate(urlbar.value);}
  });
  function setEnabled(id,enabled){
    var b=document.getElementById(id);
    if(b){b.disabled=!enabled;}
  }
  // Reverse channel: C++ pushes state updates for the active tab here.
  window.__tabbarApi={
    setActiveTitle:function(title){
      var t=tabs.find(function(x){return x.id===activeId;});
      if(t){t.title=title;render();}
    },
    setActiveUrl:function(url){
      var t=tabs.find(function(x){return x.id===activeId;});
      if(t){t.url=url;}
    },
    onAddressChanged:function(url){
      if(url!==undefined&&url!==null){
        if(document.activeElement!==urlbar){urlbar.value=url;}
        this.setActiveUrl(url);
      }
    },
    onLoadingStateChanged:function(s){
      try{
        var st=(typeof s==='string')?JSON.parse(s):s;
        setEnabled('back',!!st.canGoBack);
        setEnabled('forward',!!st.canGoForward);
        var reload=document.getElementById('reload');
        var stop=document.getElementById('stop');
        if(reload&&stop){
          if(st.isLoading){reload.style.display='none';stop.style.display='';}
          else{reload.style.display='';stop.style.display='none';}
        }
      }catch(e){}
    },
    setState:function(json){
      try{
        var s=(typeof json==='string')?JSON.parse(json):json;
        if(s.title!==undefined){this.setActiveTitle(s.title);}
        if(s.url!==undefined){this.onAddressChanged(s.url);}
        if(s.isLoading!==undefined||s.canGoBack!==undefined||
           s.canGoForward!==undefined){this.onLoadingStateChanged(s);}
      }catch(e){}
    },
    reset:function(){tabs=[];nextId=1;activeId=0;render();}
  };
  // Seed an initial tab representing the content browser already shown.
  addTab('New Tab',true);
  // Report the initial height immediately (ResizeObserver may fire later).
  reportHeight();
})();
</script>
</body>
</html>)TABBAR";
}

// Builds the JSON body used by the C++ fallback so it flows through the same
// completion path as a managed response.
std::string BuildResponseJson(int status,
                              const std::string& mime,
                              const std::string& body) {
  CefRefPtr<CefDictionaryValue> d = CefDictionaryValue::Create();
  d->SetInt("status", status);
  d->SetString("mime", mime);
  d->SetString("body", body);
  CefRefPtr<CefValue> v = CefValue::Create();
  v->SetDictionary(d);
  return CefWriteJSON(v, JSON_WRITER_DEFAULT).ToString();
}

// Generic resource handler for custom scheme requests. The content is produced
// by managed code (C#/DSL) through on_custom_scheme; when managed code is
// absent or declines, a built-in fallback page is served. The request is always
// handled asynchronously so the managed dispatch happens on the main thread
// (matching cef_query), never on this worker thread.
class CustomSchemeHandler : public CefResourceHandler {
 public:
  CustomSchemeHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
      : browser_(browser), frame_(frame) {}

  CustomSchemeHandler(const CustomSchemeHandler&) = delete;
  CustomSchemeHandler& operator=(const CustomSchemeHandler&) = delete;

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    DCHECK(!CefCurrentlyOn(TID_UI) && !CefCurrentlyOn(TID_IO));

    const std::string url = request->GetURL();
    const std::string method = request->GetMethod();
    const std::string referrer = request->GetReferrerURL();

    std::string scheme;
    const size_t sep = url.find("://");
    if (sep != std::string::npos) {
      scheme = url.substr(0, sep);
    }

    // Park an async callback so managed/DSL code can produce content on its own
    // schedule (mirrors cef_query). |this| stays alive via the captured
    // CefRefPtr. The completion is registered on the main thread (TID_UI), the
    // same thread the managed handler runs on.
    CefRefPtr<CustomSchemeHandler> self = this;
    const int browser_id = browser_.get() ? browser_->GetIdentifier() : 0;
    const int64_t handle = RegisterNativeCallback(
        browser_id, TID_UI,
        [self, callback](bool ok, const std::string& data, int code) {
          if (ok) {
            bool base64 = false;
            ParseManagedResponse(data, self->status_, self->mime_type_,
                                 self->data_, base64);
            if (base64) {
              self->data_ = DecodeBase64(self->data_);
            }
          } else {
            // Timeout / browser-close cancel: give the page a diagnosable error.
            self->status_ = code != 0 ? code : 504;
            self->mime_type_ = "text/plain";
            self->data_ = "custom scheme handler did not respond";
          }
          callback->Continue();
        },
        kResourceLoadTimeoutMs);

    // Offer the request to managed code on the main thread, matching cef_query's
    // threading discipline (the DSL engine is not driven from worker threads).
    DispatchToManaged(self, handle, scheme, url, method, referrer);

    handle_request = false;
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    CEF_REQUIRE_IO_THREAD();
    response->SetMimeType(mime_type_);
    response->SetStatus(status_);
    response_length = static_cast<int64_t>(data_.length());
  }

  void Cancel() override { CEF_REQUIRE_IO_THREAD(); }

  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    DCHECK(!CefCurrentlyOn(TID_UI) && !CefCurrentlyOn(TID_IO));

    bytes_read = 0;
    if (offset_ < data_.length()) {
      const int transfer_size =
          std::min(bytes_to_read, static_cast<int>(data_.length() - offset_));
      memcpy(data_out, data_.c_str() + offset_, transfer_size);
      offset_ += transfer_size;
      bytes_read = transfer_size;
      return true;
    }
    return false;
  }

 private:
  // Offers the request to managed code on the main thread. If it does not take
  // over, completes the parked handle with the built-in fallback so the response
  // is produced through the single completion path.
  static void DispatchToManaged(CefRefPtr<CustomSchemeHandler> self,
                                int64_t handle,
                                std::string scheme,
                                std::string url,
                                std::string method,
                                std::string referrer) {
    if (!CURRENTLY_ON_MAIN_THREAD()) {
      MAIN_POST_CLOSURE(base::BindOnce(&CustomSchemeHandler::DispatchToManaged,
                                       self, handle, scheme, url, method,
                                       referrer));
      return;
    }

    bool taken = false;
    if (on_custom_scheme_fptr && self->browser_.get()) {
      const int max_size = 4 * 1024 * 1024;
      char* buf = new char[max_size + 1];
      memset(buf, 0, max_size + 1);
      int html_size = max_size;
      taken = on_custom_scheme_fptr(self->browser_.get(), self->frame_.get(),
                                    handle, scheme.c_str(), url.c_str(),
                                    method.c_str(), referrer.c_str(), buf,
                                    html_size);
      if (taken && html_size > 0) {
        // Managed produced HTML synchronously; complete here as text/html so it
        // flows through the single completion path (matches the fallback path).
        buf[html_size] = '\0';
        CompleteNativeCallback(
            handle, true,
            BuildResponseJson(200, "text/html", std::string(buf, html_size)),
            0);
        delete[] buf;
        return;
      }
      delete[] buf;
    }
    if (!taken) {
      // Built-in fallback content. The tab bar page is served here so the HTML
      // tabbar shell works out of the box even when managed code is absent.
      if (GetUrlHost(url) == "tabbar") {
        CompleteNativeCallback(
            handle, true,
            BuildResponseJson(200, "text/html", BuildBuiltinTabbarPage()), 0);
        return;
      }
      const std::string fallback = BuildResponseJson(
          404, "text/html",
          "<html><head><title>Not Found</title></head><body>"
          "<h3>404 - not handled</h3><p>No managed handler produced content "
          "for this custom scheme request.</p></body></html>");
      CompleteNativeCallback(handle, true, fallback, 0);
    }
  }

  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefFrame> frame_;
  std::string data_;
  std::string mime_type_ = "text/html";
  int status_ = 200;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(CustomSchemeHandler);
};

// Factory that creates a CustomSchemeHandler per request.
class CustomSchemeHandlerFactory : public CefSchemeHandlerFactory {
 public:
  CustomSchemeHandlerFactory() = default;

  CustomSchemeHandlerFactory(const CustomSchemeHandlerFactory&) = delete;
  CustomSchemeHandlerFactory& operator=(const CustomSchemeHandlerFactory&) =
      delete;

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    CEF_REQUIRE_IO_THREAD();
    return new CustomSchemeHandler(browser, frame);
  }

  IMPLEMENT_REFCOUNTING(CustomSchemeHandlerFactory);
};

}  // namespace

void RegisterSchemeHandlers() {
  // Empty domain => match all hosts under the scheme.
  CefRegisterSchemeHandlerFactory(kCustomSchemeName, CefString(),
                                  new CustomSchemeHandlerFactory());
}

bool RegisterSchemeFactory(const std::string& scheme,
                           const std::string& domain) {
  if (scheme.empty()) {
    return false;
  }
  return CefRegisterSchemeHandlerFactory(scheme, domain,
                                         new CustomSchemeHandlerFactory());
}

bool UnregisterSchemeFactory(const std::string& scheme,
                             const std::string& domain) {
  if (scheme.empty()) {
    return false;
  }
  // A null factory removes the registration for the scheme/domain pair.
  return CefRegisterSchemeHandlerFactory(scheme, domain, nullptr);
}

}  // namespace client::custom_scheme
