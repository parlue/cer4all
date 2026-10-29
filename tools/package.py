"""Package only successful S3 builds; use actual IDF flash offsets."""
from pathlib import Path
import hashlib,json,shutil,zipfile
root=Path(__file__).resolve().parents[1]
version=(root/'version.txt').read_text().strip()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
results={}
tests=json.loads((root/'TEST_RESULT.json').read_text())
assert tests['passed']
for name,digest in tests['source_sha256'].items():assert sha(root/name)==digest,name
for env,size in [('cer4all_s3mini',4),('cer4all_s3wroom',16)]:
    build=root/'.pio/build'/env
    args=json.loads((build/'flasher_args.json').read_text())
    assert args['extra_esptool_args']['chip']=='esp32s3'
    assert args['flash_settings']['flash_size']==f'{size}MB',args['flash_settings']
    target=root/'docs/firmware'/env;target.mkdir(parents=True,exist_ok=True)
    parts=[]
    for offset,name in args['flash_files'].items():
        source=build/name
        if not source.is_file():
            source=build/{0:'bootloader.bin',0x8000:'partitions.bin',0x10000:'firmware.bin'}[int(offset,0)]
        assert source.is_file() and source.stat().st_size>0,source
        if int(offset,0) in (0,0x10000):assert source.read_bytes()[0]==0xE9,source
        if int(offset,0)==0x8000:assert source.read_bytes()[:2]==bytes([0xAA,0x50]),source
        shutil.copy2(source,target/source.name)
        parts.append(dict(path=source.name,offset=int(offset,0)))
    assert {p['offset'] for p in parts}=={0,0x8000,0x10000},parts
    manifest=dict(name=f'cer4all {env}',version=version,new_install_prompt_erase=True,
                  builds=[dict(chipFamily='ESP32-S3',parts=parts)])
    (target/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    hashes={p.name:sha(p) for p in target.iterdir() if p.name!='SHA256.json'}
    (target/'SHA256.json').write_text(json.dumps(hashes,indent=2)+'\n',encoding='utf-8',newline='\n')
    results[env]=dict(flash_mb=size,files=hashes)
report=dict(version=version,hardware_tested=False,host_tests=tests,builds=results,source_sha256={p.relative_to(root).as_posix():sha(p) for folder in ('src','include') for p in (root/folder).rglob('*') if p.is_file()})
(root/'BUILD_RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
archive=root/'docs/firmware/source.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for folder in ('src','include','test','tools','third_party'):
        for p in (root/folder).rglob('*'):
            if p.is_file() and '__pycache__' not in p.parts:z.write(p,p.relative_to(root))
    for name in ('README.md','PERFORMANCE.md','.gitattributes','.gitignore','.clang-format','platformio.ini','version.txt','CMakeLists.txt','partitions.csv','sdkconfig.defaults','sdkconfig.mini.defaults','sdkconfig.wroom.defaults','dependencies.lock'):
        p=root/name
        if p.exists():z.write(p,name)
report['source_archive_sha256']=sha(archive)
(root/'BUILD_RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Packaged '+version+' for both variants; matching source archive SHA256 '+report['source_archive_sha256'])
