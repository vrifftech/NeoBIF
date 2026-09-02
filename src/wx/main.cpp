#include "core/KeyBifArchive.hpp"
#include "core/Version.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "neobif_icon.xpm"

#if defined(__EMSCRIPTEN__)
#include "NeoBrowserFiles.hpp"
#include <emscripten.h>
static_assert(neobrowser::kBrowserFileApiVersion >= 8u,
              "NeoBIF requires the retained-file/range-read browser API from neoshared.");
#endif

#include <wx/app.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

#if !defined(__EMSCRIPTEN__)
constexpr const char* kArchiveWildcard =
    "KotOR archive files (*.key;*.bif)|*.key;*.bif|KEY files (*.key)|*.key|BIF files (*.bif)|*.bif|All files (*.*)|*.*";
constexpr const char* kBifWildcard =
    "BIF files (*.bif)|*.bif|All files (*.*)|*.*";
constexpr const char* kAllFilesWildcard = "All files (*.*)|*.*";
constexpr const char* kZipWildcard = "ZIP archives (*.zip)|*.zip|All files (*.*)|*.*";
#endif
constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaximumTreeResourceNodes = 50000u;

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool containsInsensitive(const std::string& value, const std::string& needle) {
    if (needle.empty()) return true;
    return lowerAscii(value).find(lowerAscii(needle)) != std::string::npos;
}

bool hasExtension(const std::filesystem::path& path, const std::string& extension) {
    return lowerAscii(path.extension().string()) == lowerAscii(extension);
}

std::string yesNo(bool value) { return value ? "Yes" : "No"; }

std::string formatBuildYear(std::uint32_t raw) {
    if (raw > 0u && raw < 1900u) return std::to_string(raw + 1900u) + " (raw " + std::to_string(raw) + ')';
    return std::to_string(raw);
}

enum class NodeKind {
    Root,
    Bif,
    Type,
    Resource,
};

class NodeData final : public wxTreeItemData {
public:
    NodeData(NodeKind nodeKind,
             std::size_t bif = kNoIndex,
             std::size_t resource = kNoIndex,
             std::uint16_t resourceType = 0u)
        : kind(nodeKind), bifIndex(bif), resourceIndex(resource), type(resourceType) {}

    NodeKind kind;
    std::size_t bifIndex;
    std::size_t resourceIndex;
    std::uint16_t type;
};

enum {
    ID_OpenArchive = wxID_HIGHEST + 410,
    ID_AddBifs,
    ID_ScanDirectory,
    ID_CloseArchive,
    ID_ExtractSelected,
    ID_ExtractAll,
    ID_ZipSelected,
    ID_ZipAll,
    ID_ExpandAll,
    ID_CollapseAll,
    ID_GroupByType,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_FilterTimer,
};

class NeoBIFFrame final : public wxFrame {
public:
    NeoBIFFrame()
        : wxFrame(nullptr, wxID_ANY, appTitle()),
          filterTimer_(this, ID_FilterTimer),
          darkMode_(wxui::readDarkMode("NeoBIF")) {
        setApplicationIcon();
        buildMenus();
        buildUi();
        bindEvents();
        wxui::createStatusBar(*this, 2);
        int widths[] = {-1, 270};
        GetStatusBar()->SetStatusWidths(2, widths);
        wxui::configureResponsiveWindow(*this, wxSize(1220, 760), wxSize(820, 520));
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        updateStatus();
    }

    ~NeoBIFFrame() override {
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
    }

    void openPath(const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec) && !ec) {
            scanDirectory(path);
            return;
        }
        if (hasExtension(path, ".key")) {
            openArchive(path, {});
            return;
        }
        wxMessageBox("NeoBIF expects a chitin.key file or a game directory.",
                     "Unsupported Input", wxOK | wxICON_ERROR, this);
    }

private:
    neobif::KeyBifArchive archive_;
    std::filesystem::path keyPath_;
    std::vector<std::filesystem::path> supplementaryFiles_;
#if defined(__EMSCRIPTEN__)
    std::vector<neobif::BrowserArchiveFile> browserFiles_;
    std::vector<std::uint32_t> browserSessions_;
    std::uint64_t browserGeneration_{};
    std::uint64_t browserSelectionRequest_{};
    bool browserIndexing_{};
#endif
    wxSearchCtrl* filter_{nullptr};
    wxTreeCtrl* tree_{nullptr};
    wxTextCtrl* details_{nullptr};
    wxStaticText* keyLabel_{nullptr};
    wxButton* extractButton_{nullptr};
    wxMenuItem* groupByTypeItem_{nullptr};
    wxMenuItem* darkModeItem_{nullptr};
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    neosettings::AppSettings settings_{"NeoBIF"};
    wxTimer filterTimer_;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool groupByType_{true};
    bool darkMode_{};

    static wxString appTitle() {
        return wxui::toWx(std::string("NeoBIF v") + neobif::kVersion);
    }

    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neobif", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) bundle.AddIcon(windowsIcon);
#endif
        wxIcon fallback(neobif_icon_xpm);
        if (fallback.IsOk()) bundle.AddIcon(fallback);
        if (bundle.GetIconCount() > 0u) SetIcons(bundle);
    }

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_OpenArchive, "&Open KEY/BIF Files...\tCtrl+O");
        file->Append(ID_AddBifs, "&Add or Relocate BIF Files...");
        file->Append(ID_ScanDirectory, "&Scan Game Directory...");
#if !defined(__EMSCRIPTEN__)
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file,
            [this](const neogames::SavedGameDirectory& directory) {
                scanDirectory(directory.path);
            },
            neogames::GameDirectoryGameIds{"kotor", "kotor2"},
            "Open Saved &KotOR Directory");
#endif
        file->AppendSeparator();
        file->Append(ID_CloseArchive, "&Close Archive");
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit\tCtrl+Q");

        auto* extract = new wxMenu;
        extract->Append(ID_ExtractSelected, "Extract &Selected...\tCtrl+E");
        extract->Append(ID_ExtractAll, "Extract &All...");
        extract->AppendSeparator();
        extract->Append(ID_ZipSelected, "Save Selected as &ZIP...");
        extract->Append(ID_ZipAll, "Save All as Z&IP...");

        auto* view = new wxMenu;
        groupByTypeItem_ = view->AppendCheckItem(ID_GroupByType, "Group Resources by &Type");
        groupByTypeItem_->Check(groupByType_);
        view->AppendSeparator();
        view->Append(ID_ExpandAll, "&Expand All");
        view->Append(ID_CollapseAll, "&Collapse All");
        view->AppendSeparator();
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        darkModeItem_->Check(darkMode_);
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About NeoBIF");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(extract, "&Extract");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);
    }

    void buildUi() {
        auto* root = new wxPanel(this);
        auto* outer = new wxBoxSizer(wxVERTICAL);

        auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
        auto* open = new wxButton(root, ID_OpenArchive, "Open KEY/BIF Files...");
        auto* scan = new wxButton(root, ID_ScanDirectory, "Scan Game Directory...");
        auto* add = new wxButton(root, ID_AddBifs, "Add BIF Files...");
        extractButton_ = new wxButton(root, ID_ExtractSelected, "Extract Selected...");
        actionRow->Add(open, 0, wxRIGHT, FromDIP(6));
        actionRow->Add(scan, 0, wxRIGHT, FromDIP(6));
        actionRow->Add(add, 0, wxRIGHT, FromDIP(6));
        actionRow->Add(extractButton_, 0, wxRIGHT, FromDIP(12));
        keyLabel_ = new wxStaticText(root, wxID_ANY, "No archive open");
        actionRow->Add(keyLabel_, 1, wxALIGN_CENTER_VERTICAL);
        outer->Add(actionRow, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(root, wxID_ANY, "Filter:"), 0,
                       wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filter_ = new wxSearchCtrl(root, wxID_ANY);
        filter_->SetDescriptiveText("resource name, type, ID, BIF path, or status");
        filter_->ShowCancelButton(true);
        filterRow->Add(filter_, 1, wxEXPAND);
        outer->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* splitter = new wxSplitterWindow(root, wxID_ANY, wxDefaultPosition,
                                              wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
        tree_ = new wxTreeCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE |
                                   wxTR_FULL_ROW_HIGHLIGHT);
        details_ = new wxTextCtrl(splitter, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP |
                                      wxBORDER_NONE);
        details_->SetFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        splitter->SplitVertically(tree_, details_, FromDIP(620));
        splitter->SetMinimumPaneSize(FromDIP(260));
        splitter->SetSashGravity(0.57);
        outer->Add(splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        root->SetSizer(outer);
        auto* frameSizer = new wxBoxSizer(wxVERTICAL);
        frameSizer->Add(root, 1, wxEXPAND);
        SetSizer(frameSizer);
        updateCommandState();
    }

    void bindEvents() {
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeArchive(); }, ID_CloseArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(true); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(false); }, ID_ExtractAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(true); }, ID_ZipSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(true); }, ID_ZipAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { tree_->ExpandAll(); }, ID_ExpandAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            tree_->CollapseAll();
            if (tree_->GetRootItem().IsOk()) tree_->Expand(tree_->GetRootItem());
        }, ID_CollapseAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            groupByType_ = event.IsChecked();
            rebuildTree();
        }, ID_GroupByType);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            darkMode_ = event.IsChecked();
            wxui::writeDarkMode("NeoBIF", darkMode_);
            applyDarkMode();
        }, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoBIFFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoBIFFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoBIFFrame::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, wxID_ABOUT);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { rebuildTree(); }, ID_FilterTimer);
        filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filterTimer_.StartOnce(180); });
        filter_->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [this](wxCommandEvent&) {
            filter_->ChangeValue(wxEmptyString);
            rebuildTree();
        });
        tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
            showSelectedDetails();
            updateCommandState();
        });
        tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent&) { extractSelected(false); });
        tree_->Bind(wxEVT_TREE_ITEM_MENU, &NeoBIFFrame::onTreeItemMenu, this);
    }

    void onTreeItemMenu(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        if (!item.IsOk()) return;

        tree_->SelectItem(item);
        showSelectedDetails();
        updateCommandState();

        const NodeData* data = selectedNodeData();
        if (data == nullptr) return;

        const auto indices = selectedResourceIndices();
        const bool hasExtractable = std::any_of(
            indices.begin(), indices.end(), [this](std::size_t index) {
                return index < archive_.resources().size() &&
                       archive_.resources()[index].extractable;
            });

        wxString extractLabel = "&Extract Selection...";
        wxString zipLabel = "Save Selection as &ZIP...";
        switch (data->kind) {
        case NodeKind::Root:
            extractLabel = "Extract &All...";
            zipLabel = "Save All as &ZIP...";
            break;
        case NodeKind::Bif:
            extractLabel = "Extract &BIF...";
            zipLabel = "Save BIF as &ZIP...";
            break;
        case NodeKind::Type:
            extractLabel = "Extract Resource &Type...";
            zipLabel = "Save Resource Type as &ZIP...";
            break;
        case NodeKind::Resource:
            extractLabel = "Extract &Resource...";
            zipLabel = "Save Resource as &ZIP...";
            break;
        }

        wxMenu menu;
        wxMenuItem* extractItem = menu.Append(ID_ExtractSelected, extractLabel);
        wxMenuItem* zipItem = menu.Append(ID_ZipSelected, zipLabel);
        extractItem->Enable(hasExtractable);
        zipItem->Enable(hasExtractable);
        PopupMenu(&menu);
    }

    void requestOpenArchiveFiles() {
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFFrame> weak(this);
        neobrowser::requestRetainedFiles(
            "Open chitin.key and optional BIF files", ".key,.bif", true,
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), false, request);
            });
#else
        wxWeakRef<NeoBIFFrame> weak(this);
        wxui::requestOpenFiles(
            this, "Open chitin.key and optional BIF files", kArchiveWildcard,
            [weak](std::vector<std::filesystem::path> paths) mutable {
                if (!weak || paths.empty()) return;
                weak->openSelectedFiles(std::move(paths));
            });
#endif
    }

    void openSelectedFiles(std::vector<std::filesystem::path> paths) {
#if !defined(__EMSCRIPTEN__)
        std::vector<std::filesystem::path> keys;
        std::vector<std::filesystem::path> bifs;
        for (const auto& path : paths) {
            if (hasExtension(path, ".key")) keys.push_back(path);
            else if (hasExtension(path, ".bif")) bifs.push_back(path);
        }
        if (keys.empty()) {
            if (archive_.isOpen() && !bifs.empty()) {
                addSupplementaryFiles(std::move(bifs));
                return;
            }
            wxMessageBox("Select chitin.key. You may select the referenced BIF files in the same operation.",
                         "KEY File Required", wxOK | wxICON_ERROR, this);
            return;
        }
        std::filesystem::path key = keys.front();
        if (keys.size() > 1u) {
            const auto preferred = std::find_if(keys.begin(), keys.end(), [](const auto& path) {
                return lowerAscii(path.filename().string()) == "chitin.key";
            });
            if (preferred != keys.end()) key = *preferred;
            else {
                wxMessageBox("More than one KEY file was selected. Select one game archive at a time.",
                             "Multiple KEY Files", wxOK | wxICON_ERROR, this);
                return;
            }
        }
        openArchive(key, std::move(bifs));
#else
        (void)paths;
#endif
    }

    void requestAddBifs() {
        if (!archive_.isOpen()) {
            requestOpenArchiveFiles();
            return;
        }
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFFrame> weak(this);
        neobrowser::requestRetainedFiles(
            "Add or relocate BIF files", ".bif", true,
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), true, request);
            });
#else
        wxWeakRef<NeoBIFFrame> weak(this);
        wxui::requestOpenFiles(
            this, "Add or relocate BIF files", kBifWildcard,
            [weak](std::vector<std::filesystem::path> paths) mutable {
                if (!weak || paths.empty()) return;
                weak->addSupplementaryFiles(std::move(paths));
            });
#endif
    }

    void addSupplementaryFiles(std::vector<std::filesystem::path> paths) {
#if !defined(__EMSCRIPTEN__)
        for (auto& path : paths) {
            if (!hasExtension(path, ".bif")) continue;
            if (std::find(supplementaryFiles_.begin(), supplementaryFiles_.end(), path) ==
                supplementaryFiles_.end()) {
                supplementaryFiles_.push_back(std::move(path));
            }
        }
        openArchive(keyPath_, supplementaryFiles_);
#else
        (void)paths;
#endif
    }

    void requestScanDirectory() {
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFFrame> weak(this);
        neobrowser::requestRetainedDirectory(
            "Select a KotOR game directory", ".key,.bif",
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), false, request);
            });
#else
        wxDirDialog dialog(this, "Select a KotOR game directory", wxEmptyString,
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        scanDirectory(neosettings::pathFromWx(dialog.GetPath()));
#endif
    }

    void scanDirectory(const std::filesystem::path& directory) {
#if !defined(__EMSCRIPTEN__)
        const auto keys = neobif::KeyBifArchive::scanForKeyFiles(directory);
        if (keys.empty()) {
            wxMessageBox("No chitin.key file was found under the selected directory.",
                         "No KEY Archive Found", wxOK | wxICON_ERROR, this);
            return;
        }
        std::filesystem::path selected = keys.front();
        if (keys.size() > 1u) {
            wxArrayString choices;
            for (const auto& key : keys) choices.Add(neosettings::pathToWx(key));
            wxSingleChoiceDialog dialog(this, "Select the game archive to open:",
                                        "Multiple KEY Archives", choices);
            if (dialog.ShowModal() != wxID_OK) return;
            selected = keys[static_cast<std::size_t>(dialog.GetSelection())];
        }
        openArchive(selected, {});
#else
        (void)directory;
#endif
    }

#if defined(__EMSCRIPTEN__)
    static std::size_t browserPathDepth(const std::string& path) {
        return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
    }

    static std::optional<neobif::BrowserArchiveFile> selectBrowserKey(
        const std::vector<neobif::BrowserArchiveFile>& files,
        std::string& error) {
        std::vector<neobif::BrowserArchiveFile> candidates;
        for (const auto& file : files) {
            const std::filesystem::path path(file.relativePath);
            if (hasExtension(path, ".key")) candidates.push_back(file);
        }
        if (candidates.empty()) {
            error = "Select chitin.key together with the required BIF files, or scan the game directory.";
            return std::nullopt;
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            const bool leftChitin = lowerAscii(std::filesystem::path(left.relativePath).filename().string()) == "chitin.key";
            const bool rightChitin = lowerAscii(std::filesystem::path(right.relativePath).filename().string()) == "chitin.key";
            if (leftChitin != rightChitin) return leftChitin;
            const std::size_t leftDepth = browserPathDepth(left.relativePath);
            const std::size_t rightDepth = browserPathDepth(right.relativePath);
            if (leftDepth != rightDepth) return leftDepth < rightDepth;
            return lowerAscii(left.relativePath) < lowerAscii(right.relativePath);
        });
        if (candidates.size() > 1u) {
            const auto& first = candidates[0];
            const auto& second = candidates[1];
            const bool firstChitin = lowerAscii(std::filesystem::path(first.relativePath).filename().string()) == "chitin.key";
            const bool secondChitin = lowerAscii(std::filesystem::path(second.relativePath).filename().string()) == "chitin.key";
            if (firstChitin == secondChitin &&
                browserPathDepth(first.relativePath) == browserPathDepth(second.relativePath)) {
                error = "More than one equally likely KEY file was selected. Select one game installation at a time.";
                return std::nullopt;
            }
        }
        return candidates.front();
    }

    static void acceptBrowserFileSet(wxWeakRef<NeoBIFFrame> weak,
                                     neobrowser::RetainedFileSetResult result,
                                     bool appendBifs,
                                     std::uint64_t request) {
        if (!weak) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        if (!result.error.empty()) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(result.error), "Browser Archive Selection Failed",
                         wxOK | wxICON_ERROR, weak.get());
            return;
        }
        if (result.cancelled()) return;

        std::vector<neobif::BrowserArchiveFile> incoming;
        incoming.reserve(result.files.size());
        for (const auto& file : result.files) {
            const std::filesystem::path path(file.relativePath);
            if (!hasExtension(path, ".key") && !hasExtension(path, ".bif")) continue;
            incoming.push_back({result.sessionId, file.fileId, file.relativePath, file.size});
        }
        if (incoming.empty()) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("The selection contains no KEY or BIF files.",
                         "No KotOR Archives Found", wxOK | wxICON_ERROR, weak.get());
            return;
        }

        std::vector<neobif::BrowserArchiveFile> combined;
        const std::uint64_t generation = weak->browserGeneration_;
        if (appendBifs) {
            combined = weak->browserFiles_;
            for (auto& file : combined) file.preferred = false;
            for (auto file : incoming) {
                if (hasExtension(std::filesystem::path(file.relativePath), ".bif")) {
                    file.preferred = true;
                    combined.push_back(std::move(file));
                }
            }
        } else {
            const bool hasKey = std::any_of(incoming.begin(), incoming.end(), [](const auto& file) {
                return hasExtension(std::filesystem::path(file.relativePath), ".key");
            });
            if (!hasKey && weak->archive_.isOpen()) {
                combined = weak->browserFiles_;
                for (auto& file : combined) file.preferred = false;
                for (auto file : incoming) {
                    if (hasExtension(std::filesystem::path(file.relativePath), ".bif")) {
                        file.preferred = true;
                        combined.push_back(std::move(file));
                    }
                }
                appendBifs = true;
            } else {
                combined = incoming;
            }
        }

        std::string keyError;
        const auto key = selectBrowserKey(combined, keyError);
        if (!key) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(keyError), "KEY File Required",
                         wxOK | wxICON_ERROR, weak.get());
            return;
        }

        weak->browserIndexing_ = true;
        weak->Enable(false);
        neobif::KeyBifArchive loaded;
        const auto reader = [](const neobif::BrowserArchiveFile& file,
                               std::uint64_t offset,
                               std::size_t length,
                               std::vector<std::uint8_t>& bytes,
                               std::string& error) {
            return neobrowser::readRetainedFileRange(
                file.sessionId, file.fileId, offset, length, bytes, error);
        };
        bool opened = false;
        std::string openError;
        try {
            const auto yield = [] { emscripten_sleep(0); };
            opened = loaded.openBrowser(*key, combined, reader, yield);
            if (!opened) openError = loaded.lastError();
        } catch (const std::exception& exception) {
            openError = exception.what();
        } catch (...) {
            openError = "An unknown error occurred while indexing the browser archive.";
        }

        if (!weak) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        NeoBIFFrame* frame = weak.get();
        frame->browserIndexing_ = false;
        frame->Enable(true);
        if (frame->browserGeneration_ != generation ||
            frame->browserSelectionRequest_ != request) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        if (!opened) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(openError.empty() ?
                             "Unable to index the selected browser archive." : openError),
                         "Unable to Open Browser Archive",
                         wxOK | wxICON_ERROR, frame);
            return;
        }

        // Keep only the KEY and the exact BIF candidates selected by the
        // completed index. This prevents a previously replaced BIF from
        // competing with its replacement during a later Add BIF operation.
        using BrowserFileKey = std::pair<std::uint32_t, std::uint32_t>;
        std::set<BrowserFileKey> activeFileKeys;
        activeFileKeys.emplace(key->sessionId, key->fileId);
        for (const auto& bif : loaded.bifs()) {
            if (bif.browserBacked && bif.browserSessionId != 0 && bif.browserFileId != 0) {
                activeFileKeys.emplace(bif.browserSessionId, bif.browserFileId);
            }
        }

        bool incomingBifUsed = false;
        for (const auto& file : incoming) {
            if (hasExtension(std::filesystem::path(file.relativePath), ".bif") &&
                activeFileKeys.count(BrowserFileKey{file.sessionId, file.fileId}) != 0u) {
                incomingBifUsed = true;
                break;
            }
        }
        if (appendBifs && !incomingBifUsed) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(
                "None of the selected BIF files matched a BIF referenced by the active chitin.key.",
                "No Matching BIF Files", wxOK | wxICON_WARNING, frame);
            return;
        }

        std::vector<neobif::BrowserArchiveFile> activeFiles;
        std::set<BrowserFileKey> emittedFiles;
        std::map<std::uint32_t, std::vector<std::uint32_t>> retainedIdsBySession;
        for (auto file : combined) {
            const BrowserFileKey fileKey{file.sessionId, file.fileId};
            if (activeFileKeys.count(fileKey) == 0u ||
                !emittedFiles.insert(fileKey).second) {
                continue;
            }
            file.preferred = false;
            retainedIdsBySession[file.sessionId].push_back(file.fileId);
            activeFiles.push_back(std::move(file));
        }
        if (emittedFiles.count(BrowserFileKey{key->sessionId, key->fileId}) == 0u) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("The selected KEY file was lost while reconciling the browser archive session.",
                         "Unable to Open Browser Archive", wxOK | wxICON_ERROR, frame);
            return;
        }

        std::set<std::uint32_t> candidateSessions(
            frame->browserSessions_.begin(), frame->browserSessions_.end());
        candidateSessions.insert(result.sessionId);
        for (const auto& file : combined) candidateSessions.insert(file.sessionId);
        for (const std::uint32_t sessionId : candidateSessions) {
            const auto retained = retainedIdsBySession.find(sessionId);
            if (retained == retainedIdsBySession.end()) {
                neobrowser::releaseRetainedFileSet(sessionId);
            } else {
                neobrowser::retainOnlyRetainedFiles(sessionId, retained->second);
            }
        }

        frame->browserFiles_ = std::move(activeFiles);
        frame->browserSessions_.clear();
        for (const auto& [sessionId, fileIds] : retainedIdsBySession) {
            if (!fileIds.empty()) frame->browserSessions_.push_back(sessionId);
        }
        frame->archive_ = std::move(loaded);
        frame->keyPath_ = std::filesystem::path(key->relativePath);
        frame->supplementaryFiles_.clear();
        ++frame->browserGeneration_;
        frame->keyLabel_->SetLabel(wxui::toWx(key->relativePath));
        frame->SetTitle(appTitle() + " - " +
                        wxui::toWx(frame->keyPath_.filename().string()));
        frame->rebuildTree();
        frame->updateStatus();
        frame->updateCommandState();
    }

    void releaseBrowserSessions() {
        ++browserSelectionRequest_;
        for (const std::uint32_t sessionId : browserSessions_) {
            neobrowser::releaseRetainedFileSet(sessionId);
        }
        browserSessions_.clear();
        browserFiles_.clear();
        ++browserGeneration_;
    }
#endif

    void openArchive(const std::filesystem::path& key,
                     std::vector<std::filesystem::path> supplementary) {
#if !defined(__EMSCRIPTEN__)
        wxBusyCursor busy;
        neobif::KeyBifArchive loaded;
        if (!loaded.open(key, supplementary)) {
            wxMessageBox(wxui::toWx(loaded.lastError()), "Unable to Open Archive",
                         wxOK | wxICON_ERROR, this);
            return;
        }
        archive_ = std::move(loaded);
        keyPath_ = key;
        supplementaryFiles_ = std::move(supplementary);
        keyLabel_->SetLabel(neosettings::pathToWx(keyPath_));
        SetTitle(appTitle() + " - " + neosettings::pathToWx(keyPath_.filename()));
        rebuildTree();
        updateStatus();
        updateCommandState();
#else
        (void)key;
        (void)supplementary;
#endif
    }

    void closeArchive() {
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
        archive_.clear();
        keyPath_.clear();
        supplementaryFiles_.clear();
        filter_->ChangeValue(wxEmptyString);
        tree_->DeleteAllItems();
        details_->ChangeValue(wxEmptyString);
        keyLabel_->SetLabel("No archive open");
        SetTitle(appTitle());
        updateStatus();
        updateCommandState();
    }

    bool resourceMatches(const neobif::ResourceInfo& resource,
                         const neobif::BifInfo& bif,
                         const std::string& filterText) const {
        return filterText.empty() ||
               containsInsensitive(resource.fileName(), filterText) ||
               containsInsensitive(resource.resref, filterText) ||
               containsInsensitive(resource.extension, filterText) ||
               containsInsensitive(neobif::resourceTypeLabel(resource.type), filterText) ||
               containsInsensitive(neobif::hexResourceId(resource.resourceId), filterText) ||
               containsInsensitive(resource.status, filterText) ||
               containsInsensitive(bif.storedPath, filterText);
    }

    void rebuildTree() {
        tree_->Freeze();
        tree_->DeleteAllItems();
        if (!archive_.isOpen()) {
            tree_->Thaw();
            updateCommandState();
            return;
        }

        const std::string filterText = lowerAscii(wxui::toStd(filter_->GetValue()));
        const wxString rootLabel = neosettings::pathToWx(keyPath_.filename()) +
            wxui::toWx(" - " + std::to_string(archive_.resources().size()) +
                       " resources (" + std::to_string(archive_.extractableResourceCount()) +
                       " extractable)");
        const wxTreeItemId root = tree_->AddRoot(rootLabel, -1, -1,
                                                new NodeData(NodeKind::Root));

        std::size_t displayedResources = 0u;
        std::size_t omittedResources = 0u;
        for (const auto& bif : archive_.bifs()) {
            std::vector<std::size_t> visible;
            visible.reserve(std::min(
                bif.resourceIndices.size(),
                kMaximumTreeResourceNodes - std::min(
                    displayedResources, kMaximumTreeResourceNodes)));
            const bool bifMatches = filterText.empty() ||
                                    containsInsensitive(bif.storedPath, filterText) ||
                                    containsInsensitive(bif.resolvedPath.string(), filterText);
            std::size_t matchingResources = 0u;
            for (const std::size_t resourceIndex : bif.resourceIndices) {
                if (resourceIndex >= archive_.resources().size()) continue;
                const auto& resource = archive_.resources()[resourceIndex];
                if (!bifMatches && !resourceMatches(resource, bif, filterText)) continue;
                ++matchingResources;
                if (displayedResources < kMaximumTreeResourceNodes) {
                    visible.push_back(resourceIndex);
                    ++displayedResources;
                } else {
                    ++omittedResources;
                }
            }
            if (matchingResources == 0u) continue;

            std::ostringstream label;
            label << '[' << std::setw(2) << std::setfill('0') << bif.index << "] "
                  << bif.storedPath << " - " << bif.resourceIndices.size() << " resources";
            if (!bif.available) label << " [MISSING]";
            else if (!bif.valid) label << " [INVALID]";
            const wxTreeItemId bifNode = tree_->AppendItem(
                root, wxui::toWx(label.str()), -1, -1,
                new NodeData(NodeKind::Bif, bif.index));

            if (groupByType_) {
                std::map<std::uint16_t, std::vector<std::size_t>> groups;
                for (const std::size_t resourceIndex : visible) {
                    groups[archive_.resources()[resourceIndex].type].push_back(resourceIndex);
                }
                for (const auto& [type, indices] : groups) {
                    const std::string typeLabel = neobif::resourceTypeLabel(type) + " - " +
                                                  std::to_string(indices.size());
                    const wxTreeItemId typeNode = tree_->AppendItem(
                        bifNode, wxui::toWx(typeLabel), -1, -1,
                        new NodeData(NodeKind::Type, bif.index, kNoIndex, type));
                    for (const std::size_t resourceIndex : indices) {
                        appendResourceNode(typeNode, resourceIndex);
                    }
                }
            } else {
                for (const std::size_t resourceIndex : visible) {
                    appendResourceNode(bifNode, resourceIndex);
                }
            }
            if (matchingResources > visible.size()) {
                tree_->AppendItem(
                    bifNode,
                    wxui::toWx(std::to_string(matchingResources - visible.size()) +
                               " matching resources omitted; refine the filter to display them"));
            }
        }
        if (omittedResources != 0u) {
            tree_->AppendItem(
                root,
                wxui::toWx(std::to_string(omittedResources) +
                           " matching resources omitted by the 50,000-node safety limit"));
        }
        tree_->Expand(root);
        tree_->SelectItem(root);
        tree_->Thaw();
        showSelectedDetails();
        updateCommandState();
    }

    void appendResourceNode(const wxTreeItemId& parent, std::size_t resourceIndex) {
        const auto& resource = archive_.resources()[resourceIndex];
        std::ostringstream label;
        label << resource.fileName() << " - " << neobif::formatByteSize(resource.size);
        if (!resource.extractable) label << " [UNAVAILABLE]";
        if (!resource.keyed) label << " [UNINDEXED]";
        tree_->AppendItem(parent, wxui::toWx(label.str()), -1, -1,
                          new NodeData(NodeKind::Resource, resource.bifIndex, resourceIndex,
                                       resource.type));
    }

    NodeData* selectedNodeData() const {
        const wxTreeItemId selected = tree_->GetSelection();
        if (!selected.IsOk()) return nullptr;
        return dynamic_cast<NodeData*>(tree_->GetItemData(selected));
    }

    std::vector<std::size_t> selectedResourceIndices() const {
        const NodeData* data = selectedNodeData();
        if (data == nullptr || !archive_.isOpen()) return {};
        switch (data->kind) {
        case NodeKind::Root: {
            std::vector<std::size_t> indices;
            indices.reserve(archive_.resources().size());
            for (const auto& resource : archive_.resources()) indices.push_back(resource.index);
            return indices;
        }
        case NodeKind::Bif:
            if (data->bifIndex < archive_.bifs().size()) {
                return archive_.bifs()[data->bifIndex].resourceIndices;
            }
            break;
        case NodeKind::Type: {
            std::vector<std::size_t> indices;
            if (data->bifIndex < archive_.bifs().size()) {
                for (const std::size_t index : archive_.bifs()[data->bifIndex].resourceIndices) {
                    if (index < archive_.resources().size() &&
                        archive_.resources()[index].type == data->type) {
                        indices.push_back(index);
                    }
                }
            }
            return indices;
        }
        case NodeKind::Resource:
            if (data->resourceIndex < archive_.resources().size()) {
                return {data->resourceIndex};
            }
            break;
        }
        return {};
    }

    std::vector<std::size_t> allResourceIndices() const {
        std::vector<std::size_t> indices;
        indices.reserve(archive_.resources().size());
        for (const auto& resource : archive_.resources()) indices.push_back(resource.index);
        return indices;
    }

    std::string selectionBaseName(const std::vector<std::size_t>& indices) const {
        const NodeData* data = selectedNodeData();
        if (data != nullptr) {
            if (data->kind == NodeKind::Resource && data->resourceIndex < archive_.resources().size()) {
                return archive_.resources()[data->resourceIndex].resref;
            }
            if (data->kind == NodeKind::Bif && data->bifIndex < archive_.bifs().size()) {
                std::string stored = archive_.bifs()[data->bifIndex].storedPath;
                std::replace(stored.begin(), stored.end(), '\\', '/');
                const auto path = std::filesystem::path(stored);
                return path.stem().string().empty() ? "bif_resources" : path.stem().string();
            }
            if (data->kind == NodeKind::Type) return neobif::resourceTypeExtension(data->type);
        }
        return indices.size() == archive_.resources().size() ? "chitin_resources" : "neobif_selection";
    }

    void extractSelected(bool forceZip) {
        const auto indices = selectedResourceIndices();
        if (indices.empty()) {
            wxMessageBox("Select a resource or archive branch first.", "Nothing Selected",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        extractIndices(indices, selectionBaseName(indices), forceZip);
    }

    void extractAll(bool forceZip) {
        if (!archive_.isOpen()) return;
        const auto indices = allResourceIndices();
        extractIndices(indices, "chitin_resources", forceZip);
    }

    void extractIndices(const std::vector<std::size_t>& indices,
                        std::string baseName,
                        bool forceZip) {
        std::vector<std::size_t> extractable;
        extractable.reserve(indices.size());
        for (const std::size_t index : indices) {
            if (index < archive_.resources().size() && archive_.resources()[index].extractable) {
                extractable.push_back(index);
            }
        }
        if (extractable.empty()) {
            wxMessageBox("The selection contains no resources whose BIF payload is available and valid.",
                         "Nothing to Extract", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (extractable.size() != indices.size()) {
            const std::string message = std::to_string(indices.size() - extractable.size()) +
                " selected resource(s) are unavailable because their BIF is missing, invalid, or out of bounds. They will be omitted.\n\nContinue with " +
                std::to_string(extractable.size()) + " extractable resource(s)?";
            if (wxMessageBox(wxui::toWx(message), "Incomplete Selection",
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
                return;
            }
        }

        const bool singleResource = extractable.size() == 1u &&
                                    indices.size() == 1u;
#if defined(__EMSCRIPTEN__)
        const auto outputPaths = archive_.outputPaths(
            extractable, neobif::ExtractionLayout::BifAndType);
        if (outputPaths.size() != extractable.size()) {
            wxMessageBox("NeoBIF could not construct the browser extraction layout.",
                         "Extraction Failed", wxOK | wxICON_ERROR, this);
            return;
        }

        std::vector<neobrowser::RetainedExportEntry> entries;
        entries.reserve(extractable.size());
        for (std::size_t position = 0; position < extractable.size(); ++position) {
            const auto& resource = archive_.resources()[extractable[position]];
            if (resource.bifIndex >= archive_.bifs().size()) {
                wxMessageBox("A selected resource references an invalid BIF index.",
                             "Extraction Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            const auto& bif = archive_.bifs()[resource.bifIndex];
            if (!bif.browserBacked || bif.browserSessionId == 0 || bif.browserFileId == 0 ||
                outputPaths[position].empty()) {
                wxMessageBox("A selected resource is not backed by an active browser file.",
                             "Extraction Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            entries.push_back(neobrowser::RetainedExportEntry{
                bif.browserSessionId,
                bif.browserFileId,
                resource.offset,
                resource.size,
                outputPaths[position].generic_string()});
        }

        neobrowser::RetainedExportMode mode = neobrowser::RetainedExportMode::DirectDownload;
        std::string outputName;
        if (singleResource && !forceZip) {
            outputName = archive_.resources()[extractable.front()].fileName();
        } else if (forceZip || !neobrowser::retainedDirectoryWriteSupported()) {
            mode = neobrowser::RetainedExportMode::ZipDownload;
            outputName = sanitizeDownloadName(baseName) + ".zip";
        } else {
            mode = neobrowser::RetainedExportMode::Directory;
            outputName = sanitizeDownloadName(baseName);
        }

        const std::uint64_t generation = browserGeneration_;
        const std::size_t requestedCount = entries.size();
        const bool directDownload = mode == neobrowser::RetainedExportMode::DirectDownload;
        wxWeakRef<NeoBIFFrame> weak(this);
        wxui::setStatusText(*this, "Preparing " + std::to_string(requestedCount) +
                                      " browser extraction item(s)...");
        neobrowser::requestExportRetainedFiles(
            mode, outputName, entries,
            [weak, generation, outputName, directDownload](
                neobrowser::RetainedExportResult result) {
                if (!weak || weak->browserGeneration_ != generation) return;
                NeoBIFFrame* frame = weak.get();
                if (!result.error.empty()) {
                    wxMessageBox(wxui::toWx(result.error), "Extraction Failed",
                                 wxOK | wxICON_ERROR, frame);
                    wxui::setStatusText(*frame, "Browser extraction failed");
                    return;
                }
                if (result.cancelled()) {
                    wxui::setStatusText(*frame, "Browser extraction cancelled");
                    return;
                }
                std::ostringstream status;
                if (result.usedDirectory) {
                    status << "Extracted " << result.filesWritten << " resource(s) to the selected directory";
                } else if (directDownload) {
                    status << "Prepared download: " << outputName;
                } else {
                    status << "Prepared " << result.filesWritten << " resource(s) in " << outputName;
                }
                status << " (" << neobif::formatByteSize(result.bytesWritten) << ')';
                wxui::setStatusText(*frame, status.str());
            });
#else
        if (singleResource && !forceZip) {
            const auto& resource = archive_.resources()[extractable.front()];
            const auto selected = wxui::chooseSaveFile(this, "Extract resource",
                                                       kAllFilesWildcard,
                                                       resource.fileName());
            if (!selected) return;
            std::vector<std::uint8_t> bytes;
            std::string error;
            if (!archive_.readResource(resource.index, bytes, error) ||
                !writeExactFile(*selected, bytes, error)) {
                wxMessageBox(wxui::toWx(error), "Extraction Failed",
                             wxOK | wxICON_ERROR, this);
                return;
            }
            wxui::setStatusText(*this, "Extracted " + resource.fileName());
            return;
        }
        if (forceZip) {
            const auto selected = wxui::chooseSaveFile(this, "Save extracted resources as ZIP",
                                                       kZipWildcard,
                                                       sanitizeDownloadName(baseName) + ".zip");
            if (!selected) return;
            std::filesystem::path output = *selected;
            if (output.extension().empty()) output += ".zip";
            std::string error;
            if (!archive_.writeZip(extractable, output,
                                   neobif::ExtractionLayout::BifAndType, error)) {
                wxMessageBox(wxui::toWx(error), "ZIP Extraction Failed",
                             wxOK | wxICON_ERROR, this);
                return;
            }
            wxui::setStatusText(*this, "Wrote " + output.string());
            return;
        }
        wxDirDialog directory(this, "Select extraction directory", wxEmptyString,
                              wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (directory.ShowModal() != wxID_OK) return;
        const auto report = archive_.extractResources(
            extractable, neosettings::pathFromWx(directory.GetPath()),
            neobif::ExtractionLayout::BifAndType, false);
        std::ostringstream message;
        message << "Extracted " << report.written << " resource(s).";
        if (report.skipped != 0u) message << " Skipped " << report.skipped << '.';
        if (report.failed != 0u) message << " Failed " << report.failed << '.';
        wxui::setStatusText(*this, message.str());
        if (report.failed != 0u) {
            std::ostringstream details;
            details << message.str();
            const std::size_t limit = std::min<std::size_t>(report.messages.size(), 20u);
            for (std::size_t i = 0; i < limit; ++i) details << "\n" << report.messages[i];
            wxMessageBox(wxui::toWx(details.str()), "Extraction Completed with Errors",
                         wxOK | wxICON_WARNING, this);
        }
#endif
    }

    static std::string sanitizeDownloadName(std::string value) {
        for (char& ch : value) {
            if (static_cast<unsigned char>(ch) < 0x20u || ch == '/' || ch == '\\' ||
                ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                ch == '>' || ch == '|') ch = '_';
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
        return value.empty() ? "neobif_resources" : value;
    }

    static bool writeExactFile(const std::filesystem::path& output,
                               const std::vector<std::uint8_t>& bytes,
                               std::string& error) {
        error.clear();
        std::error_code ec;
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path(), ec);
        if (ec) {
            error = "Unable to create output directory: " + ec.message();
            return false;
        }
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to create output file";
            return false;
        }
        if (!bytes.empty()) {
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            error = "Unable to write complete output file";
            return false;
        }
        return true;
    }

    void showSelectedDetails() {
        if (!archive_.isOpen()) {
            details_->ChangeValue(wxEmptyString);
            return;
        }
        const NodeData* data = selectedNodeData();
        if (data == nullptr) return;
        std::ostringstream text;
        if (data->kind == NodeKind::Root) {
            text << "KEY archive\n"
                 << "===========\n\n"
                 << "Path: " << archive_.keyPath().string() << '\n'
                 << "Build year: " << formatBuildYear(archive_.buildYear()) << '\n'
                 << "Build day-of-year: " << archive_.buildDay() << '\n'
                 << "BIF entries: " << archive_.bifs().size() << '\n'
                 << "Missing BIFs: " << archive_.missingBifCount() << '\n'
                 << "Resources: " << archive_.resources().size() << '\n'
                 << "Extractable: " << archive_.extractableResourceCount() << '\n'
                 << "Issues: " << archive_.issues().size() << "\n\n";
            if (!archive_.issues().empty()) {
                text << "Issues\n------\n";
                const std::size_t limit = std::min<std::size_t>(archive_.issues().size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    const auto& issue = archive_.issues()[i];
                    text << (issue.severity == neobif::IssueSeverity::Error ? "ERROR" : "WARN")
                         << ": " << issue.message;
                    if (issue.bifIndex) text << " [BIF " << *issue.bifIndex << ']';
                    if (issue.resourceId) text << " [" << neobif::hexResourceId(*issue.resourceId) << ']';
                    text << '\n';
                }
                if (archive_.issues().size() > limit) {
                    text << "... " << (archive_.issues().size() - limit) << " additional issues\n";
                }
            }
        } else if (data->kind == NodeKind::Bif && data->bifIndex < archive_.bifs().size()) {
            const auto& bif = archive_.bifs()[data->bifIndex];
            text << "BIF entry " << bif.index << "\n"
                 << "===========" << std::string(std::to_string(bif.index).size(), '=') << "\n\n"
                 << "Stored path: " << bif.storedPath << '\n'
                 << "Resolved path: " << (bif.resolvedPath.empty() ? "<missing>" : bif.resolvedPath.string()) << '\n'
                 << "Drive flags: 0x" << std::hex << std::uppercase << bif.driveFlags << std::dec << '\n'
                 << "Declared file size: " << bif.declaredFileSize << " ("
                 << neobif::formatByteSize(bif.declaredFileSize) << ")\n"
                 << "Actual file size: " << bif.actualFileSize << " ("
                 << neobif::formatByteSize(bif.actualFileSize) << ")\n"
                 << "Available: " << yesNo(bif.available) << '\n'
                 << "Valid BIFF V1 archive: " << yesNo(bif.valid) << '\n'
                 << "Variable resources: " << bif.variableResourceCount << '\n'
                 << "Fixed resources: " << bif.fixedResourceCount << '\n'
                 << "Variable table offset: " << bif.tableOffset << '\n'
                 << "KEY-linked and orphan resources shown: " << bif.resourceIndices.size() << "\n";
            if (!bif.messages.empty()) {
                text << "\nMessages\n--------\n";
                for (const auto& message : bif.messages) text << message << '\n';
            }
        } else if (data->kind == NodeKind::Type) {
            const auto indices = selectedResourceIndices();
            text << "Resource type\n=============\n\n"
                 << "Type: " << neobif::resourceTypeLabel(data->type) << '\n'
                 << "BIF index: " << data->bifIndex << '\n'
                 << "Resources: " << indices.size() << '\n';
            std::size_t extractable = 0;
            std::uint64_t bytes = 0;
            for (const std::size_t index : indices) {
                const auto& resource = archive_.resources()[index];
                if (resource.extractable) {
                    ++extractable;
                    bytes += resource.size;
                }
            }
            text << "Extractable: " << extractable << '\n'
                 << "Extractable bytes: " << bytes << " (" << neobif::formatByteSize(bytes) << ")\n";
        } else if (data->kind == NodeKind::Resource &&
                   data->resourceIndex < archive_.resources().size()) {
            const auto& resource = archive_.resources()[data->resourceIndex];
            const auto* bif = resource.bifIndex < archive_.bifs().size()
                ? &archive_.bifs()[resource.bifIndex] : nullptr;
            text << "Resource\n========\n\n"
                 << "Name: " << resource.resref << '\n'
                 << "Output filename: " << resource.fileName() << '\n'
                 << "Type: " << neobif::resourceTypeLabel(resource.type) << '\n'
                 << "Resource ID: " << neobif::hexResourceId(resource.resourceId) << '\n'
                 << "BIF index: " << resource.bifIndex << '\n'
                 << "20-bit table index: " << resource.tableIndex << '\n'
                 << "KotOR 14-bit loader index: " << resource.engineIndex << '\n'
                 << "Payload offset: " << resource.offset << '\n'
                 << "Payload size: " << resource.size << " (" << neobif::formatByteSize(resource.size) << ")\n"
                 << "Indexed by KEY: " << yesNo(resource.keyed) << '\n'
                 << "BIF entry found: " << yesNo(resource.bifEntryFound) << '\n'
                 << "ID matches: " << yesNo(resource.idMatches) << '\n'
                 << "Type matches: " << yesNo(resource.typeMatches) << '\n'
                 << "Bounds valid: " << yesNo(resource.boundsValid) << '\n'
                 << "Extractable: " << yesNo(resource.extractable) << '\n'
                 << "Status: " << resource.status << '\n';
            if (bif != nullptr) {
                text << "BIF stored path: " << bif->storedPath << '\n'
                     << "BIF resolved path: "
                     << (bif->resolvedPath.empty() ? "<missing>" : bif->resolvedPath.string()) << '\n';
            }
        }
        details_->ChangeValue(wxui::toWx(text.str()));
        details_->SetInsertionPoint(0);
    }

    void updateStatus() {
        if (!archive_.isOpen()) {
            wxui::setStatusText(*this, "Open chitin.key or scan a KotOR installation", 0);
            wxui::setStatusText(*this, "No archive", 1);
            return;
        }
        std::ostringstream left;
        left << archive_.resources().size() << " resources; "
             << archive_.extractableResourceCount() << " extractable; "
             << archive_.missingBifCount() << " missing BIFs";
        wxui::setStatusText(*this, left.str(), 0);
        std::ostringstream right;
        right << archive_.issues().size() << " issues | " << archive_.bifs().size() << " BIF entries";
        wxui::setStatusText(*this, right.str(), 1);
    }

    void updateCommandState() {
        const bool open = archive_.isOpen();
        const bool selection = open && selectedNodeData() != nullptr;
        if (extractButton_ != nullptr) extractButton_->Enable(selection);
        if (GetMenuBar() != nullptr) {
            GetMenuBar()->Enable(ID_AddBifs, open);
            GetMenuBar()->Enable(ID_CloseArchive, open);
            GetMenuBar()->Enable(ID_ExtractSelected, selection);
            GetMenuBar()->Enable(ID_ExtractAll, open);
            GetMenuBar()->Enable(ID_ZipSelected, selection);
            GetMenuBar()->Enable(ID_ZipAll, open);
            GetMenuBar()->Enable(ID_ExpandAll, open);
            GetMenuBar()->Enable(ID_CollapseAll, open);
        }
    }

    void applyDarkMode() {
        if (darkModeItem_ != nullptr) darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this, darkMode_);
        if (tree_ != nullptr) wxui::applyTreeTheme(*tree_, darkMode_);
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }

    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }

    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void showAbout() {
        const std::string message = std::string("NeoBIF v") + neobif::kVersion +
            "\n\nKotOR chitin.key and BIFF V1 archive browser and extractor."
            "\n\nNeoBIF indexes resources from chitin.key, validates the referenced BIF tables, and extracts individual resources, archive branches, directory trees, or ZIP packages.";
        wxMessageBox(wxui::toWx(message), "About NeoBIF",
                     wxOK | wxICON_INFORMATION, this);
    }
};

class NeoBIFApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName("NeoBIF");
        SetVendorName("Neo Tools");
        wxInitAllImageHandlers();
        auto* frame = new NeoBIFFrame();
        frame->Show();
        if (argc > 1) {
            const std::filesystem::path path = neosettings::pathFromWx(wxString(argv[1]));
            frame->CallAfter([frame, path] { frame->openPath(path); });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoBIFApp);
