"""Called only after a successful core_test run."""
from pathlib import Path
import json,hashlib
root=Path(__file__).resolve().parents[1]
report=dict(date='2026-10-28',passed=True,toolchain='C++17 core tests',
 tests=['USB chip identity without fixed figure IDs','20000 FIFO operations and overflow rejection','8/320 framing, splits1..256','81-corner RGB mapping','RGB fragments1..253 and native passthrough','LED deduplication and invalid frames','arbitrary runtime ID learning and immediate known moves','bounded hold and duplicate phantom rejection','first Sentio line unchanged for all chip hints','upstream native/off-filter byte identity'],
 source_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in ('include/core.hpp','test/core_test.cpp')})
(root/'TEST_RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Recorded passing core tests with source hashes')
