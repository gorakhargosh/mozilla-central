/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

function test() {
  let cw;

  let createGroupItem = function () {
    let bounds = new cw.Rect(20, 20, 150, 150);
    let groupItem = new cw.GroupItem([], {bounds: bounds, immediately: true});

    cw.UI.setActive(groupItem);
    gBrowser.loadOneTab('about:blank', {inBackground: true});

    return groupItem;
  }

  let testVeryQuickDragAndDrop = function () {
    let sourceGroup = createGroupItem();
    let targetGroup = cw.GroupItems.groupItems[0];

    sourceGroup.pushAway(true);
    targetGroup.pushAway(true);

    let sourceTab = sourceGroup.getChild(0).container;
    EventUtils.synthesizeMouseAtCenter(sourceTab, {type: 'mousedown'}, cw);

    let targetTab = targetGroup.getChild(0).container;
    EventUtils.synthesizeMouseAtCenter(targetTab, {type: 'mousemove'}, cw);
    EventUtils.synthesizeMouseAtCenter(targetTab, {type: 'mouseup'}, cw);

    is(cw.GroupItems.groupItems.length, 2, 'there are two groupItems');
    is(sourceGroup.getChildren().length, 0, 'source group has no tabs');
    is(targetGroup.getChildren().length, 2, 'target group has two tabs');

    targetGroup.getChild(0).close();

    closeGroupItem(sourceGroup, function () {
      hideTabView(finish);
    });
  }

  waitForExplicitFinish();

  showTabView(function () {
    cw = TabView.getContentWindow();
    afterAllTabsLoaded(testVeryQuickDragAndDrop);
  });
}
