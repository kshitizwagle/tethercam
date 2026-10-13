"""Adds a release to altstore/source.json (newest version first).

Usage: python scripts/update_altstore_source.py TAG IPA_PATH [NOTES]
Reads the version and build number from the IPA's Info.plist, so the source always matches the binary.
"""
import datetime
import json
import os
import plistlib
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "altstore", "source.json")
REPO = os.environ.get("GITHUB_REPOSITORY", "kshitizwagle/tethercam")


def ipa_info(path):
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if n.startswith("Payload/") and n.endswith(".app/Info.plist"))
        return plistlib.loads(z.read(name))


def main():
    tag, ipa = sys.argv[1], sys.argv[2]
    notes = sys.argv[3] if len(sys.argv) > 3 else f"Tethercam {tag}"
    info = ipa_info(ipa)
    entry = {
        "version": info["CFBundleShortVersionString"],
        "buildVersion": info["CFBundleVersion"],
        "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "localizedDescription": notes,
        "downloadURL": f"https://github.com/{REPO}/releases/download/{tag}/{os.path.basename(ipa)}",
        "size": os.path.getsize(ipa),
        "minOSVersion": info.get("MinimumOSVersion", "16.0"),
    }
    with open(SOURCE, encoding="utf-8") as f:
        source = json.load(f)
    app = next(a for a in source["apps"] if a["bundleIdentifier"] == info["CFBundleIdentifier"])
    app["versions"] = [v for v in app["versions"] if v["downloadURL"] != entry["downloadURL"]]
    app["versions"].insert(0, entry)
    with open(SOURCE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(source, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(json.dumps(entry, indent=2))


if __name__ == "__main__":
    main()
