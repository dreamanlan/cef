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
// renders a tab strip whose membership and activation are owned by native code,
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
  .tab .favicon{width:16px;height:16px;margin-right:6px;flex:none;}
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
  var nativeSynced=false;
  var readyAttempts=0;
  var activeId=0;
  // Multi-tab permission is independent of navigation row visibility.
  var multiTab=/[?&]multitab=1(&|$)/.test(location.search);
  if(!multiTab){
    document.getElementById('newtab').style.display='none';
  }
  // Navigation row visibility follows the ?nav flag set by C++ at creation:
  // nav=0 (Chrome-style, Chrome draws its own toolbar) hides it; otherwise
  // (Alloy-style) the HTML owns the address/nav row.
  var navOn=!/[?&]nav=0(&|$)/.test(location.search);
  if(!navOn){
    var nr=document.getElementById('navrow');
    if(nr){nr.classList.add('hidden');}
  }
  var isMac=/Mac/i.test(navigator.platform);
  var isLinux=/Linux/i.test(navigator.platform);
  if(isMac){
    document.getElementById('tabrow').classList.add('mac');
  }
  // Linux JS-fed drag loop state: no native capture exists, but the implicit
  // X pointer grab keeps delivering mousemove to this page (even outside the
  // window) while the button is held, so the page signals each frame and the
  // native cursor position is queried C++-side (no DPI math crosses IPC).
  var winDrag=false;
  var lastDragFeed=0;
  function send(action,extra){
    if(!multiTab&&(action==='newtab'||action==='closetab'||action==='reordertab')){return;}
    if(!window.cefQuery){return;}
    var msg={channel:'tabbar',action:action};
    if(extra){for(var k in extra){msg[k]=extra[k];}}
    window.cefQuery({request:JSON.stringify(msg),
      onSuccess:function(){},
      onFailure:function(code,message){
        console.warn('Tab bar request failed:',action,code,message);
        if(action==='ready'&&!nativeSynced&&readyAttempts<20){
          setTimeout(requestSnapshot,250);
        }
      }});
  }
  function requestSnapshot(){
    ++readyAttempts;
    send('ready');
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
      if(t.favicon){
        var fv=document.createElement('img');
        fv.className='favicon';
        fv.src=t.favicon;
        fv.alt='';
        el.appendChild(fv);
      }
      var ti=document.createElement('span');
      ti.className='title';
      ti.textContent=(t.loading?'Loading: ':'')+(t.title||'New Tab');
      el.title=t.url||t.title||'New Tab';
      el.appendChild(ti);
      var cl=document.createElement('span');
      cl.className='close';
      cl.textContent='\u00d7';
      cl.setAttribute('data-close',t.id);
      if(!multiTab){cl.style.display='none';}
      el.appendChild(cl);
      c.appendChild(el);
    });
  }
  // Mouse-driven tab drag: horizontal drag reorders (optimistically, with the
  // native order confirmed via the next snapshot), vertical drag detaches the
  // tab into a new window, and a single-tab strip drags the whole window so it
  // can be merged into another tab bar on drop (M4a).
  var dragInfo=null;
  var suppressClick=false;
  var dropIndicator=null;
  var lastDropBefore=-1;
  var strip=document.getElementById('tabs');
  function tabAt(node){
    var el=node;
    while(el&&el!==strip&&!(el.getAttribute&&el.getAttribute('data-id'))){
      el=el.parentNode;
    }
    return (el&&el!==strip&&el.getAttribute)?el:null;
  }
  function inCloseBtn(node){
    var el=node;
    while(el&&el!==strip){
      if(el.className==='close'){return true;}
      el=el.parentNode;
    }
    return false;
  }
  // Move |id| locally so the strip follows the pointer during the drag. The
  // insertion slot is computed against the tab midpoints, not the element
  // under the release point: tabs are wide and pointer-anchored feedback is
  // what makes reordering usable.
  function applyLocalReorder(id,x){
    var from=-1;
    for(var i=0;i<tabs.length;i++){if(tabs[i].id===id){from=i;break;}}
    if(from<0){return;}
    var to=tabs.length;
    var els=strip.children;
    for(var j=0;j<els.length;j++){
      var r=els[j].getBoundingClientRect();
      if(x<r.left+r.width/2){to=j;break;}
    }
    if(to>from){to--;}
    if(to===from){return;}
    tabs.splice(to,0,tabs.splice(from,1)[0]);
    render();
  }
  strip.addEventListener('mousedown',function(e){
    if(!multiTab||e.button!==0||inCloseBtn(e.target)){return;}
    var el=tabAt(e.target);
    if(!el){return;}
    var id=parseInt(el.getAttribute('data-id'),10)||0;
    if(id){dragInfo={id:id,x:e.clientX,y:e.clientY,detached:false,moved:false};}
    e.preventDefault();
  });
  // Middle-click closes the tab (Chrome parity).
  strip.addEventListener('auxclick',function(e){
    if(e.button!==1||!multiTab){return;}
    var el=tabAt(e.target);
    if(!el){return;}
    var id=parseInt(el.getAttribute('data-id'),10)||0;
    if(id){send('closetab',{id:id});}
  });
  // Whole-window drag from the strip's blank area (M4b). Only multitab
  // windows route through the JS gesture: their controller accepts the
  // dragwindow request. Chrome-style and non-multitab windows keep the
  // native drag region (RequestWindowDrag rejects them, and removing the
  // region would leave them undraggable). macOS keeps the native region
  // everywhere (double-click to zoom stays native there).
  var rowDrag=null;
  if(!isMac&&multiTab){
    var tabrow=document.getElementById('tabrow');
    tabrow.style.webkitAppRegion='no-drag';
    tabrow.addEventListener('mousedown',function(e){
      if(!multiTab||e.button!==0){return;}
      if(tabAt(e.target)||e.target.id==='newtab'||inCloseBtn(e.target)){return;}
      rowDrag={x:e.clientX,y:e.clientY,sent:false};
    });
    // Compensate the double-click affordance lost with the drag region.
    tabrow.addEventListener('dblclick',function(e){
      if(!multiTab){return;}
      if(tabAt(e.target)||e.target.id==='newtab'||inCloseBtn(e.target)){return;}
      send('togglemaximize');
    });
  }
  document.addEventListener('mousemove',function(e){
    if(!e.buttons){rowDrag=null;return;}
    if(!rowDrag||rowDrag.sent){return;}
    if(Math.abs(e.clientX-rowDrag.x)>=10||Math.abs(e.clientY-rowDrag.y)>=10){
      rowDrag.sent=true;
      // Blank-area window move: never merges (Chrome parity) — only tab
      // gestures may merge into another window's tab bar.
      send('dragwindow',{merge:0});
      if(isLinux){winDrag=true;}
    }
  });
  document.addEventListener('mouseup',function(e){
    if(rowDrag){rowDrag=null;}
  });
  // Linux drag-loop feeding: each throttled move signals one native cursor
  // query + window move; release ends the session (merge on hover); Escape
  // cancels it.
  document.addEventListener('mousemove',function(e){
    if(!winDrag||!e.buttons){return;}
    var now=Date.now();
    if(now-lastDragFeed<16){return;}
    lastDragFeed=now;
    send('windowdragmove');
  });
  document.addEventListener('mouseup',function(e){
    if(winDrag){winDrag=false;send('windowdragend');}
  });
  document.addEventListener('keydown',function(e){
    if(winDrag&&(e.key==='Escape'||e.keyCode===27)){
      winDrag=false;
      send('windowdragend',{cancel:1});
    }
  });
  document.addEventListener('mousemove',function(e){
    if(!e.buttons){dragInfo=null;return;}
    if(!dragInfo||dragInfo.detached){return;}
    var dx=e.clientX-dragInfo.x,dy=e.clientY-dragInfo.y;
    if(tabs.length>1&&Math.abs(dy)>=24){
      dragInfo.detached=true;
      send('detachtab',{id:dragInfo.id});
      // Linux: the torn-off window's drag loop is fed from this page, since
      // the gesture's implicit X grab stays here after the detach.
      if(isLinux){winDrag=true;}
      return;
    }
    if(tabs.length===1&&(Math.abs(dx)>=10||Math.abs(dy)>=10)){
      // Hand the gesture to the native drag session: the window follows the
      // cursor and can merge into another tab bar on drop.
      dragInfo.detached=true;
      send('dragwindow');
      if(isLinux){winDrag=true;}
      return;
    }
    if(tabs.length>1&&Math.abs(dx)>=10){
      dragInfo.moved=true;
      applyLocalReorder(dragInfo.id,e.clientX);
    }
  });
  document.addEventListener('mouseup',function(e){
    if(!dragInfo){return;}
    var d=dragInfo;dragInfo=null;
    if(d.detached){return;}
    if(d.moved){
      suppressClick=true;
      var beforeId=0;
      for(var i=0;i<tabs.length;i++){
        if(tabs[i].id===d.id){
          beforeId=(i+1<tabs.length)?tabs[i+1].id:0;
          break;
        }
      }
      send('reordertab',{id:d.id,beforeId:beforeId});
    }
  });

  // Requests never change local membership or activation optimistically.
  document.getElementById('newtab').addEventListener('click',function(){
    if(!multiTab){return;}
    send('newtab');
  });
  document.getElementById('tabs').addEventListener('click',function(e){
    if(suppressClick){suppressClick=false;return;}
    var closeId=e.target.getAttribute&&e.target.getAttribute('data-close');
    if(closeId&&!multiTab){return;}
    if(closeId){
      send('closetab',{id:parseInt(closeId,10)});return;}
    var el=e.target;
    while(el&&el!==this&&!(el.getAttribute&&el.getAttribute('data-id'))){
      el=el.parentNode;
    }
    if(el&&el.getAttribute){
      var sid=parseInt(el.getAttribute('data-id'),10);
      if(sid&&multiTab){send('selecttab',{id:sid});}
    }
  });
  // Navigation controls request native operations. State arrives through
  // window.__tabbarApi after native browser callbacks.
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
    onTabsChanged:function(items,selectedId){
      if(!Array.isArray(items)){return;}
      var previousId=activeId;
      nativeSynced=true;
      tabs=items.filter(function(t){
        return t&&Number.isInteger(t.id)&&t.id>0;
      });
      var selected=tabs.find(function(t){return t.id===selectedId;});
      activeId=selected?selected.id:0;
      render();
      if(previousId!==activeId||document.activeElement!==urlbar){
        urlbar.value=selected?(selected.url||''):'';
      }
      this.onLoadingStateChanged({
        isLoading:!!(selected&&selected.loading),
        canGoBack:!!(selected&&selected.canGoBack),
        canGoForward:!!(selected&&selected.canGoForward)
      },true);
    },
    setActiveTitle:function(title){
      if(nativeSynced){return;}
      var t=tabs.find(function(x){return x.id===activeId;});
      if(t){t.title=title;render();}
    },
    setActiveUrl:function(url){
      if(nativeSynced){return;}
      var t=tabs.find(function(x){return x.id===activeId;});
      if(t){t.url=url;}
    },
    onAddressChanged:function(url){
      if(nativeSynced){return;}
      if(url!==undefined&&url!==null){
        if(document.activeElement!==urlbar){urlbar.value=url;}
        this.setActiveUrl(url);
      }
    },
    onLoadingStateChanged:function(s,fromSnapshot){
      if(nativeSynced&&!fromSnapshot){return;}
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
    // Merged-window drop indicator (M4a): C++ pushes hover state with a
    // strip-normalized cursor position while a TabDragController session is
    // active. The page owns the layout, so it resolves the insertion slot,
    // draws the indicator and reports the slot back via "drophover".
    onDropHover:function(active,relativeX){
      if(dropIndicator&&dropIndicator.parentNode){
        dropIndicator.parentNode.removeChild(dropIndicator);
      }
      dropIndicator=null;
      if(!active){lastDropBefore=-1;return;}
      var row=document.getElementById('tabrow');
      if(!row){return;}
      var r=strip.getBoundingClientRect();
      // relativeX is normalized over the tab bar view's width (= this page's
      // viewport), not the #tabs element: the strip is inset by the row
      // padding and may be clipped when overflowing, so re-basing it on the
      // element would skew the insertion slot left by roughly half the
      // insets (observed: cursor on tab 2 showed the slot before tab 1).
      var px=(+relativeX||0)*window.innerWidth;
      var els=strip.children;
      var beforeId=0;
      var indX=r.right;
      for(var j=0;j<els.length;j++){
        var tr=els[j].getBoundingClientRect();
        if(px<tr.left+tr.width/2){
          beforeId=parseInt(els[j].getAttribute('data-id'),10)||0;
          indX=tr.left;
          break;
        }
      }
      row.style.position='relative';
      var ind=document.createElement('div');
      ind.style.cssText='position:absolute;top:2px;bottom:2px;width:2px;'+
        'background:#4a90e2;border-radius:1px;z-index:9;';
      ind.style.left=(indX-1)+'px';
      row.appendChild(ind);
      dropIndicator=ind;
      if(lastDropBefore!==beforeId){
        lastDropBefore=beforeId;
        send('drophover',{beforeId:beforeId});
      }
    },
    reset:function(){
      requestSnapshot();
    }
  };
  // Install the reverse API before requesting the authoritative initial list.
  render();
  requestSnapshot();
  window.addEventListener('load',requestSnapshot);
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
