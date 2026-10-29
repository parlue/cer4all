"""Called only after a successful core_test run."""
from pathlib import Path
import json,hashlib
root=Path(__file__).resolve().parents[1]
report=dict(date='2026-10-29',passed=True,toolchain='C++17 core tests',
 tests=['Queue residence telemetry, partial completion, reset and timer wrap','Gateway log headers and fragmented commands','USB chip identity without fixed figure IDs','20000 FIFO operations and overflow rejection','8/320 framing, splits1..256','81-corner RGB mapping','RGB fragments1..253 and native passthrough','repeated LED commands retained and invalid frames rejected','all-model immediate byte identity across chunks1..253','first Sentio line unchanged for all chip hints','unfiltered arbitrary IDs, duplicates, noise and incomplete lines'],
 source_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in ('include/core.hpp','include/queue_probe.hpp','include/log_protocol.hpp','test/core_test.cpp')})
(root/'TEST_RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Recorded passing core tests with source hashes')
