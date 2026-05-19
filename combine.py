import zipfile, glob, sys

slot0 = glob.glob('build/*_slot0.pbz')[0]
slot1 = glob.glob('build/*_slot1.pbz')[0]
out   = slot0.replace('_slot0.pbz', '.pbz')

with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as combined:
    for slot_num, pbz_path in [(0, slot0), (1, slot1)]:
        with zipfile.ZipFile(pbz_path, 'r') as z:
            for name in z.namelist():
                combined.writestr(f'slot{slot_num}/{name}', z.read(name))

print(f'Written: {out}')
