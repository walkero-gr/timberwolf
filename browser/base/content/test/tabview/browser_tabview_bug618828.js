/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

function test() {
  waitForExplicitFinish();

  ok(!TabView.isVisible(), 'TabView is hidden');
  let tab = gBrowser.loadOneTab('about:blank#other', {inBackground: true});

  TabView._initFrame(function () {
    newWindowWithTabView(function (win) {
      onTabViewWindowLoaded(win, tab);
    });
  });
}

function onTabViewWindowLoaded(win, tab) {
  let contentWindow = win.TabView.getContentWindow();
  let search = contentWindow.document.getElementById('search');
  let searchbox = contentWindow.document.getElementById('searchbox');
  let searchButton = contentWindow.document.getElementById('searchbutton');
  let results = contentWindow.document.getElementById('results');

  let isSearchEnabled = function () {
    return 'none' != search.style.display;
  }

  let assertSearchIsEnabled = function () {
    ok(isSearchEnabled(), 'search is enabled');
  }

  let assertSearchIsDisabled = function () {
    ok(!isSearchEnabled(), 'search is disabled');
  }

  let enableSearch = function () {
    assertSearchIsDisabled();
    EventUtils.sendMouseEvent({type: 'mousedown'}, searchButton, contentWindow);
  }

  let finishTest = function () {
    win.close();
    gBrowser.removeTab(tab);
    finish();
  }

  let testClickOnSearchBox = function () {
    EventUtils.synthesizeMouseAtCenter(searchbox, {}, contentWindow);
    assertSearchIsEnabled();
  }

  let testClickOnOtherSearchResult = function () {
    // search for the tab from our main window
    searchbox.setAttribute('value', 'other');
    contentWindow.performSearch();

    // prepare to finish when the main window gets focus back
    window.addEventListener('focus', function () {
      window.removeEventListener('focus', arguments.callee, true);
      assertSearchIsDisabled();

      // check that the right tab is active
      is(gBrowser.selectedTab, tab, 'search result is the active tab');

      finishTest();
    }, true);

    // click the first result
    ok(results.firstChild, 'search returns one result');
    EventUtils.synthesizeMouseAtCenter(results.firstChild, {}, contentWindow);
  }

  enableSearch();
  assertSearchIsEnabled();

  testClickOnSearchBox();
  testClickOnOtherSearchResult();
}

// ---------
function newWindowWithTabView(callback) {
  let win = window.openDialog(getBrowserURL(), "_blank", 
                              "chrome,all,dialog=no,height=800,width=800");
  let onLoad = function() {
    win.removeEventListener("load", onLoad, false);
    let onShown = function() {
      win.removeEventListener("tabviewshown", onShown, false);
      callback(win);
    };
    win.addEventListener("tabviewshown", onShown, false);
    win.TabView.toggle();
  }
  win.addEventListener("load", onLoad, false);
}
