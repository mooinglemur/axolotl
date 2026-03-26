#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

struct SpoilerItem {
    std::string name;
    std::string player;
};

struct SpoilerLocation {
    std::string name;
    std::string player;
    SpoilerItem item;
};

struct Sphere {
    int number;
    std::vector<SpoilerItem> items; // For Sphere 0
    std::vector<SpoilerLocation> locations; // For Sphere 1+
};

class SpoilerLog {
public:
    static SpoilerLog Parse(const std::string& path, const std::vector<std::string>& player_names);

    const std::vector<Sphere>& GetSpheres() const { return spheres_; }
    bool IsLoaded() const { return !spheres_.empty(); }

private:
    std::vector<Sphere> spheres_;
};
