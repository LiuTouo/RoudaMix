import unittest
from release import source_allowed, validate_version


class ReleaseContractTests(unittest.TestCase):
    def test_windows_versions_and_prereleases(self):
        for version in ("0.1.0", "1.20.3", "1.0.0-rc.1", "1.0.0-beta.0"):
            self.assertEqual(validate_version(version), version)

    def test_rejects_unsafe_or_ambiguous_version_input(self):
        for version in ("v1.0.0", "01.0.0", "1.0", "1.0.0+build", "1.0.0-rc.01", "65536.0.0",
                        "1.0.0\n", "../x", "1.0.0; echo injected", "${{ secrets.TOKEN }}"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate_version(version)

    def test_source_archive_includes_build_inputs(self):
        for path in ("engine/src/audio_engine.cpp", "engine/third_party/asio-sdk/common/asio.h",
                     "src-tauri/icons/icon.ico", "ui/src/App.svelte", "contracts/protocol.md",
                     "scripts/build-release.ps1", "Cargo.lock", "BUILDING.md"):
            self.assertTrue(source_allowed(path), path)

    def test_source_archive_excludes_local_and_internal_material(self):
        for path in (".env", ".git/config", ".claude/skills/test.md", "AGENTS.md", "CLAUDE.md",
                     "docs/agents/issue-tracker.md", "output/session.json", "target/release/app.pdb",
                     "engine/build/Release/app.exe", "ui/node_modules/svelte/package.json",
                     "src-tauri/WebView2Runtime/browser.exe", ".scratch/notes.txt"):
            self.assertFalse(source_allowed(path), path)
