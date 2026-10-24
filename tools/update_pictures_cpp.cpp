#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

int main() {
    std::ifstream inExp("pictures_export.cpp");
    if (!inExp.is_open()) {
        std::cerr << "Failed to open pictures_export.cpp\n";
        return 1;
    }
    std::string expContent;
    std::string line;
    // Skip header lines
    while (std::getline(inExp, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("// 1. A - APPLE") != std::string::npos) {
            expContent += line + "\n";
            break;
        }
    }
    while (std::getline(inExp, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        expContent += line + "\n";
    }
    inExp.close();

    std::ifstream inSrc("src/pictures.cpp");
    if (!inSrc.is_open()) {
        std::cerr << "Failed to open src/pictures.cpp\n";
        return 1;
    }
    std::string srcBefore, srcAfter;
    bool inReplace = false;
    bool doneReplace = false;
    while (std::getline(inSrc, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!doneReplace) {
            if (line.find("// 1. A - APPLE") != std::string::npos) {
                inReplace = true;
            }
            if (!inReplace) {
                srcBefore += line + "\n";
            } else {
                if (line.find("return list;") != std::string::npos) {
                    inReplace = false;
                    doneReplace = true;
                    srcAfter += line + "\n";
                }
            }
        } else {
            srcAfter += line + "\n";
        }
    }
    inSrc.close();

    if (!doneReplace) {
        std::cerr << "Could not find start/end markers in src/pictures.cpp\n";
        return 1;
    }

    std::string total = srcBefore + expContent + "\n    " + srcAfter;
    std::ofstream outSrc("src/pictures.cpp");
    outSrc << total;
    outSrc.close();

    std::cout << "Successfully updated src/pictures.cpp!\n";
    return 0;
}
