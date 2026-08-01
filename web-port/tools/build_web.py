#!/usr/bin/env python3
"""
Epic Astro Story - Web port build tool.

Extracts the Android APK into a browser-ready game directory consumed by the
in-browser Dalvik VM (js/dex.js + js/vm.js).

Usage:
    python3 tools/build_web.py [--apk ../Epic_Astro_Story-NTU1NTM3.apk]
                               [--base64 ../base64.txt]
                               [--out  ../web-port/game]

The extractor:
  1. Reads the APK (raw, or base64-encoded text as shipped in this repo).
  2. Dumps every ZIP entry verbatim into game/files/ (the VM loads
     classes.dex, assets/*.dat and res/raw/* from this virtual APK tree).
  3. Parses AndroidManifest.xml      -> game/app.json
  4. Parses resources.arsc           -> game/resources.json
"""
import argparse
import base64
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # web-port/
REPO = os.path.dirname(ROOT)                      # repo root


def log(*a):
    print('[build]', *a)
    sys.stdout.flush()


def read_apk_bytes(apk_path, b64_path):
    if apk_path and os.path.exists(apk_path):
        log('reading raw APK:', apk_path)
        return open(apk_path, 'rb').read()
    if b64_path and os.path.exists(b64_path):
        log('decoding base64 APK:', b64_path)
        txt = open(b64_path, 'rb').read()
        txt = re.sub(rb'\s+', b'', txt)
        return base64.b64decode(txt)
    raise SystemExit('no APK found (looked for %s and %s)' % (apk_path, b64_path))


def parse_manifest(apk_path):
    """Return (app.json dict, public-resource dict). Falls back to constants
    when androguard is unavailable (the values are stable for this APK)."""
    app = {
        'package': 'net.kairosoft.android.frontier_en',
        'versionCode': 2,
        'versionName': '1.0.1',
        'mainActivity': 'net.kairosoft.android.frontier_en.Main',
        'appName': 'Epic Astro Story',
    }
    resources = {
        'raw': {
            '0x7f040000': 'battle', '0x7f040001': 'damage',
            '0x7f040002': 'death', '0x7f040003': 'happy',
            '0x7f040004': 'lose', '0x7f040005': 'main1',
            '0x7f040006': 'main2', '0x7f040007': 'main3',
            '0x7f040008': 'manzokudo_up', '0x7f040009': 'money_nyuukin',
            '0x7f04000a': 'money_shukkin', '0x7f04000b': 'sad',
            '0x7f04000c': 'secchi', '0x7f04000d': 'snd',
            '0x7f04000e': 'tekkyo', '0x7f04000f': 'title',
            '0x7f040010': 'win',
        },
        'string': {'0x7f050000': 'app_name'},
        'layout': {'0x7f030000': 'main'},
        'drawable': {'0x7f020000': 'icon'},
        'values': {'app_name': 'Epic Astro Story'},
    }
    try:
        sys.path.insert(0, '/home/user/venv/lib/python3.11/site-packages')
        import logging
        logging.disable(logging.CRITICAL)
        from androguard.core.apk import APK
        from androguard.core.axml import ARSCParser
        a = APK(apk_path)
        app['package'] = a.get_package() or app['package']
        app['versionCode'] = int(a.get_androidversion_code() or app['versionCode'])
        app['versionName'] = a.get_androidversion_name() or app['versionName']
        app['mainActivity'] = a.get_main_activity() or app['mainActivity']
        try:
            for p in ('app_name',):
                v = a.get_app_name()
                if v:
                    app['appName'] = v
                    resources['values']['app_name'] = v
        except Exception:
            pass
        z = zipfile.ZipFile(apk_path)
        arsc = ARSCParser(z.read('resources.arsc'))
        for pkg in arsc.get_packages_names():
            try:
                pub = arsc.get_public_resources(pkg)
                if isinstance(pub, bytes):
                    pub = pub.decode('utf-8', 'replace')
                for m in re.finditer(r'<public type="([^"]+)" name="([^"]+)" id="(0x[0-9a-fA-F]+)"', pub):
                    t, n, i = m.groups()
                    if t == 'public':
                        continue
                    resources.setdefault(t, {})[i.lower()] = n
            except Exception as e:
                log('arsc warn:', e)
        log('manifest+arsc parsed with androguard')
    except Exception as e:
        log('androguard unavailable (%s); using embedded constants' % e)
    return app, resources


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default=os.path.join(REPO, 'Epic_Astro_Story-NTU1NTM3.apk'))
    ap.add_argument('--base64', default=os.path.join(REPO, 'base64.txt'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'game'))
    args = ap.parse_args()

    raw = read_apk_bytes(args.apk, args.base64)
    tmp = os.path.join('/tmp', '_eas_web.apk')
    with open(tmp, 'wb') as f:
        f.write(raw)
    log('APK bytes: %d' % len(raw))

    z = zipfile.ZipFile(tmp)
    out = args.out
    files_dir = os.path.join(out, 'files')
    os.makedirs(files_dir, exist_ok=True)

    n = 0
    for name in z.namelist():
        data = z.read(name)
        dest = os.path.join(files_dir, name)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as f:
            f.write(data)
        n += 1
    log('extracted %d entries -> %s' % (n, files_dir))

    app, resources = parse_manifest(tmp)
    with open(os.path.join(out, 'app.json'), 'w') as f:
        json.dump(app, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out, 'resources.json'), 'w') as f:
        json.dump(resources, f, indent=2, ensure_ascii=False)

    # flat manifest of every file for the loader
    listing = []
    for base, _, names in os.walk(files_dir):
        for nm in names:
            p = os.path.join(base, nm)
            listing.append([os.path.relpath(p, files_dir).replace(os.sep, '/'),
                            os.path.getsize(p)])
    listing.sort()
    with open(os.path.join(out, 'filelist.json'), 'w') as f:
        json.dump(listing, f)
    log('wrote app.json / resources.json / filelist.json (%d files)' % len(listing))
    log('done')


if __name__ == '__main__':
    main()
