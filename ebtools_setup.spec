# PyInstaller spec for the standalone ebtools-setup helper. Build with:
#   pyinstaller ebtools_setup.spec
# Produces a single self-contained executable (no Python install needed on
# the machine that runs it) at dist/ebtools-setup[.exe], bundled alongside
# the game binary in a release so it can build assets.pak from a player's
# ROM with zero manual setup. See ebtools_setup_entry.py, ebtools/cli/setup.py.

# -*- mode: python ; coding: utf-8 -*-

a = Analysis(
    ['ebtools_setup_entry.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('earthbound.yml', '.'),
        ('commondefs.yml', '.'),
        ('src/custom_assets', 'custom_assets'),
    ],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='ebtools-setup',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
