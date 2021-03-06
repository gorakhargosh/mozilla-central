/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

function test() {
  is(gBrowser.tabs.length, 1, "Only one tab exist");

  let originalTab = gBrowser.tabs[0];

  popup(originalTab);
  ok(document.getElementById("context_closeTab").disabled, "The 'Close tab' menu item is disabled");
  ok(document.getElementById("context_openTabInWindow").disabled, "The 'Move to New Window' menu item is disabled");

  let newTabOne = gBrowser.addTab("about:blank", {skipAnimation: true});

  waitForExplicitFinish();

  showTabView(function() {
    registerCleanupFunction(function () {
      while (gBrowser.tabs.length > 1)
        gBrowser.removeTab(gBrowser.tabs[1]);
      TabView.hide();
    });

    let contentWindow = TabView.getContentWindow();
    is(contentWindow.GroupItems.groupItems.length, 1, "Has one group only");

    let tabItems = contentWindow.GroupItems.groupItems[0].getChildren();
    ok(tabItems.length, 2, "There are two tabItems in this group");

    whenTabViewIsHidden(function() {
      popup(gBrowser.tabs[0]);

      ok(!document.getElementById("context_closeTab").disabled, "The 'Close tab' menu item is enabled");
      ok(!document.getElementById("context_openTabInWindow").disabled, "The 'Move to New Window' menu item is enabled");

      gBrowser.selected = originalTab;
      gBrowser.removeTab(newTabOne);

      closeGroupItem(newGroup, finish);
    });
    let newGroup = contentWindow.GroupItems.newGroup();
    newGroup.newTab();
  });
}

function popup(tab) {
  document.popupNode = tab;
  TabContextMenu.updateContextMenu(document.getElementById("tabContextMenu"));
  is(TabContextMenu.contextTab, tab, "TabContextMenu context is the expected tab");
  TabContextMenu.contextTab = null;
}

