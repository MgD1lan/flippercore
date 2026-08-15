#!/usr/bin/env python3
"""Extract build info (main app sources/cdefines, embedded plugins) from an application.fam.

Used by buildFap.sh to support:
  1. Apps that declare explicit sources=[...] and cdefines=[...] in their .fam
  2. Multi-plugin apps that declare fal_embedded / FlipperAppType.PLUGIN

Output format (tab-separated):
  APPSOURCE   <relative_path_to_source>
  APPCDEFINE  <define_name_or_assignment>
  PLUGIN      <plugin_appid>              <entry_point>
  PLUGINSRC   <plugin_appid>              <relative_path_to_source>
  PLUGINDEF   <plugin_appid>              <define_name_or_assignment>
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Iterable


class _AnyEnum:
    """Stand-in for FlipperAppType.* — returns attribute name as string."""

    def __getattr__(self, name: str) -> str:
        return f"FlipperAppType.{name}"

    def __eq__(self, other: object) -> bool:
        if isinstance(other, str):
            return other in ("FlipperAppType.PLUGIN", "Plugin")
        return False


def _has_wildcards(pattern: str) -> bool:
    return any(c in pattern for c in "*?[]")


def _gather_matching_sources(base_dir: Path, patterns: Iterable[str]) -> list[str]:
    include_patterns = [p for p in patterns if not p.startswith("!")]
    exclude_patterns = [p[1:] for p in patterns if p.startswith("!")]

    included: set[Path] = set()
    for pattern in include_patterns:
        p = base_dir / pattern
        if _has_wildcards(pattern):
            for root, _, _ in os.walk(base_dir):
                included.update(f for f in Path(root).glob(pattern) if f.is_file())
        elif p.is_dir():
            included.update(f for f in p.rglob("*") if f.is_file())
        elif p.is_file():
            included.add(p)

    excluded: set[Path] = set()
    for pattern in exclude_patterns:
        p = base_dir / pattern
        if _has_wildcards(pattern):
            for root, _, _ in os.walk(base_dir):
                excluded.update(f for f in Path(root).glob(pattern) if f.is_file())
        elif p.is_dir():
            excluded.update(f for f in p.rglob("*") if f.is_file())
        elif p.is_file():
            excluded.add(p)

    results: list[str] = []
    for f in sorted(included):
        if f.is_file() and f not in excluded and "/lib/" not in f.as_posix():
            results.append(f.relative_to(base_dir).as_posix())
    return results


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: fap_app_info.py <app_dir>\n")
        return 2

    app_dir = Path(sys.argv[1]).resolve()
    fam_path = app_dir / "application.fam"
    if not fam_path.is_file():
        return 0

    apps: list[dict] = []

    def App(*args, **kw):
        apps.append(kw)

    def Lib(*args, **kw):
        return kw

    def ExtFile(*args, **kw):
        return kw

    flipper_app_type = _AnyEnum()

    namespace = {
        "App": App,
        "Lib": Lib,
        "ExtFile": ExtFile,
        "FlipperAppType": flipper_app_type,
        "app_manifest_path": str(fam_path),
    }

    try:
        content = fam_path.read_text(encoding="utf-8")
        exec(compile(content, str(fam_path), "exec"), namespace)
    except Exception as exc:
        sys.stderr.write(f"fap_app_info.py: warning: failed to parse {fam_path}: {exc}\n")
        return 0

    # Separate main app and plugin apps
    main_app = None
    plugin_apps = []

    for app in apps:
        apptype_val = str(app.get("apptype", ""))
        is_plugin = (
            "PLUGIN" in apptype_val
            or apptype_val == "Plugin"
            or bool(app.get("fal_embedded", False))
        )
        if is_plugin:
            plugin_apps.append(app)
        elif main_app is None:
            main_app = app

    # Main app outputs
    if main_app:
        sources = main_app.get("sources", [])
        if sources:
            for src_rel in _gather_matching_sources(app_dir, sources):
                sys.stdout.write(f"APPSOURCE\t{src_rel}\n")
        for cdef in main_app.get("cdefines", []):
            sys.stdout.write(f"APPCDEFINE\t{cdef}\n")

    # Plugin apps outputs
    for plugin in plugin_apps:
        appid = plugin.get("appid", "")
        entry_point = plugin.get("entry_point", "")
        if not appid:
            continue
        sys.stdout.write(f"PLUGIN\t{appid}\t{entry_point}\n")
        sources = plugin.get("sources", [])
        if sources:
            for src_rel in _gather_matching_sources(app_dir, sources):
                sys.stdout.write(f"PLUGINSRC\t{appid}\t{src_rel}\n")
        for cdef in plugin.get("cdefines", []):
            sys.stdout.write(f"PLUGINDEF\t{appid}\t{cdef}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
