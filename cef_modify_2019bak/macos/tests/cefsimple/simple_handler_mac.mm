// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefsimple/simple_handler.h"

#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include "include/cef_browser.h"

void SimpleHandler::PlatformTitleChange(CefRefPtr<CefBrowser> browser,
                                        const CefString& title) {
  NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
  NSWindow* window = [view window];
  std::string titleStr(title);
  NSString* str = [NSString stringWithUTF8String:titleStr.c_str()];
  [window setTitle:str];
}

std::string SimpleHandler::GetAppPath()
{
    char path[MAXPATHLEN+1];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
        printf("executable path is %s\n", path);
    else
        printf("buffer too small; need size %u\n", size);
    
    if(strlen(path))
    {
        int end = 0, count = 0;
        for(int i = strlen(path) - 1; i>0; i--)
        {
            if(path[i] == '/')
            {
                count++;
            }
            if(count == 4)
            {
                end = i;
                break;
            }
        }
        if(end > 0)
        {
            memset(path+end,0,strlen(path)-end);
        }
    }
    return path;
}
