// pictures.h - traceable picture templates for letters A to Z.
#pragma once
#include <string>
#include <vector>
#include "glyphs.h"

struct PictureItem {
    char letter;
    std::string name;
    Glyph glyph;
    std::vector<Glyph> variants;
};

int pictureCount();
const PictureItem& pictureAt(int idx);
PictureItem& mutablePictureAt(int idx);

void setPictureGlyph(int idx, const Glyph& glyph);
void resetPictureToDefault(int idx);

bool saveCustomPictures(const std::string& filename = "pictures_custom.dat");
bool loadCustomPictures(const std::string& filename = "pictures_custom.dat");
std::string exportPictureCpp(int idx);
