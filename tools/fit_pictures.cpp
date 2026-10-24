#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cmath>
#include <algorithm>

struct Point { float x, y; };
using Stroke = std::vector<Point>;
using Glyph = std::vector<Stroke>;

struct Picture {
    char letter;
    std::string name;
    Glyph glyph;
};

static Stroke catmullRom(const Stroke& pts, int samples = 8) {
    if (pts.size() < 3) return pts;
    size_t n = pts.size();
    Stroke out;
    for (size_t i = 0; i + 1 < n; ++i) {
        Point p1 = pts[i];
        Point p2 = pts[i + 1];
        Point p0 = (i == 0) ? Point{2.0f * p1.x - p2.x, 2.0f * p1.y - p2.y} : pts[i - 1];
        Point p3 = (i + 2 < n) ? pts[i + 2] : Point{2.0f * p2.x - p1.x, 2.0f * p2.y - p1.y};

        for (int step = 0; step < samples; ++step) {
            float t = (float)step / (float)samples;
            float t2 = t * t, t3 = t2 * t;
            float x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                              (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                              (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
            float y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                              (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                              (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
            out.push_back({x, y});
        }
    }
    out.push_back(pts.back());
    return out;
}

Glyph buildHorseZebra() {
    // 1. Head & Neck Outline
    Stroke headPts = {
        {0.56f, 0.22f}, // poll
        {0.59f, 0.08f}, // tip of ear
        {0.53f, 0.14f}, // base of ear front
        {0.48f, 0.24f}, // forehead
        {0.38f, 0.34f}, // bridge of nose
        {0.28f, 0.44f}, // lower nose
        {0.16f, 0.50f}, // upper muzzle
        {0.13f, 0.55f}, // nose tip
        {0.15f, 0.60f}, // lower lip
        {0.22f, 0.62f}, // chin
        {0.32f, 0.58f}, // jaw curve
        {0.38f, 0.52f}, // throat
        {0.41f, 0.66f}, // front neck
        {0.43f, 0.88f}, // chest bottom
        {0.84f, 0.88f}, // back of neck base
        {0.80f, 0.64f}, // neck crest mid
        {0.72f, 0.42f}, // neck crest upper
        {0.64f, 0.26f}, // neck poll
        {0.56f, 0.22f}  // closed
    };
    Stroke outline = catmullRom(headPts, 6);

    // 2. Upright Zebra Mane (serrated along neck crest)
    Stroke mane = {
        {0.56f, 0.22f}, {0.64f, 0.20f}, {0.62f, 0.27f},
        {0.69f, 0.25f}, {0.67f, 0.34f},
        {0.75f, 0.32f}, {0.72f, 0.42f},
        {0.80f, 0.41f}, {0.77f, 0.52f},
        {0.84f, 0.52f}, {0.80f, 0.64f},
        {0.87f, 0.64f}, {0.83f, 0.76f},
        {0.88f, 0.77f}, {0.84f, 0.88f}
    };

    // 3. Eye & Face/Cheek stripe
    Stroke faceDetail = catmullRom({
        {0.44f, 0.27f}, {0.42f, 0.35f}, {0.35f, 0.46f},
        {0.37f, 0.36f}, {0.42f, 0.34f}, {0.44f, 0.36f}
    }, 6);

    // 4. Upper Neck Stripe
    Stroke sNeckUpper = catmullRom({
        {0.64f, 0.31f}, {0.54f, 0.44f}, {0.42f, 0.58f}
    }, 6);

    // 5. Lower Neck Stripe
    Stroke sNeckLower = catmullRom({
        {0.73f, 0.52f}, {0.61f, 0.65f}, {0.44f, 0.77f}
    }, 6);

    return { outline, mane, faceDetail, sNeckUpper, sNeckLower };
}

int main() {
    std::ifstream in("pictures_custom.dat");
    if (!in.is_open()) {
        std::cerr << "Failed to open pictures_custom.dat\n";
        return 1;
    }

    std::vector<Picture> pics;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string tag;
        if (!(iss >> tag)) continue;
        if (tag == "PIC") {
            char let;
            std::string name;
            int nStrokes = 0;
            if (iss >> let >> name >> nStrokes) {
                Picture pic{let, name, {}};
                for (int s = 0; s < nStrokes; ++s) {
                    if (!std::getline(in, line)) break;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    int nPts = std::stoi(line);
                    Stroke strk;
                    for (int p = 0; p < nPts; ++p) {
                        if (!std::getline(in, line)) break;
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        std::istringstream ptIss(line);
                        float x, y;
                        ptIss >> x >> y;
                        strk.push_back({x, y});
                    }
                    pic.glyph.push_back(strk);
                }
                pics.push_back(pic);
            }
        }
    }
    in.close();

    std::cout << "Loaded " << pics.size() << " pictures.\n";

    // Replace Zebra with horse-like zebra
    for (auto& pic : pics) {
        if (pic.letter == 'Z') {
            pic.glyph = buildHorseZebra();
            std::cout << "Replaced Zebra with horse-like zebra!\n";
        }
    }

    // Now auto-fit each picture to fill [0.07, 0.93] (width/height ~ 0.86), centered
    for (auto& pic : pics) {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        int count = 0;
        for (const auto& strk : pic.glyph) {
            for (const auto& p : strk) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
                count++;
            }
        }
        if (count == 0) continue;

        float w = maxX - minX;
        float h = maxY - minY;
        float cx = 0.5f * (minX + maxX);
        float cy = 0.5f * (minY + maxY);

        // Target span is 0.86
        float targetSpan = 0.86f;
        float maxDim = std::max(w, h);
        if (maxDim < 1e-4f) continue;

        float scale = targetSpan / maxDim;

        std::cout << "Pic " << pic.letter << " (" << pic.name << "): w=" << w << " h=" << h
                  << " -> scale=" << scale << "\n";

        for (auto& strk : pic.glyph) {
            for (auto& p : strk) {
                p.x = 0.5f + (p.x - cx) * scale;
                p.y = 0.5f + (p.y - cy) * scale;
                p.x = std::clamp(p.x, 0.05f, 0.95f);
                p.y = std::clamp(p.y, 0.05f, 0.95f);
            }
        }
    }

    // Write out updated pictures_custom.dat
    auto saveDat = [&](const std::string& path) {
        std::ofstream out(path);
        out << "# LetraTo Custom Pictures Data\n";
        out << "COUNT " << pics.size() << "\n";
        for (const auto& pic : pics) {
            out << "PIC " << pic.letter << " " << pic.name << " " << pic.glyph.size() << "\n";
            for (const auto& strk : pic.glyph) {
                out << strk.size() << "\n";
                for (const auto& p : strk) {
                    out << std::fixed << std::setprecision(4) << p.x << " " << p.y << "\n";
                }
            }
        }
    };
    saveDat("pictures_custom.dat");
    saveDat("build/pictures_custom.dat");

    // Write out pictures_export.cpp
    std::ofstream expOut("pictures_export.cpp");
    expOut << "// Exported picture templates from LetraTo in-app editor\n";
    expOut << "// You can paste these into src/pictures.cpp to hardcode them!\n\n";
    for (size_t i = 0; i < pics.size(); ++i) {
        const auto& pic = pics[i];
        expOut << "    // " << (i + 1) << ". " << pic.letter << " - " << pic.name << "\n";
        expOut << "    add(list, '" << pic.letter << "', \"" << pic.name << "\", {\n";
        for (size_t s = 0; s < pic.glyph.size(); ++s) {
            const auto& strk = pic.glyph[s];
            expOut << "        S(";
            if (!strk.empty()) {
                expOut << std::fixed << std::setprecision(2) << strk[0].x << "f, " << strk[0].y << "f)";
                for (size_t p = 1; p < strk.size(); ++p) {
                    expOut << ".L(" << std::fixed << std::setprecision(2) << strk[p].x << "f, " << strk[p].y << "f)";
                }
            } else {
                expOut << "0.50f, 0.50f)";
            }
            if (s + 1 < pic.glyph.size()) expOut << ",\n";
            else expOut << "\n";
        }
        expOut << "    });\n\n";
    }

    std::cout << "Done! All pictures scaled and exported.\n";
    return 0;
}
