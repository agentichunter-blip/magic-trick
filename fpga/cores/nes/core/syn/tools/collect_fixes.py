import re, sys
from pathlib import Path
log = Path(sys.argv[1]).read_text(errors='replace')
fix = Path(__file__).with_name('decl_fixes.txt')
have = set(fix.read_text().splitlines()) if fix.exists() else set()
new = set()
for ident, f, ln in re.findall(r"EX3900\) : Procedural assignment to a non-register '(\w+)' is not permitted\(\"[^\"]*/src/([^\"]+)\":(\d+)\)", log):
    new.add(f'{f} {ln} {ident}')
add = sorted(new - have)
fix.write_text('\n'.join(sorted(have | new)) + '\n')
print('added', len(add)); print('\n'.join(add))
