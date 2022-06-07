/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "DocController.h"
#include "AppColors.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "Caption.h"
#include "Menu.h"
#include "TableOfContents.h"
#include "Tabs.h"

#include "utils/Log.h"

static void UpdateTabTitle(WindowTab* tab) {
    if (!tab) {
        return;
    }
    MainWindow* win = tab->win;
    int idx = win->Tabs().Find(tab);
    const char* title = tab->GetTabTitle();
    const char* tooltip = tab->filePath.Get();
    win->tabsCtrl->SetTextAndTooltip(idx, title, tooltip);
}

int GetTabbarHeight(HWND hwnd, float factor) {
    int dy = DpiScale(hwnd, kTabBarDy);
    return (int)(dy * factor);
}

static inline Size GetTabSize(HWND hwnd) {
    int dx = DpiScale(hwnd, std::max(gGlobalPrefs->tabWidth, kTabMinDx));
    int dy = DpiScale(hwnd, kTabBarDy);
    return Size(dx, dy);
}

static void ShowTabBar(MainWindow* win, bool show) {
    if (show == win->tabsVisible) {
        return;
    }
    win->tabsVisible = show;
    win->tabsCtrl->SetIsVisible(show);
    RelayoutWindow(win);
}

void UpdateTabWidth(MainWindow* win) {
    int nTabs = (int)win->TabsCount();
    bool showSingleTab = gGlobalPrefs->useTabs || win->tabsInTitlebar;
    bool showTabs = (nTabs > 1) || (showSingleTab && (nTabs > 0));
    if (!showTabs) {
        ShowTabBar(win, false);
        return;
    }
    ShowTabBar(win, true);
}

static void RemoveTab(MainWindow* win, int idx) {
    WindowTab* tab = win->tabsCtrl->RemoveTab<WindowTab*>(idx);
    UpdateTabFileDisplayStateForTab(tab);
    win->tabSelectionHistory->Remove(tab);
    if (tab == win->CurrentTab()) {
        win->ctrl = nullptr;
        win->currentTabTemp = nullptr;
    }
    delete tab;
    UpdateTabWidth(win);
}

static void WinTabClosedHandler(MainWindow* win, TabsCtrl* tabs, int closedTabIdx) {
    int current = win->tabsCtrl->GetSelected();
    if (closedTabIdx == current) {
        CloseCurrentTab(win);
    } else {
        RemoveTab(win, closedTabIdx);
    }
}

// Selects the given tab (0-based index)
// TODO: this shouldn't go through the same notifications, just do it
void TabsSelect(MainWindow* win, int tabIndex) {
    auto tabs = win->Tabs();
    int count = tabs.Size();
    if (count < 2 || tabIndex < 0 || tabIndex >= count) {
        return;
    }
    TabsCtrl* tabsCtrl = win->tabsCtrl;
    int currIdx = tabsCtrl->GetSelected();
    if (tabIndex == currIdx) {
        return;
    }

    // same work as in onSelectionChanging and onSelectionChanged
    SaveCurrentWindowTab(win);
    int prevIdx = tabsCtrl->SetSelected(tabIndex);
    if (prevIdx < 0) {
        return;
    }
    WindowTab* tab = tabs[tabIndex];
    LoadModelIntoTab(tab);
}

void CreateTabbar(MainWindow* win) {
    TabsCtrl* tabsCtrl = new TabsCtrl();
    tabsCtrl->onTabClosed = [win](TabClosedEvent* ev) { WinTabClosedHandler(win, ev->tabs, ev->tabIdx); };
    tabsCtrl->onSelectionChanging = [win](TabsSelectionChangingEvent* ev) -> bool {
        // TODO: Should we allow the switch of the tab if we are in process of printing?
        SaveCurrentWindowTab(win);
        return false;
    };

    tabsCtrl->onSelectionChanged = [win](TabsSelectionChangedEvent* ev) {
        int currentIdx = win->tabsCtrl->GetSelected();
        WindowTab* tab = win->Tabs()[currentIdx];
        LoadModelIntoTab(tab);
    };

    TabsCreateArgs args;
    args.parent = win->hwndFrame;
    args.createToolTipsHwnd = true;
    tabsCtrl->Create(args);
    win->tabsCtrl = tabsCtrl;
    win->tabSelectionHistory = new Vec<WindowTab*>();
}

// verifies that WindowTab state is consistent with MainWindow state
static NO_INLINE void VerifyWindowTab(MainWindow* win, WindowTab* tdata) {
    CrashIf(tdata->ctrl != win->ctrl);
#if 0
    // disabling this check. best I can tell, external apps can change window
    // title and trigger this
    auto winTitle = win::GetTextTemp(win->hwndFrame);
    if (!str::Eq(winTitle.Get(), tdata->frameTitle.Get())) {
        logf(L"VerifyWindowTab: winTitle: '%s', tdata->frameTitle: '%s'\n", winTitle.Get(), tdata->frameTitle.Get());
        ReportIf(!str::Eq(winTitle.Get(), tdata->frameTitle));
    }
#endif
    bool expectedTocVisibility = tdata->showToc; // if not in presentation mode
    if (PM_DISABLED != win->presentation) {
        expectedTocVisibility = false; // PM_BLACK_SCREEN, PM_WHITE_SCREEN
        if (PM_ENABLED == win->presentation) {
            expectedTocVisibility = tdata->showTocPresentation;
        }
    }
    ReportIf(win->tocVisible != expectedTocVisibility);
    ReportIf(tdata->canvasRc != win->canvasRc);
}

// Must be called when the active tab is losing selection.
// This happens when a new document is loaded or when another tab is selected.
void SaveCurrentWindowTab(MainWindow* win) {
    if (!win) {
        return;
    }

    int current = win->tabsCtrl->GetSelected();
    if (-1 == current) {
        return;
    }
    if (win->CurrentTab() != win->Tabs().at(current)) {
        return; // TODO: restore CrashIf() ?
    }

    WindowTab* tab = win->CurrentTab();
    if (win->tocLoaded) {
        TocTree* tocTree = tab->ctrl->GetToc();
        UpdateTocExpansionState(tab->tocState, win->tocTreeView, tocTree);
    }
    VerifyWindowTab(win, tab);

    // update the selection history
    win->tabSelectionHistory->Remove(tab);
    win->tabSelectionHistory->Append(tab);
}

void UpdateTabsColors(TabsCtrl* tab) {
    tab->currBgCol = kTabDefaultBgCol;
    tab->tabBackgroundBg = GetAppColor(AppColor::TabBackgroundBg);
    tab->tabBackgroundText = GetAppColor(AppColor::TabBackgroundText);
    tab->tabBackgroundCloseX = GetAppColor(AppColor::TabBackgroundCloseX);
    tab->tabBackgroundCloseCircle = GetAppColor(AppColor::TabBackgroundCloseCircle);
    tab->tabSelectedBg = GetAppColor(AppColor::TabSelectedBg);
    tab->tabSelectedText = GetAppColor(AppColor::TabSelectedText);
    tab->tabSelectedCloseX = GetAppColor(AppColor::TabSelectedCloseX);
    tab->tabSelectedCloseCircle = GetAppColor(AppColor::TabSelectedCloseCircle);
    tab->tabHighlightedBg = GetAppColor(AppColor::TabHighlightedBg);
    tab->tabHighlightedText = GetAppColor(AppColor::TabHighlightedText);
    tab->tabHighlightedCloseX = GetAppColor(AppColor::TabHighlightedCloseX);
    tab->tabHighlightedCloseCircle = GetAppColor(AppColor::TabHighlightedCloseCircle);
    tab->tabHoveredCloseX = GetAppColor(AppColor::TabHoveredCloseX);
    tab->tabHoveredCloseCircle = GetAppColor(AppColor::TabHoveredCloseCircle);
    tab->tabClickedCloseX = GetAppColor(AppColor::TabClickedCloseX);
    tab->tabClickedCloseCircle = GetAppColor(AppColor::TabClickedCloseCircle);
}

// On load of a new document we insert a new tab item in the tab bar.
WindowTab* CreateNewTab(MainWindow* win, const char* filePath) {
    CrashIf(!win);
    if (!win) {
        return nullptr;
    }

    auto tabs = win->tabsCtrl;
    int idx = win->TabsCount();
    bool useTabs = gGlobalPrefs->useTabs;
    if (useTabs && idx == 0) {
        // create about tab
        WindowTab* tab = new WindowTab(win, nullptr);
        tab->canvasRc = win->canvasRc;
        TabInfo* newTab = new TabInfo();
        newTab->text = str::Dup("Home");
        newTab->tooltip = nullptr;
        newTab->isPinned = true;
        newTab->userData = (UINT_PTR)tab;
        int insertedIdx = tabs->InsertTab(idx, newTab);
        CrashIf(insertedIdx != 0);
        idx++;
    }

    WindowTab* tab = new WindowTab(win, filePath);
    tab->canvasRc = win->canvasRc;
    TabInfo* newTab = new TabInfo();
    newTab->text = str::Dup(tab->GetTabTitle());
    newTab->tooltip = str::Dup(tab->filePath.Get());
    newTab->userData = (UINT_PTR)tab;

    int insertedIdx = tabs->InsertTab(idx, newTab);
    CrashIf(insertedIdx == -1);
    tabs->SetSelected(idx);
    UpdateTabWidth(win);
    return tab;
}

// Refresh the tab's title
void TabsOnChangedDoc(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    CrashIf(!tab != !win->TabsCount());
    if (!tab) {
        return;
    }

    CrashIf(win->Tabs().Find(tab) != win->tabsCtrl->GetSelected());
    VerifyWindowTab(win, tab);
    UpdateTabTitle(tab);
}

// Called when we're closing a document
void TabsOnCloseDoc(MainWindow* win) {
    if (win->TabsCount() == 0) {
        return;
    }

    /*
    DisplayModel* dm = win->AsFixed();
    if (dm) {
        EngineBase* engine = dm->GetEngine();
        if (EngineHasUnsavedAnnotations(engine)) {
            // TODO: warn about unsaved annotations
            logf("File has unsaved annotations\n");
        }
    }
    */

    int current = win->tabsCtrl->GetSelected();
    RemoveTab(win, current);

    // TODO(tabs): why do I need win->tabSelectionHistory.Size() > 0
    if ((win->TabsCount() > 0)) {
        WindowTab* tab = nullptr;
        int toSelect = 0;
        if (win->tabSelectionHistory->Size() > 0) {
            tab = win->tabSelectionHistory->Pop();
            toSelect = win->Tabs().Find(tab);
        } else {
            tab = win->Tabs()[toSelect];
        }
        win->tabsCtrl->SetSelected(toSelect);
        LoadModelIntoTab(tab);
    }
}

// Called when we're closing an entire window (quitting)
void TabsOnCloseWindow(MainWindow* win) {
    auto tabs = win->Tabs();
    DeleteVecMembers(tabs);
    win->tabsCtrl->RemoveAllTabs();
    win->tabSelectionHistory->Reset();
    win->currentTabTemp = nullptr;
    win->ctrl = nullptr;
}

void SetTabsInTitlebar(MainWindow* win, bool inTitleBar) {
    if (inTitleBar == win->tabsInTitlebar) {
        return;
    }
    win->tabsInTitlebar = inTitleBar;
    win->tabsCtrl->inTitleBar = inTitleBar;
    SetParent(win->tabsCtrl->hwnd, inTitleBar ? win->hwndCaption : win->hwndFrame);
    ShowWindow(win->hwndCaption, inTitleBar ? SW_SHOW : SW_HIDE);
    if (inTitleBar != win->isMenuHidden) {
        ToggleMenuBar(win);
    }
    if (inTitleBar) {
        CaptionUpdateUI(win, win->caption);
        RelayoutCaption(win);
    } else if (dwm::IsCompositionEnabled()) {
        // remove the extended frame
        MARGINS margins{};
        dwm::ExtendFrameIntoClientArea(win->hwndFrame, &margins);
        win->extendedFrameHeight = 0;
    }
    uint flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE;
    SetWindowPos(win->hwndFrame, nullptr, 0, 0, 0, 0, flags);
}

// Selects the next (or previous) tab.
void TabsOnCtrlTab(MainWindow* win, bool reverse) {
    if (!win) {
        return;
    }
    int count = (int)win->TabsCount();
    if (count < 2) {
        return;
    }
    int idx = win->tabsCtrl->GetSelected() + 1;
    if (reverse) {
        idx -= 2;
    }
    idx += count; // ensure > 0
    idx = idx % count;
    TabsSelect(win, idx);
}
