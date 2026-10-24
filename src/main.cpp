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
#include "render.h"
#include "stroke_api.h"

namespace {

// ---------------------------------------------------------------- layout
constexpr int    kW = 1024, kH = 720;            // logical resolution
constexpr float  kCanvasX = 40, kCanvasY = 100, kCanvasS = 560;
constexpr float  kGlyphMargin = 80;              // template inset inside canvas
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

enum class Screen { Menu, Trace, Result };
enum ButtonId { kCheck, kClear, kMinus, kPlus, kPrev, kNext, kMenu, kRetry, kResultNext };

struct Button {
    SDL_FRect r;
    std::string label;
    Color bg;
    int id;
    float textH;
    bool hit(float x, float y) const { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
};

int starsFor(float score) { return score >= 85 ? 3 : score >= 65 ? 2 : score >= 40 ? 1 : 0; }

// Album grid: 9 x 4 polaroids.
constexpr float kTileW = 92, kTileH = 106, kPhotoInset = 6, kPhotoH = 72;
SDL_FRect tileRect(int i) { return {42 + (i % 9) * 106.0f, 116 + (i / 9) * 118.0f, kTileW, kTileH}; }

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
    int symbol_ = 0;
    int guideAlpha_ = 160;
    bool draggingSlider_ = false;
    float mouseX_ = 0, mouseY_ = 0;
    std::array<float, 36> best_{};
    std::array<std::vector<Stroke>, 36> bestStrokes_{};
    float highScore_ = 0.0f;
    std::vector<Button> buttons_;
    int hoverTile_ = -1;

    // strokes for the current attempt (canvas-local coordinates)
    std::vector<Stroke> strokes_;
    std::vector<size_t> inkDrawn_;
    bool penDown_ = false;
    bool evaluateRequested_ = false;
    float lastScore_ = 0;
    Uint32 snapAt_ = 0;                              // when SNAP fired (flash / develop)
    std::array<SDL_Texture*, 36> snaps_{};           // best tracing per symbol, for the album

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
    if (idx < 0 || idx >= 36 || bestStrokes_[idx].empty()) return;
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
    for (int i = 0; i < 36; ++i) {
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
                char sym;
                int nStrokes = 0;
                if (iss >> sym >> nStrokes && nStrokes > 0 && nStrokes <= 128) {
                    size_t idx = kSymbols.find(sym);
                    if (idx != std::string::npos && idx < bestStrokes_.size()) {
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
            } else if (key.size() == 1) {
                char sym = key[0];
                size_t idx = kSymbols.find(sym);
                if (idx != std::string::npos && idx < best_.size()) {
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
        for (size_t i = 0; i < kSymbols.size() && i < best_.size(); ++i) {
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
    clearCanvas();
    rebuildAllSnapshots();
    layoutButtons();
}

void App::startTrace(int index) {
    symbol_ = (index + (int)kSymbols.size()) % (int)kSymbols.size();
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
    for (const Glyph& variant : glyphVariants(kSymbols[symbol_])) {
        tmpls.push_back(glyphPath(variant, 0, 0, kCanvasS, kGlyphMargin));
    }

    lastScore_ = 0;

    char symbolChar = kSymbols[symbol_];
    bool isDigit = (symbolChar >= '0' && symbolChar <= '9');
    bool isEight = (symbolChar == '8');

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
    if (nStrokes <= 4) {
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
    if (screen_ == Screen::Trace) {
        buttons_.push_back({{640, 100, 344, 84}, "SNAP", kTeal, kCheck, 34});   // camera icon added in renderButtons
        buttons_.push_back({{640, 200, 344, 84}, "CLEAR", kMustard, kClear, 34});
        buttons_.push_back({{640, 346, 64, 64}, "-", kLilac, kMinus, 30});
        buttons_.push_back({{920, 346, 64, 64}, "+", kLilac, kPlus, 30});
        buttons_.push_back({{640, 440, 164, 70}, "< BACK", kCoral, kPrev, 24});
        buttons_.push_back({{820, 440, 164, 70}, "NEXT >", kCoral, kNext, 24});
        buttons_.push_back({{640, 576, 344, 84}, "ALBUM", kSky, kMenu, 34});
    } else if (screen_ == Screen::Result) {
        buttons_.push_back({{470, 576, 160, 84}, "AGAIN", kMustard, kRetry, 28});
        buttons_.push_back({{648, 576, 160, 84}, "NEXT", kTeal, kResultNext, 28});
        buttons_.push_back({{826, 576, 160, 84}, "ALBUM", kSky, kMenu, 28});
    }
}

void App::onButton(int id) {
    audio_.play(Audio::Cue::Click);
    switch (id) {
        case kCheck: if (!strokes_.empty()) evaluateRequested_ = true; break;
        case kClear: clearCanvas(); break;
        case kMinus: guideAlpha_ = std::max(0, guideAlpha_ - 32); break;
        case kPlus: guideAlpha_ = std::min(255, guideAlpha_ + 32); break;
        case kPrev: startTrace(symbol_ - 1); break;
        case kNext: startTrace(symbol_ + 1); break;
        case kMenu: showMenu(); break;
        case kRetry: startTrace(symbol_); break;
        case kResultNext: startTrace(symbol_ + 1); break;
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
    const float k = (float)kTexScale, inner = kCanvasS - 2 * kGlyphMargin;
    auto toTex = [&](const Point& p) {
        return Point{(kGlyphMargin + p.x * inner) * k, (kGlyphMargin + p.y * inner) * k};
    };
    const Glyph& g = glyphFor(kSymbols[symbol_]);
    std::vector<std::vector<Point>> strokes;
    for (const Stroke& s : g) {
        std::vector<Point> pts;
        for (const Point& p : s) pts.push_back(toTex(p));
        strokes.push_back(pts);
    }

    SDL_SetRenderTarget(ren_, guideTex_);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    for (const auto& pts : strokes) draw::polyline(ren_, pts, kGuideWidth * k, kGuide);
    for (const auto& pts : strokes) draw::polyline(ren_, pts, 3 * k, kGuideLine);

    std::vector<Point> dirs;
    for (const auto& s : strokes) {                   // direction chevron + start dot
        Point a = s[0], b = a;
        float walked = 0;
        for (size_t j = 1; j < s.size() && walked < 32 * k; ++j) {
            walked += std::hypot(s[j].x - s[j - 1].x, s[j].y - s[j - 1].y);
            b = s[j];
        }
        float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len > 1) { dx /= len; dy /= len; } else { dx = 0; dy = 1; }
        dirs.push_back({dx, dy});
        Point tip{a.x + dx * 40 * k, a.y + dy * 40 * k};
        float w = 9 * k, back = 12 * k;
        draw::polyline(ren_, {{tip.x - dx * back + dy * w, tip.y - dy * back - dx * w}, tip,
                              {tip.x - dx * back - dy * w, tip.y - dy * back + dx * w}},
                       4 * k, kStart);
        draw::disc(ren_, a.x, a.y, 7 * k, kStart);
    }

    // ---- numbered badges ----
    const float r = 17 * k, bandHalf = kGuideWidth * 0.5f * k, texS = kCanvasS * k;
    std::vector<Point> placed;
    auto clearance = [&](Point c) {
        float best = 1e9f;
        for (const auto& s : strokes)
            for (size_t j = 1; j < s.size(); ++j)
                best = std::min(best, distToSegment(c, s[j - 1], s[j]) - bandHalf - r);
        for (const Point& q : placed) best = std::min(best, std::hypot(c.x - q.x, c.y - q.y) - 2 * r - 6 * k);
        float edge = std::min({c.x, c.y, texS - c.x, texS - c.y}) - r - 4 * k;
        return std::min(best, edge);
    };
    const float offsets[] = {0, 45, -45, 90, -90, 135, -135, 180};
    for (size_t i = 0; i < strokes.size(); ++i) {
        Point a = strokes[i][0], d = dirs[i];
        // For downward/diagonal strokes, place the badge towards the side the stroke goes,
        // so badges sharing an apex/corner (like A or 4) don't cross over each other.
        float base = (d.y > 0.3f && std::fabs(d.x) > 0.15f)
                         ? std::atan2(-std::fabs(d.y), d.x)
                         : std::atan2(-d.y, -d.x);
        Point bestC = a;
        float bestScore = -1e9f;
        for (float dist : {r + bandHalf + 6 * k, r + bandHalf + 22 * k, r + bandHalf + 40 * k}) {
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
    for (size_t i = 0; i < placed.size(); ++i) {
        Point a = strokes[i][0], c = placed[i];
        draw::polyline(ren_, {a, c}, 3 * k, kStart);
        draw::disc(ren_, c.x, c.y, r + 2 * k, kWhite);
        draw::disc(ren_, c.x, c.y, r, kStart);
        draw::textCentered(ren_, std::string(1, (char)('1' + i)), c.x, c.y - 10 * k, 20 * k, kWhite, 0.2f);
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
            switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: if (screen_ != Screen::Menu) showMenu(); break;
                case SDLK_LEFT: if (screen_ != Screen::Menu) startTrace(symbol_ - 1); break;
                case SDLK_RIGHT: if (screen_ != Screen::Menu) startTrace(symbol_ + 1); break;
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
            if (draggingSlider_) setSliderFromX(mouseX_);
            hoverTile_ = -1;
            bool overButton = false;
            for (const Button& b : buttons_) overButton |= b.hit(mouseX_, mouseY_);
            if (screen_ == Screen::Menu) {
                for (int i = 0; i < 36; ++i) {
                    SDL_FRect t = tileRect(i);
                    if (mouseX_ >= t.x && mouseX_ < t.x + t.w && mouseY_ >= t.y && mouseY_ < t.y + t.h) hoverTile_ = i;
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
            if (e.button.button != SDL_BUTTON_LEFT) break;
            float x = (float)e.button.x, y = (float)e.button.y;
            if (screen_ == Screen::Menu) {
                if (hoverTile_ >= 0) { audio_.play(Audio::Cue::Click); startTrace(hoverTile_); }
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
        } else {
            draw::textCentered(ren_, b.label, cx, ty, b.textH, kText);
        }
    }
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
    float top = r.y + kGlyphMargin * s, base = r.y + (kCanvasS - kGlyphMargin) * s;
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
    draw::camera(ren_, x0 + iconW / 2, 30 + th / 2, iconW, kCamBody, kCamLens);
    draw::text(ren_, "LETRATO", x0 + iconW + gap, 30, th, kText, 0.14f);

    for (int i = 0; i < 36; ++i) {
        SDL_FRect t = tileRect(i);
        if (i == hoverTile_) t.y -= 4;
        Color c = kTiles[i % 6];
        draw::roundRect(ren_, {t.x + 3, tileRect(i).y + 6, t.w, t.h}, 6, kShadow);
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
    draw::textCentered(ren_, "CLICK A PHOTO TO START TRACING", kW / 2.0f, 610, 20, kTextSoft, 0.12f);
}

void App::renderTrace() {
    char c = kSymbols[symbol_];
    std::string title = std::string(c >= '0' && c <= '9' ? "TRACE THE NUMBER " : "TRACE THE LETTER ") + c;
    draw::text(ren_, title, kCanvasX, 24, 32, kText);

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
    char c = kSymbols[symbol_];
    bool digit = c >= '0' && c <= '9';
    draw::text(ren_, std::string("YOUR PHOTO OF ") + c, kCanvasX, 24, 32, kText);

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
    draw::textCentered(ren_, std::string(digit ? "NUMBER " : "LETTER ") + c, photo.x + photo.w / 2,
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
}  // namespace

int main(int, char**) {
    App app;
    if (!app.init()) { app.shutdown(); return 1; }
    app.run();
    app.shutdown();
    return 0;
}
