import type { ScanModule } from "./types";

export type PluginGrouping = "type" | "vendor";
export interface CatalogPlugin {
  key: string;
  path: string;
  classId: string;
  name: string;
  vendor: string;
  version: string;
  categories: string[];
  source: string;
  searchText: string;
}
export interface PluginGroup {
  name: string;
  plugins: CatalogPlugin[];
}

const collator = new Intl.Collator("zh-TW", { numeric: true, sensitivity: "base" });
const compare = (a: string, b: string) => collator.compare(a, b) || (a < b ? -1 : a > b ? 1 : 0);
const normalize = (text: string) => text.normalize("NFKC").toLowerCase();
const filename = (path: string) => path.split(/[\\/]/).pop() || path;

// 對照隨專案附帶 SDK 的 PlugType 功能名稱，不以插件名稱猜測用途。
const effectTypes = new Map([
  "Analyzer", "Bass", "Channel Strip", "Delay", "Distortion", "Drums",
  "Dynamics", "EQ", "Filter", "Generator", "Guitar", "Mastering",
  "Microphone", "Modulation", "Network", "Pitch Shift", "Restoration",
  "Reverb", "Spatial", "Tools", "Vocals",
].map((name) => [normalize(name), name]));

function categoriesOf(raw: string): string[] {
  const tokens = raw.split("|").map((part) => normalize(part.trim())).filter(Boolean);
  const found = new Set<string>();
  for (const token of tokens) {
    if (token === "instrument") found.add("樂器");
    const category = effectTypes.get(token);
    if (category) found.add(category);
  }
  // Fx|Surround 是 SDK 功能類別；EQ|Surround 等末尾標記僅描述聲道。
  if (!found.size && tokens.includes("fx") && tokens.includes("surround")) found.add("Surround");
  return found.size ? [...found].sort(compare) : ["未分類"];
}

/** 建立一次目錄供搜尋重用；每個可加入 class 保留完整來源身分。 */
export function buildPluginCatalog(modules: ScanModule[]): CatalogPlugin[] {
  const plugins = new Map<string, CatalogPlugin>();
  for (const module of modules) {
    for (const info of module.classes) {
      const key = JSON.stringify([module.path.replaceAll("/", "\\").toLowerCase(), info.uid]);
      if (plugins.has(key)) continue;
      const name = info.name.trim() || filename(module.path);
      const vendor = info.vendor.trim() || "未知廠牌";
      const categories = categoriesOf(info.subcategories ?? "");
      plugins.set(key, {
        key, path: module.path, classId: info.uid, name, vendor,
        version: info.version.trim() || "版本未提供", categories,
        source: filename(module.path),
        searchText: normalize([name, vendor, info.subcategories, ...categories].join("\n")),
      });
    }
  }
  const catalog = [...plugins.values()].sort((a, b) =>
    compare(a.name, b.name) || compare(a.vendor, b.vendor) || compare(a.path, b.path) || compare(a.classId, b.classId));
  const collisions = new Map<string, number>();
  for (const plugin of catalog) {
    const key = normalize(JSON.stringify([plugin.name, plugin.source]));
    collisions.set(key, (collisions.get(key) ?? 0) + 1);
  }
  for (const plugin of catalog) {
    if ((collisions.get(normalize(JSON.stringify([plugin.name, plugin.source]))) ?? 0) > 1) {
      plugin.source = plugin.path;
      // 同一模組亦可能提供多個同名 class。
      if (catalog.some((other) => other !== plugin && other.path === plugin.path && other.name === plugin.name)) {
        plugin.source += ` · ${plugin.classId}`;
      }
    }
  }
  return catalog;
}

/** 對外結果以唯一 class 計數；多分類只增加組內呈現，不增加插件數。 */
export function browsePlugins(catalog: CatalogPlugin[], query: string, grouping: PluginGrouping) {
  const terms = normalize(query.trim()).split(/\s+/).filter(Boolean);
  const matches = catalog.filter((plugin) => terms.every((term) => plugin.searchText.includes(term)));
  const groups = new Map<string, PluginGroup>();
  for (const plugin of matches) {
    for (const name of grouping === "vendor" ? [plugin.vendor] : plugin.categories) {
      const key = normalize(name);
      const group = groups.get(key) ?? { name, plugins: [] };
      if (compare(name, group.name) < 0) group.name = name;
      group.plugins.push(plugin);
      groups.set(key, group);
    }
  }
  const fallback = grouping === "vendor" ? "未知廠牌" : "未分類";
  return {
    count: matches.length,
    groups: [...groups.values()].sort((a, b) =>
      Number(a.name === fallback) - Number(b.name === fallback) || compare(a.name, b.name)),
  };
}
