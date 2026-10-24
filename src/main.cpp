// main.cpp - C++ / SDL2 frontend.
//
// Responsibilities (per the system design):
//   * Window creation and UI: symbol menu, guide overlay, drawing tools,
//     scoring page.
//   * Canvas event loop: mouse reports are forwarded, as they arrive, to the
//     assembly sampling routine (sample_isr) which fills a fixed buffer;
//     the frontend drains it into per-stroke arrays.
//   * Path combination: every stroke drawn until the child presses SNAP is
//     merged into one continuous path, then the raw pointer and count are
//     passed to stroke_evaluate() in stroke.dll (__cdecl).
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "audio.h"
#include "glyphs.h"
#include "pictures.h"
#include "render.h"
#include "stroke_api.h"

namespace {

// ---------------------------------------------------------------- layout
constexpr int    kW = 1024, kH = 720;            // logical resolution
constexpr float  kCanvasX = 40, kCanvasY = 100, kCanvasS = 560;
constexpr float  kGlyphMargin = 80;              // template inset inside canvas
constexpr float  kPictureMargin = 40;            // picture inset inside canvas
constexpr int    kTexScale = 2;                  // canvas textures at 2x for crisp HiDPI
constexpr float  kInkWidth = 14, kGuideWidth = 38;
constexpr float  kMinPathPx = 12;                // shorter than this = accidental click

// ---------------------------------------------------------------- palette
// "Litrato" theme: a photo-album page, white polaroid frames, washi tape,
// a camera viewfinder over the canvas and soft retro-film accent colours.
const Color kBg{244, 236, 221};                       // album page
const Color kShadow{219, 207, 186};                   // photo shadow on the page
const Color kPaper{255, 255, 255}, kPhoto{252, 250, 245}, kPaperEdge{226, 218, 204};
const Color kText{58, 54, 60}, kTextSoft{146, 134, 120}, kInk{40, 84, 170};
const Color kGuide{244, 176, 120}, kGuideLine{214, 124, 72}, kStart{86, 168, 140};
const Color kRule{212, 226, 236}, kRuleMid{238, 208, 204}, kGold{246, 190, 70};
const Color kStarOff{226, 216, 200}, kWhite{255, 255, 255}, kFinder{176, 166, 154};
const Color kTiles[] = {{250, 209, 176}, {196, 228, 204}, {184, 218, 236},   // faded film prints
                        {214, 202, 236}, {246, 200, 206}, {244, 230, 176}};
const Color kTapes[] = {{246, 168, 150, 150}, {130, 196, 186, 150}, {244, 206, 110, 150},
                        {150, 182, 228, 150}};
const Color kTeal{126, 196, 180}, kMustard{244, 200, 96}, kSky{150, 196, 232},
            kCoral{244, 164, 150}, kLilac{200, 186, 236};
const Color kCamBody{58, 54, 60}, kCamLens{150, 196, 232};

constexpr Uint32 kFlashMs = 260, kDevelopMs = 900;    // shutter flash, photo developing

Color darker(Color c, float k = 0.82f) {
    return {(Uint8)(c.r * k), (Uint8)(c.g * k), (Uint8)(c.b * k), c.a};
}

enum class Screen { Menu, Trace, Result, EditPicture };
enum ButtonId {
    kCheck, kClear, kMinus, kPlus, kPrev, kNext, kMenu, kRetry, kResultNext,
    kMenuPrevPage, kMenuPrevPageDuplicateUnused, kMenuNextPage,
    kEditPic, kEditDone, kEditPrevStroke, kEditNextStroke, kEditAddStroke,
    kEditDelStroke, kEditModeToggle, kEditSave, kEditReset,
    kEditSmooth, kEditSpline, kEditSimplify,
    kSettings, kToggleEditorSetting, kCloseSettings
};

struct Button {
    SDL_FRect r;
    std::string label;
    Color bg;
    int id;
    float textH;
    bool hit(float x, float y) const { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
};

int starsFor(float score) { return score >= 85 ? 3 : score >= 65 ? 2 : score >= 40 ? 1 : 0; }

constexpr int kSymbolCount = 36;
constexpr int kPictureCount = 26;
constexpr int kTotalSymbols = kSymbolCount + kPictureCount;

// Album grid: 9 x 4 polaroids.
constexpr float kTileW = 92, kTileH = 106, kPhotoInset = 6, kPhotoH = 72;
inline SDL_FRect tileRectPage1(int i) { return {42 + (i % 9) * 106.0f, 116 + (i / 9) * 118.0f, kTileW, kTileH}; }
inline SDL_FRect tileRectPage2(int i) {
    float y0 = 152.0f, pitchY = 138.0f;
    if (i < 18) {
        return {42 + (i % 9) * 106.0f, y0 + (i / 9) * pitchY, kTileW, kTileH};
    } else {
        int col = i - 18;
        return {95 + col * 106.0f, y0 + 2 * pitchY, kTileW, kTileH};
    }
}

class App {
public:
    bool init();
    void run();
    void shutdown();

    // Sampling hook entry points (called from the SDL event watch).
    void penDown(int x, int y);
    void penMove(int x, int y);
    void penUp(int x, int y);
    bool tracing() const { return screen_ == Screen::Trace; }
    bool isPenDown() const { return penDown_; }
    static bool inCanvas(float x, float y) {
        return x >= kCanvasX && x < kCanvasX + kCanvasS && y >= kCanvasY && y < kCanvasY + kCanvasS;
    }

private:
    // flow
    void showMenu();
    void startTrace(int index);
    void evaluate();
    void layoutButtons();
    void onButton(int id);
    // canvas
    void clearCanvas();
    void drainSamples();
    void drawNewInk();
    void rebuildGuide();
    void redrawAllInk();
    void feedSample(int x, int y);
    // events / drawing
    void handleEvent(const SDL_Event& e);
    void setSliderFromX(float x);
    void render();
    void renderMenu();
    void renderTrace();
    void renderResult();
    void renderEditPicture();
    void renderSettingsModal();
    void startEditPicture();
    Point screenToNormalized(float sx, float sy);
    Point normalizedToScreen(const Point& np);
    void pushEditUndo();
    void undoEdit();
    void renderButtons();
    void renderPhotoFrame(const SDL_FRect& r, float bottom);
    void saveSnapshot(const std::vector<Stroke>& strks);
    void dropSnapshots();
    void rebuildSnapshot(int idx);
    void rebuildAllSnapshots();
    void loadScores();
    void saveScores();

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* guideTex_ = nullptr;
    SDL_Texture* markTex_ = nullptr;                 // numbered stroke-order badges
    SDL_Texture* inkTex_ = nullptr;
    SDL_Cursor *curArrow_ = nullptr, *curHand_ = nullptr, *curCross_ = nullptr;
    Audio audio_;
    bool running_ = true;

    Screen screen_ = Screen::Menu;
    int menuPage_ = 0;
    int symbol_ = 0;
    int guideAlpha_ = 160;
    bool draggingSlider_ = false;
    float mouseX_ = 0, mouseY_ = 0;
    std::array<float, kTotalSymbols> best_{};
    std::array<std::vector<Stroke>, kTotalSymbols> bestStrokes_{};
    float highScore_ = 0.0f;
    std::vector<Button> buttons_;
    int hoverTile_ = -1;
    bool editModeEnabled_ = false;
    bool showingSettings_ = false;

    // picture editor state
    int activeStroke_ = 0;
    int hoveredStroke_ = -1;
    int hoveredPoint_ = -1;
    int hoveredMidpoint_ = -1;
    int dragStroke_ = -1;
    int dragPoint_ = -1;
    bool freehandDrawMode_ = false;
    bool freehandDrawing_ = false;
    Stroke freehandStroke_;
    Uint32 savedFeedbackUntil_ = 0;
    std::vector<Glyph> editHistory_;

    // strokes for the current attempt (canvas-local coordinates)
    std::vector<Stroke> strokes_;
    std::vector<size_t> inkDrawn_;
    bool penDown_ = false;
    bool evaluateRequested_ = false;
    float lastScore_ = 0;
    Uint32 snapAt_ = 0;                              // when SNAP fired (flash / develop)
    std::array<SDL_Texture*, kTotalSymbols> snaps_{}; // best tracing per symbol, for the album

    const SDL_FRect kTrack{720, 372, 184, 12};
};

// -------------------------------------------------------------------------
//  Sampling hook. SDL calls event watches synchronously while it pumps OS
//  input, before the event reaches the application queue - the closest
//  user-mode analogue of a mouse interrupt. Each report goes straight to
//  the assembly ISR; no per-sample work happens on the C++ side.
//  (The renderer installs its own watch first, so coordinates here are
//  already converted to the logical 1024x720 space.)
// -------------------------------------------------------------------------
int SDLCALL samplingHook(void* userdata, SDL_Event* e) {
    App* app = static_cast<App*>(userdata);
    if (!app->tracing()) return 0;
    switch (e->type) {
        case SDL_MOUSEBUTTONDOWN:
            // Touch-synthesised events are skipped: the tool trains the mouse.
            if (e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT &&
                App::inCanvas((float)e->button.x, (float)e->button.y))
                app->penDown(e->button.x, e->button.y);
            break;
        case SDL_MOUSEMOTION:
            if (e->motion.which != SDL_TOUCH_MOUSEID && app->isPenDown())
                app->penMove(e->motion.x, e->motion.y);
            break;
        case SDL_MOUSEBUTTONUP:
            if (e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT &&
                app->isPenDown())
                app->penUp(e->button.x, e->button.y);
            break;
    }
    return 0;
}

// ---------------------------------------------------------------- init
bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    win_ = SDL_CreateWindow("LetraTo", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, kW, kH,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win_) { SDL_Log("window: %s", SDL_GetError()); return false; }
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                            SDL_RENDERER_TARGETTEXTURE);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!ren_) { SDL_Log("renderer: %s", SDL_GetError()); return false; }
    SDL_RenderSetLogicalSize(ren_, kW, kH);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

    int ts = (int)kCanvasS * kTexScale;
    guideTex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, ts, ts);
    inkTex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, ts, ts);
    markTex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, ts, ts);
    if (!guideTex_ || !inkTex_ || !markTex_) { SDL_Log("texture: %s", SDL_GetError()); return false; }
    SDL_SetTextureBlendMode(guideTex_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(markTex_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(inkTex_, SDL_BLENDMODE_BLEND);

    curArrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    curHand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    curCross_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);

    if (!audio_.open()) SDL_Log("audio unavailable: %s", SDL_GetError());
    best_.fill(-1);
    loadScores();
    loadCustomPictures();
    rebuildAllSnapshots();

    SDL_AddEventWatch(samplingHook, this);   // after the renderer's own watch
    showMenu();
    return true;
}

void App::saveSnapshot(const std::vector<Stroke>& strks) {
    if (!strks.empty()) {
        bestStrokes_[symbol_] = strks;
    }
    rebuildSnapshot(symbol_);
}

void App::rebuildSnapshot(int idx) {
    if (idx < 0 || idx >= kTotalSymbols || bestStrokes_[idx].empty()) return;
    SDL_Texture*& t = snaps_[idx];
    if (!t) {
        t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 160, 160);
        if (!t) return;
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    SDL_SetRenderTarget(ren_, t);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    float scale = 160.0f / kCanvasS;
    for (const Stroke& s : bestStrokes_[idx]) {
        std::vector<Point> pts;
        pts.reserve(s.size());
        for (const Point& p : s) {
            pts.push_back({p.x * scale, p.y * scale});
        }
        draw::polyline(ren_, pts, kInkWidth * scale, kInk);
    }
    SDL_SetRenderTarget(ren_, nullptr);
}

void App::rebuildAllSnapshots() {
    for (int i = 0; i < kTotalSymbols; ++i) {
        rebuildSnapshot(i);
    }
}

void App::dropSnapshots() {
    for (SDL_Texture*& t : snaps_)
        if (t) { SDL_DestroyTexture(t); t = nullptr; }
}

void App::loadScores() {
    highScore_ = 0.0f;
    for (auto& s : bestStrokes_) s.clear();
    auto readFrom = [this](const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::istringstream iss(line);
            std::string key;
            if (!(iss >> key)) continue;
            std::string upperKey = key;
            for (char& c : upperKey) c = (char)std::toupper((unsigned char)c);
            if (!upperKey.empty() && (upperKey.back() == ':' || upperKey.back() == '=')) {
                upperKey.pop_back();
            }
            if (upperKey == "HIGH_SCORE" || upperKey == "HIGHSCORE" || upperKey == "BEST" || upperKey == "SCORE") {
                float val = 0.0f;
                if (iss >> val && std::isfinite(val) && val >= 0.0f && val <= 100.0f) {
                    highScore_ = std::max(highScore_, val);
                }
            } else if (upperKey == "SNAP" || upperKey == "STROKES") {
                std::string symStr;
                int nStrokes = 0;
                if (iss >> symStr >> nStrokes && nStrokes > 0 && nStrokes <= 128) {
                    int idx = -1;
                    if (symStr.rfind("PIC_", 0) == 0 && symStr.size() == 5) {
                        char let = (char)std::toupper((unsigned char)symStr[4]);
                        if (let >= 'A' && let <= 'Z') idx = 36 + (let - 'A');
                    } else if (symStr.size() == 1) {
                        size_t pos = kSymbols.find(symStr[0]);
                        if (pos != std::string::npos) idx = (int)pos;
                    }
                    if (idx >= 0 && idx < (int)bestStrokes_.size()) {
                        bestStrokes_[idx].clear();
                        for (int s = 0; s < nStrokes; ++s) {
                            if (!std::getline(in, line)) break;
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            std::istringstream sline(line);
                            int nPts = 0;
                            if (sline >> nPts && nPts > 0 && nPts <= 4096) {
                                Stroke strk;
                                strk.reserve(nPts);
                                for (int p = 0; p < nPts; ++p) {
                                    float x, y;
                                    if (sline >> x >> y) strk.push_back({x, y});
                                }
                                if (!strk.empty()) bestStrokes_[idx].push_back(strk);
                            }
                        }
                    }
                }
            } else if (upperKey.rfind("PIC_", 0) == 0 && upperKey.size() == 5) {
                char let = upperKey[4];
                if (let >= 'A' && let <= 'Z') {
                    int idx = 36 + (let - 'A');
                    float val = -1.0f;
                    if (iss >> val && std::isfinite(val)) {
                        if (val >= 0.0f && val <= 100.0f) {
                            best_[idx] = val;
                            highScore_ = std::max(highScore_, val);
                        } else if (val < 0.0f) {
                            best_[idx] = -1.0f;
                        }
                    }
                }
            } else if (key.size() == 1) {
                char sym = key[0];
                size_t idx = kSymbols.find(sym);
                if (idx != std::string::npos && idx < 36 && idx < best_.size()) {
                    float val = -1.0f;
                    if (iss >> val && std::isfinite(val)) {
                        if (val >= 0.0f && val <= 100.0f) {
                            best_[idx] = val;
                            highScore_ = std::max(highScore_, val);
                        } else if (val < 0.0f) {
                            best_[idx] = -1.0f;
                        }
                    }
                }
            } else {
                try {
                    size_t pos = 0;
                    float val = std::stof(key, &pos);
                    if (pos > 0 && std::isfinite(val) && val >= 0.0f && val <= 100.0f) {
                        highScore_ = std::max(highScore_, val);
                    }
                } catch (...) {}
            }
        }
        return true;
    };

    bool found = false;
    char* bp = SDL_GetBasePath();
    if (bp) {
        std::string bpPath = std::string(bp) + "scores.dat";
        SDL_free(bp);
        if (std::ifstream(bpPath).good()) {
            found = readFrom(bpPath);
        }
    }
    if (!found && std::ifstream("scores.dat").good()) {
        found = readFrom("scores.dat");
    }
    if (!found) {
        readFrom("highscore.dat");
        readFrom("scores.txt");
    }
}

void App::saveScores() {
    auto writeScores = [this](const std::string& path) {
        std::ofstream out(path);
        if (!out.is_open()) return;
        out << "HIGH_SCORE " << highScore_ << "\n";
        for (size_t i = 0; i < 36 && i < best_.size(); ++i) {
            out << kSymbols[i] << " " << best_[i] << "\n";
            const auto& strks = bestStrokes_[i];
            if (!strks.empty()) {
                out << "SNAP " << kSymbols[i] << " " << strks.size() << "\n";
                for (const auto& s : strks) {
                    out << s.size();
                    for (const auto& pt : s) {
                        out << " " << (int)std::lround(pt.x) << " " << (int)std::lround(pt.y);
                    }
                    out << "\n";
                }
            }
        }
        for (size_t i = 0; i < 26 && (36 + i) < best_.size(); ++i) {
            std::string key = "PIC_" + std::string(1, pictureAt((int)i).letter);
            out << key << " " << best_[36 + i] << "\n";
            const auto& strks = bestStrokes_[36 + i];
            if (!strks.empty()) {
                out << "SNAP " << key << " " << strks.size() << "\n";
                for (const auto& s : strks) {
                    out << s.size();
                    for (const auto& pt : s) {
                        out << " " << (int)std::lround(pt.x) << " " << (int)std::lround(pt.y);
                    }
                    out << "\n";
                }
            }
        }
    };
    writeScores("scores.dat");
    char* bp = SDL_GetBasePath();
    if (bp) {
        std::string bpPath = std::string(bp) + "scores.dat";
        SDL_free(bp);
        if (bpPath != "scores.dat") {
            writeScores(bpPath);
        }
    }
}

void App::shutdown() {
    saveScores();
    saveCustomPictures();
    SDL_DelEventWatch(samplingHook, this);
    dropSnapshots();
    audio_.close();
    if (curArrow_) SDL_FreeCursor(curArrow_);
    if (curHand_) SDL_FreeCursor(curHand_);
    if (curCross_) SDL_FreeCursor(curCross_);
    if (guideTex_) SDL_DestroyTexture(guideTex_);
    if (markTex_) SDL_DestroyTexture(markTex_);
    if (inkTex_) SDL_DestroyTexture(inkTex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    SDL_Quit();
}

void App::run() {
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);   // pumping also fires samplingHook

        if (screen_ == Screen::Trace) {
            drainSamples();
            drawNewInk();
            if (evaluateRequested_) evaluate();      // only SNAP (or Enter/Space) scores
        }
        render();
    }
}

// ---------------------------------------------------------------- flow
void App::showMenu() {
    screen_ = Screen::Menu;
    showingSettings_ = false;
    menuPage_ = (symbol_ >= 36) ? 1 : 0;
    clearCanvas();
    rebuildAllSnapshots();
    layoutButtons();
}

void App::startTrace(int index) {
    showingSettings_ = false;
    if (index < 0) {
        index = (symbol_ >= 36) ? (kTotalSymbols - 1) : (kSymbolCount - 1);
    } else if (index >= kTotalSymbols) {
        index = (symbol_ >= 36) ? kSymbolCount : 0;
    } else if (symbol_ < kSymbolCount && index >= kSymbolCount && index - symbol_ == 1) {
        index = 0;
    } else if (symbol_ >= kSymbolCount && index < kSymbolCount && symbol_ - index == 1) {
        index = kTotalSymbols - 1;
    }
    symbol_ = index;
    screen_ = Screen::Trace;
    clearCanvas();
    rebuildGuide();
    layoutButtons();
}

void App::evaluate() {
    evaluateRequested_ = false;
    if (penDown_) penUp((int)mouseX_, (int)mouseY_);

    // Filter out accidental micro-clicks/noise strokes
    std::vector<Stroke> validStrokes;
    for (const Stroke& s : strokes_) {
        if (s.size() >= 2 && stroke_path_length(s.data(), (int)s.size()) >= 3.0f) {
            validStrokes.push_back(s);
        }
    }
    if (validStrokes.empty()) validStrokes = strokes_;

    float totalLength = 0;
    for (const Stroke& s : validStrokes) {
        if (s.size() >= 2) totalLength += stroke_path_length(s.data(), (int)s.size());
    }
    if (totalLength < kMinPathPx) { clearCanvas(); return; }

    // Pre-calculate template paths for all accepted variants
    std::vector<std::vector<Point>> tmpls;
    if (symbol_ < 36) {
        for (const Glyph& variant : glyphVariants(kSymbols[symbol_])) {
            tmpls.push_back(glyphPath(variant, 0, 0, kCanvasS, kGlyphMargin));
        }
    } else {
        int picIdx = symbol_ - 36;
        for (const Glyph& variant : pictureAt(picIdx).variants) {
            tmpls.push_back(glyphPath(variant, 0, 0, kCanvasS, kPictureMargin));
        }
    }

    lastScore_ = 0;

    bool isDigit = (symbol_ < 36 && kSymbols[symbol_] >= '0' && kSymbols[symbol_] <= '9');
    bool isEight = (symbol_ < 36 && kSymbols[symbol_] == '8');

    // 8 is drawn many ways: accept the mirrored direction (start top-left,
    // go right first) and, for closed single-loop templates, any start point.
    if (isEight) {
        std::vector<std::vector<Point>> extra;
        for (const auto& t : tmpls) {
            std::vector<Point> m = t;
            for (Point& p : m) p.x = kCanvasS - p.x;
            extra.push_back(m);
        }
        tmpls.insert(tmpls.end(), extra.begin(), extra.end());
        extra.clear();
        for (const auto& t : tmpls) {
            size_t n = t.size();
            if (n < 8 || std::hypot(t[0].x - t[n - 1].x, t[0].y - t[n - 1].y) > 0.06f * kCanvasS) continue;
            std::vector<Point> loop(t.begin(), t.end() - 1);
            for (int k = 1; k < 16; ++k) {
                size_t s = loop.size() * k / 16;
                std::vector<Point> sh(loop.begin() + s, loop.end());
                sh.insert(sh.end(), loop.begin(), loop.begin() + s + 1);
                extra.push_back(sh);
            }
        }
        tmpls.insert(tmpls.end(), extra.begin(), extra.end());
    }

    // Order-independent and direction-flexible evaluation:
    // Permute stroke sequence.
    // For letters and for '8', allow forward/reverse directions for each stroke.
    // For numbers other than '8', enforce strict stroke direction (forward only).
    int nStrokes = (int)validStrokes.size();
    if (nStrokes <= 5) {
        std::vector<int> perm(nStrokes);
        std::iota(perm.begin(), perm.end(), 0);
        int maxMask = (isDigit && !isEight) ? 1 : (1 << nStrokes);

        do {
            for (int mask = 0; mask < maxMask; ++mask) {
                std::vector<Point> cand;
                for (int i = 0; i < nStrokes; ++i) {
                    const Stroke& s = validStrokes[perm[i]];
                    if (mask & (1 << i)) {
                        cand.insert(cand.end(), s.rbegin(), s.rend());
                    } else {
                        cand.insert(cand.end(), s.begin(), s.end());
                    }
                }
                for (const auto& tmpl : tmpls) {
                    float sc = stroke_evaluate(cand.data(), (int)cand.size(),
                                               tmpl.data(), (int)tmpl.size());
                    lastScore_ = std::max(lastScore_, sc);
                    if (lastScore_ >= 99.5f) break;
                }
                if (lastScore_ >= 99.5f) break;
            }
            if (lastScore_ >= 99.5f) break;
        } while (std::next_permutation(perm.begin(), perm.end()));
    } else {
        // Fallback for >4 strokes: evaluate in drawn order (and reversed if permitted)
        std::vector<Point> cand;
        for (const Stroke& s : validStrokes) cand.insert(cand.end(), s.begin(), s.end());
        for (const auto& tmpl : tmpls) {
            lastScore_ = std::max(lastScore_, stroke_evaluate(cand.data(), (int)cand.size(),
                                                              tmpl.data(), (int)tmpl.size()));
        }
        if (!isDigit || isEight) {
            cand.clear();
            for (auto it = validStrokes.rbegin(); it != validStrokes.rend(); ++it) {
                cand.insert(cand.end(), it->rbegin(), it->rend());
            }
            for (const auto& tmpl : tmpls) {
                lastScore_ = std::max(lastScore_, stroke_evaluate(cand.data(), (int)cand.size(),
                                                                  tmpl.data(), (int)tmpl.size()));
            }
        }
    }

    // Accuracy grading curve:
    // Stricter for numbers (0-7, 9) to require closer adherence to the guide.
    // More lenient for number 8 due to complex figure-8 crossing.
    if (isDigit) {
        if (isEight) {
            // More lenient: boost wobbly figure-8 curves
            lastScore_ = std::pow(lastScore_ / 100.0f, 0.6f) * 100.0f;
        } else {
            // Stricter: require tighter tracing for digits
            lastScore_ = std::pow(lastScore_ / 100.0f, 1.22f) * 100.0f;
        }
        lastScore_ = std::clamp(lastScore_, 0.0f, 100.0f);
    }

    if (lastScore_ >= best_[symbol_]) {
        best_[symbol_] = lastScore_;
        saveSnapshot(validStrokes);   // best photo goes in the album
    }
    if (lastScore_ > highScore_) highScore_ = lastScore_;
    saveScores();
    snapAt_ = SDL_GetTicks();
    int stars = starsFor(lastScore_);
    audio_.play(stars >= 3 ? Audio::Cue::Great : stars >= 2 ? Audio::Cue::Good : Audio::Cue::TryAgain);
    screen_ = Screen::Result;
    layoutButtons();
}

void App::layoutButtons() {
    buttons_.clear();
    if (showingSettings_) {
        float mw = 440, mh = 260;
        float bx = kW / 2.0f - mw / 2, by = kH / 2.0f - mh / 2;
        buttons_.push_back({{bx + 40, by + 120, mw - 80, 52},
                            editModeEnabled_ ? "PICTURE EDITOR: ON" : "PICTURE EDITOR: OFF",
                            editModeEnabled_ ? kTeal : kLilac,
                            kToggleEditorSetting, 22});
        buttons_.push_back({{bx + mw / 2 - 60, by + mh - 56, 120, 42},
                            "CLOSE", kSky, kCloseSettings, 24});
        return;
    }
    if (screen_ == Screen::Menu) {
        buttons_.push_back({{40, 24, 64, 54}, "<", menuPage_ == 0 ? kLilac : kCoral, kMenuPrevPage, 32});
        buttons_.push_back({{kW - 40 - 136, 24, 64, 54}, ">", menuPage_ == 1 ? kLilac : kTeal, kMenuNextPage, 32});
        buttons_.push_back({{kW - 40 - 64, 24, 64, 54}, "", kLilac, kSettings, 20});
    } else if (screen_ == Screen::Trace) {
        buttons_.push_back({{640, 100, 344, 84}, "SNAP", kTeal, kCheck, 34});   // camera icon added in renderButtons
        buttons_.push_back({{640, 200, 344, 84}, "CLEAR", kMustard, kClear, 34});
        buttons_.push_back({{640, 346, 64, 64}, "-", kLilac, kMinus, 30});
        buttons_.push_back({{920, 346, 64, 64}, "+", kLilac, kPlus, 30});
        buttons_.push_back({{640, 440, 164, 70}, "< BACK", kCoral, kPrev, 24});
        buttons_.push_back({{820, 440, 164, 70}, "NEXT >", kCoral, kNext, 24});
        if (symbol_ >= 36 && editModeEnabled_) {
            buttons_.push_back({{640, 576, 164, 84}, "EDIT", kCoral, kEditPic, 28});
            buttons_.push_back({{820, 576, 164, 84}, "ALBUM", kSky, kMenu, 28});
        } else {
            buttons_.push_back({{640, 576, 344, 84}, "ALBUM", kSky, kMenu, 34});
        }
    } else if (screen_ == Screen::EditPicture) {
        buttons_.push_back({{640, 96, 344, 62}, "DONE / TRACE", kTeal, kEditDone, 26});
        buttons_.push_back({{640, 168, 164, 52}, "< STROKE", kLilac, kEditPrevStroke, 20});
        buttons_.push_back({{820, 168, 164, 52}, "STROKE >", kLilac, kEditNextStroke, 20});
        buttons_.push_back({{640, 226, 164, 52}, "+ ADD STROKE", kSky, kEditAddStroke, 18});
        buttons_.push_back({{820, 226, 164, 52}, "- DEL STROKE", kLilac, kEditDelStroke, 18});
        std::string modeTxt = freehandDrawMode_ ? "TWEAK NODES" : "DRAW WITH MOUSE";
        Color modeCol = freehandDrawMode_ ? kCoral : kMustard;
        buttons_.push_back({{640, 284, 344, 52}, modeTxt, modeCol, kEditModeToggle, 20});
        buttons_.push_back({{640, 342, 164, 52}, "SMOOTH (C)", kLilac, kEditSmooth, 18});
        buttons_.push_back({{820, 342, 164, 52}, "BEND SPLINE (K)", kLilac, kEditSpline, 18});
        buttons_.push_back({{640, 400, 164, 52}, "SIMPLIFY (V)", kMustard, kEditSimplify, 18});
        buttons_.push_back({{820, 400, 164, 52}, "RESET DEFAULT", kCoral, kEditReset, 18});
        buttons_.push_back({{640, 458, 344, 52}, "SAVE TO FILE", kSky, kEditSave, 20});
    } else if (screen_ == Screen::Result) {
        buttons_.push_back({{470, 576, 160, 84}, "AGAIN", kMustard, kRetry, 28});
        buttons_.push_back({{648, 576, 160, 84}, "NEXT", kTeal, kResultNext, 28});
        buttons_.push_back({{826, 576, 160, 84}, "ALBUM", kSky, kMenu, 28});
    }
}

static Stroke smoothStroke(const Stroke& s, int iterations = 1) {
    if (s.size() < 3) return s;
    Stroke cur = s;
    for (int iter = 0; iter < iterations; ++iter) {
        Stroke next;
        next.reserve(cur.size() * 2);
        next.push_back(cur.front());
        for (size_t i = 0; i + 1 < cur.size(); ++i) {
            Point p0 = cur[i];
            Point p1 = cur[i + 1];
            Point q{0.75f * p0.x + 0.25f * p1.x, 0.75f * p0.y + 0.25f * p1.y};
            Point r{0.25f * p0.x + 0.75f * p1.x, 0.25f * p0.y + 0.75f * p1.y};
            next.push_back(q);
            next.push_back(r);
        }
        next.push_back(cur.back());
        cur = std::move(next);
    }
    return cur;
}

static Stroke catmullRomSpline(const Stroke& pts, int samplesPerSegment = 8) {
    if (pts.size() < 3) return pts;
    size_t n = pts.size();
    Stroke out;
    out.reserve((n - 1) * samplesPerSegment + 1);

    for (size_t i = 0; i + 1 < n; ++i) {
        Point p1 = pts[i];
        Point p2 = pts[i + 1];
        Point p0 = (i == 0) ? Point{2.0f * p1.x - p2.x, 2.0f * p1.y - p2.y} : pts[i - 1];
        Point p3 = (i + 2 < n) ? pts[i + 2] : Point{2.0f * p2.x - p1.x, 2.0f * p2.y - p1.y};

        for (int step = 0; step < samplesPerSegment; ++step) {
            float t = (float)step / (float)samplesPerSegment;
            float t2 = t * t;
            float t3 = t2 * t;

            float x = 0.5f * ((2.0f * p1.x) +
                              (-p0.x + p2.x) * t +
                              (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                              (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
            float y = 0.5f * ((2.0f * p1.y) +
                              (-p0.y + p2.y) * t +
                              (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                              (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);

            out.push_back({std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)});
        }
    }
    out.push_back(pts.back());
    return out;
}

static void ramerDouglasPeucker(const Stroke& pts, size_t start, size_t end, float epsilon, std::vector<bool>& keep) {
    if (end <= start + 1) return;
    float maxDist = 0.0f;
    size_t maxIdx = start;
    Point a = pts[start], b = pts[end];
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;

    for (size_t i = start + 1; i < end; ++i) {
        Point p = pts[i];
        float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
        float d = std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
        if (d > maxDist) {
            maxDist = d;
            maxIdx = i;
        }
    }

    if (maxDist > epsilon) {
        keep[maxIdx] = true;
        ramerDouglasPeucker(pts, start, maxIdx, epsilon, keep);
        ramerDouglasPeucker(pts, maxIdx, end, epsilon, keep);
    }
}

static Stroke simplifyStroke(const Stroke& pts, float epsilon = 0.035f) {
    if (pts.size() <= 3) return pts;
    std::vector<bool> keep(pts.size(), false);
    keep.front() = true;
    keep.back() = true;
    ramerDouglasPeucker(pts, 0, pts.size() - 1, epsilon, keep);
    Stroke out;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (keep[i]) out.push_back(pts[i]);
    }
    return out;
}

void App::onButton(int id) {
    audio_.play(Audio::Cue::Click);
    switch (id) {
        case kMenuPrevPage:
        case kMenuNextPage:
            menuPage_ = (menuPage_ == 0) ? 1 : 0;
            layoutButtons();
            break;
        case kCheck: if (!strokes_.empty()) evaluateRequested_ = true; break;
        case kClear: clearCanvas(); break;
        case kMinus: guideAlpha_ = std::max(0, guideAlpha_ - 32); break;
        case kPlus: guideAlpha_ = std::min(255, guideAlpha_ + 32); break;
        case kPrev: startTrace(symbol_ - 1); break;
        case kNext: startTrace(symbol_ + 1); break;
        case kMenu: showMenu(); break;
        case kRetry: startTrace(symbol_); break;
        case kResultNext: startTrace(symbol_ + 1); break;
        case kSettings:
            showingSettings_ = true;
            layoutButtons();
            break;
        case kToggleEditorSetting:
            editModeEnabled_ = !editModeEnabled_;
            layoutButtons();
            break;
        case kCloseSettings:
            showingSettings_ = false;
            layoutButtons();
            break;
        case kEditPic: startEditPicture(); break;
        case kEditDone:
            screen_ = Screen::Trace;
            rebuildGuide();
            layoutButtons();
            saveCustomPictures();
            break;
        case kEditPrevStroke: {
            if (symbol_ >= 36) {
                int n = (int)pictureAt(symbol_ - 36).glyph.size();
                if (n > 0) activeStroke_ = (activeStroke_ - 1 + n) % n;
            }
            break;
        }
        case kEditNextStroke: {
            if (symbol_ >= 36) {
                int n = (int)pictureAt(symbol_ - 36).glyph.size();
                if (n > 0) activeStroke_ = (activeStroke_ + 1) % n;
            }
            break;
        }
        case kEditAddStroke: {
            if (symbol_ >= 36) {
                pushEditUndo();
                auto& pic = mutablePictureAt(symbol_ - 36);
                pic.glyph.push_back({ {0.40f, 0.50f}, {0.60f, 0.50f} });
                pic.variants = { pic.glyph };
                activeStroke_ = (int)pic.glyph.size() - 1;
            }
            break;
        }
        case kEditDelStroke: {
            if (symbol_ >= 36) {
                auto& pic = mutablePictureAt(symbol_ - 36);
                if (pic.glyph.size() > 1) {
                    pushEditUndo();
                    pic.glyph.erase(pic.glyph.begin() + activeStroke_);
                    if (activeStroke_ >= (int)pic.glyph.size()) activeStroke_ = (int)pic.glyph.size() - 1;
                    pic.variants = { pic.glyph };
                }
            }
            break;
        }
        case kEditModeToggle:
            freehandDrawMode_ = !freehandDrawMode_;
            freehandDrawing_ = false;
            layoutButtons();
            break;
        case kEditSave:
            saveCustomPictures();
            savedFeedbackUntil_ = SDL_GetTicks() + 1800;
            audio_.play(Audio::Cue::Great);
            break;
        case kEditReset:
            if (symbol_ >= 36) {
                pushEditUndo();
                resetPictureToDefault(symbol_ - 36);
                activeStroke_ = 0;
                audio_.play(Audio::Cue::Click);
            }
            break;
        case kEditSmooth: {
            if (symbol_ >= 36) {
                auto& pic = mutablePictureAt(symbol_ - 36);
                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size() && pic.glyph[activeStroke_].size() >= 3) {
                    pushEditUndo();
                    pic.glyph[activeStroke_] = smoothStroke(pic.glyph[activeStroke_], 1);
                    pic.variants = { pic.glyph };
                    audio_.play(Audio::Cue::Click);
                }
            }
            break;
        }
        case kEditSpline: {
            if (symbol_ >= 36) {
                auto& pic = mutablePictureAt(symbol_ - 36);
                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size() && pic.glyph[activeStroke_].size() >= 3) {
                    pushEditUndo();
                    pic.glyph[activeStroke_] = catmullRomSpline(pic.glyph[activeStroke_], 8);
                    pic.variants = { pic.glyph };
                    audio_.play(Audio::Cue::Click);
                }
            }
            break;
        }
        case kEditSimplify: {
            if (symbol_ >= 36) {
                auto& pic = mutablePictureAt(symbol_ - 36);
                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size() && pic.glyph[activeStroke_].size() > 3) {
                    pushEditUndo();
                    pic.glyph[activeStroke_] = simplifyStroke(pic.glyph[activeStroke_], 0.035f);
                    pic.variants = { pic.glyph };
                    audio_.play(Audio::Cue::Click);
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- sampling
void App::feedSample(int x, int y) {
    // canvas-local, clamped so a stroke that wanders off the paper stays on it
    int lx = std::clamp(x - (int)kCanvasX, 0, (int)kCanvasS - 1);
    int ly = std::clamp(y - (int)kCanvasY, 0, (int)kCanvasS - 1);
    sample_isr(lx, ly);
}

void App::penDown(int x, int y) {
    sample_reset();
    strokes_.emplace_back();
    inkDrawn_.push_back(0);
    penDown_ = true;
    feedSample(x, y);
}

void App::penMove(int x, int y) { feedSample(x, y); }

void App::penUp(int x, int y) {
    feedSample(x, y);
    drainSamples();
    penDown_ = false;
}

void App::drainSamples() {
    if (strokes_.empty()) { sample_reset(); return; }
    Point chunk[512];
    int n;
    while ((n = sample_drain(chunk, 512)) > 0) {
        Stroke& s = strokes_.back();
        s.insert(s.end(), chunk, chunk + n);
        if (n < 512) break;
    }
}

void App::clearCanvas() {
    strokes_.clear();
    inkDrawn_.clear();
    penDown_ = false;
    evaluateRequested_ = false;
    sample_reset();
    SDL_SetRenderTarget(ren_, inkTex_);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    SDL_SetRenderTarget(ren_, nullptr);
}

static std::vector<Point> scaled(const std::vector<Point>& pts, size_t from) {
    std::vector<Point> out;
    for (size_t i = from; i < pts.size(); ++i) out.push_back({pts[i].x * kTexScale, pts[i].y * kTexScale});
    return out;
}

void App::drawNewInk() {
    bool any = false;
    for (size_t i = 0; i < strokes_.size(); ++i)
        if (inkDrawn_[i] < strokes_[i].size()) any = true;
    if (!any) return;
    SDL_SetRenderTarget(ren_, inkTex_);
    for (size_t i = 0; i < strokes_.size(); ++i) {
        const Stroke& s = strokes_[i];
        if (inkDrawn_[i] >= s.size()) continue;
        size_t from = inkDrawn_[i] > 0 ? inkDrawn_[i] - 1 : 0;   // join to last drawn point
        draw::polyline(ren_, scaled(s, from), kInkWidth * kTexScale, kInk);
        inkDrawn_[i] = s.size();
    }
    SDL_SetRenderTarget(ren_, nullptr);
}

void App::redrawAllInk() {
    SDL_SetRenderTarget(ren_, inkTex_);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    SDL_SetRenderTarget(ren_, nullptr);
    std::fill(inkDrawn_.begin(), inkDrawn_.end(), 0);
    drawNewInk();
}

static float distToSegment(Point p, Point a, Point b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Guide texture: wide band, thin centre line, and a small dot + chevron on
// each stroke's start. Marker texture: numbered badges placed in clear space
// next to each start (never on top of the band or of another badge), joined
// to their start by a short line, so shared starts like the apex of A or
// the corner of E stay readable.
void App::rebuildGuide() {
    bool isPic = (symbol_ >= 36);
    float margin = isPic ? kPictureMargin : kGlyphMargin;
    const float k = (float)kTexScale, inner = kCanvasS - 2 * margin;
    auto toTex = [&](const Point& p) {
        return Point{(margin + p.x * inner) * k, (margin + p.y * inner) * k};
    };
    Glyph g;
    if (!isPic) {
        g = glyphFor(kSymbols[symbol_]);
    } else {
        g = pictureAt(symbol_ - 36).glyph;
    }
    std::vector<std::vector<Point>> strokes;
    for (const Stroke& s : g) {
        std::vector<Point> pts;
        for (const Point& p : s) pts.push_back(toTex(p));
        strokes.push_back(pts);
    }

    SDL_SetRenderTarget(ren_, guideTex_);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    float guideW = isPic ? (22.0f * k) : (kGuideWidth * k);
    float centerW = isPic ? (2.5f * k) : (3.0f * k);
    for (const auto& pts : strokes) draw::polyline(ren_, pts, guideW, kGuide);
    for (const auto& pts : strokes) draw::polyline(ren_, pts, centerW, kGuideLine);

    std::vector<Point> dirs;
    for (const auto& s : strokes) {                   // direction chevron + start dot
        if (s.empty()) continue;
        Point a = s[0];
        float startDotR = isPic ? (5.5f * k) : (7.0f * k);
        draw::disc(ren_, a.x, a.y, startDotR, kStart);

        float totalLen = 0.0f;
        for (size_t j = 1; j < s.size(); ++j) {
            totalLen += std::hypot(s[j].x - s[j - 1].x, s[j].y - s[j - 1].y);
        }

        Point dir{0, 1};
        float walkedInit = 0.0f;
        for (size_t j = 1; j < s.size(); ++j) {
            float d = std::hypot(s[j].x - s[j - 1].x, s[j].y - s[j - 1].y);
            walkedInit += d;
            if (walkedInit >= 12.0f * k || j == s.size() - 1) {
                float dx = s[j].x - a.x, dy = s[j].y - a.y;
                float l = std::hypot(dx, dy);
                if (l > 1e-3f) dir = {dx / l, dy / l};
                break;
            }
        }
        dirs.push_back(dir);

        float targetWalk = isPic ? std::min(22.0f * k, totalLen * 0.45f) : std::min(34.0f * k, totalLen * 0.45f);
        if (totalLen >= 10.0f * k) {
            float curWalk = 0.0f;
            Point tip = a;
            Point segDir = dir;
            for (size_t j = 1; j < s.size(); ++j) {
                float d = std::hypot(s[j].x - s[j - 1].x, s[j].y - s[j - 1].y);
                if (curWalk + d >= targetWalk || j == s.size() - 1) {
                    float rem = targetWalk - curWalk;
                    float frac = (d > 1e-4f) ? std::clamp(rem / d, 0.0f, 1.0f) : 0.0f;
                    tip = {s[j - 1].x + (s[j].x - s[j - 1].x) * frac,
                           s[j - 1].y + (s[j].y - s[j - 1].y) * frac};
                    float dx = s[j].x - s[j - 1].x, dy = s[j].y - s[j - 1].y;
                    float l = std::hypot(dx, dy);
                    if (l > 1e-3f) segDir = {dx / l, dy / l};
                    break;
                }
                curWalk += d;
            }
            float w = isPic ? (6.5f * k) : (9.0f * k);
            float back = isPic ? (9.0f * k) : (12.0f * k);
            float chevThick = isPic ? (3.0f * k) : (4.0f * k);
            draw::polyline(ren_,
                           {{tip.x - segDir.x * back + segDir.y * w, tip.y - segDir.y * back - segDir.x * w},
                            tip,
                            {tip.x - segDir.x * back - segDir.y * w, tip.y - segDir.y * back + segDir.x * w}},
                           chevThick, kStart);
        }
    }

    // ---- numbered badges ----
    const float r = isPic ? (13.0f * k) : (17.0f * k);
    const float bandHalf = guideW * 0.5f;
    const float texS = kCanvasS * k;
    std::vector<Point> placed;
    auto clearance = [&](Point c) {
        float best = 1e9f;
        for (const auto& s : strokes)
            for (size_t j = 1; j < s.size(); ++j)
                best = std::min(best, distToSegment(c, s[j - 1], s[j]) - bandHalf - r);
        for (const Point& q : placed) best = std::min(best, std::hypot(c.x - q.x, c.y - q.y) - 2 * r - 4 * k);
        float edge = std::min({c.x, c.y, texS - c.x, texS - c.y}) - r - 4 * k;
        return std::min(best, edge);
    };
    const float offsets[] = {0, 45, -45, 90, -90, 135, -135, 180};
    for (size_t i = 0; i < strokes.size(); ++i) {
        Point a = strokes[i][0], d = dirs[i];
        float base = (d.y > 0.3f && std::fabs(d.x) > 0.15f)
                         ? std::atan2(-std::fabs(d.y), d.x)
                         : std::atan2(-d.y, -d.x);
        Point bestC = a;
        float bestScore = -1e9f;
        std::vector<float> dists = isPic
            ? std::vector<float>{r + bandHalf + 2.0f * k, r + bandHalf + 8.0f * k, r + bandHalf + 16.0f * k}
            : std::vector<float>{r + bandHalf + 6.0f * k, r + bandHalf + 22.0f * k, r + bandHalf + 40.0f * k};
        for (float dist : dists) {
            for (float off : offsets) {
                float ang = base + off * 3.14159265f / 180.0f;
                Point c{a.x + std::cos(ang) * dist, a.y + std::sin(ang) * dist};
                float sc = clearance(c);
                if (sc > bestScore) { bestScore = sc; bestC = c; }
                if (sc >= 0) break;
            }
            if (bestScore >= 0) break;
        }
        placed.push_back(bestC);
    }

    SDL_SetRenderTarget(ren_, markTex_);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    float fontH = isPic ? (15.0f * k) : (20.0f * k);
    for (size_t i = 0; i < placed.size(); ++i) {
        Point a = strokes[i][0], c = placed[i];
        float leaderDist = std::hypot(c.x - a.x, c.y - a.y);
        if (leaderDist > r + 3.0f * k) {
            draw::polyline(ren_, {a, c}, 2.5f * k, kStart);
        }
        draw::disc(ren_, c.x, c.y, r + 2 * k, kWhite);
        draw::disc(ren_, c.x, c.y, r, kStart);
        std::string numStr = std::to_string(i + 1);
        draw::textCentered(ren_, numStr, c.x, c.y - fontH * 0.5f, fontH, kWhite, 0.2f);
    }
    SDL_SetRenderTarget(ren_, nullptr);
}

// ---------------------------------------------------------------- events
void App::setSliderFromX(float x) {
    float t = std::clamp((x - kTrack.x) / kTrack.w, 0.0f, 1.0f);
    guideAlpha_ = (int)std::lround(t * 255);
}

void App::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: running_ = false; break;

        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:                 // GPU lost our textures
            dropSnapshots();
            rebuildAllSnapshots();
            if (screen_ != Screen::Menu) { rebuildGuide(); redrawAllInk(); }
            break;

        case SDL_KEYDOWN:
            if (screen_ == Screen::EditPicture) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: onButton(kEditDone); break;
                    case SDLK_s: onButton(kEditSave); break;
                    case SDLK_r: onButton(kEditReset); break;
                    case SDLK_d: onButton(kEditModeToggle); break;
                    case SDLK_c: onButton(kEditSmooth); break;
                    case SDLK_k: onButton(kEditSpline); break;
                    case SDLK_v: onButton(kEditSimplify); break;
                    case SDLK_TAB: onButton(kEditNextStroke); break;
                    case SDLK_z: undoEdit(); break;
                    case SDLK_DELETE:
                    case SDLK_BACKSPACE: onButton(kEditDelStroke); break;
                }
                break;
            }
            switch (e.key.keysym.sym) {
                case SDLK_e:
                    if (screen_ == Screen::Trace && symbol_ >= 36 && editModeEnabled_) {
                        startEditPicture();
                    }
                    break;
                case SDLK_ESCAPE:
                    if (showingSettings_) {
                        showingSettings_ = false;
                        layoutButtons();
                    } else if (screen_ != Screen::Menu) {
                        showMenu();
                    }
                    break;
                case SDLK_LEFT:
                    if (screen_ != Screen::Menu) startTrace(symbol_ - 1);
                    else onButton(kMenuPrevPage);
                    break;
                case SDLK_RIGHT:
                    if (screen_ != Screen::Menu) startTrace(symbol_ + 1);
                    else onButton(kMenuNextPage);
                    break;
                case SDLK_RETURN:
                case SDLK_SPACE:
                    if (screen_ == Screen::Trace && !strokes_.empty()) evaluateRequested_ = true;
                    else if (screen_ == Screen::Result) startTrace(symbol_);
                    break;
                case SDLK_BACKSPACE:
                case SDLK_DELETE: if (screen_ == Screen::Trace) clearCanvas(); break;
            }
            break;

        case SDL_MOUSEMOTION: {
            mouseX_ = (float)e.motion.x;
            mouseY_ = (float)e.motion.y;
            if (screen_ == Screen::EditPicture) {
                if (freehandDrawing_) {
                    Point np = screenToNormalized(mouseX_, mouseY_);
                    if (freehandStroke_.empty() ||
                        std::hypot(mouseX_ - normalizedToScreen(freehandStroke_.back()).x,
                                   mouseY_ - normalizedToScreen(freehandStroke_.back()).y) >= 8.0f) {
                        freehandStroke_.push_back(np);
                    }
                } else if (dragPoint_ >= 0 && dragStroke_ >= 0) {
                    Point np = screenToNormalized(mouseX_, mouseY_);
                    auto& pic = mutablePictureAt(symbol_ - 36);
                    if (dragStroke_ < (int)pic.glyph.size() && dragPoint_ < (int)pic.glyph[dragStroke_].size()) {
                        pic.glyph[dragStroke_][dragPoint_] = np;
                        pic.variants = { pic.glyph };
                    }
                } else {
                    hoveredStroke_ = -1;
                    hoveredPoint_ = -1;
                    hoveredMidpoint_ = -1;
                    if (!freehandDrawMode_ && symbol_ >= 36) {
                        const auto& pic = pictureAt(symbol_ - 36);
                        float closestDist = 14.0f;
                        for (size_t sIdx = 0; sIdx < pic.glyph.size(); ++sIdx) {
                            for (size_t pIdx = 0; pIdx < pic.glyph[sIdx].size(); ++pIdx) {
                                Point sp = normalizedToScreen(pic.glyph[sIdx][pIdx]);
                                float d = std::hypot(mouseX_ - sp.x, mouseY_ - sp.y);
                                if (d < closestDist) {
                                    closestDist = d;
                                    hoveredStroke_ = (int)sIdx;
                                    hoveredPoint_ = (int)pIdx;
                                }
                            }
                        }
                        if (hoveredPoint_ < 0 && activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size()) {
                            const auto& strk = pic.glyph[activeStroke_];
                            for (size_t j = 1; j < strk.size(); ++j) {
                                Point pA = normalizedToScreen(strk[j - 1]);
                                Point pB = normalizedToScreen(strk[j]);
                                Point mid{0.5f * (pA.x + pB.x), 0.5f * (pA.y + pB.y)};
                                if (std::hypot(mouseX_ - mid.x, mouseY_ - mid.y) < 14.0f) {
                                    hoveredMidpoint_ = (int)j;
                                    break;
                                }
                            }
                        }
                    }
                }
                bool overButton = false;
                for (const Button& b : buttons_) overButton |= b.hit(mouseX_, mouseY_);
                if (overButton || hoveredPoint_ >= 0 || hoveredMidpoint_ >= 0) SDL_SetCursor(curHand_);
                else if (inCanvas(mouseX_, mouseY_)) SDL_SetCursor(curCross_);
                else SDL_SetCursor(curArrow_);
                break;
            }
            if (draggingSlider_) setSliderFromX(mouseX_);
            hoverTile_ = -1;
            bool overButton = false;
            for (const Button& b : buttons_) overButton |= b.hit(mouseX_, mouseY_);
            if (screen_ == Screen::Menu && !showingSettings_) {
                if (menuPage_ == 0) {
                    for (int i = 0; i < 36; ++i) {
                        SDL_FRect t = tileRectPage1(i);
                        if (mouseX_ >= t.x && mouseX_ < t.x + t.w && mouseY_ >= t.y && mouseY_ < t.y + t.h) hoverTile_ = i;
                    }
                } else {
                    for (int i = 0; i < 26; ++i) {
                        SDL_FRect t = tileRectPage2(i);
                        if (mouseX_ >= t.x && mouseX_ < t.x + t.w && mouseY_ >= t.y && mouseY_ < t.y + t.h) hoverTile_ = 36 + i;
                    }
                }
            }
            bool overSlider = screen_ == Screen::Trace && mouseX_ >= kTrack.x - 12 &&
                              mouseX_ <= kTrack.x + kTrack.w + 12 && std::fabs(mouseY_ - (kTrack.y + 6)) < 22;
            if (screen_ == Screen::Trace && inCanvas(mouseX_, mouseY_)) SDL_SetCursor(curCross_);
            else if (overButton || hoverTile_ >= 0 || overSlider) SDL_SetCursor(curHand_);
            else SDL_SetCursor(curArrow_);
            break;
        }

        case SDL_MOUSEBUTTONDOWN: {
            float x = (float)e.button.x, y = (float)e.button.y;
            if (screen_ == Screen::EditPicture) {
                bool hitBtn = false;
                for (const Button& b : buttons_) {
                    if (b.hit(x, y)) { onButton(b.id); hitBtn = true; break; }
                }
                if (!hitBtn && inCanvas(x, y)) {
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        if (freehandDrawMode_) {
                            freehandDrawing_ = true;
                            freehandStroke_.clear();
                            freehandStroke_.push_back(screenToNormalized(x, y));
                            pushEditUndo();
                        } else {
                            if (hoveredPoint_ >= 0 && hoveredStroke_ >= 0) {
                                activeStroke_ = hoveredStroke_;
                                dragStroke_ = hoveredStroke_;
                                dragPoint_ = hoveredPoint_;
                                pushEditUndo();
                            } else if (hoveredMidpoint_ >= 1) {
                                auto& pic = mutablePictureAt(symbol_ - 36);
                                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size()) {
                                    auto& strk = pic.glyph[activeStroke_];
                                    if (hoveredMidpoint_ < (int)strk.size()) {
                                        pushEditUndo();
                                        Point newPt = screenToNormalized(x, y);
                                        strk.insert(strk.begin() + hoveredMidpoint_, newPt);
                                        pic.variants = { pic.glyph };
                                        dragStroke_ = activeStroke_;
                                        dragPoint_ = hoveredMidpoint_;
                                        hoveredStroke_ = activeStroke_;
                                        hoveredPoint_ = hoveredMidpoint_;
                                        hoveredMidpoint_ = -1;
                                        audio_.play(Audio::Cue::Click);
                                    }
                                }
                            } else {
                                auto& pic = mutablePictureAt(symbol_ - 36);
                                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size()) {
                                    auto& strk = pic.glyph[activeStroke_];
                                    int insertIdx = -1;
                                    Point insertPt;
                                    for (size_t j = 1; j < strk.size(); ++j) {
                                        Point pA = normalizedToScreen(strk[j - 1]);
                                        Point pB = normalizedToScreen(strk[j]);
                                        float d = distToSegment({x, y}, pA, pB);
                                        if (d < 12.0f) {
                                            insertIdx = (int)j;
                                            insertPt = screenToNormalized(x, y);
                                            break;
                                        }
                                    }
                                    if (insertIdx >= 0) {
                                        pushEditUndo();
                                        strk.insert(strk.begin() + insertIdx, insertPt);
                                        pic.variants = { pic.glyph };
                                        dragStroke_ = activeStroke_;
                                        dragPoint_ = insertIdx;
                                        hoveredStroke_ = activeStroke_;
                                        hoveredPoint_ = insertIdx;
                                        audio_.play(Audio::Cue::Click);
                                    }
                                }
                            }
                        }
                    } else if (e.button.button == SDL_BUTTON_RIGHT) {
                        if (hoveredPoint_ >= 0 && hoveredStroke_ >= 0) {
                            auto& pic = mutablePictureAt(symbol_ - 36);
                            if (hoveredStroke_ < (int)pic.glyph.size() && pic.glyph[hoveredStroke_].size() > 2) {
                                pushEditUndo();
                                pic.glyph[hoveredStroke_].erase(pic.glyph[hoveredStroke_].begin() + hoveredPoint_);
                                pic.variants = { pic.glyph };
                                hoveredPoint_ = -1;
                                hoveredStroke_ = -1;
                                audio_.play(Audio::Cue::Click);
                            }
                        }
                    }
                }
                break;
            }
            if (e.button.button != SDL_BUTTON_LEFT) break;
            if (showingSettings_) {
                bool hitBtn = false;
                for (const Button& b : buttons_) {
                    if (b.hit(x, y)) { onButton(b.id); hitBtn = true; break; }
                }
                if (!hitBtn) {
                    float mw = 440, mh = 260;
                    SDL_FRect box{kW / 2.0f - mw / 2, kH / 2.0f - mh / 2, mw, mh};
                    if (x < box.x || x > box.x + box.w || y < box.y || y > box.y + box.h) {
                        showingSettings_ = false;
                        layoutButtons();
                    }
                }
                break;
            }
            if (screen_ == Screen::Menu) {
                bool hitBtn = false;
                for (const Button& b : buttons_) {
                    if (b.hit(x, y)) { onButton(b.id); hitBtn = true; break; }
                }
                if (!hitBtn && hoverTile_ >= 0) {
                    audio_.play(Audio::Cue::Click);
                    startTrace(hoverTile_);
                }
                break;
            }
            if (screen_ == Screen::Trace && x >= kTrack.x - 12 && x <= kTrack.x + kTrack.w + 12 &&
                std::fabs(y - (kTrack.y + 6)) < 22) {
                draggingSlider_ = true;
                setSliderFromX(x);
                break;
            }
            for (const Button& b : buttons_)
                if (b.hit(x, y)) { onButton(b.id); break; }
            break;
        }

        case SDL_MOUSEBUTTONUP:
            if (screen_ == Screen::EditPicture) {
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (freehandDrawing_) {
                        freehandDrawing_ = false;
                        if (freehandStroke_.size() >= 2) {
                            Stroke simp;
                            simp.push_back(freehandStroke_.front());
                            for (size_t i = 1; i + 1 < freehandStroke_.size(); ++i) {
                                Point prev = simp.back();
                                if (std::hypot(freehandStroke_[i].x - prev.x, freehandStroke_[i].y - prev.y) >= 0.04f) {
                                    simp.push_back(freehandStroke_[i]);
                                }
                            }
                            simp.push_back(freehandStroke_.back());
                            if (simp.size() >= 2) {
                                auto& pic = mutablePictureAt(symbol_ - 36);
                                if (activeStroke_ >= 0 && activeStroke_ < (int)pic.glyph.size()) {
                                    pic.glyph[activeStroke_] = simp;
                                } else {
                                    pic.glyph.push_back(simp);
                                    activeStroke_ = (int)pic.glyph.size() - 1;
                                }
                                pic.variants = { pic.glyph };
                            }
                        }
                        freehandStroke_.clear();
                    }
                    if (dragPoint_ >= 0) {
                        dragPoint_ = -1;
                        dragStroke_ = -1;
                    }
                }
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT) draggingSlider_ = false;
            break;
    }
}

// ---------------------------------------------------------------- drawing
void App::render() {
    SDL_SetRenderDrawColor(ren_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(ren_);
    switch (screen_) {
        case Screen::Menu: renderMenu(); break;
        case Screen::Trace: renderTrace(); break;
        case Screen::Result: renderResult(); break;
        case Screen::EditPicture: renderEditPicture(); break;
    }
    if (showingSettings_) {
        renderSettingsModal();
    }
    SDL_RenderPresent(ren_);
}

void App::renderButtons() {
    for (const Button& b : buttons_) {
        bool hover = b.hit(mouseX_, mouseY_);
        float lift = hover ? 4.0f : 0.0f;
        SDL_FRect shadow{b.r.x, b.r.y + 6, b.r.w, b.r.h};
        draw::roundRect(ren_, shadow, 18, darker(b.bg, 0.75f));
        SDL_FRect face{b.r.x, b.r.y - lift, b.r.w, b.r.h};
        draw::roundRect(ren_, face, 18, b.bg);
        float cx = b.r.x + b.r.w / 2, ty = face.y + (b.r.h - b.textH) / 2;
        if (b.id == kCheck) {                         // SNAP: camera + label
            float iconW = b.textH * 1.4f, gap = b.textH * 0.4f;
            float total = iconW + gap + draw::textWidth(b.label, b.textH);
            float x0 = cx - total / 2;
            draw::camera(ren_, x0 + iconW / 2, face.y + b.r.h / 2, iconW, kCamBody, kWhite);
            draw::text(ren_, b.label, x0 + iconW + gap, ty, b.textH, kText);
        } else if (b.id == kSettings) {
            float rad = b.r.h * 0.28f;
            draw::gear(ren_, cx, face.y + b.r.h / 2, rad, kText);
        } else {
            draw::textCentered(ren_, b.label, cx, ty, b.textH, kText);
        }
    }
}

void App::renderSettingsModal() {
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren_, 40, 36, 48, 140);
    SDL_FRect fullscreen{0, 0, (float)kW, (float)kH};
    SDL_RenderFillRectF(ren_, &fullscreen);

    float mw = 440, mh = 260;
    SDL_FRect box{kW / 2.0f - mw / 2, kH / 2.0f - mh / 2, mw, mh};
    draw::roundRect(ren_, {box.x + 4, box.y + 8, mw, mh}, 16, kShadow);
    draw::roundRect(ren_, box, 16, kPaper);
    draw::roundRect(ren_, {box.x - 1, box.y - 1, mw + 2, mh + 2}, 16, kPaperEdge);

    draw::gear(ren_, box.x + 42, box.y + 38, 14, kText);
    draw::text(ren_, "SETTINGS", box.x + 66, box.y + 26, 24, kText);

    draw::text(ren_, "ENABLE PICTURE EDITOR", box.x + 40, box.y + 74, 16, kText);
    draw::text(ren_, "SHOWS EDIT BUTTON ON PICTURE TRACING SCREEN", box.x + 40, box.y + 98, 11, kTextSoft);

    renderButtons();
}

// A polaroid: white frame with a deeper bottom strip, the photo area inside
// with handwriting rules, and viewfinder brackets marking the shot.
void App::renderPhotoFrame(const SDL_FRect& r, float bottom) {
    float s = r.w / kCanvasS, m = 14 * s;
    SDL_FRect frame{r.x - m, r.y - m, r.w + 2 * m, r.h + m + bottom};
    draw::roundRect(ren_, {frame.x + 4, frame.y + 7, frame.w, frame.h}, 8, kShadow);
    draw::roundRect(ren_, frame, 8, kPaper);
    draw::roundRect(ren_, {r.x - 1, r.y - 1, r.w + 2, r.h + 2}, 4, kPaperEdge);
    draw::roundRect(ren_, r, 4, kPhoto);
    float margin = (symbol_ < 36) ? kGlyphMargin : kPictureMargin;
    float top = r.y + margin * s, base = r.y + (kCanvasS - margin) * s;
    float mid = (top + base) / 2, x0 = r.x + 24 * s, x1 = r.x + r.w - 24 * s;
    draw::polyline(ren_, {{x0, top}, {x1, top}}, 2 * s, kRule);
    draw::polyline(ren_, {{x0, base}, {x1, base}}, 3 * s, kRule);
    for (float x = x0; x < x1; x += 22 * s)
        draw::polyline(ren_, {{x, mid}, {std::min(x + 11 * s, x1), mid}}, 2 * s, kRuleMid);
    draw::viewfinder(ren_, r, 16 * s, 34 * s, 4 * s, kFinder);
}

void App::renderMenu() {
    // title: camera + LETRATO
    float th = 44, iconW = 60, gap = 18;
    float total = iconW + gap + draw::textWidth("LETRATO", th), x0 = kW / 2.0f - total / 2;
    draw::camera(ren_, x0 + iconW / 2, 26 + th / 2, iconW, kCamBody, kCamLens);
    draw::text(ren_, "LETRATO", x0 + iconW + gap, 26, th, kText, 0.14f);

    std::string pageIndicator = (menuPage_ == 0) ? "PAGE 1 OF 2  -  LETTERS & NUMBERS"
                                                 : "PAGE 2 OF 2  -  PICTURES (A - Z)";
    float badgeW = draw::textWidth(pageIndicator, 16) + 36.0f;
    SDL_FRect badgeRect{kW / 2.0f - badgeW / 2.0f, 76, badgeW, 28};
    draw::roundRect(ren_, {badgeRect.x + 2, badgeRect.y + 3, badgeRect.w, badgeRect.h}, 14, kShadow);
    draw::roundRect(ren_, badgeRect, 14, kPaper);
    draw::textCentered(ren_, pageIndicator, kW / 2.0f, 82, 16, kText, 0.13f);

    renderButtons();

    if (menuPage_ == 0) {
        for (int i = 0; i < 36; ++i) {
            SDL_FRect t = tileRectPage1(i);
            if (i == hoverTile_) t.y -= 4;
            Color c = kTiles[i % 6];
            draw::roundRect(ren_, {t.x + 3, tileRectPage1(i).y + 6, t.w, t.h}, 6, kShadow);
            draw::roundRect(ren_, t, 6, kPaper);
            SDL_FRect photo{t.x + kPhotoInset, t.y + kPhotoInset, t.w - 2 * kPhotoInset, kPhotoH};
            draw::roundRect(ren_, photo, 3, c);
            std::string sym(1, kSymbols[i]);
            if (!snaps_[i] && !bestStrokes_[i].empty()) {
                rebuildSnapshot(i);
            }
            if (snaps_[i]) {                              // the child's own best tracing
                draw::textCentered(ren_, sym, photo.x + photo.w / 2, photo.y + 14, 44, darker(c, 0.88f), 0.15f);
                SDL_FRect img{photo.x + (photo.w - kPhotoH) / 2, photo.y, kPhotoH, kPhotoH};
                SDL_RenderCopyF(ren_, snaps_[i], nullptr, &img);
            } else {
                draw::textCentered(ren_, sym, photo.x + photo.w / 2, photo.y + 14, 44, kText, 0.15f);
            }
            int stars = best_[i] < 0 ? 0 : starsFor(best_[i]);
            for (int s = 0; s < 3; ++s)
                draw::star(ren_, t.x + 30 + s * 16, t.y + kPhotoInset + kPhotoH + 14, 7,
                           s < stars ? kGold : kStarOff);
            draw::tape(ren_, t.x + t.w / 2, t.y + 1, 36, 12, (i % 2) ? 7.0f : -7.0f, kTapes[i % 4]);
        }
    } else {
        for (int i = 0; i < 26; ++i) {
            int globalIdx = 36 + i;
            SDL_FRect t = tileRectPage2(i);
            if (globalIdx == hoverTile_) t.y -= 4;
            Color c = kTiles[i % 6];
            draw::roundRect(ren_, {t.x + 3, tileRectPage2(i).y + 6, t.w, t.h}, 6, kShadow);
            draw::roundRect(ren_, t, 6, kPaper);
            SDL_FRect photo{t.x + kPhotoInset, t.y + kPhotoInset, t.w - 2 * kPhotoInset, kPhotoH};
            draw::roundRect(ren_, photo, 3, c);
            const auto& pic = pictureAt(i);
            if (!snaps_[globalIdx] && !bestStrokes_[globalIdx].empty()) {
                rebuildSnapshot(globalIdx);
            }
            float side = 48.0f;
            float ox = photo.x + (photo.w - side) * 0.5f;
            float oy = photo.y + 12.0f;
            if (snaps_[globalIdx]) {                              // the child's own best tracing
                draw::text(ren_, std::string(1, pic.letter), photo.x + 5, photo.y + 3, 13, darker(c, 0.70f), 0.16f);
                SDL_FRect img{ox, oy, side, side};
                SDL_RenderCopyF(ren_, snaps_[globalIdx], nullptr, &img);
                draw::textCentered(ren_, pic.name, photo.x + photo.w * 0.5f, photo.y + photo.h - 12, 11, darker(c, 0.55f), 0.14f);
            } else {
                Color lineCol = darker(c, 0.65f);
                for (const Stroke& st : pic.glyph) {
                    std::vector<Point> pts;
                    pts.reserve(st.size());
                    for (const Point& p : st) {
                        pts.push_back({ox + p.x * side, oy + p.y * side});
                    }
                    draw::polyline(ren_, pts, 2.5f, lineCol);
                }
                draw::text(ren_, std::string(1, pic.letter), photo.x + 5, photo.y + 3, 13, kText, 0.16f);
                draw::textCentered(ren_, pic.name, photo.x + photo.w * 0.5f, photo.y + photo.h - 12, 11, darker(c, 0.55f), 0.14f);
            }
            int stars = best_[globalIdx] < 0 ? 0 : starsFor(best_[globalIdx]);
            for (int s = 0; s < 3; ++s)
                draw::star(ren_, t.x + 30 + s * 16, t.y + kPhotoInset + kPhotoH + 14, 7,
                           s < stars ? kGold : kStarOff);
            draw::tape(ren_, t.x + t.w / 2, t.y + 1, 36, 12, (i % 2) ? 7.0f : -7.0f, kTapes[i % 4]);
        }
    }

    if (hoverTile_ >= 36) {
        const auto& pic = pictureAt(hoverTile_ - 36);
        draw::textCentered(ren_, "CLICK TO TRACE: " + pic.name, kW / 2.0f, 616, 20, kText, 0.14f);
    } else if (hoverTile_ >= 0) {
        char c = kSymbols[hoverTile_];
        std::string desc = std::string(c >= '0' && c <= '9' ? "NUMBER " : "LETTER ") + c;
        draw::textCentered(ren_, "CLICK TO TRACE: " + desc, kW / 2.0f, 616, 20, kText, 0.14f);
    } else {
        const char* tip = (menuPage_ == 0)
                              ? "CLICK A PHOTO TO START TRACING  |  PRESS > FOR PICTURES"
                              : "CLICK A PICTURE TO START TRACING  |  PRESS < FOR LETTERS";
        draw::textCentered(ren_, tip, kW / 2.0f, 616, 18, kTextSoft, 0.12f);
    }
}

void App::renderTrace() {
    std::string title;
    if (symbol_ < 36) {
        char c = kSymbols[symbol_];
        title = std::string(c >= '0' && c <= '9' ? "TRACE THE NUMBER " : "TRACE THE LETTER ") + c;
    } else {
        const auto& pic = pictureAt(symbol_ - 36);
        title = "TRACE THE PICTURE: " + pic.name;
    }
    draw::text(ren_, title, kCanvasX, 24, 30, kText);

    SDL_FRect canvas{kCanvasX, kCanvasY, kCanvasS, kCanvasS};
    renderPhotoFrame(canvas, 34);
    SDL_SetTextureAlphaMod(guideTex_, (Uint8)guideAlpha_);
    SDL_RenderCopyF(ren_, guideTex_, nullptr, &canvas);
    SDL_RenderCopyF(ren_, inkTex_, nullptr, &canvas);
    // stroke-order badges stay readable whenever the guide is visible at all
    SDL_SetTextureAlphaMod(markTex_, guideAlpha_ == 0 ? 0 : (Uint8)std::max(guideAlpha_, 210));
    SDL_RenderCopyF(ren_, markTex_, nullptr, &canvas);
    draw::tape(ren_, kCanvasX - 4, kCanvasY - 6, 64, 20, -35, kTapes[0]);
    draw::tape(ren_, kCanvasX + kCanvasS + 4, kCanvasY - 6, 64, 20, 35, kTapes[1]);

    // no timer: strokes keep adding up until the child presses SNAP
    if (!penDown_ && !strokes_.empty())
        draw::textCentered(ren_, "PRESS SNAP WHEN DONE", kCanvasX + kCanvasS / 2, 667, 16, kTextSoft, 0.12f);

    renderButtons();
    float symScore = best_[symbol_];
    int hsPct = symScore < 0.0f ? 0 : (int)std::lround(symScore);
    draw::textCentered(ren_, "HIGH SCORE " + std::to_string(hsPct) + "%", 812, 54, 22, kText);
    draw::textCentered(ren_, "GUIDE", 812, 312, 22, kText);
    draw::roundRect(ren_, kTrack, 6, kStarOff);
    float t = guideAlpha_ / 255.0f;
    draw::roundRect(ren_, {kTrack.x, kTrack.y, std::max(12.0f, kTrack.w * t), kTrack.h}, 6, kGuide);
    draw::disc(ren_, kTrack.x + kTrack.w * t, kTrack.y + 6, 15, darker(kGuide, 0.85f));
    draw::disc(ren_, kTrack.x + kTrack.w * t, kTrack.y + 6, 11, kWhite);
}

void App::renderResult() {
    std::string title, sub;
    if (symbol_ < 36) {
        char c = kSymbols[symbol_];
        bool digit = c >= '0' && c <= '9';
        title = std::string("YOUR PHOTO OF ") + c;
        sub = std::string(digit ? "NUMBER " : "LETTER ") + c;
    } else {
        const auto& pic = pictureAt(symbol_ - 36);
        title = "YOUR PHOTO OF " + pic.name;
        sub = pic.name;
    }
    draw::text(ren_, title, kCanvasX, 24, 32, kText);

    // the developed polaroid
    SDL_FRect photo{80, 120, 360, 360};
    renderPhotoFrame(photo, 70);
    SDL_SetTextureAlphaMod(guideTex_, 140);
    SDL_RenderCopyF(ren_, guideTex_, nullptr, &photo);
    SDL_RenderCopyF(ren_, inkTex_, nullptr, &photo);
    Uint32 since = SDL_GetTicks() - snapAt_;
    if (since < kDevelopMs) {                         // image fades up like an instant photo
        float t = (float)since / kDevelopMs;
        SDL_SetRenderDrawColor(ren_, 120, 106, 92, (Uint8)(235 * (1 - t) * (1 - t)));
        SDL_RenderFillRectF(ren_, &photo);
    }
    draw::textCentered(ren_, sub, photo.x + photo.w / 2,
                       photo.y + photo.h + 22, 28, kInk, 0.12f);
    draw::tape(ren_, photo.x + photo.w / 2, photo.y - 14, 110, 26, -4, kTapes[2]);

    int pct = (int)std::lround(lastScore_);
    draw::textCentered(ren_, std::to_string(pct) + "%", 728, 140, 110, kText, 0.12f);
    int stars = starsFor(lastScore_);
    for (int s = 0; s < 3; ++s)
        draw::star(ren_, 618 + s * 110, 360, 44, s < stars ? kGold : kStarOff);
    const char* msg = stars == 3 ? "AMAZING!" : stars == 2 ? "GREAT JOB!" : stars == 1 ? "GOOD TRY!" : "KEEP TRYING!";
    draw::textCentered(ren_, msg, 728, 432, 40, kInk);
    renderButtons();

    if (since < kFlashMs) {                           // camera flash
        float t = (float)since / kFlashMs;
        SDL_SetRenderDrawColor(ren_, 255, 255, 255, (Uint8)(230 * (1 - t)));
        SDL_FRect all{0, 0, (float)kW, (float)kH};
        SDL_RenderFillRectF(ren_, &all);
    }
}

void App::startEditPicture() {
    if (symbol_ < 36) return;
    screen_ = Screen::EditPicture;
    activeStroke_ = 0;
    hoveredStroke_ = -1;
    hoveredPoint_ = -1;
    hoveredMidpoint_ = -1;
    dragStroke_ = -1;
    dragPoint_ = -1;
    freehandDrawMode_ = false;
    freehandDrawing_ = false;
    freehandStroke_.clear();
    savedFeedbackUntil_ = 0;
    editHistory_.clear();
    pushEditUndo();
    layoutButtons();
}

Point App::normalizedToScreen(const Point& np) {
    float margin = (symbol_ < 36) ? kGlyphMargin : kPictureMargin;
    const float inner = kCanvasS - 2 * margin;
    return { kCanvasX + margin + np.x * inner, kCanvasY + margin + np.y * inner };
}

Point App::screenToNormalized(float sx, float sy) {
    float margin = (symbol_ < 36) ? kGlyphMargin : kPictureMargin;
    const float inner = kCanvasS - 2 * margin;
    float nx = std::clamp((sx - (kCanvasX + margin)) / inner, 0.0f, 1.0f);
    float ny = std::clamp((sy - (kCanvasY + margin)) / inner, 0.0f, 1.0f);
    return { nx, ny };
}

void App::pushEditUndo() {
    if (symbol_ < 36) return;
    int idx = symbol_ - 36;
    editHistory_.push_back(pictureAt(idx).glyph);
    if (editHistory_.size() > 30) editHistory_.erase(editHistory_.begin());
}

void App::undoEdit() {
    if (editHistory_.empty() || symbol_ < 36) return;
    int idx = symbol_ - 36;
    setPictureGlyph(idx, editHistory_.back());
    editHistory_.pop_back();
    int nStrokes = (int)pictureAt(idx).glyph.size();
    if (activeStroke_ >= nStrokes) {
        activeStroke_ = std::max(0, nStrokes - 1);
    }
    audio_.play(Audio::Cue::Click);
}

void App::renderEditPicture() {
    if (symbol_ < 36) return;
    int picIdx = symbol_ - 36;
    const auto& pic = pictureAt(picIdx);

    // Title and status
    std::string title = "PICTURE EDITOR: " + pic.name;
    draw::text(ren_, title, kCanvasX, 24, 28, kText);

    if (freehandDrawMode_) {
        draw::text(ren_, "DRAW MODE: CLICK & DRAG ON CANVAS TO DRAW STROKE", kCanvasX, 60, 16, kCoral, 0.12f);
    } else {
        draw::text(ren_, "DRAG NODES TO TWEAK  |  CLICK LINE TO ADD NODE  |  RIGHT-CLICK TO REMOVE", kCanvasX, 60, 15, kTextSoft, 0.12f);
    }

    // Canvas frame
    SDL_FRect canvas{kCanvasX, kCanvasY, kCanvasS, kCanvasS};
    renderPhotoFrame(canvas, 34);

    // Grid lines for precision
    const float inner = kCanvasS - 2 * kPictureMargin;
    for (int g = 1; g < 8; ++g) {
        float frac = g / 8.0f;
        float gx = kCanvasX + kPictureMargin + frac * inner;
        float gy = kCanvasY + kPictureMargin + frac * inner;
        Color gridCol = (g == 4) ? Color{200, 192, 184, 160} : Color{220, 216, 210, 100};
        draw::polyline(ren_, {{gx, kCanvasY + kPictureMargin}, {gx, kCanvasY + kCanvasS - kPictureMargin}}, 1.0f, gridCol);
        draw::polyline(ren_, {{kCanvasX + kPictureMargin, gy}, {kCanvasX + kCanvasS - kPictureMargin, gy}}, 1.0f, gridCol);
    }

    // Render strokes
    for (size_t sIdx = 0; sIdx < pic.glyph.size(); ++sIdx) {
        const Stroke& s = pic.glyph[sIdx];
        if (s.empty()) continue;

        std::vector<Point> screenPts;
        screenPts.reserve(s.size());
        for (const Point& p : s) {
            screenPts.push_back(normalizedToScreen(p));
        }

        bool isActive = ((int)sIdx == activeStroke_);
        Color lineCol = isActive ? Color{235, 104, 88} : Color{150, 165, 185};
        float lineThick = isActive ? 5.0f : 3.0f;

        draw::polyline(ren_, screenPts, lineThick, lineCol);

        // Direction chevron along the stroke
        if (screenPts.size() >= 2) {
            Point a = screenPts[0];
            float totalLen = 0.0f;
            for (size_t j = 1; j < screenPts.size(); ++j)
                totalLen += std::hypot(screenPts[j].x - screenPts[j - 1].x, screenPts[j].y - screenPts[j - 1].y);
            float targetWalk = std::min(22.0f, totalLen * 0.45f);
            float curWalk = 0.0f;
            Point tip = a, segDir{0, 1};
            for (size_t j = 1; j < screenPts.size(); ++j) {
                float d = std::hypot(screenPts[j].x - screenPts[j - 1].x, screenPts[j].y - screenPts[j - 1].y);
                if (curWalk + d >= targetWalk || j == screenPts.size() - 1) {
                    float frac = (d > 1e-4f) ? std::clamp((targetWalk - curWalk) / d, 0.0f, 1.0f) : 0.0f;
                    tip = {screenPts[j - 1].x + (screenPts[j].x - screenPts[j - 1].x) * frac,
                           screenPts[j - 1].y + (screenPts[j].y - screenPts[j - 1].y) * frac};
                    float dx = screenPts[j].x - screenPts[j - 1].x, dy = screenPts[j].y - screenPts[j - 1].y;
                    float l = std::hypot(dx, dy);
                    if (l > 1e-3f) segDir = {dx / l, dy / l};
                    break;
                }
                curWalk += d;
            }
            float w = 6.0f, back = 8.0f;
            draw::polyline(ren_,
                           {{tip.x - segDir.x * back + segDir.y * w, tip.y - segDir.y * back - segDir.x * w},
                            tip,
                            {tip.x - segDir.x * back - segDir.y * w, tip.y - segDir.y * back + segDir.x * w}},
                           2.5f, lineCol);
        }

        // Start dot and badge
        Point startPt = screenPts[0];
        draw::disc(ren_, startPt.x, startPt.y, 6.0f, kWhite);
        draw::disc(ren_, startPt.x, startPt.y, 4.5f, lineCol);
        draw::disc(ren_, startPt.x - 14.0f, startPt.y - 14.0f, 10.0f, kWhite);
        draw::disc(ren_, startPt.x - 14.0f, startPt.y - 14.0f, 8.5f, lineCol);
        draw::textCentered(ren_, std::to_string(sIdx + 1), startPt.x - 14.0f, startPt.y - 20.0f, 12.0f, kWhite, 0.15f);

        // Control point handles and segment midpoint bend handles for the active stroke
        if (isActive && !freehandDrawMode_) {
            // Draw segment midpoint handles ("Drag to bend")
            for (size_t j = 1; j < screenPts.size(); ++j) {
                Point pA = screenPts[j - 1];
                Point pB = screenPts[j];
                Point mid{0.5f * (pA.x + pB.x), 0.5f * (pA.y + pB.y)};
                bool isMidHover = (hoveredMidpoint_ == (int)j);
                if (isMidHover) {
                    draw::disc(ren_, mid.x, mid.y, 9.0f, kMustard);
                    draw::disc(ren_, mid.x, mid.y, 6.5f, kWhite);
                    draw::disc(ren_, mid.x, mid.y, 4.0f, kCoral);
                    draw::text(ren_, "DRAG TO BEND", mid.x + 12.0f, mid.y - 12.0f, 13.0f, kText, 0.14f);
                } else {
                    draw::disc(ren_, mid.x, mid.y, 4.5f, Color{255, 255, 255, 200});
                    draw::disc(ren_, mid.x, mid.y, 3.0f, Color{235, 104, 88, 200});
                }
            }

            for (size_t pIdx = 0; pIdx < screenPts.size(); ++pIdx) {
                Point pt = screenPts[pIdx];
                bool isHovered = (hoveredStroke_ == (int)sIdx && hoveredPoint_ == (int)pIdx);
                bool isDragged = (dragStroke_ == (int)sIdx && dragPoint_ == (int)pIdx);

                if (isHovered || isDragged) {
                    draw::disc(ren_, pt.x, pt.y, 10.0f, kMustard);
                    draw::disc(ren_, pt.x, pt.y, 7.5f, kWhite);
                    draw::disc(ren_, pt.x, pt.y, 5.0f, kCoral);

                    // Coordinate label
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f)", s[pIdx].x, s[pIdx].y);
                    draw::text(ren_, buf, pt.x + 12.0f, pt.y - 14.0f, 13.0f, kText, 0.14f);
                } else {
                    draw::disc(ren_, pt.x, pt.y, 6.0f, kWhite);
                    draw::disc(ren_, pt.x, pt.y, 4.5f, kCoral);
                }
            }
        }
    }

    // Live preview during freehand drawing
    if (freehandDrawing_ && freehandStroke_.size() >= 2) {
        std::vector<Point> livePts;
        livePts.reserve(freehandStroke_.size());
        for (const Point& p : freehandStroke_) livePts.push_back(normalizedToScreen(p));
        draw::polyline(ren_, livePts, 4.5f, kInk);
    }

    // Buttons in sidebar
    renderButtons();

    // Sidebar status information
    int totalStrokes = (int)pic.glyph.size();
    int curStrokePts = (activeStroke_ >= 0 && activeStroke_ < totalStrokes) ? (int)pic.glyph[activeStroke_].size() : 0;
    std::string strokeInfo = "STROKE " + std::to_string(activeStroke_ + 1) + " / " + std::to_string(totalStrokes) +
                             " (" + std::to_string(curStrokePts) + " PTS)";
    draw::textCentered(ren_, strokeInfo, 812, 514, 17, kText, 0.14f);

    if (savedFeedbackUntil_ > SDL_GetTicks()) {
        draw::roundRect(ren_, {640, 536, 344, 30}, 8, kTeal);
        draw::textCentered(ren_, "CHANGES SAVED TO FILE!", 812, 542, 16, kWhite, 0.12f);
    }

    // Keyboard shortcut help box
    SDL_FRect helpBox{640, 570, 344, 110};
    draw::roundRect(ren_, helpBox, 8, Color{242, 238, 230});
    draw::text(ren_, "CURVING & SHORTCUTS:", 652, 576, 13, kText, 0.14f);
    draw::text(ren_, "* [C] SMOOTH CURVE    * [K] BEND SPLINE", 652, 594, 12, kTextSoft, 0.12f);
    draw::text(ren_, "* [V] SIMPLIFY POINTS * [D] DRAW MODE", 652, 612, 12, kTextSoft, 0.12f);
    draw::text(ren_, "* [TAB] NEXT STROKE   * [S] SAVE FILE", 652, 630, 12, kTextSoft, 0.12f);
    draw::text(ren_, "* [Z] UNDO            * [ESC] DONE", 652, 648, 12, kTextSoft, 0.12f);
    draw::text(ren_, "* DRAG LINE MIDPOINT TO BEND INTO CURVE", 652, 664, 12, Color{210, 90, 70}, 0.12f);
}
}  // namespace

int main(int, char**) {
    App app;
    if (!app.init()) { app.shutdown(); return 1; }
    app.run();
    app.shutdown();
    return 0;
}
