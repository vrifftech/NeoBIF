#include "BrowserPanel.hpp"
#include "core/ResourceDocument.hpp"
#include <wx/wrapsizer.h>
#include "core/ArchiveExport.hpp"
#include "core/KeyBifArchive.hpp"
#include "core/LooseArchiveCatalog.hpp"
#include "core/ResourceQuery.hpp"
#include "core/Version.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include <neoshared/PathUtf8.hpp>

#if defined(__EMSCRIPTEN__)
#include "NeoBrowserFiles.hpp"
#include <emscripten.h>
static_assert(neobrowser::kBrowserFileApiVersion >= 11u,
              "NeoBIF requires NeoShared browser API 11 for safe extraction conflict handling.");
#endif

#include <wx/app.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/weakref.h>
#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/config.h>
#include <wx/utils.h>
#if !defined(__EMSCRIPTEN__)
#include <atomic>
#include <mutex>
#include <thread>
#endif

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


std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool hasExtension(const std::filesystem::path& path, const std::string& extension) {
    return lowerAscii(neoshared::pathToUtf8(path.extension())) == lowerAscii(extension);
}

using neobif::ResourceSource;
using neobif::ResourceSelection;

enum class NodeKind {
    Root,
    Bif,
    LooseRoot,
    Directory,
    LooseArchive,
    Type,
    Resource,
    Page,
};

class NodeData final : public wxTreeItemData {
public:
    NodeData(NodeKind nodeKind,
             std::size_t owner = kNoIndex,
             std::size_t resource = kNoIndex,
             std::uint16_t resourceType = 0u,
             ResourceSource resourceSource = ResourceSource::KeyBif,
             std::string directory = {})
        : kind(nodeKind), source(resourceSource), ownerIndex(owner),
          resourceIndex(resource), type(resourceType),
          directoryPath(std::move(directory)) {}

    NodeKind kind;
    ResourceSource source;
    std::size_t ownerIndex;
    std::size_t resourceIndex;
    std::uint16_t type;
    std::string directoryPath;
    std::shared_ptr<const std::vector<std::size_t>> lazyResources;
    bool populated{};
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
    ID_ExtractBranch,
    ID_ZipBranch,
    ID_CopyResref,
    ID_CopySource,
    ID_LocateSource,
    ID_Rescan,
    ID_FindResource,
    ID_ExportByType,
    ID_OpenResource=neobif::ui::kOpenResourceCommandId, ID_ModuleAbout, ID_ModuleExit,
};

struct NeoBIFGuiProbe;
class NeoBIFPanelImpl final : public neobif::ui::BrowserPanel {
    friend struct NeoBIFGuiProbe;
public:
    NeoBIFPanelImpl(wxWindow* parent, neomodules::Context context = {})
        : BrowserPanel(parent,std::move(context)),
          filterTimer_(this, ID_FilterTimer),
          groupByType_(settings_.readBool("View/GroupByType", true)),
          darkMode_(wxui::readDarkMode("NeoBIF")) {
        buildMenus();
        buildUi();
        bindEvents();
        createModuleStatusBar(2);
        int widths[] = {-3, -2};
        moduleStatusBar()->SetStatusWidths(2, widths);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        updateStatus();
    }

    ~NeoBIFPanelImpl() override {
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
    }

    void openPath(const std::filesystem::path& path) override {
        if(jobRunning_)return;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec) && !ec) {
            scanDirectory(path);
            return;
        }
        if (hasExtension(path, ".key")) {
            openArchive(path, {}, path.parent_path());
            return;
        }
        wxMessageBox("NeoBIF expects a chitin.key file or a game directory.",
                     "Unsupported Input", wxOK | wxICON_ERROR, this);
    }

    void setOpenHandler(neobif::ui::OpenHandler handler) override {openHandler_=std::move(handler);}
    std::size_t resourceCount() const override {return archive_.resources().size()+looseArchives_.resources().size();}
    bool canClose() override {
#if !defined(__EMSCRIPTEN__)
        if(jobRunning_){cancelJob_.store(true);return false;}
#else
        if(jobRunning_||browserIndexing_)return false;
#endif
        return true;
    }
    void setAppearance(bool dark,double scale) override {darkMode_=dark;fontScale_=scale;applyDarkMode();}
    bool openResource(const ResourceSelection& selected) override {
        return openResource(selected, {});
    }
    std::vector<neobif::ui::OpenTarget> openTargets(const ResourceSelection& selected) const override {
        if (!canOpen(selected) || !openHandler_.targets || !openHandler_.openWith) return {};
        const auto type=selected.source==ResourceSource::KeyBif ?
            archive_.resources()[selected.resourceIndex].type : looseArchives_.resources()[selected.resourceIndex].type;
        return openHandler_.targets(type);
    }
    bool openResource(const ResourceSelection& selected, const std::string& editor) override {
        if(jobRunning_||!canOpen(selected))return false;
        if (!editor.empty()) {
            const auto choices=openTargets(selected);
            if (std::none_of(choices.begin(),choices.end(),[&](const auto& c){return c.id==editor;})) return false;
        }
        try {
            neoshared::ResourceDocument document;
#if !defined(__EMSCRIPTEN__)
            if(!runJob("Opening resource",[&](const neobif::JobControl& job){
                job.check();document=neobif::readResourceDocument(archive_,looseArchives_,selected);job.check();
            }))return false;
#else
            document=neobif::readResourceDocument(archive_,looseArchives_,selected);
#endif
            if (editor.empty()) openHandler_.open(std::move(document));
            else openHandler_.openWith(std::move(document),editor);
            return true;
        }catch(const std::exception& ex){wxui::showError(this,ex);return false;}
    }

    void setArchiveOpenHandler(ArchiveOpenHandler handler) override {archiveOpen_=std::move(handler);}
    std::vector<std::filesystem::path> sourcePaths() const override {return allInputPaths();}
    bool openDiscoveredArchive(std::size_t index) override {
        if(jobRunning_||!archiveOpen_||index>=looseArchives_.archives().size())return false;
        const auto& entry=looseArchives_.archives()[index];
        if(!entry.valid||entry.browserBacked)return false;
        if(!neobif::unchangedInput(entry.resolvedPath,entry.snapshot))
            throw std::runtime_error("Archive changed since NeoBIF indexed it. Rescan before opening.");
        archiveOpen_(entry.resolvedPath,allInputPaths());return true;
    }
    void appendOpenCommands(wxMenu& menu) override {
        contextOpenSelections_.clear();
        contextOpenTargets_.clear();

        wxArrayTreeItemIds nodes;
        tree_->GetSelections(nodes);
        const auto* node = nodes.size() == 1
            ? static_cast<NodeData*>(tree_->GetItemData(nodes.front()))
            : nullptr;
        if (node && node->kind == NodeKind::LooseArchive && archiveOpen_) {
            const auto index = node->ownerIndex;
            const bool enabled = !jobRunning_ &&
                index < looseArchives_.archives().size() &&
                looseArchives_.archives()[index].valid &&
                !looseArchives_.archives()[index].browserBacked;
            if (enabled) {
                menu.Append(neobif::ui::kOpenArchiveEditorCommandId, "Open with NeoERF");
                menu.AppendSeparator();
            }
            return;
        }
        if (!selectedNodesAreResources() || !openHandler_.open) return;

        contextOpenSelections_ = selectedResources();
        const auto& selections = contextOpenSelections_;
        const bool enabled = !jobRunning_ && !selections.empty() &&
            std::all_of(selections.begin(), selections.end(),
                        [this](const auto& resource) { return canOpen(resource); });
        if (!enabled) return;

        contextOpenTargets_ = openTargets(selections.front());
        for (const auto& selected : selections) {
            const auto available = openTargets(selected);
            contextOpenTargets_.erase(
                std::remove_if(contextOpenTargets_.begin(), contextOpenTargets_.end(),
                    [&](const auto& choice) {
                        return std::none_of(available.begin(), available.end(),
                            [&](const auto& other) { return other.id == choice.id; });
                    }),
                contextOpenTargets_.end());
        }
        // DLG/JRL expose the requested two explicit commands, in default-first order.
        // Mixed resource selections can always use the ordinary default-per-resource Open.
        if (contextOpenTargets_.size() < 2) menu.Append(ID_OpenResource, "&Open");
        if (contextOpenTargets_.size() > 1 ||
            (contextOpenTargets_.size() == 1 && selections.size() > 1)) {
            const auto count = std::min(
                contextOpenTargets_.size(),
                static_cast<std::size_t>(neobif::ui::kOpenWithCommandCapacity));
            for (std::size_t i = 0; i < count; ++i) {
                menu.Append(neobif::ui::kOpenWithFirstCommandId + static_cast<int>(i),
                            wxui::toWx(contextOpenTargets_[i].label));
            }
        }
        menu.AppendSeparator();
    }

private:
    ArchiveOpenHandler archiveOpen_;
    neobif::ui::OpenHandler openHandler_;
    std::vector<neobif::ui::OpenTarget> contextOpenTargets_;
    std::vector<ResourceSelection> contextOpenSelections_;
    bool canOpen(const ResourceSelection& selected) const {
        if(!openHandler_.supports||!openHandler_.open||!resourceIsExtractable(selected))return false;
        const auto type=selected.source==ResourceSource::KeyBif?
            archive_.resources()[selected.resourceIndex].type:looseArchives_.resources()[selected.resourceIndex].type;
        return openHandler_.supports(type);
    }
    neobif::KeyBifArchive archive_;
    neobif::LooseArchiveCatalog looseArchives_;
    std::filesystem::path keyPath_;
    std::filesystem::path scanRoot_;
    std::vector<std::filesystem::path> supplementaryFiles_;
#if defined(__EMSCRIPTEN__)
    std::vector<neobif::BrowserArchiveFile> browserFiles_;
    std::vector<std::uint32_t> browserSessions_;
    std::uint64_t browserGeneration_{};
    std::uint64_t browserSelectionRequest_{};
    bool browserIndexing_{};
#endif
    wxSearchCtrl* filter_{nullptr};
    wxStaticText* searchCount_{nullptr};
    wxSplitterWindow* splitter_{nullptr};
    wxTreeCtrl* tree_{nullptr};
    wxTextCtrl* details_{nullptr};
    wxStaticText* keyLabel_{nullptr};
    wxButton* openDirectoryButton_{nullptr};
    wxButton* openFilesButton_{nullptr};
    wxButton* addBifsButton_{nullptr};
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
    bool rebuilding_{};
    bool jobRunning_{};
    std::set<std::string> expandedNodes_,selectedNodes_;
    std::vector<bool> visibleBif_,visibleLoose_;
    std::map<std::size_t,std::filesystem::path> explicitRelocations_;
#if !defined(__EMSCRIPTEN__)
    std::atomic<bool> cancelJob_{false};
#endif

    static wxString appTitle() {
        return wxui::toWx(std::string("NeoBIF v") + neobif::kVersion);
    }

    static std::string countText(
        std::size_t count, const char* singular, const char* plural) {
        return std::to_string(count) + " " + (count == 1u ? singular : plural);
    }

    static std::string resourceCountText(std::size_t count) {
        return countText(count, "resource", "resources");
    }

    static wxString emptyStateText() {
        return "Open a KotOR game directory or chitin.key file to browse its resources.\n\n"
               "Select a resource or branch, then use Extract Selection or save it as a ZIP.";
    }

    void updateSessionLabel() {
        if (keyLabel_ == nullptr) return;
        if (!archive_.isOpen()) {
            keyLabel_->SetLabel("No game session open");
            keyLabel_->UnsetToolTip();
            keyLabel_->GetParent()->Layout();
            return;
        }
        const std::filesystem::path displayPath = !scanRoot_.empty() ? scanRoot_ : keyPath_;
        const wxString path = neosettings::pathToWx(displayPath);
        keyLabel_->SetLabel(wxString(!scanRoot_.empty() ? "Game directory: " : "Archive: ") + path);
        keyLabel_->SetToolTip(path);
        keyLabel_->GetParent()->Layout();
    }

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_ScanDirectory, "Open &Game Directory...");
#if !defined(__EMSCRIPTEN__)
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file,
            [this](const neogames::SavedGameDirectory& directory) {
                scanDirectory(directory.path);
            },
            neogames::GameDirectoryGameIds{"kotor", "kotor2"},
            "Open Saved &KotOR Directory");
#endif
        file->Append(ID_OpenArchive, "Open &chitin.key...\tCtrl+O");
        file->Append(ID_AddBifs, "Add / &Relocate BIF Files...");
        file->AppendSeparator();
#if defined(__EMSCRIPTEN__)
        file->Append(ID_Rescan, "Reopen &Game Directory...\tF5");
#else
        file->Append(ID_Rescan, "&Rescan Current Session\tF5");
#endif
        file->Append(ID_CloseArchive, "&Close Session");
        file->AppendSeparator();
        if(!context_.embedded)file->Append(ID_ModuleExit, "E&xit\tCtrl+Q");

        auto* extract = new wxMenu;
        extract->Append(ID_ExtractSelected, "Extract &Selection...\tCtrl+E",
            "Extract resources represented by the current tree selection; Search narrows branch selections");
        extract->Append(ID_ExtractBranch,"Extract Entire Selection (ignore &Search)...",
            "Extract the complete selected branches without applying Search");
        extract->Append(ID_ExtractAll, "Extract &All Resources...");
        extract->AppendSeparator();
        extract->Append(ID_ZipSelected, "Save Selection as &ZIP...",
            "Save resources represented by the current tree selection; Search narrows branch selections");
        extract->Append(ID_ZipBranch,"Save Entire Selection as ZIP (ignore Search)...",
            "Save the complete selected branches without applying Search");
        extract->Append(ID_ZipAll, "Save All Resources as Z&IP...");
        extract->AppendSeparator();
        extract->Append(ID_ExportByType, "Save File &Type as ZIP...",
            "Choose a resource type and export all its entries from the open game session to a ZIP");

        auto* view = new wxMenu;
        view->Append(ID_FindResource, context_.embedded?"&Find resources...\tCtrl+Shift+F":"&Find resources...\tCtrl+F");
        view->AppendSeparator();
        groupByTypeItem_ = view->AppendCheckItem(ID_GroupByType, "Group Resources by &Type");
        groupByTypeItem_->Check(groupByType_);
        view->AppendSeparator();
        view->Append(ID_ExpandAll, "&Expand Archive/Type Branches");
        view->Append(ID_CollapseAll, "&Collapse All");
        view->AppendSeparator();
        if(!context_.embedded) {
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        darkModeItem_->Check(darkMode_);
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        }

        auto* help = new wxMenu;
        help->Append(ID_ModuleAbout, "&About NeoBIF");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(extract, "&Extract");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        setModuleMenus(bar);
    }

    void buildUi() {
        auto* root = new wxPanel(this);
        auto* outer = new wxBoxSizer(wxVERTICAL);

        auto* actionRow = new wxWrapSizer(wxHORIZONTAL);
        openDirectoryButton_ = new wxButton(root, ID_ScanDirectory, "Open Game Directory...");
        openFilesButton_ = new wxButton(root, ID_OpenArchive, "Open chitin.key...");
        addBifsButton_ = new wxButton(root, ID_AddBifs, "Add / Relocate BIF Files...");
        extractButton_ = new wxButton(root, ID_ExtractSelected, "Extract Selection...");
        openDirectoryButton_->SetToolTip(
            "Recommended: select a KotOR installation to index chitin.key, its BIF files, and game archives.");
        openFilesButton_->SetToolTip(
            "Open chitin.key directly. Relocated BIF files can be selected in the same operation.");
        addBifsButton_->SetToolTip(
            "Associate selected BIF files with entries from the currently open chitin.key.");
        extractButton_->SetToolTip(
            "Extract the resources represented by the current tree selection. Search narrows branch selections.");
        actionRow->Add(openDirectoryButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(openFilesButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(addBifsButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(extractButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        outer->Add(actionRow, 0, wxEXPAND | wxALL, FromDIP(8));

        keyLabel_ = new wxStaticText(root, wxID_ANY, "No game session open",
                                     wxDefaultPosition, wxDefaultSize,
                                     wxST_ELLIPSIZE_MIDDLE);
        keyLabel_->SetName("Current game session");
        keyLabel_->SetMinSize(FromDIP(wxSize(1, -1)));
        outer->Add(keyLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* filterBlock = new wxBoxSizer(wxVERTICAL);
        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(root, wxID_ANY, "Search:"), 0,
                       wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filter_ = new wxSearchCtrl(root, wxID_ANY);
        filter_->SetName("Search resources");
        filter_->SetDescriptiveText("Resource name, file type, source, ID, or status");
        filter_->SetToolTip("Search names, source paths, IDs or status. File types: .tga, tga, *.tga. "
                            "Filename wildcards: p_*.tpc. Ctrl+F focuses Search; Escape clears it.");
        filter_->ShowCancelButton(true);
        filterRow->Add(filter_, 1, wxEXPAND);
        filterBlock->Add(filterRow, 0, wxEXPAND);
        searchCount_ = new wxStaticText(root, wxID_ANY, "No session");
        searchCount_->SetName("Search result count");
        filterBlock->Add(searchCount_, 0, wxALIGN_RIGHT | wxTOP, FromDIP(3));
        outer->Add(filterBlock, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        splitter_ = new wxSplitterWindow(root, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
        tree_ = new wxTreeCtrl(splitter_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_MULTIPLE |
                                   wxTR_FULL_ROW_HIGHLIGHT);
        details_ = new wxTextCtrl(splitter_, wxID_ANY, emptyStateText(),
                                  wxDefaultPosition,
                                  wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP |
                                      wxBORDER_NONE);
        details_->SetName("Selection details");
        const bool compactLayout = context_.compact;
        const std::string splitterSetting = compactLayout
            ? "View/CompactSplitterRatio" : "View/SplitterRatio";
        const double defaultRatio = compactLayout ? 0.8 : 0.57;
        const double splitterRatio = std::clamp(
            settings_.readDouble(splitterSetting, defaultRatio), 0.2, 0.88);
        if(compactLayout) {
            splitter_->SplitHorizontally(tree_,details_,FromDIP(460));
            splitter_->SetMinimumPaneSize(FromDIP(75));
        } else {
            splitter_->SplitVertically(tree_,details_,FromDIP(620));
            splitter_->SetMinimumPaneSize(FromDIP(260));
        }
        splitter_->SetSashGravity(splitterRatio);
        splitter_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED,
            [this, splitterSetting, compactLayout](wxSplitterEvent& event) {
                const wxSize size = splitter_->GetClientSize();
                const int span = compactLayout ? size.GetHeight() : size.GetWidth();
                if (span > 0) {
                    const double ratio = std::clamp(
                        static_cast<double>(event.GetSashPosition()) /
                            static_cast<double>(span),
                        0.2, 0.88);
                    splitter_->SetSashGravity(ratio);
                    settings_.writeDouble(splitterSetting, ratio);
                }
                event.Skip();
            });
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        CallAfter([weak, splitterRatio, compactLayout]() {
            if (!weak || weak->splitter_ == nullptr || !weak->splitter_->IsSplit()) return;
            const wxSize size = weak->splitter_->GetClientSize();
            const int span = compactLayout ? size.GetHeight() : size.GetWidth();
            if (span > 0) {
                weak->splitter_->SetSashPosition(
                    static_cast<int>(static_cast<double>(span) * splitterRatio));
            }
        });
        tree_->SetName("NeoBIF resource tree");
        outer->Add(splitter_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        root->SetSizer(outer);
        auto* frameSizer = new wxBoxSizer(wxVERTICAL);
        frameSizer->Add(root, 1, wxEXPAND);
        SetSizer(frameSizer);
        updateCommandState();
    }

    void bindEvents() {
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
            const auto* node=selectedNodeData();if(!node||node->kind!=NodeKind::LooseArchive)return;
            try{openDiscoveredArchive(node->ownerIndex);}catch(const std::exception& ex){wxui::showError(this,ex);}
        },neobif::ui::kOpenArchiveEditorCommandId);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeArchive(); }, ID_CloseArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, ID_ModuleExit);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
#if !defined(__EMSCRIPTEN__)
            if(archive_.isOpen())openArchive(keyPath_,supplementaryFiles_,scanRoot_);
#else
            requestScanDirectory();
#endif
        },ID_Rescan);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){extractSelected(false,false);},ID_ExtractBranch);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){extractSelected(true,false);},ID_ZipBranch);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){copySelection(false);},ID_CopyResref);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){copySelection(true);},ID_CopySource);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){locateSource();},ID_LocateSource);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(false); }, ID_ExtractAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(true); }, ID_ZipSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(true); }, ID_ZipAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { exportByFileType(); }, ID_ExportByType);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            filter_->SetFocus();
            filter_->SelectAll();
        }, ID_FindResource);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { expandStructure(tree_->GetRootItem()); }, ID_ExpandAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            tree_->CollapseAll();
            if (tree_->GetRootItem().IsOk()) tree_->Expand(tree_->GetRootItem());
        }, ID_CollapseAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            groupByType_ = event.IsChecked();
            settings_.writeBool("View/GroupByType", groupByType_);
            rebuildTree();
        }, ID_GroupByType);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            darkMode_ = event.IsChecked();
            wxui::writeDarkMode("NeoBIF", darkMode_);
            applyDarkMode();
        }, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, ID_ModuleAbout);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
            const auto selections=selectedResources();
            for(const auto& resource:selections)if(canOpen(resource))openResource(resource);
        },ID_OpenResource);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){
            const auto index=static_cast<std::size_t>(event.GetId()-neobif::ui::kOpenWithFirstCommandId);
            if(index>=contextOpenTargets_.size())return;
            const auto editor=contextOpenTargets_[index].id;
            const auto selections=contextOpenSelections_;
            for(const auto& resource:selections)openResource(resource,editor);
        },neobif::ui::kOpenWithFirstCommandId,
          neobif::ui::kOpenWithFirstCommandId+neobif::ui::kOpenWithCommandCapacity-1);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { rebuildTree(); }, ID_FilterTimer);
        filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filterTimer_.StartOnce(180); });
        filter_->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [this](wxCommandEvent&) { clearSearch(); });
        filter_->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, [this](wxCommandEvent&) { flushSearch(); });
        filter_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { flushSearch(); });
        filter_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_ESCAPE && !filter_->GetValue().empty()) clearSearch();
            else if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) flushSearch();
            else event.Skip();
        });
        tree_->Bind(wxEVT_TREE_ITEM_EXPANDING,[this](wxTreeEvent& event){populateNode(event.GetItem());});
        tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
            if(rebuilding_)return;
            selectedNodes_.clear();
            showSelectedDetails();
            updateCommandState();
        });
        tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& event) {
            const wxTreeItemId item = event.GetItem();
            if (!item.IsOk()) return;
            tree_->UnselectAll();tree_->SelectItem(item);
            const auto* archiveNode=static_cast<NodeData*>(tree_->GetItemData(item));
            if(archiveNode&&archiveNode->kind==NodeKind::LooseArchive&&archiveOpen_) {
                try{openDiscoveredArchive(archiveNode->ownerIndex);}catch(const std::exception& ex){wxui::showError(this,ex);}return;
            }
            if (tree_->ItemHasChildren(item)) {
                if (tree_->IsExpanded(item)) tree_->Collapse(item);
                else tree_->Expand(item);
                return;
            }
            const NodeData* data = selectedNodeData();
            if (data != nullptr && data->kind == NodeKind::Resource) {
                const ResourceSelection selection{data->source,data->resourceIndex};
                if(canOpen(selection)) openResource(selection);
                else extractSelected(false);
            }
        });
        tree_->Bind(wxEVT_TREE_ITEM_MENU, &NeoBIFPanelImpl::onTreeItemMenu, this);
    }

    void onTreeItemMenu(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        if (!item.IsOk()) return;

        if (!tree_->IsSelected(item)) {
            tree_->UnselectAll();
            tree_->SelectItem(item);
        }
        showSelectedDetails();
        updateCommandState();

        const NodeData* data = selectedNodeData();
        if (data == nullptr) return;

        const auto selections = selectedResources();
        const auto entireSelections = selectedResources(false);
        const auto hasExtractable = [this](const auto& resources) {
            return std::any_of(resources.begin(), resources.end(),
                [this](const ResourceSelection& resource) {
                    return resourceIsExtractable(resource);
                });
        };

        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        const auto sourcePaths = selectedSourceArchivePaths();
        const bool multipleNodes = selectedItems.size() > 1;
        const bool resourceNodes = selectedNodesAreResources();
        const bool filterActive = !filter_->GetValue().empty();
        const bool scopeNarrowed = selections.size() < entireSelections.size();
        bool branchNode = false;
        for (const auto& selectedItem : selectedItems) {
            const auto* selectedData =
                dynamic_cast<NodeData*>(tree_->GetItemData(selectedItem));
            if (selectedData != nullptr && selectedData->kind != NodeKind::Resource &&
                selectedData->kind != NodeKind::Page) {
                branchNode = true;
                break;
            }
        }

        wxString extractLabel = "Extract Selection...";
        wxString zipLabel = "Save Selection as ZIP...";
        wxString entireLabel = "Extract Entire Selection...";
        wxString entireZipLabel = "Save Entire Selection as ZIP...";
        switch (data->kind) {
        case NodeKind::Root:
            extractLabel = "Extract All Resources...";
            zipLabel = "Save All Resources as ZIP...";
            entireLabel = extractLabel;
            entireZipLabel = zipLabel;
            break;
        case NodeKind::Bif:
            extractLabel = "Extract BIF...";
            zipLabel = "Save BIF as ZIP...";
            entireLabel = "Extract Entire BIF...";
            entireZipLabel = "Save Entire BIF as ZIP...";
            break;
        case NodeKind::LooseRoot:
            extractLabel = "Extract Game Archives...";
            zipLabel = "Save Game Archives as ZIP...";
            entireLabel = "Extract All Game Archives...";
            entireZipLabel = "Save All Game Archives as ZIP...";
            break;
        case NodeKind::Directory:
            extractLabel = "Extract Archive Folder...";
            zipLabel = "Save Archive Folder as ZIP...";
            entireLabel = "Extract Entire Archive Folder...";
            entireZipLabel = "Save Entire Archive Folder as ZIP...";
            break;
        case NodeKind::LooseArchive:
            extractLabel = "Extract Archive...";
            zipLabel = "Save Archive as ZIP...";
            entireLabel = "Extract Entire Archive...";
            entireZipLabel = "Save Entire Archive as ZIP...";
            break;
        case NodeKind::Type:
            extractLabel = "Extract Resource Type...";
            zipLabel = "Save Resource Type as ZIP...";
            entireLabel = "Extract Entire Resource Type...";
            entireZipLabel = "Save Entire Resource Type as ZIP...";
            break;
        case NodeKind::Page:
            extractLabel = "Extract Page...";
            zipLabel = "Save Page as ZIP...";
            break;
        case NodeKind::Resource:
            extractLabel = "Extract Resource...";
            zipLabel = "Save Resource as ZIP...";
            break;
        }

        if (multipleNodes && !resourceNodes) {
            entireLabel = "Extract Entire Selection...";
            entireZipLabel = "Save Entire Selection as ZIP...";
        }

        if (resourceNodes) {
            extractLabel = selections.size() == 1
                ? "Extract Resource..." : "Extract Selected Resources...";
            zipLabel = selections.size() == 1
                ? "Save Resource as ZIP..." : "Save Selected Resources as ZIP...";
        } else if (filterActive && scopeNarrowed) {
            extractLabel = "Extract Matching Resources...";
            zipLabel = "Save Matching Resources as ZIP...";
        } else if (multipleNodes) {
            extractLabel = "Extract Selected Items...";
            zipLabel = "Save Selected Items as ZIP...";
        }

        wxMenu menu;
        appendOpenCommands(menu);
        menu.Append(ID_ExtractSelected, extractLabel)->Enable(hasExtractable(selections));
        menu.Append(ID_ZipSelected, zipLabel)->Enable(hasExtractable(selections));

        // Without a Search filter, these would duplicate the extraction commands above.
        if (filterActive && scopeNarrowed && branchNode) {
            menu.AppendSeparator();
            menu.Append(ID_ExtractBranch, entireLabel)->Enable(hasExtractable(entireSelections));
            menu.Append(ID_ZipBranch, entireZipLabel)->Enable(hasExtractable(entireSelections));
        }

        const bool showResourceNames = resourceNodes && !selections.empty();
        const bool showSourceActions = !sourcePaths.empty();
        if (showResourceNames || showSourceActions) {
            menu.AppendSeparator();
            if (showResourceNames) {
                menu.Append(ID_CopyResref,
                            selections.size() == 1
                                ? "Copy ResRef" : "Copy ResRefs");
            }
            if (showSourceActions) {
                menu.Append(ID_CopySource,
                            sourcePaths.size() == 1
                                ? "Copy source archive path" : "Copy source archive paths");
            }
#if !defined(__EMSCRIPTEN__)
            if (sourcePaths.size() == 1) {
                menu.Append(ID_LocateSource, "Show in File Manager");
            }
#endif
        }
        PopupMenu(&menu);
    }

    void requestOpenArchiveFiles() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        neobrowser::requestRetainedFiles(
            "Open chitin.key and optional relocated BIF files", ".key,.bif", true,
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
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        wxui::requestOpenFiles(
            this, "Open chitin.key and optional relocated BIF files", kArchiveWildcard,
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
            wxArrayString choices;
            int preferred = 0;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                choices.Add(neosettings::pathToWx(keys[i]));
                if (lowerAscii(neoshared::pathToUtf8(keys[i].filename())) == "chitin.key") {
                    preferred = static_cast<int>(i);
                }
            }
            wxSingleChoiceDialog dialog(this,
                "More than one KEY file was selected. Choose the game archive to open:",
                "Select KEY File", choices);
            dialog.SetSelection(preferred);
            if (dialog.ShowModal() != wxID_OK) return;
            key = keys[static_cast<std::size_t>(dialog.GetSelection())];
        }
        openArchive(key, std::move(bifs), key.parent_path());
#else
        (void)paths;
#endif
    }

    void requestAddBifs() {
        if(jobRunning_)return;
        if (!archive_.isOpen()) {
            requestOpenArchiveFiles();
            return;
        }
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
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
        wxWeakRef<NeoBIFPanelImpl> weak(this);
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
        auto replacements=explicitRelocations_;
        for(const auto& path:paths) {
            if(!hasExtension(path,".bif"))continue;
            wxArrayString choices;
            int suggested = wxNOT_FOUND;
            int firstMissing = wxNOT_FOUND;
            const std::string selectedName = lowerAscii(
                neoshared::pathToUtf8(path.filename()));
            for (std::size_t i = 0; i < archive_.bifs().size(); ++i) {
                const auto& bif = archive_.bifs()[i];
                choices.Add(wxui::toWx(std::to_string(bif.index)+": "+bif.storedPath+
                    (bif.available?" [currently available]":" [missing]") ));
                if (!bif.available && firstMissing == wxNOT_FOUND) {
                    firstMissing = static_cast<int>(i);
                }
                std::string portableStoredPath = bif.storedPath;
                std::replace(portableStoredPath.begin(), portableStoredPath.end(), '\\', '/');
                const std::string storedName = lowerAscii(neoshared::pathToUtf8(
                    std::filesystem::path(portableStoredPath).filename()));
                if (storedName == selectedName && suggested == wxNOT_FOUND) {
                    suggested = static_cast<int>(i);
                }
            }
            wxSingleChoiceDialog dialog(this,
                "Which KEY entry should use this exact file?\n"+neosettings::pathToWx(path),
                "Relocate BIF",choices);
            if (suggested != wxNOT_FOUND) dialog.SetSelection(suggested);
            else if (firstMissing != wxNOT_FOUND) dialog.SetSelection(firstMissing);
            else if (!choices.IsEmpty()) dialog.SetSelection(0);
            if(dialog.ShowModal()!=wxID_OK)return;
            const std::size_t selection = static_cast<std::size_t>(dialog.GetSelection());
            if (selection >= archive_.bifs().size()) return;
            replacements[archive_.bifs()[selection].index]=path;
        }
        explicitRelocations_=std::move(replacements);
        openArchive(keyPath_,supplementaryFiles_,scanRoot_);
        // openArchive keeps the old session on failure; a later explicit rescan
        // can retry a missing relocated file without guessing another candidate.
#else
        (void)paths;
#endif
    }

    void requestScanDirectory() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        neobrowser::requestRetainedDirectory(
            "Select a KotOR game directory",
            ".key,.bif,.erf,.mod,.sav,.hak,.nwm,.rim",
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
        std::filesystem::path initialDirectory = scanRoot_;
        if (initialDirectory.empty()) {
            initialDirectory = settings_.readPath("Paths/LastGameDirectory").value_or(
                std::filesystem::path{});
        }
        wxDirDialog dialog(this, "Select a KotOR game directory",
                           neosettings::pathToWx(initialDirectory),
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto directory = neosettings::pathFromWx(dialog.GetPath());
        settings_.writePath("Paths/LastGameDirectory", directory);
        scanDirectory(directory);
#endif
    }

    void scanDirectory(const std::filesystem::path& directory) {
#if !defined(__EMSCRIPTEN__)
        std::vector<std::filesystem::path> keys;
        if(!runJob("Finding chitin.key",[&](const neobif::JobControl& job){keys=neobif::KeyBifArchive::scanForKeyFiles(directory,128u,job);}))return;
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
        openArchive(selected, {}, selected.parent_path());
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
            const bool leftChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(left.relativePath).filename())) == "chitin.key";
            const bool rightChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(right.relativePath).filename())) == "chitin.key";
            if (leftChitin != rightChitin) return leftChitin;
            const std::size_t leftDepth = browserPathDepth(left.relativePath);
            const std::size_t rightDepth = browserPathDepth(right.relativePath);
            if (leftDepth != rightDepth) return leftDepth < rightDepth;
            return lowerAscii(left.relativePath) < lowerAscii(right.relativePath);
        });
        if (candidates.size() > 1u) {
            const auto& first = candidates[0];
            const auto& second = candidates[1];
            const bool firstChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(first.relativePath).filename())) == "chitin.key";
            const bool secondChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(second.relativePath).filename())) == "chitin.key";
            if (firstChitin == secondChitin &&
                browserPathDepth(first.relativePath) == browserPathDepth(second.relativePath)) {
                error = "More than one equally likely KEY file was selected. Select one game installation at a time.";
                return std::nullopt;
            }
        }
        return candidates.front();
    }

    static void acceptBrowserFileSet(wxWeakRef<NeoBIFPanelImpl> weak,
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
            if (!hasExtension(path, ".key") && !hasExtension(path, ".bif") &&
                !neobif::LooseArchiveCatalog::hasSupportedExtension(path)) {
                continue;
            }
            neobif::BrowserArchiveFile selected;
            selected.sessionId=result.sessionId; selected.fileId=file.fileId;
            selected.relativePath=file.relativePath; selected.size=file.size;
            incoming.push_back(std::move(selected));
        }
        if (incoming.empty()) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("The selection contains no KEY, BIF, ERF, MOD, SAV, HAK, NWM, or RIM files.",
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

        if(appendBifs && weak->archive_.isOpen()) {
            wxArrayString choices;
            for(const auto& bif:weak->archive_.bifs()) {
                choices.Add(wxui::toWx("["+std::to_string(bif.index)+"] "+bif.storedPath+
                    (bif.available ? " [currently available]" : " [missing]")));
            }
            for(auto& selected:combined) if(selected.sessionId==result.sessionId && hasExtension(std::filesystem::path(selected.relativePath),".bif")) {
                int suggested = wxNOT_FOUND;
                int firstMissing = wxNOT_FOUND;
                const std::string selectedName = lowerAscii(neoshared::pathToUtf8(
                    std::filesystem::path(selected.relativePath).filename()));
                for (std::size_t i = 0; i < weak->archive_.bifs().size(); ++i) {
                    const auto& bif = weak->archive_.bifs()[i];
                    if (!bif.available && firstMissing == wxNOT_FOUND) {
                        firstMissing = static_cast<int>(i);
                    }
                    std::string portableStoredPath = bif.storedPath;
                    std::replace(portableStoredPath.begin(), portableStoredPath.end(), '\\', '/');
                    const std::string storedName = lowerAscii(neoshared::pathToUtf8(
                        std::filesystem::path(portableStoredPath).filename()));
                    if (storedName == selectedName && suggested == wxNOT_FOUND) {
                        suggested = static_cast<int>(i);
                    }
                }
                wxSingleChoiceDialog dialog(weak.get(),
                    wxui::toWx("Choose the KEY entry supplied by " + selected.relativePath),
                    "Relocate BIF Explicitly",choices);
                if (suggested != wxNOT_FOUND) dialog.SetSelection(suggested);
                else if (firstMissing != wxNOT_FOUND) dialog.SetSelection(firstMissing);
                else if (!choices.IsEmpty()) dialog.SetSelection(0);
                if(dialog.ShowModal()!=wxID_OK) { neobrowser::releaseRetainedFileSet(result.sessionId); return; }
                const auto choice=static_cast<std::size_t>(dialog.GetSelection());
                if (choice >= weak->archive_.bifs().size()) {
                    neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                const std::size_t bifIndex = weak->archive_.bifs()[choice].index;
                for(auto& previous:combined) if(&previous!=&selected && previous.explicitBifIndex==bifIndex) previous.explicitBifIndex.reset();
                selected.explicitBifIndex=bifIndex;
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

        if (weak->filterTimer_.IsRunning()) weak->flushSearch();
        weak->browserIndexing_ = true;
        weak->Enable(false);
        neobif::KeyBifArchive loaded;
        neobif::LooseArchiveCatalog loadedLoose;
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
            wxProgressDialog progress("Indexing game archives","Reading KEY/BIF and game-directory archive tables...",100,weak.get(),wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME);
            const auto yield = [&progress] {
                if(!progress.Pulse())throw neobif::JobCancelled();
                emscripten_sleep(0);
            };
            opened = loaded.openBrowser(*key, combined, reader, yield);
            if (!opened) {
                openError = loaded.lastError();
            } else {
                const std::filesystem::path browserRoot =
                    std::filesystem::path(key->relativePath).parent_path();
                opened = loadedLoose.openBrowser(browserRoot, combined, reader, yield);
                if (!opened) openError = loadedLoose.lastError();
            }
        } catch (const std::exception& exception) {
            openError = exception.what();
        } catch (...) {
            openError = "An unknown error occurred while indexing the browser archive.";
        }

        if (!weak) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        NeoBIFPanelImpl* frame = weak.get();
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
        for (const auto& looseArchive : loadedLoose.archives()) {
            if (looseArchive.browserBacked && looseArchive.browserSessionId != 0 &&
                looseArchive.browserFileId != 0) {
                activeFileKeys.emplace(looseArchive.browserSessionId,
                                       looseArchive.browserFileId);
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
        if (!appendBifs) {
            frame->filterTimer_.Stop();
            frame->filter_->ChangeValue(wxEmptyString);
        }
        frame->expandedNodes_.clear();frame->selectedNodes_.clear();frame->tree_->DeleteAllItems();
        frame->archive_ = std::move(loaded);
        frame->looseArchives_ = std::move(loadedLoose);
        frame->keyPath_ = std::filesystem::path(key->relativePath);
        frame->scanRoot_ = frame->keyPath_.parent_path();
        frame->supplementaryFiles_.clear();
        ++frame->browserGeneration_;
        frame->updateSessionLabel();
        frame->setModuleTitle(appTitle() + " - " +
                        wxui::toWx(neoshared::pathToUtf8(frame->keyPath_.filename())));
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

#if !defined(__EMSCRIPTEN__)
    template<class Work>
    bool runJob(const wxString& title, Work work) {
        if(jobRunning_)return false;
        if (filterTimer_.IsRunning()) flushSearch();
        jobRunning_=true;cancelJob_.store(false);
        struct BusyReset { bool& flag; ~BusyReset(){flag=false;} } busyReset{jobRunning_};
        std::atomic<bool> finished{false};
        std::mutex mutex;std::string detail="Preparing...";std::size_t done=0,total=0;
        std::exception_ptr failure;
        neobif::JobControl control;
        control.cancelled=[this]{return cancelJob_.load();};
        control.progress=[&](std::size_t current,std::size_t count,const std::string& text){std::lock_guard<std::mutex> lock(mutex);done=current;total=count;detail=text;};
        wxProgressDialog dialog(title,"Preparing...",1000,this,wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME|wxPD_AUTO_HIDE);
        std::thread worker([&]{try{work(control);}catch(...){failure=std::current_exception();}finished.store(true);});
        struct JoinWorker { std::thread& thread; std::atomic<bool>& cancel;
            ~JoinWorker(){if(thread.joinable()){cancel.store(true);thread.join();}}
        } joinWorker{worker,cancelJob_};
        while(!finished.load()) {
            std::string text;std::size_t current,count;
            {std::lock_guard<std::mutex> lock(mutex);text=detail;current=done;count=total;}
            bool proceed=count?dialog.Update(static_cast<int>(std::min<std::size_t>(999,current*1000/std::max<std::size_t>(1,count))),wxui::toWx(text)):
                               dialog.Pulse(wxui::toWx(text));
            if(!proceed)cancelJob_.store(true);
            wxMilliSleep(10);
        }
        worker.join();dialog.Update(1000);jobRunning_=false;
        if(failure) {
            try{std::rethrow_exception(failure);}
            catch(const neobif::JobCancelled&){setModuleStatusText("Operation cancelled; current session retained");return false;}
            catch(const std::exception& ex){wxMessageBox(wxui::toWx(ex.what()),title,wxOK|wxICON_ERROR,this);return false;}
        }
        return true;
    }
#endif

    void openArchive(const std::filesystem::path& key,
                     std::vector<std::filesystem::path> supplementary,
                     std::filesystem::path scanRoot) {
#if !defined(__EMSCRIPTEN__)
        if(jobRunning_)return;
        const bool sameSession = archive_.isOpen() &&
            neosettings::samePathForMru(keyPath_, key);
        const auto mappings=sameSession?explicitRelocations_:std::map<std::size_t,std::filesystem::path>{};
        if(scanRoot.empty())scanRoot=key.parent_path();
        if(scanRoot.empty())scanRoot=std::filesystem::current_path();
        scanRoot=std::filesystem::absolute(scanRoot).lexically_normal();
        neobif::KeyBifArchive loaded;neobif::LooseArchiveCatalog loose;
        if(!runJob("Indexing game archives",[&](const neobif::JobControl& job){
            if(!loaded.open(key,supplementary,mappings,job))throw std::runtime_error(loaded.lastError());
            if(!loose.scan(scanRoot,8192u,job))throw std::runtime_error(loose.lastError());
            job.check();
        }))return;
        if(!sameSession) {
            explicitRelocations_.clear();
            filterTimer_.Stop();
            filter_->ChangeValue(wxEmptyString);
        }
        // Indices may change on rescan. Preserve navigation across filters,
        // never reinterpret an old numeric selection as a new resource.
        expandedNodes_.clear();selectedNodes_.clear();tree_->DeleteAllItems();
        archive_ = std::move(loaded);
        looseArchives_ = std::move(loose);
        keyPath_ = key;
        scanRoot_ = std::move(scanRoot);
        supplementaryFiles_ = std::move(supplementary);
        updateSessionLabel();
        setModuleTitle(appTitle() + " - " + neosettings::pathToWx(keyPath_.filename()));
        rebuildTree();
        updateStatus();
        updateCommandState();
#else
        (void)key;
        (void)supplementary;
        (void)scanRoot;
#endif
    }

    void closeArchive() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
        archive_.clear();
        looseArchives_.clear();
        keyPath_.clear();
        scanRoot_.clear();
        supplementaryFiles_.clear();
        explicitRelocations_.clear();expandedNodes_.clear();selectedNodes_.clear();
        filterTimer_.Stop();
        filter_->ChangeValue(wxEmptyString);
        visibleBif_.clear();
        visibleLoose_.clear();
        tree_->DeleteAllItems();
        updateSessionLabel();
        details_->ChangeValue(emptyStateText());
        setModuleTitle(appTitle());
        updateStatus();
        updateCommandState();
        updateSearchCount();
    }

    void clearSearch() {
        filterTimer_.Stop();
        filter_->ChangeValue(wxEmptyString);
        rebuildTree();
    }

    void flushSearch() {
        filterTimer_.Stop();
        rebuildTree();
    }

    bool resourceMatches(const neobif::ResourceInfo& resource,
                         const neobif::BifInfo& bif,
                         const neobif::ResourceQuery& query) const {
        return query.matches(resource.fileName(), resource.extension,
            {resource.resref, neobif::resourceTypeLabel(resource.type),
             neobif::hexResourceId(resource.resourceId), resource.status, bif.storedPath});
    }

    bool looseResourceMatches(const neobif::LooseResourceInfo& resource,
                              const neobif::LooseArchiveInfo& archive,
                              const neobif::ResourceQuery& query) const {
        return query.matches(resource.fileName(), resource.extension,
            {resource.resref, neobif::resourceTypeLabel(resource.type),
             neobif::hexResourceId(resource.resourceId), resource.status,
             neoshared::genericPathToUtf8(archive.relativePath),
             neobif::looseArchiveKindName(archive.kind)});
    }

    void updateSearchCount() {
        if (!searchCount_) return;
        if (!archive_.isOpen()) {
            searchCount_->SetLabel("No session");
            searchCount_->UnsetToolTip();
        } else {
            const auto count = std::count(visibleBif_.begin(), visibleBif_.end(), true) +
                               std::count(visibleLoose_.begin(), visibleLoose_.end(), true);
            const auto total = archive_.resources().size() + looseArchives_.resources().size();
            if (filter_->GetValue().empty()) {
                searchCount_->SetLabel(wxui::toWx(
                    std::to_string(total) + (total == 1u ? " resource" : " resources")));
                searchCount_->UnsetToolTip();
            } else {
                const wxTreeItemId root = tree_->GetRootItem();
                const bool archiveBranchMatches = root.IsOk() &&
                    tree_->GetChildrenCount(root, false) != 0u;
                searchCount_->SetLabel(wxui::toWx(
                    std::to_string(count) +
                    (count == 1u ? " resource match" : " resource matches")));
                std::string tip = std::to_string(count) + " of " +
                    std::to_string(total) +
                    " indexed resources match the current Search.";
                if (count == 0u && archiveBranchMatches) {
                    tip += " An archive or folder name matches, so that branch remains visible.";
                }
                searchCount_->SetToolTip(wxui::toWx(tip));
            }
        }
        searchCount_->GetParent()->Layout();
    }

    void revealSearchResults(const wxTreeItemId& item, std::size_t& budget) {
        const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if (!data) return;
        if (data->lazyResources) {
            // Reveal small result sets, but do not undo lazy pagination on
            // queries matching tens of thousands of resources.
            if (data->lazyResources->size() > budget) return;
            budget -= data->lazyResources->size();
            populateNode(item);
            tree_->Expand(item);
            return;
        }
        tree_->Expand(item);
        wxTreeItemIdValue cookie;
        for (auto child = tree_->GetFirstChild(item, cookie); child.IsOk();
             child = tree_->GetNextChild(item, cookie)) revealSearchResults(child, budget);
    }

    std::string nodeKey(const NodeData& data) const {
        return std::to_string(static_cast<int>(data.kind))+":"+
            std::to_string(static_cast<int>(data.source))+":"+
            std::to_string(data.ownerIndex)+":"+std::to_string(data.resourceIndex)+":"+
            std::to_string(data.type)+":"+data.directoryPath;
    }

    void captureTreeState(const wxTreeItemId& item) {
        if(!item.IsOk()) return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data) {
            const auto key=nodeKey(*data);
            if(tree_->IsExpanded(item)) expandedNodes_.insert(key);
            else expandedNodes_.erase(key);
            if(tree_->IsSelected(item)) selectedNodes_.insert(key);
            else selectedNodes_.erase(key);
        }
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie))captureTreeState(child);
    }

    void attachResourceList(const wxTreeItemId& item, const std::vector<std::size_t>& indices) {
        auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        data->lazyResources=std::make_shared<const std::vector<std::size_t>>(indices);
        tree_->SetItemHasChildren(item,!indices.empty());
    }

    void populateNode(const wxTreeItemId& item) {
        auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(!data || data->populated || !data->lazyResources) return;
        data->populated=true;
        const auto& indices=*data->lazyResources;
        constexpr std::size_t pageSize=512;
        if(indices.size()>pageSize) {
            for(std::size_t start=0;start<indices.size();start+=pageSize) {
                const auto end=std::min(indices.size(),start+pageSize);
                auto* page=new NodeData(NodeKind::Page,data->ownerIndex,start,data->type,data->source);
                const auto child=tree_->AppendItem(item,wxui::toWx("Resources "+std::to_string(start+1)+"-"+std::to_string(end)), -1,-1,page);
                attachResourceList(child,std::vector<std::size_t>(indices.begin()+static_cast<std::ptrdiff_t>(start),indices.begin()+static_cast<std::ptrdiff_t>(end)));
            }
        } else {
            for(auto index:indices) {
                if(data->source==ResourceSource::KeyBif)appendBifResourceNode(item,index);
                else appendLooseResourceNode(item,index);
            }
        }
    }

    void restoreTreeState(const wxTreeItemId& item) {
        if(!item.IsOk())return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data) {
            const auto key=nodeKey(*data);
            bool reveal=expandedNodes_.count(key)!=0;
            if(data->lazyResources) for(auto index:*data->lazyResources) {
                const auto type=data->source==ResourceSource::KeyBif?archive_.resources()[index].type:looseArchives_.resources()[index].type;
                const NodeData resource(NodeKind::Resource,data->ownerIndex,index,type,data->source);
                if(selectedNodes_.count(nodeKey(resource))){reveal=true;break;}
            }
            if(reveal) { populateNode(item);tree_->Expand(item); }
            if(selectedNodes_.count(key))tree_->SelectItem(item);
        }
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie))restoreTreeState(child);
    }

    void addResourceGroups(const wxTreeItemId& parent,ResourceSource source,std::size_t owner,
                           const std::vector<std::size_t>& visible) {
        if(!groupByType_) {attachResourceList(parent,visible);return;}
        std::map<std::uint16_t,std::vector<std::size_t>> groups;
        for(auto index:visible)groups[source==ResourceSource::KeyBif?archive_.resources()[index].type:looseArchives_.resources()[index].type].push_back(index);
        for(const auto& [type,indices]:groups) {
            const auto node=tree_->AppendItem(parent,wxui::toWx("."+neobif::resourceTypeExtension(type)+" ("+std::to_string(indices.size())+")"), -1,-1,
                new NodeData(NodeKind::Type,owner,kNoIndex,type,source));
            attachResourceList(node,indices);
        }
    }

    void expandStructure(const wxTreeItemId& item) {
        if(!item.IsOk()) return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data && data->kind==NodeKind::Page) return;
        populateNode(item);
        tree_->Expand(item);
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie)) {
            const auto* value=dynamic_cast<NodeData*>(tree_->GetItemData(child));
            if(!value || value->kind!=NodeKind::Resource) expandStructure(child);
        }
    }

    void rebuildTree() {
        if(rebuilding_ || jobRunning_)return;
        rebuilding_=true;
        captureTreeState(tree_->GetRootItem());
        tree_->Freeze();tree_->DeleteAllItems();
        visibleBif_.assign(archive_.resources().size(),false);
        visibleLoose_.assign(looseArchives_.resources().size(),false);
        if(!archive_.isOpen()) {tree_->Thaw();rebuilding_=false;updateSearchCount();return;}
        const neobif::ResourceQuery query(wxui::toStd(filter_->GetValue()));
        const auto root=tree_->AddRoot(neosettings::pathToWx(keyPath_.filename()),-1,-1,new NodeData(NodeKind::Root));
        for(const auto& bif:archive_.bifs()) {
            std::vector<std::size_t> visible;
            const bool ownerMatches=query.empty()||query.matchesContext(bif.storedPath)||
                std::any_of(bif.messages.begin(),bif.messages.end(),[&](const std::string& message){return query.matchesContext(message);});
            for(auto index:bif.resourceIndices)if(index<archive_.resources().size()&&(ownerMatches||resourceMatches(archive_.resources()[index],bif,query))) {
                visible.push_back(index);visibleBif_[index]=true;
            }
            if(visible.empty()&&!ownerMatches)continue;
            const std::string label=bif.storedPath+" ("+std::to_string(visible.size())+
                (query.empty()?" resources)":" matches)")+
                (!bif.available?" [missing]":(!bif.valid?" [invalid]":""));
            const auto node=tree_->AppendItem(root,wxui::toWx(label),-1,-1,new NodeData(NodeKind::Bif,bif.index));
            addResourceGroups(node,ResourceSource::KeyBif,bif.index,visible);
        }
        wxTreeItemId looseRoot;
        std::map<std::string,wxTreeItemId> directories;
        for(const auto& archive:looseArchives_.archives()) {
            std::vector<std::size_t> visible;
            const bool ownerMatches=query.empty()||query.matchesContext(neoshared::genericPathToUtf8(archive.relativePath))||
                query.matchesContext(neobif::looseArchiveKindName(archive.kind))||
                std::any_of(archive.messages.begin(),archive.messages.end(),[&](const std::string& message){return query.matchesContext(message);});
            for(auto index:archive.resourceIndices)if(index<looseArchives_.resources().size()&&(ownerMatches||looseResourceMatches(looseArchives_.resources()[index],archive,query))) {
                visible.push_back(index);visibleLoose_[index]=true;
            }
            if(visible.empty()&&!ownerMatches)continue;
            if(!looseRoot.IsOk())looseRoot=tree_->AppendItem(root,"Game Archives",-1,-1,
                new NodeData(NodeKind::LooseRoot,kNoIndex,kNoIndex,0,ResourceSource::LooseArchive));
            auto parent=looseRoot;std::filesystem::path accumulated;
            for(const auto& component:archive.relativePath.parent_path()) {
                const auto part=neoshared::genericPathToUtf8(component);if(part.empty()||part=="."||part=="..")continue;
                accumulated/=component;const auto pathKey=lowerAscii(neoshared::genericPathToUtf8(accumulated));
                auto found=directories.find(pathKey);
                if(found==directories.end()) {
                    parent=tree_->AppendItem(parent,wxui::toWx(part),-1,-1,new NodeData(NodeKind::Directory,kNoIndex,kNoIndex,0,ResourceSource::LooseArchive,neoshared::genericPathToUtf8(accumulated)));
                    directories.emplace(pathKey,parent);
                }else parent=found->second;
            }
            const auto label=neoshared::pathToUtf8(archive.relativePath.filename())+" ("+
                std::to_string(visible.size())+(query.empty()?" resources)":" matches)")+
                (!archive.valid?" [invalid]":"");
            const auto node=tree_->AppendItem(parent,wxui::toWx(label),-1,-1,new NodeData(NodeKind::LooseArchive,archive.index,kNoIndex,0,ResourceSource::LooseArchive));
            addResourceGroups(node,ResourceSource::LooseArchive,archive.index,visible);
        }
        restoreTreeState(root);tree_->Expand(root);
        if (!query.empty()) {
            std::size_t revealBudget = 256;
            revealSearchResults(root, revealBudget);
        }
        if(selectedNodes_.empty()&&query.empty())tree_->SelectItem(root);
        tree_->Thaw();rebuilding_=false;
        showSelectedDetails();updateCommandState();updateSearchCount();
    }

    void appendBifResourceNode(const wxTreeItemId& parent, std::size_t resourceIndex) {
        const auto& resource = archive_.resources()[resourceIndex];
        std::ostringstream label;
        label << resource.fileName() << " - " << neobif::formatByteSize(resource.size);
        if (!resource.extractable) label << " [unavailable]";
        if (!resource.keyed) label << " [not indexed]";
        tree_->AppendItem(parent, wxui::toWx(label.str()), -1, -1,
                          new NodeData(NodeKind::Resource, resource.bifIndex,
                                       resourceIndex, resource.type,
                                       ResourceSource::KeyBif));
    }

    void appendLooseResourceNode(const wxTreeItemId& parent,
                                 std::size_t resourceIndex) {
        const auto& resource = looseArchives_.resources()[resourceIndex];
        std::ostringstream label;
        label << resource.fileName() << " - " << neobif::formatByteSize(resource.size);
        if (!resource.extractable) label << " [unavailable]";
        tree_->AppendItem(parent, wxui::toWx(label.str()), -1, -1,
                          new NodeData(NodeKind::Resource, resource.archiveIndex,
                                       resourceIndex, resource.type,
                                       ResourceSource::LooseArchive));
    }

    NodeData* selectedNodeData() const {
        wxArrayTreeItemIds selections;tree_->GetSelections(selections);
        return selections.empty()?nullptr:dynamic_cast<NodeData*>(tree_->GetItemData(selections.front()));
    }

    bool selectedNodesAreResources() const {
        wxArrayTreeItemIds selections;
        tree_->GetSelections(selections);
        if (selections.empty()) return false;
        for (const auto& item : selections) {
            const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
            if (data == nullptr || data->kind != NodeKind::Resource) return false;
        }
        return true;
    }

    static bool archiveIsUnderDirectory(const std::filesystem::path& archivePath,
                                        const std::string& directoryPath) {
        std::string parent = lowerAscii(neoshared::genericPathToUtf8(
            archivePath.parent_path().lexically_normal()));
        std::string directory = lowerAscii(neoshared::genericPathToUtf8(
            neoshared::pathFromUtf8(directoryPath).lexically_normal()));
        while (!parent.empty() && parent.back() == '/') parent.pop_back();
        while (!directory.empty() && directory.back() == '/') directory.pop_back();
        return parent == directory ||
               (!directory.empty() && parent.size() > directory.size() &&
                parent.compare(0u, directory.size(), directory) == 0 &&
                parent[directory.size()] == '/');
    }

    std::vector<ResourceSelection> nodeResources(const NodeData* data) const {
        if (data == nullptr || !archive_.isOpen()) return {};
        std::vector<ResourceSelection> selected;
        const auto appendAllBif = [&] {
            selected.reserve(selected.size() + archive_.resources().size());
            for (const auto& resource : archive_.resources()) {
                selected.push_back({ResourceSource::KeyBif, resource.index});
            }
        };
        const auto appendAllLoose = [&] {
            selected.reserve(selected.size() + looseArchives_.resources().size());
            for (const auto& resource : looseArchives_.resources()) {
                selected.push_back({ResourceSource::LooseArchive, resource.index});
            }
        };

        switch (data->kind) {
        case NodeKind::Root:
            appendAllBif();
            appendAllLoose();
            break;
        case NodeKind::Bif:
            if (data->ownerIndex < archive_.bifs().size()) {
                for (const std::size_t index : archive_.bifs()[data->ownerIndex].resourceIndices) {
                    selected.push_back({ResourceSource::KeyBif, index});
                }
            }
            break;
        case NodeKind::LooseRoot:
            appendAllLoose();
            break;
        case NodeKind::Directory:
            for (const auto& looseArchive : looseArchives_.archives()) {
                if (!archiveIsUnderDirectory(looseArchive.relativePath,
                                             data->directoryPath)) {
                    continue;
                }
                for (const std::size_t index : looseArchive.resourceIndices) {
                    selected.push_back({ResourceSource::LooseArchive, index});
                }
            }
            break;
        case NodeKind::LooseArchive:
            if (data->ownerIndex < looseArchives_.archives().size()) {
                for (const std::size_t index :
                     looseArchives_.archives()[data->ownerIndex].resourceIndices) {
                    selected.push_back({ResourceSource::LooseArchive, index});
                }
            }
            break;
        case NodeKind::Type:
            if (data->source == ResourceSource::KeyBif &&
                data->ownerIndex < archive_.bifs().size()) {
                for (const std::size_t index : archive_.bifs()[data->ownerIndex].resourceIndices) {
                    if (index < archive_.resources().size() &&
                        archive_.resources()[index].type == data->type) {
                        selected.push_back({ResourceSource::KeyBif, index});
                    }
                }
            } else if (data->source == ResourceSource::LooseArchive &&
                       data->ownerIndex < looseArchives_.archives().size()) {
                for (const std::size_t index :
                     looseArchives_.archives()[data->ownerIndex].resourceIndices) {
                    if (index < looseArchives_.resources().size() &&
                        looseArchives_.resources()[index].type == data->type) {
                        selected.push_back({ResourceSource::LooseArchive, index});
                    }
                }
            }
            break;
        case NodeKind::Page:
            if(data->lazyResources)for(auto index:*data->lazyResources)selected.push_back({data->source,index});
            break;
        case NodeKind::Resource:
            selected.push_back({data->source, data->resourceIndex});
            break;
        }
        return selected;
    }

    std::vector<ResourceSelection> selectedResources(bool respectFilter=true) const {
        wxArrayTreeItemIds selectedItems;tree_->GetSelections(selectedItems);
        std::vector<ResourceSelection> result;std::set<std::pair<int,std::size_t>> used;
        for(const auto& item:selectedItems) {
            const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
            for(const auto& resource:nodeResources(data)) {
                const auto& visible=resource.source==ResourceSource::KeyBif?visibleBif_:visibleLoose_;
                if(respectFilter && (resource.resourceIndex>=visible.size()||!visible[resource.resourceIndex]))continue;
                if(used.emplace(static_cast<int>(resource.source),resource.resourceIndex).second)result.push_back(resource);
            }
        }
        return result;
    }

    std::vector<ResourceSelection> allResourceSelections() const {
        std::vector<ResourceSelection> selected;
        selected.reserve(archive_.resources().size() + looseArchives_.resources().size());
        for (const auto& resource : archive_.resources()) {
            selected.push_back({ResourceSource::KeyBif, resource.index});
        }
        for (const auto& resource : looseArchives_.resources()) {
            selected.push_back({ResourceSource::LooseArchive, resource.index});
        }
        return selected;
    }

    bool resourceIsExtractable(const ResourceSelection& selection) const {
        if (selection.source == ResourceSource::KeyBif) {
            return selection.resourceIndex < archive_.resources().size() &&
                   archive_.resources()[selection.resourceIndex].extractable;
        }
        return selection.resourceIndex < looseArchives_.resources().size() &&
               looseArchives_.resources()[selection.resourceIndex].extractable;
    }

    std::string resourceFileName(const ResourceSelection& selection) const {
        if (selection.source == ResourceSource::KeyBif) {
            return selection.resourceIndex < archive_.resources().size()
                ? archive_.resources()[selection.resourceIndex].fileName()
                : std::string{};
        }
        return selection.resourceIndex < looseArchives_.resources().size()
            ? looseArchives_.resources()[selection.resourceIndex].fileName()
            : std::string{};
    }

    std::string resourceDisplayName(const ResourceSelection& selection) const {
        if (selection.source == ResourceSource::KeyBif) {
            if (selection.resourceIndex >= archive_.resources().size()) return "Invalid BIF resource";
            const auto& resource = archive_.resources()[selection.resourceIndex];
            const std::string archivePath = resource.bifIndex < archive_.bifs().size()
                ? archive_.bifs()[resource.bifIndex].storedPath : "<invalid BIF>";
            return archivePath + "/" + resource.fileName();
        }
        if (selection.resourceIndex >= looseArchives_.resources().size()) {
            return "Invalid game-archive resource";
        }
        const auto& resource = looseArchives_.resources()[selection.resourceIndex];
        const std::string archivePath = resource.archiveIndex < looseArchives_.archives().size()
            ? neoshared::genericPathToUtf8(
                  looseArchives_.archives()[resource.archiveIndex].relativePath)
            : "<invalid archive>";
        return archivePath + "/" + resource.fileName();
    }

    std::uint64_t resourceSize(const ResourceSelection& selection) const {
        if (selection.source == ResourceSource::KeyBif) {
            return selection.resourceIndex < archive_.resources().size()
                ? archive_.resources()[selection.resourceIndex].size : 0u;
        }
        return selection.resourceIndex < looseArchives_.resources().size()
            ? looseArchives_.resources()[selection.resourceIndex].size : 0u;
    }

    std::string resourceIdentity(const ResourceSelection& selection) const {
        std::ostringstream identity;
        if (selection.source == ResourceSource::KeyBif &&
            selection.resourceIndex < archive_.resources().size()) {
            identity << "bif_" << neobif::hexResourceId(
                archive_.resources()[selection.resourceIndex].resourceId).substr(2);
        } else if (selection.source == ResourceSource::LooseArchive &&
                   selection.resourceIndex < looseArchives_.resources().size()) {
            const auto& resource = looseArchives_.resources()[selection.resourceIndex];
            identity << "arc_" << resource.archiveIndex << '_'
                     << neobif::hexResourceId(resource.resourceId).substr(2);
        } else {
            identity << "resource_" << selection.resourceIndex;
        }
        return identity.str();
    }

    bool readResourceSelection(const ResourceSelection& selection,
                               std::vector<std::uint8_t>& bytes,
                               std::string& error) const {
        if (selection.source == ResourceSource::KeyBif) {
            return archive_.readResource(selection.resourceIndex, bytes, error);
        }
        return looseArchives_.readResource(selection.resourceIndex, bytes, error);
    }

    std::filesystem::path resourceOutputPath(
        const ResourceSelection& selection,
        neobif::ExtractionLayout layout) const {
        if (selection.source == ResourceSource::KeyBif) {
            const auto paths = archive_.outputPaths({selection.resourceIndex}, layout);
            return paths.empty() ? std::filesystem::path{} : paths.front();
        }
        return looseArchives_.outputPath(selection.resourceIndex, layout);
    }

    std::vector<std::filesystem::path> resourceOutputPaths(
        const std::vector<ResourceSelection>& selections,
        neobif::ExtractionLayout layout) const {
        std::vector<std::filesystem::path> desired;
        desired.reserve(selections.size());
        for (const auto& selection : selections) {
            desired.push_back(resourceOutputPath(selection, layout));
        }
        return desired; // Only explicit Keep both is allowed to rename outputs.
    }

#if defined(__EMSCRIPTEN__)
    bool browserRangeForSelection(const ResourceSelection& selection,
                                  std::uint32_t& sessionId,
                                  std::uint32_t& fileId,
                                  std::uint64_t& offset,
                                  std::uint64_t& size) const {
        if (selection.source == ResourceSource::KeyBif) {
            if (selection.resourceIndex >= archive_.resources().size()) return false;
            const auto& resource = archive_.resources()[selection.resourceIndex];
            if (resource.bifIndex >= archive_.bifs().size()) return false;
            const auto& bif = archive_.bifs()[resource.bifIndex];
            if (!bif.browserBacked) return false;
            sessionId = bif.browserSessionId;
            fileId = bif.browserFileId;
            offset = resource.offset;
            size = resource.size;
            return sessionId != 0u && fileId != 0u;
        }
        if (selection.resourceIndex >= looseArchives_.resources().size()) return false;
        const auto& resource = looseArchives_.resources()[selection.resourceIndex];
        if (!resource.directRangeExtractable ||
            resource.archiveIndex >= looseArchives_.archives().size()) return false;
        const auto& looseArchive = looseArchives_.archives()[resource.archiveIndex];
        if (!looseArchive.browserBacked) return false;
        sessionId = looseArchive.browserSessionId;
        fileId = looseArchive.browserFileId;
        offset = resource.offset;
        size = resource.storedSize;
        return sessionId != 0u && fileId != 0u;
    }
#endif

    std::string selectionBaseName(const std::vector<ResourceSelection>& selections) const {
        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        if (selectedItems.size() > 1u) return "selected_resources";

        const NodeData* data = selectedNodeData();
        if (data != nullptr) {
            if (data->kind == NodeKind::Root) {
                return filter_->GetValue().empty() ? "game_resources" : "search_results";
            }
            if (data->kind == NodeKind::Resource) {
                const std::filesystem::path file(resourceFileName(
                    {data->source, data->resourceIndex}));
                const auto stem = neoshared::pathToUtf8(file.stem());
                return stem.empty() ? "resource" : stem;
            }
            if (data->kind == NodeKind::Bif && data->ownerIndex < archive_.bifs().size()) {
                std::string stored = archive_.bifs()[data->ownerIndex].storedPath;
                std::replace(stored.begin(), stored.end(), '\\', '/');
                const auto path = std::filesystem::path(stored);
                const auto stem = neoshared::pathToUtf8(path.stem());
                return stem.empty() ? "bif_resources" : stem;
            }
            if (data->kind == NodeKind::LooseArchive &&
                data->ownerIndex < looseArchives_.archives().size()) {
                const auto stem = neoshared::pathToUtf8(
                    looseArchives_.archives()[data->ownerIndex].relativePath.stem());
                return stem.empty() ? "archive_resources" : stem;
            }
            if (data->kind == NodeKind::Directory) {
                const auto name = neoshared::pathToUtf8(
                    neoshared::pathFromUtf8(data->directoryPath).filename());
                return name.empty() ? "directory_archives" : name;
            }
            if (data->kind == NodeKind::LooseRoot) return "game_archives";
            if (data->kind == NodeKind::Type) {
                return neobif::resourceTypeExtension(data->type) + "_resources";
            }
            if (data->kind == NodeKind::Page) {
                return neobif::resourceTypeExtension(data->type) + "_resource_page";
            }
        }
        const std::size_t total = archive_.resources().size() + looseArchives_.resources().size();
        return selections.size() == total ? "game_resources" : "neobif_selection";
    }

    void extractSelected(bool forceZip, bool respectFilter=true) {
        if (jobRunning_) return;
        if (filterTimer_.IsRunning()) flushSearch();
        const auto selections = selectedResources(respectFilter);
        if (selections.empty()) {
            wxMessageBox("Select a resource or branch first.", "Nothing Selected",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        extractSelections(selections, selectionBaseName(selections), forceZip);
    }

    void extractAll(bool forceZip) {
        if (!archive_.isOpen()) return;
        const auto selections = allResourceSelections();
        extractSelections(selections, "game_resources", forceZip);
    }

    void exportByFileType() {
        if (jobRunning_ || !archive_.isOpen()) return;
        const auto types = neobif::summarizeResourceTypes(archive_, looseArchives_);
        if (types.empty()) {
            wxMessageBox("The open game session has no indexed resource types.",
                         "Nothing to Export", wxOK | wxICON_INFORMATION, this);
            return;
        }
        wxArrayString labels;
        int initial = 0;
        const neobif::ResourceQuery query(wxui::toStd(filter_->GetValue()));
        bool pickedAvailable = false;
        for (std::size_t i = 0; i < types.size(); ++i) {
            const auto& entry = types[i];
            std::string label = "." + entry.extension + " - " +
                std::to_string(entry.total) +
                (entry.total == 1u ? " resource" : " resources");
            if (entry.available != entry.total) {
                label += " (" + std::to_string(entry.total - entry.available) +
                    " unavailable)";
            }
            labels.Add(wxui::toWx(label));
            if (entry.available && !pickedAvailable) {
                initial = static_cast<int>(i);
                pickedAvailable = true;
            }
        }
        // A type in Search is a convenient initial choice, never an export
        // scope restriction. Hidden pages and unselected archives are included.
        if (query.isTypeQuery()) {
            for (std::size_t i = 0; i < types.size(); ++i) {
                if (lowerAscii(types[i].extension) == query.extension()) initial = static_cast<int>(i);
            }
        }
        wxSingleChoiceDialog dialog(this,
            "Choose the file type to export to one ZIP",
            "Save File Type as ZIP", labels);
        dialog.SetSelection(initial);
        if (auto* button = dynamic_cast<wxButton*>(dialog.FindWindow(wxID_OK))) {
            button->SetLabel("Save ZIP...");
        }
        wxui::applyTheme(&dialog, darkMode_);
        neoview::applyFontScale(&dialog, fontScale_);
        if (dialog.ShowModal() != wxID_OK) return;
        const int chosen = dialog.GetSelection();
        if (chosen == wxNOT_FOUND || static_cast<std::size_t>(chosen) >= types.size()) return;
        const auto& type = types[static_cast<std::size_t>(chosen)];
        const auto resources = neobif::selectResourceType(archive_, looseArchives_, type.type);
        // Route through the existing native/browser ZIP export, including
        // unavailable-resource confirmation, collision and source protection,
        // staging, cancellation and size limits. Even one match is a ZIP.
        extractSelections(resources, "game_" + type.extension + "_resources", true);
    }

    std::vector<neobif::ExportItem> makeNativeExportItems(
        const std::vector<ResourceSelection>& selections,
        const std::vector<std::filesystem::path>& paths) {
        std::vector<neobif::ExportItem> items;
        items.reserve(selections.size());
        for (std::size_t index = 0; index < selections.size(); ++index) {
            const ResourceSelection selection = selections[index];
            neobif::ExportItem item;
            item.relativePath = paths[index];
            item.displayName = resourceDisplayName(selection);
            item.identitySuffix = resourceIdentity(selection);
            item.expectedSize = resourceSize(selection);
            const auto source=sourceArchivePath(selection);
            if(!source.empty())item.sourcePaths.push_back(source);
            item.sourcePaths.push_back(keyPath_);
            item.stream=[this,selection](const neobif::ByteSink& sink,std::string& error,const neobif::JobControl& job) {
                return selection.source==ResourceSource::KeyBif
                    ? archive_.streamResource(selection.resourceIndex,sink,error,job)
                    : looseArchives_.streamResource(selection.resourceIndex,sink,error,job);
            };
            item.read = [this, selection](std::vector<std::uint8_t>& bytes,
                                          std::string& error) {
                return readResourceSelection(selection, bytes, error);
            };
            items.push_back(std::move(item));
        }
        return items;
    }

    void extractSelections(const std::vector<ResourceSelection>& selections,
                           std::string baseName,
                           bool forceZip) {
        if(jobRunning_)return;
        std::vector<ResourceSelection> extractable;
        extractable.reserve(selections.size());
        for (const auto& selection : selections) {
            if (resourceIsExtractable(selection)) extractable.push_back(selection);
        }
        if (extractable.empty()) {
            wxMessageBox("The requested selection contains no extractable resources.",
                         "Nothing to Extract", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (extractable.size() != selections.size()) {
            const std::string message = std::to_string(
                selections.size() - extractable.size()) +
                " unavailable resource(s) will be omitted. Continue?";
            if (wxMessageBox(wxui::toWx(message), "Incomplete Selection",
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
                return;
            }
        }

        const bool singleResource = extractable.size() == 1u && selections.size() == 1u;
        auto outputPaths = resourceOutputPaths(
            extractable, neobif::ExtractionLayout::BifAndType);
        if (outputPaths.size() != extractable.size()) {
            wxMessageBox("NeoBIF could not construct the extraction layout.",
                         "Extraction Failed", wxOK | wxICON_ERROR, this);
            return;
        }

#if defined(__EMSCRIPTEN__)
        if(jobRunning_)return;
        int directoryPolicy=0;
        if(!singleResource&&!forceZip&&neobrowser::retainedDirectoryWriteSupported()) {
            wxArrayString choices;choices.Add("Skip existing files (recommended)");choices.Add("Replace existing files");choices.Add("Keep both with explicit filename suffixes");
            wxSingleChoiceDialog dialog(this,"Choose output conflict behavior", "Extraction conflicts",choices);
            if(dialog.ShowModal()!=wxID_OK)return;
            directoryPolicy=dialog.GetSelection();
        }
        std::set<std::string> names;bool duplicate=false;
        for(const auto& path:outputPaths)if(!names.insert(lowerAscii(neoshared::genericPathToUtf8(path))).second)duplicate=true;
        if(duplicate) {
            const bool keepBothAlreadySelected = directoryPolicy == 2 &&
                !singleResource && !forceZip &&
                neobrowser::retainedDirectoryWriteSupported();
            if (!keepBothAlreadySelected &&
                wxMessageBox(
                    "The selection contains duplicate output paths. Keep every copy by "
                    "adding explicit filename suffixes? Choose No to return and narrow "
                    "the selection.",
                    "Resource Name Conflicts",
                    wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
                return;
            }
            std::vector<std::string> identities;
            for(const auto& selection:extractable)identities.push_back(resourceIdentity(selection));
            outputPaths=neobif::makeUniqueExportPaths(outputPaths,identities);
            std::ostringstream namesText;namesText<<"Keep-both output names:\n";
            for(const auto& path:outputPaths)namesText<<neoshared::genericPathToUtf8(path)<<'\n';
            details_->ChangeValue(wxui::toWx(namesText.str()));
            details_->SetInsertionPoint(0);
        }
        std::vector<neobrowser::RetainedExportEntry> entries;
        entries.reserve(extractable.size());
        for (std::size_t position = 0; position < extractable.size(); ++position) {
            std::uint32_t sessionId = 0u;
            std::uint32_t fileId = 0u;
            std::uint64_t offset = 0u;
            std::uint64_t size = 0u;
            if (!browserRangeForSelection(
                    extractable[position], sessionId, fileId, offset, size) ||
                outputPaths[position].empty()) {
                wxMessageBox("A selected resource is not backed by an active browser file.",
                             "Extraction Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            entries.push_back(neobrowser::RetainedExportEntry{
                sessionId, fileId, offset, size,
                neoshared::genericPathToUtf8(outputPaths[position])});
        }

        neobrowser::RetainedExportMode mode = neobrowser::RetainedExportMode::DirectDownload;
        std::string outputName;
        if (singleResource && !forceZip) {
            outputName = resourceFileName(extractable.front());
        } else if (forceZip || !neobrowser::retainedDirectoryWriteSupported()) {
            mode = neobrowser::RetainedExportMode::ZipDownload;
            outputName = sanitizeDownloadName(baseName) + ".zip";
        } else {
            mode = directoryPolicy==1?neobrowser::RetainedExportMode::DirectoryReplace:
                (directoryPolicy==2?neobrowser::RetainedExportMode::DirectoryKeepBoth:neobrowser::RetainedExportMode::Directory);
            outputName = sanitizeDownloadName(baseName);
        }

        std::string confirmation;
        if (mode == neobrowser::RetainedExportMode::DirectDownload) {
            confirmation = "Download " + resourceCountText(extractable.size()) +
                " as " + outputName + "?";
        } else if (mode == neobrowser::RetainedExportMode::ZipDownload) {
            confirmation = "Save " + resourceCountText(extractable.size()) +
                " in " + outputName + "?";
        } else {
            confirmation = "Extract " + resourceCountText(extractable.size()) +
                " to a selected browser directory?";
        }
        if (wxMessageBox(wxui::toWx(confirmation), "Confirm Extraction",
                         wxOK | wxCANCEL | wxICON_QUESTION, this) != wxOK) {
            return;
        }

        const std::uint64_t generation = browserGeneration_;
        const std::size_t requestedCount = entries.size();
        const bool directDownload = mode == neobrowser::RetainedExportMode::DirectDownload;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        setModuleStatusText("Preparing " + resourceCountText(requestedCount) + "...");
        jobRunning_=true;Enable(false);
        neobrowser::requestExportRetainedFiles(
            mode, outputName, entries,
            [weak, generation, directDownload](
                neobrowser::RetainedExportResult result) {
                if (!weak) return;
                NeoBIFPanelImpl* frame = weak.get();frame->jobRunning_=false;frame->Enable(true);
                frame->updateCommandState();
                if(frame->browserGeneration_!=generation)return;
                if (!result.error.empty()) {
                    wxString message = wxui::toWx(result.error);
                    if (!result.details.empty()) {
                        message += "\n\n" + wxui::toWx(result.details);
                    }
                    wxMessageBox(message, "Extraction Failed",
                                 wxOK | wxICON_ERROR, frame);
                    frame->setModuleStatusText("Extraction failed");
                    return;
                }
                if (result.cancelled()) {
                    frame->setModuleStatusText("Extraction cancelled");
                    return;
                }
                if (result.stopped) {
                    frame->setModuleStatusText("Extraction stopped; completed files retained");
                    wxMessageBox(
                        "Extraction stopped. Any completed files were retained.",
                        "Extraction Stopped", wxOK | wxICON_INFORMATION, frame);
                    return;
                }
                if (result.usedDirectory) {
                    frame->setModuleStatusText("Extraction complete");
                    wxMessageBox("Extraction complete.", "Extraction Complete",
                                 wxOK | wxICON_INFORMATION, frame);
                } else if (directDownload) {
                    frame->setModuleStatusText("Download prepared");
                    wxMessageBox("Download prepared.", "Download Ready",
                                 wxOK | wxICON_INFORMATION, frame);
                } else {
                    frame->setModuleStatusText("ZIP download prepared");
                    wxMessageBox("ZIP download prepared.", "ZIP Ready",
                                 wxOK | wxICON_INFORMATION, frame);
                }
            });
#else
        neobif::ExportOptions options;
        options.protectedInputs=allInputPaths();
        auto items=makeNativeExportItems(extractable,outputPaths);
        if(singleResource&&!forceZip) {
            const auto selected=chooseOutputFile("Extract resource",kAllFilesWildcard,resourceFileName(extractable.front()));
            if(!selected)return;
            items[0].relativePath=selected->filename();options.existing=neobif::ExistingPolicy::Replace;
            neobif::ExtractionReport report;
            if(!runJob("Extracting resource",[&](const neobif::JobControl& job){options.job=job;report=neobif::extractExportItems(items,selected->parent_path(),options);}))return;
            showExtractionResult(report);return;
        }
        if(forceZip) {
            std::set<std::string> names;bool duplicate=false;
            for(const auto& item:items)if(!names.insert(lowerAscii(neoshared::genericPathToUtf8(item.relativePath))).second)duplicate=true;
            if(duplicate) {
                if(wxMessageBox("Several resources have the same output name. Keep both by explicitly adding suffixes inside the ZIP?", "ZIP name conflicts",wxYES_NO|wxICON_WARNING,this)!=wxYES)return;
                options.keepDuplicateNames=true;
            }
            const auto selected=chooseOutputFile("Save resources as ZIP",kZipWildcard,
                sanitizeDownloadName(baseName)+".zip", ".zip");
            if(!selected)return;
            const auto output=*selected;
            std::uint64_t inputBytes = 0u;
            for (const auto& item : items) inputBytes += item.expectedSize;
            const std::string confirmation = "Save " + resourceCountText(items.size()) +
                " (" + neobif::formatByteSize(inputBytes) + ") to:\n\n" +
                neoshared::pathToUtf8(output);
            if(wxMessageBox(wxui::toWx(confirmation), "Confirm ZIP",
                            wxOK|wxCANCEL|wxICON_QUESTION,this)!=wxOK)return;
            options.existing=neobif::ExistingPolicy::Replace;std::string error;bool written=false;
            if(!runJob("Writing ZIP",[&](const neobif::JobControl& job){options.job=job;written=neobif::writeExportZip(items,output,error,options);}))return;
            if(!written) {
                if (error.empty()) error = "NeoBIF could not commit the ZIP archive.";
                wxMessageBox(wxui::toWx(error),"ZIP Was Not Saved",wxOK|wxICON_WARNING,this);
            } else {
                setModuleStatusText("ZIP saved");
                wxMessageBox("ZIP saved successfully.", "ZIP Saved",
                             wxOK | wxICON_INFORMATION, this);
            }
            return;
        }
        wxString lastDirectory;
        if(auto* config=wxConfigBase::Get())config->Read("extraction/lastDirectory",&lastDirectory);
        wxDirDialog directory(this,"Select extraction directory",lastDirectory,wxDD_DEFAULT_STYLE|wxDD_DIR_MUST_EXIST);
        if(directory.ShowModal()!=wxID_OK)return;
        const auto output=neosettings::pathFromWx(directory.GetPath());rememberOutputDirectory(output);
        wxArrayString policies;
        policies.Add("Skip existing files; do not write name conflicts (recommended)");
        policies.Add("Replace existing files; do not write name conflicts");
        policies.Add("Keep both by adding explicit filename suffixes");
        wxSingleChoiceDialog policy(this,"How should output name conflicts be handled?", "Extraction conflicts",policies);
        if(policy.ShowModal()!=wxID_OK)return;
        options.existing=policy.GetSelection()==1?neobif::ExistingPolicy::Replace:
            (policy.GetSelection()==2?neobif::ExistingPolicy::KeepBoth:neobif::ExistingPolicy::Skip);
        neobif::ExportPlan plan;
        if(!runJob("Checking extraction destinations",[&](const neobif::JobControl& job){options.job=job;plan=neobif::planExportItems(items,output,options);}))return;
        std::ostringstream summary;
        summary << countText(plan.writes, "file", "files") << " to write\n"
                << countText(plan.skipped, "existing file", "existing files") << " to skip\n"
                << countText(plan.conflicts, "conflicting or unsafe file",
                             "conflicting or unsafe files") << " not written\n\n"
                << neoshared::pathToUtf8(output);
        const std::size_t messageCount = static_cast<std::size_t>(std::count_if(
            plan.entries.begin(), plan.entries.end(),
            [](const neobif::PlannedExport& entry) { return !entry.message.empty(); }));
        std::size_t shown = 0u;
        for (const auto& entry : plan.entries) {
            if (entry.message.empty() || shown >= 15u) continue;
            summary << "\n" << items[entry.itemIndex].displayName << ": " << entry.message;
            ++shown;
        }
        if (messageCount > shown) {
            summary << "\n... " << (messageCount - shown)
                    << " additional destination notes are not shown.";
        }
        if(plan.writes==0){wxMessageBox(wxui::toWx(summary.str()),"No Files to Write",wxOK|wxICON_INFORMATION,this);return;}
        summary << "\n\nContinue with "
                << (plan.writes == 1u ? "this file?" : "these files?");
        if(wxMessageBox(wxui::toWx(summary.str()),"Confirm extraction scope",wxOK|wxCANCEL|wxICON_QUESTION,this)!=wxOK)return;
        std::vector<neobif::ExportItem> approved;
        for(const auto& entry:plan.entries) if(entry.action==neobif::ExportAction::Write) {
            auto item=items[entry.itemIndex]; item.relativePath=entry.relativePath; approved.push_back(std::move(item));
        }
        // Keep-both suffixes were explicitly reviewed; do not generate new names after confirmation.
        if(options.existing==neobif::ExistingPolicy::KeepBoth) options.existing=neobif::ExistingPolicy::Skip;
        neobif::ExtractionReport report;
        if(!runJob("Extracting resources",[&](const neobif::JobControl& job){options.job=job;report=neobif::extractExportItems(approved,output,options);}))return;
        report.failed+=plan.conflicts;
        showExtractionResult(report);
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

    std::filesystem::path sourceArchivePath(const ResourceSelection& selection) const {
        if(selection.source==ResourceSource::KeyBif) {
            if(selection.resourceIndex>=archive_.resources().size())return {};
            const auto owner=archive_.resources()[selection.resourceIndex].bifIndex;
            if(owner>=archive_.bifs().size())return {};
            const auto& bif=archive_.bifs()[owner];
            return bif.browserBacked?std::filesystem::path(bif.browserRelativePath):bif.resolvedPath;
        }
        if(selection.resourceIndex>=looseArchives_.resources().size())return {};
        const auto owner=looseArchives_.resources()[selection.resourceIndex].archiveIndex;
        if(owner>=looseArchives_.archives().size())return {};
        const auto& archive=looseArchives_.archives()[owner];
        return archive.browserBacked?std::filesystem::path(archive.browserRelativePath):archive.resolvedPath;
    }

    std::filesystem::path sourceArchivePath(const NodeData& node) const {
        if (node.kind == NodeKind::Resource) {
            return sourceArchivePath(
                ResourceSelection{node.source, node.resourceIndex});
        }
        if ((node.kind == NodeKind::Bif || node.kind == NodeKind::Type ||
             node.kind == NodeKind::Page) &&
            node.source == ResourceSource::KeyBif &&
            node.ownerIndex < archive_.bifs().size()) {
            const auto& bif = archive_.bifs()[node.ownerIndex];
            return bif.browserBacked ? std::filesystem::path(bif.browserRelativePath)
                                     : bif.resolvedPath;
        }
        if ((node.kind == NodeKind::LooseArchive || node.kind == NodeKind::Type ||
             node.kind == NodeKind::Page) &&
            node.source == ResourceSource::LooseArchive &&
            node.ownerIndex < looseArchives_.archives().size()) {
            const auto& archive = looseArchives_.archives()[node.ownerIndex];
            return archive.browserBacked
                ? std::filesystem::path(archive.browserRelativePath)
                : archive.resolvedPath;
        }
        return {};
    }

    std::vector<std::filesystem::path> selectedSourceArchivePaths() const {
        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        std::vector<std::filesystem::path> paths;
        std::set<std::string> seen;
        for (const auto& item : selectedItems) {
            const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
            if (data == nullptr) continue;
            const auto path = sourceArchivePath(*data);
            if (path.empty()) continue;
            std::string key = neoshared::genericPathToUtf8(path.lexically_normal());
#if defined(_WIN32)
            key = lowerAscii(std::move(key));
#endif
            if (seen.insert(key).second) paths.push_back(path);
        }
        return paths;
    }

    std::vector<std::filesystem::path> allInputPaths() const {
        auto paths=archive_.inputPaths();
        for(const auto& archive:looseArchives_.archives())if(!archive.resolvedPath.empty())paths.push_back(archive.resolvedPath);
        return paths;
    }
    void copySelection(bool sourcePaths) {
        std::set<std::string> seen;
        std::string text;
        if (sourcePaths) {
            for (const auto& path : selectedSourceArchivePaths()) {
                const auto value = neoshared::pathToUtf8(path);
                if (value.empty() || !seen.insert(value).second) continue;
                if (!text.empty()) text += '\n';
                text += value;
            }
        } else {
            for (const auto& selected : selectedResources()) {
                const auto value = selected.source == ResourceSource::KeyBif
                    ? archive_.resources()[selected.resourceIndex].resref
                    : looseArchives_.resources()[selected.resourceIndex].resref;
                if (value.empty() || !seen.insert(value).second) continue;
                if (!text.empty()) text += '\n';
                text += value;
            }
        }
        if (text.empty()) return;
        if (!wxTheClipboard->Open()) {
            wxMessageBox("The clipboard is unavailable.", "Copy Failed",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(text)));
        wxTheClipboard->Close();
        setModuleStatusText("Copied " + std::to_string(seen.size()) +
            (sourcePaths ? (seen.size() == 1u ? " source path" : " source paths")
                         : (seen.size() == 1u ? " ResRef" : " ResRefs")));
    }
    void locateSource() {
#if !defined(__EMSCRIPTEN__)
        const auto paths = selectedSourceArchivePaths();
        const std::filesystem::path source = paths.size() == 1u ? paths.front()
                                                               : std::filesystem::path{};
        if(source.empty()||!wxLaunchDefaultApplication(neosettings::pathToWx(source.parent_path())))
            wxMessageBox("The source folder is unavailable.","Show in File Manager",wxOK|wxICON_INFORMATION,this);
#endif
    }
#if !defined(__EMSCRIPTEN__)
    void rememberOutputDirectory(const std::filesystem::path& directory) {
        if(auto* config=wxConfigBase::Get()){config->Write("extraction/lastDirectory",neosettings::pathToWx(directory));config->Flush();}
    }
    std::optional<std::filesystem::path> chooseOutputFile(
        const std::string& title, const std::string& wildcard,
        const std::string& name, std::string requiredExtension = {}) {
        wxString directory;if(auto* config=wxConfigBase::Get())config->Read("extraction/lastDirectory",&directory);
        wxFileDialog dialog(this,wxui::toWx(title),directory,wxui::toWx(name),wxui::toWx(wildcard),wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(dialog.ShowModal()!=wxID_OK)return std::nullopt;
        auto path=neosettings::pathFromWx(dialog.GetPath());
        if (!requiredExtension.empty()) {
            if (requiredExtension.front() != '.') requiredExtension.insert(requiredExtension.begin(), '.');
            if (lowerAscii(neoshared::pathToUtf8(path.extension())) !=
                lowerAscii(requiredExtension)) {
                path.replace_extension(requiredExtension);
                std::error_code ec;
                if (std::filesystem::exists(path, ec) && !ec &&
                    wxMessageBox(wxString("The ZIP already exists:\n\n") +
                                     neosettings::pathToWx(path) +
                                     "\n\nReplace it?",
                                 "Replace Existing ZIP", wxYES_NO | wxNO_DEFAULT |
                                     wxICON_WARNING, this) != wxYES) {
                    return std::nullopt;
                }
            }
        }
        rememberOutputDirectory(path.parent_path());return path;
    }
    void showExtractionResult(const neobif::ExtractionReport& report) {
        if (report.cancelled) {
            setModuleStatusText("Extraction cancelled");
            wxMessageBox(
                "Extraction was cancelled. Any completed files were retained.",
                "Extraction Cancelled", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxString message;
        if (report.failed != 0u) {
            setModuleStatusText("Extraction finished with warnings");
            message = "Extraction finished, but some files could not be written.";
            wxMessageBox(message, "Extraction Needs Attention",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        setModuleStatusText("Extraction complete");
        message = "Extraction complete.";
        wxMessageBox(message, "Extraction Complete",
                     wxOK | wxICON_INFORMATION, this);
    }
#endif

    void showSelectedDetails() {
        if (rebuilding_) return;
        if (!archive_.isOpen()) {
            details_->ChangeValue(emptyStateText());
            details_->SetInsertionPoint(0);
            return;
        }

        const std::size_t visibleCount =
            static_cast<std::size_t>(std::count(visibleBif_.begin(), visibleBif_.end(), true)) +
            static_cast<std::size_t>(std::count(visibleLoose_.begin(), visibleLoose_.end(), true));
        const wxTreeItemId rootItem = tree_->GetRootItem();
        const bool treeHasMatchingBranch = rootItem.IsOk() &&
            tree_->GetChildrenCount(rootItem, false) != 0u;
        if (!filter_->GetValue().empty() && visibleCount == 0u &&
            !treeHasMatchingBranch) {
            details_->ChangeValue(
                wxString("No resources match \"") + filter_->GetValue() +
                "\".\n\nClear Search or try a broader resource name or file type.");
            details_->SetInsertionPoint(0);
            return;
        }

        const NodeData* data = selectedNodeData();
        if (data == nullptr) {
            details_->ChangeValue(
                "Select a resource or branch to view its details and extraction scope.");
            details_->SetInsertionPoint(0);
            return;
        }

        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        std::ostringstream text;

        const auto writeSelectionSummary = [&](const std::vector<ResourceSelection>& selections) {
            std::size_t extractable = 0u;
            std::uint64_t bytes = 0u;
            for (const auto& selection : selections) {
                if (!resourceIsExtractable(selection)) continue;
                ++extractable;
                bytes += resourceSize(selection);
            }
            const bool filteredScope = !filter_->GetValue().empty();
            text << (filteredScope ? "Resources in Search scope: " : "Resources: ")
                 << selections.size() << '\n'
                 << "Extractable: " << extractable << '\n'
                 << "Extractable size: " << neobif::formatByteSize(bytes) << '\n';
        };

        if (selectedItems.size() > 1u) {
            text << "Selection\n"
                 << "=========\n\n"
                 << "Selected items: " << selectedItems.size() << '\n';
            writeSelectionSummary(selectedResources());
            text << "\nOverlapping branch selections are counted once.";
            details_->ChangeValue(wxui::toWx(text.str()));
            details_->SetInsertionPoint(0);
            return;
        }

        if (data->kind == NodeKind::Root) {
            std::uint64_t extractableBytes = 0u;
            for (const auto& resource : archive_.resources()) {
                if (resource.extractable) extractableBytes += resource.size;
            }
            for (const auto& resource : looseArchives_.resources()) {
                if (resource.extractable) extractableBytes += resource.size;
            }
            const std::size_t totalResources =
                archive_.resources().size() + looseArchives_.resources().size();
            const std::size_t totalExtractable = archive_.extractableResourceCount() +
                looseArchives_.extractableResourceCount();
            text << "Game Session\n"
                 << "============\n\n"
                 << "Game directory: "
                 << neoshared::pathToUtf8(scanRoot_.empty()
                        ? archive_.keyPath().parent_path() : scanRoot_) << '\n'
                 << "KEY file: " << neoshared::pathToUtf8(archive_.keyPath()) << '\n'
                 << "BIF archives: " << archive_.bifs().size() << '\n'
                 << "Game archives: " << looseArchives_.archives().size() << '\n'
                 << "Resources: " << totalResources << '\n'
                 << "Extractable: " << totalExtractable << '\n'
                 << "Extractable size: " << neobif::formatByteSize(extractableBytes) << '\n';
            if (!filter_->GetValue().empty()) {
                text << "Search matches: " << visibleCount << '\n';
            }
            if (archive_.missingBifCount() != 0u) {
                text << "Missing BIFs: " << archive_.missingBifCount() << '\n';
            }
            if (looseArchives_.invalidArchiveCount() != 0u) {
                text << "Invalid game archives: "
                     << looseArchives_.invalidArchiveCount() << '\n';
            }

            if (!archive_.issues().empty()) {
                text << "\nKEY/BIF Issues\n"
                     << "--------------\n";
                const std::size_t limit =
                    std::min<std::size_t>(archive_.issues().size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    const auto& issue = archive_.issues()[i];
                    text << (issue.severity == neobif::IssueSeverity::Error
                                 ? "Error: " : "Warning: ")
                         << issue.message;
                    if (issue.bifIndex) text << " (BIF " << *issue.bifIndex << ')';
                    text << '\n';
                }
                if (archive_.issues().size() > limit) {
                    text << archive_.issues().size() - limit
                         << " additional issues are not shown.\n";
                }
            }

            bool wroteLooseIssues = false;
            std::size_t looseIssueCount = 0u;
            for (const auto& looseArchive : looseArchives_.archives()) {
                if (looseArchive.valid && looseArchive.messages.empty()) continue;
                if (!wroteLooseIssues) {
                    text << "\nGame Archive Issues\n"
                         << "-------------------\n";
                    wroteLooseIssues = true;
                }
                if (looseIssueCount++ >= 100u) continue;
                text << neoshared::genericPathToUtf8(looseArchive.relativePath) << ":\n";
                if (!looseArchive.valid && looseArchive.messages.empty()) {
                    text << "  Error: archive is invalid\n";
                }
                for (const auto& message : looseArchive.messages) {
                    text << "  " << (looseArchive.valid ? "Warning: " : "Error: ")
                         << message << '\n';
                }
            }
            if (looseIssueCount > 100u) {
                text << looseIssueCount - 100u
                     << " additional archive issue groups are not shown.\n";
            }
        } else if (data->kind == NodeKind::Bif &&
                   data->ownerIndex < archive_.bifs().size()) {
            const auto& bif = archive_.bifs()[data->ownerIndex];
            text << "Stored Path: " << bif.storedPath << '\n'
                 << "Size: " << neobif::formatByteSize(bif.actualFileSize) << '\n'
                 << "Resources: " << bif.resourceIndices.size() << '\n';
        } else if (data->kind == NodeKind::LooseRoot) {
            text << "Game Archives\n"
                 << "=============\n\n"
                 << "Root: " << neoshared::pathToUtf8(scanRoot_) << '\n'
                 << "Archives: " << looseArchives_.archives().size() << '\n';
            writeSelectionSummary(selectedResources());
            if (looseArchives_.invalidArchiveCount() != 0u) {
                text << "Invalid archives: " << looseArchives_.invalidArchiveCount() << '\n';
            }
            if (!looseArchives_.messages().empty()) {
                text << "\nScan Messages\n"
                     << "-------------\n";
                const std::size_t limit =
                    std::min<std::size_t>(looseArchives_.messages().size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    text << looseArchives_.messages()[i] << '\n';
                }
                if (looseArchives_.messages().size() > limit) {
                    text << looseArchives_.messages().size() - limit
                         << " additional scan messages are not shown.\n";
                }
            }
        } else if (data->kind == NodeKind::Directory) {
            std::size_t archiveCount = 0u;
            for (const auto& looseArchive : looseArchives_.archives()) {
                if (archiveIsUnderDirectory(looseArchive.relativePath,
                                            data->directoryPath)) {
                    ++archiveCount;
                }
            }
            text << "Archive Folder\n"
                 << "==============\n\n"
                 << "Path: " << data->directoryPath << '\n'
                 << "Archives: " << archiveCount << '\n';
            writeSelectionSummary(selectedResources());
        } else if (data->kind == NodeKind::LooseArchive &&
                   data->ownerIndex < looseArchives_.archives().size()) {
            const auto& looseArchive = looseArchives_.archives()[data->ownerIndex];
            text << "Archive\n"
                 << "=======\n\n"
                 << "Path: " << neoshared::genericPathToUtf8(looseArchive.relativePath) << '\n'
                 << "Type: " << neobif::looseArchiveKindName(looseArchive.kind) << '\n'
                 << "Size: " << neobif::formatByteSize(looseArchive.actualFileSize) << '\n'
                 << "Resources: " << looseArchive.resourceIndices.size() << '\n';
            if (!looseArchive.valid || !looseArchive.messages.empty()) {
                text << "\nIssues\n"
                     << "------\n";
                if (!looseArchive.valid && looseArchive.messages.empty()) {
                    text << "Archive is invalid.\n";
                }
                const std::size_t limit =
                    std::min<std::size_t>(looseArchive.messages.size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    text << looseArchive.messages[i] << '\n';
                }
                if (looseArchive.messages.size() > limit) {
                    text << looseArchive.messages.size() - limit
                         << " additional messages are not shown.\n";
                }
            }
        } else if (data->kind == NodeKind::Type || data->kind == NodeKind::Page) {
            text << (data->kind == NodeKind::Page ? "Resource Page\n=============\n\n"
                                                  : "File Type\n=========\n\n")
                 << "Type: ." << neobif::resourceTypeExtension(data->type) << '\n';
            if (data->source == ResourceSource::KeyBif &&
                data->ownerIndex < archive_.bifs().size()) {
                text << "Source: " << archive_.bifs()[data->ownerIndex].storedPath << '\n';
            } else if (data->source == ResourceSource::LooseArchive &&
                       data->ownerIndex < looseArchives_.archives().size()) {
                text << "Source: "
                     << neoshared::genericPathToUtf8(
                            looseArchives_.archives()[data->ownerIndex].relativePath) << '\n';
            }
            writeSelectionSummary(selectedResources());
        } else if (data->kind == NodeKind::Resource) {
            text << "Resource\n"
                 << "========\n\n";
            if (data->source == ResourceSource::KeyBif &&
                data->resourceIndex < archive_.resources().size()) {
                const auto& resource = archive_.resources()[data->resourceIndex];
                text << "Filename: " << resource.fileName() << '\n'
                     << "ResRef: " << resource.resref << '\n'
                     << "Type: ." << resource.extension << '\n'
                     << "Size: " << neobif::formatByteSize(resource.size) << '\n';
                if (resource.bifIndex < archive_.bifs().size()) {
                    text << "Source: " << archive_.bifs()[resource.bifIndex].storedPath
                         << '\n';
                }
                text << "Status: " << resource.status << '\n';
            } else if (data->source == ResourceSource::LooseArchive &&
                       data->resourceIndex < looseArchives_.resources().size()) {
                const auto& resource = looseArchives_.resources()[data->resourceIndex];
                text << "Filename: " << resource.fileName() << '\n'
                     << "ResRef: " << resource.resref << '\n'
                     << "Type: ." << resource.extension << '\n'
                     << "Size: " << neobif::formatByteSize(resource.size) << '\n';
                if (resource.archiveIndex < looseArchives_.archives().size()) {
                    text << "Source: "
                         << neoshared::genericPathToUtf8(
                                looseArchives_.archives()[resource.archiveIndex].relativePath) << '\n';
                }
                text << "Status: " << resource.status << '\n';
            }
        }

        details_->ChangeValue(wxui::toWx(text.str()));
        details_->SetInsertionPoint(0);
    }

    void updateStatus() {
        if (!archive_.isOpen()) {
            setModuleStatusText(
                                "Open a KotOR game directory or chitin.key", 0);
            setModuleStatusText("No session", 1);
            return;
        }
        const std::size_t totalResources =
            archive_.resources().size() + looseArchives_.resources().size();
        const std::size_t totalExtractable =
            archive_.extractableResourceCount() + looseArchives_.extractableResourceCount();
        std::ostringstream left;
        left << resourceCountText(totalResources) << "; "
             << totalExtractable << " extractable";
        setModuleStatusText(left.str(), 0);
        std::ostringstream right;
        right << countText(archive_.bifs().size(), "BIF", "BIFs") << " | "
              << countText(looseArchives_.archives().size(),
                           "game archive", "game archives");
        if (archive_.missingBifCount() != 0u) {
            right << " | " << countText(archive_.missingBifCount(),
                                         "missing BIF", "missing BIFs");
        }
        if (looseArchives_.invalidArchiveCount() != 0u) {
            right << " | " << looseArchives_.invalidArchiveCount() << " invalid";
        }
        setModuleStatusText(right.str(), 1);
    }

    void updateCommandState() {
        if(rebuilding_)return;
        const bool open = archive_.isOpen();
        const auto selections = open ? selectedResources()
                                     : std::vector<ResourceSelection>{};
        const auto entireSelections = open ? selectedResources(false)
                                           : std::vector<ResourceSelection>{};
        const std::size_t extractableSelectionCount = static_cast<std::size_t>(std::count_if(
            selections.begin(), selections.end(),
            [this](const ResourceSelection& selection) {
                return resourceIsExtractable(selection);
            }));
        const bool selectionExtractable = extractableSelectionCount != 0u;
        const bool entireSelectionExtractable = std::any_of(
            entireSelections.begin(), entireSelections.end(),
            [this](const ResourceSelection& selection) {
                return resourceIsExtractable(selection);
            });
        const bool filterActive = !filter_->GetValue().empty();
        const bool scopeNarrowed = filterActive &&
            selections.size() < entireSelections.size();
        const bool anyExtractable = open &&
            (archive_.extractableResourceCount() != 0u ||
             looseArchives_.extractableResourceCount() != 0u);
        if (openDirectoryButton_ != nullptr) openDirectoryButton_->Enable(!jobRunning_);
        if (openFilesButton_ != nullptr) openFilesButton_->Enable(!jobRunning_);
        if (addBifsButton_ != nullptr) addBifsButton_->Enable(open && !jobRunning_);
        if (extractButton_ != nullptr) {
            extractButton_->Enable(selectionExtractable && !jobRunning_);
            if (!open || selections.empty()) {
                extractButton_->SetLabel("Extract Selection...");
                extractButton_->SetToolTip(
                    "Extract resources represented by the current tree selection. "
                    "Search narrows branch selections.");
            } else {
                const std::size_t count = selections.size();
                const std::string noun = scopeNarrowed
                    ? (count == 1u ? " Match" : " Matches")
                    : (count == 1u ? " Resource" : " Resources");
                const std::string countText = extractableSelectionCount == count
                    ? std::to_string(count)
                    : std::to_string(extractableSelectionCount) + " of " +
                        std::to_string(count);
                extractButton_->SetLabel(wxui::toWx(
                    "Extract " + countText + noun + "..."));
                std::string tip = "Extract " + std::to_string(extractableSelectionCount) +
                    (extractableSelectionCount == 1u
                        ? " extractable resource" : " extractable resources") +
                    " from the current selection.";
                if (extractableSelectionCount != count) {
                    tip += " " + std::to_string(count - extractableSelectionCount) +
                        (count - extractableSelectionCount == 1u
                            ? " unavailable resource will be omitted after confirmation."
                            : " unavailable resources will be omitted after confirmation.");
                }
                if (scopeNarrowed) tip += " Search is narrowing this branch.";
                extractButton_->SetToolTip(wxui::toWx(tip));
            }
        }
        {
            enableModuleCommand(ID_Rescan,open && !jobRunning_);
            enableModuleCommand(ID_ExtractBranch,scopeNarrowed && entireSelectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipBranch,scopeNarrowed && entireSelectionExtractable && !jobRunning_);
            enableModuleCommand(ID_AddBifs, open && !jobRunning_);
            enableModuleCommand(ID_CloseArchive, open && !jobRunning_);
            enableModuleCommand(ID_ExtractSelected, selectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ExtractAll, anyExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipSelected, selectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipAll, anyExtractable && !jobRunning_);
            enableModuleCommand(ID_ExportByType, open && !jobRunning_ &&
                (!archive_.resources().empty() || !looseArchives_.resources().empty()));
            enableModuleCommand(ID_ExpandAll, open && !jobRunning_);
            enableModuleCommand(ID_CollapseAll, open && !jobRunning_);
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
            "\n\nKotOR game archive browser and extractor."
            "\n\nNeoBIF indexes chitin.key/BIFF resources and recursively catalogs "
            "ERF, MOD, SAV, HAK, NWM, and RIM archives under the game directory. "
            "Every archive, resource type, and individual resource can be extracted "
            "to a directory or ZIP package.";
        wxMessageBox(wxui::toWx(message), "About NeoBIF",
                     wxOK | wxICON_INFORMATION, this);
    }
};

} // namespace
namespace neobif::ui {
BrowserPanel* createBrowserPanel(wxWindow* parent, neomodules::Context context) {
    return new NeoBIFPanelImpl(parent,std::move(context));
}
}
