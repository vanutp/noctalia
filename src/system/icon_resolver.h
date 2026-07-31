#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// One concrete icon-theme directory to search, with the metadata needed for
// size-aware selection. size == 0 means the nominal size is unknown.
struct IconSearchDir {
  std::string path;
  int size = 0;
  bool scalable = false;
  std::string theme;
};

// Icon names resolved ahead of time (off the main thread) for a single target
// size, ready to be folded into a resolver's cache. An empty path means the
// name did not resolve.
struct IconWarmBatch {
  std::uint64_t generation = 0;
  int targetSize = 0;
  std::vector<std::pair<std::string, std::string>> entries;
};

class IconResolver {
public:
  IconResolver();
  explicit IconResolver(bool cacheMissing);

  // targetSize is the intended on-screen pixel size. The first theme in
  // inheritance order that has the icon wins. When > 0, a vector (SVG) icon is
  // preferred within that theme and, among its bitmaps, the smallest size that is
  // still >= targetSize wins (falling back to the largest available) so we
  // downscale gently instead of crushing a 1024px PNG. targetSize == 0 keeps the
  // legacy "prefer scalable, then largest" behavior for callers that have no size.
  const std::string& resolve(const std::string& iconName, int targetSize = 0);
  void invalidateMissingCache();

  // Resolves `names` without touching any existing resolver, so it is safe to
  // call from a worker thread; the result is merged in with applyWarmBatch().
  static IconWarmBatch warmBatch(const std::vector<std::string>& names, int targetSize);
  // Merges pre-resolved paths into this resolver's cache. A batch built against
  // an older icon theme is discarded.
  void applyWarmBatch(const IconWarmBatch& batch);

  static bool checkThemeChanged();
  static std::uint64_t themeGeneration();
  static std::string activeThemeName();

private:
  void rebuild();
  void ensureFresh();
  std::string findIcon(const std::string& name, int targetSize) const;

  std::unordered_map<std::string, std::string> m_cache;
  std::unordered_set<std::string> m_missingCache;
  std::vector<std::string> m_baseDirs;     // XDG icon theme roots
  std::vector<IconSearchDir> m_searchDirs; // Ordered list of concrete theme dirs to search
  std::vector<std::string> m_pixmapDirs;   // XDG pixmap fallback roots
  std::string m_empty;
  std::uint64_t m_generation = 0;
  bool m_cacheMissing = false;
};
