/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Original Author: David W. Hyatt (hyatt@netscape.com)
 *   Benjamin Smedberg <benjamin@smedbergs.us>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef nsChromeRegistry_h
#define nsChromeRegistry_h

#include "nsIChromeRegistry.h"
#include "nsIToolkitChromeRegistry.h"
#include "nsIObserver.h"
#include "nsWeakReference.h"
#include "nsIPrefBranch.h"

#ifdef MOZ_XUL
#include "nsIXULOverlayProvider.h"
#endif

#include "pldhash.h"

#include "nsCOMArray.h"
#include "nsString.h"
#include "nsTHashtable.h"
#include "nsURIHashKey.h"
#include "nsInterfaceHashtable.h"
#include "nsXULAppAPI.h"
#include "nsIResProtocolHandler.h"
#include "nsIXPConnect.h"

#include "mozilla/Omnijar.h"

class nsIDOMWindowInternal;
class nsIURL;

// The chrome registry is actually split between nsChromeRegistryChrome and
// nsChromeRegistryContent. The work/data that is common to both resides in
// the shared nsChromeRegistry implementation, with operations that only make
// sense for one side erroring out in the other.

// for component registration
// {47049e42-1d87-482a-984d-56ae185e367a}
#define NS_CHROMEREGISTRY_CID \
{ 0x47049e42, 0x1d87, 0x482a, { 0x98, 0x4d, 0x56, 0xae, 0x18, 0x5e, 0x36, 0x7a } }

class nsChromeRegistry : public nsIToolkitChromeRegistry,
#ifdef MOZ_XUL
                         public nsIXULOverlayProvider,
#endif
                         public nsIObserver,
                         public nsSupportsWeakReference
{
public:
  NS_DECL_ISUPPORTS

  // nsIXULChromeRegistry methods:
  NS_IMETHOD ReloadChrome();
  NS_IMETHOD RefreshSkins();
  NS_IMETHOD AllowScriptsForPackage(nsIURI* url,
                                    PRBool* _retval NS_OUTPARAM);
  NS_IMETHOD AllowContentToAccess(nsIURI* url,
                                  PRBool* _retval NS_OUTPARAM);

  // nsIChromeRegistry methods:
  NS_IMETHOD_(PRBool) WrappersEnabled(nsIURI *aURI);
  NS_IMETHOD ConvertChromeURL(nsIURI* aChromeURI, nsIURI* *aResult);

  // nsChromeRegistry methods:
  nsChromeRegistry() : mInitialized(PR_FALSE) { }
  virtual ~nsChromeRegistry();

  virtual nsresult Init();

  static already_AddRefed<nsIChromeRegistry> GetService();

  static nsChromeRegistry* gChromeRegistry;

  static nsresult Canonify(nsIURL* aChromeURL);

protected:
  void FlushSkinCaches();
  void FlushAllCaches();

  // Update the selected locale used by the chrome registry, and fire a
  // notification about this change
  virtual nsresult UpdateSelectedLocale() = 0;

  static void LogMessage(const char* aMsg, ...);
  static void LogMessageWithContext(nsIURI* aURL, PRUint32 aLineNumber, PRUint32 flags,
                                    const char* aMsg, ...);

  virtual nsIURI* GetBaseURIFromPackage(const nsCString& aPackage,
                                        const nsCString& aProvider,
                                        const nsCString& aPath) = 0;
  virtual nsresult GetFlagsFromPackage(const nsCString& aPackage,
                                       PRUint32* aFlags) = 0;

  nsresult SelectLocaleFromPref(nsIPrefBranch* prefs);

  static nsresult RefreshWindow(nsIDOMWindowInternal* aWindow);
  static nsresult GetProviderAndPath(nsIURL* aChromeURL,
                                     nsACString& aProvider, nsACString& aPath);

public:
  static already_AddRefed<nsChromeRegistry> GetSingleton();

  struct ManifestProcessingContext
  {
    ManifestProcessingContext(NSLocationType aType, nsILocalFile* aFile)
      : mType(aType)
      , mFile(aFile)
      , mPath(NULL)
    { }

    ManifestProcessingContext(NSLocationType aType, nsILocalFile* aFile, const char* aPath)
      : mType(aType)
      , mFile(aFile)
      , mPath(aPath)
    { }

    ~ManifestProcessingContext()
    { }

    nsIURI* GetManifestURI();
    nsIXPConnect* GetXPConnect();

    already_AddRefed<nsIURI> ResolveURI(const char* uri);

    NSLocationType mType;
    nsILocalFile* mFile;
    const char* mPath;
    nsCOMPtr<nsIURI> mManifestURI;
    nsCOMPtr<nsIXPConnect> mXPConnect;
  };

  virtual void ManifestContent(ManifestProcessingContext& cx, int lineno,
                               char *const * argv, bool platform,
                               bool contentaccessible) = 0;
  virtual void ManifestLocale(ManifestProcessingContext& cx, int lineno,
                              char *const * argv, bool platform,
                              bool contentaccessible) = 0;
  virtual void ManifestSkin(ManifestProcessingContext& cx, int lineno,
                            char *const * argv, bool platform,
                            bool contentaccessible) = 0;
  virtual void ManifestOverlay(ManifestProcessingContext& cx, int lineno,
                               char *const * argv, bool platform,
                               bool contentaccessible) = 0;
  virtual void ManifestStyle(ManifestProcessingContext& cx, int lineno,
                             char *const * argv, bool platform,
                             bool contentaccessible) = 0;
  virtual void ManifestOverride(ManifestProcessingContext& cx, int lineno,
                                char *const * argv, bool platform,
                                bool contentaccessible) = 0;
  virtual void ManifestResource(ManifestProcessingContext& cx, int lineno,
                                char *const * argv, bool platform,
                                bool contentaccessible) = 0;

  // Available flags
  enum {
    // This is a "platform" package (e.g. chrome://global-platform/).
    // Appends one of win/ unix/ mac/ to the base URI.
    PLATFORM_PACKAGE = 1 << 0,

    // This package should use the new XPCNativeWrappers to separate
    // content from chrome. This flag is currently unused (because we call
    // into xpconnect at registration time).
    XPCNATIVEWRAPPERS = 1 << 1,

    // Content script may access files in this package
    CONTENT_ACCESSIBLE = 1 << 2
  };

  PRBool mInitialized;

  // "Override" table (chrome URI string -> real URI)
  nsInterfaceHashtable<nsURIHashKey, nsIURI> mOverrideTable;
};

#endif // nsChromeRegistry_h
