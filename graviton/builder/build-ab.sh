#!/bin/bash
exec >> /opt/haiku/opt-build.log 2>&1
set -x
export HAIKU_REVISION=hrev59996
G=/opt/haiku/haiku-opt/generated.arm64
X=$G/cross-tools-arm64/bin/aarch64-unknown-haiku-
EV=/opt/haiku/opt-evidence
rm -rf $EV; mkdir -p $EV/before $EV/after
echo "ZGlmZiAtLWdpdCBhL2J1aWxkL2phbS9BcmNoaXRlY3R1cmVSdWxlcyBiL2J1aWxkL2phbS9BcmNoaXRlY3R1cmVSdWxlcwppbmRleCBmOTZjMWExMzZmLi5lYTBlOTVkZDE0IDEwMDY0NAotLS0gYS9idWlsZC9qYW0vQXJjaGl0ZWN0dXJlUnVsZXMKKysrIGIvYnVpbGQvamFtL0FyY2hpdGVjdHVyZVJ1bGVzCkBAIC0zOCw3ICszOCwxOCBAQCBydWxlIEFyY2hpdGVjdHVyZVNldHVwIGFyY2hpdGVjdHVyZQogCXN3aXRjaCAkKGNwdSkgewogCQljYXNlIHBwYyA6IGFyY2hGbGFncyArPSAtbWNwdT00NDBmcCA7CiAJCWNhc2UgYXJtIDogYXJjaEZsYWdzICs9IC1tYXJjaD1hcm12Ny1hIC1tZmxvYXQtYWJpPWhhcmQgOwotCQljYXNlIGFybTY0IDogYXJjaEZsYWdzICs9IC1tYXJjaD1hcm12OC1hK2NyYyA7CisJCSMgTmVvdmVyc2UtTjEgaXMgdGhlIEFXUyBHcmF2aXRvbjIgY29yZSBhbmQgdGhlIG9sZGVzdCBjb3JlIHRoZSBhcm02NAorCQkjIHBvcnQgdGFyZ2V0czsgR3Jhdml0b24zIChOZW92ZXJzZS1WMSkgYW5kIEdyYXZpdG9uNCAoTmVvdmVyc2UtVjIpIGFyZQorCQkjIGZlYXR1cmUgc3VwZXJzZXRzIG9mIGl0LCBzbyBhIHNpbmdsZSBpbWFnZSBzdGF5cyBwb3J0YWJsZSBhY3Jvc3MgdGhlCisJCSMgZmxlZXQuIE92ZXIgcGxhaW4gYXJtdjgtYSB0aGlzIGJ1eXMgTFNFIGF0b21pY3MgKEFSTXY4LjEpIC0tIGV2ZXJ5CisJCSMgX19hdG9taWMgYnVpbHRpbiBpbiB0aGUga2VybmVsIGFuZCBsaWJyb290IGJlY29tZXMgYSBzaW5nbGUKKwkJIyBjYXMvbGRhZGQvc3dwIGluc3RlYWQgb2YgYW4gb3V0LW9mLWxpbmUgX19hYXJjaDY0XyogaGVscGVyIGNhbGwgLS0KKwkJIyBwbHVzIFJDcGMgbG9hZHMsIGRvdCBwcm9kdWN0LCBhbmQgTmVvdmVyc2UgaW5zdHJ1Y3Rpb24gc2NoZWR1bGluZy4KKwkJIyBDUkMzMiBpcyBtYW5kYXRvcnkgZnJvbSBBUk12OC4xIG9uLCBzbyArY3JjIG5lZWQgbm90IGJlIHNwZWxsZWQgb3V0OworCQkjICtjcnlwdG8gKEFFUy9TSEEvUE1VTEwpIGlzIG5vdCBpbXBsaWVkIGJ5IHRoZSBjb3JlIGRlZmluaXRpb24gYnV0IGlzCisJCSMgcHJlc2VudCBvbiBldmVyeSBHcmF2aXRvbiwgYW5kIGlzIHdoYXQgbGV0cyBhY2NlbGVyYXRlZCBjaGVja3N1bSBhbmQKKwkJIyBjcnlwdG8gY29kZSBiZSBidWlsdCB3aXRob3V0IHRvdWNoaW5nIHRoaXMgbGluZSBhZ2Fpbi4KKwkJY2FzZSBhcm02NCA6IGFyY2hGbGFncyArPSAtbWNwdT1uZW92ZXJzZS1uMStjcnlwdG8gOwogCQljYXNlIHg4NiA6IGFyY2hGbGFncyArPSAtbWFyY2g9cGVudGl1bSA7CiAJCWNhc2UgcmlzY3Y2NCA6IGFyY2hGbGFncyArPSAtbWFyY2g9cnY2NGdjIDsKIAl9Cg==" | base64 -d > /opt/haiku/opt-arch.patch

census() {  # $1 = binary, $2 = label
  ${X}objdump -d "$1" > /tmp/c.dis
  echo "##### CENSUS $2 : $1"
  echo "-- LSE ops:"
  grep -oE '^[[:space:]]*[0-9a-f]+:[[:space:]]+[0-9a-f]{8}[[:space:]]+(cas[apl]*[bh]*|casp[apl]*|ldadd[apl]*[bh]*|ldclr[apl]*[bh]*|ldset[apl]*[bh]*|ldeor[apl]*[bh]*|ldsmax[apl]*|ldsmin[apl]*|ldumax[apl]*|ldumin[apl]*|swp[apl]*[bh]*|stadd[lbh]*|stclr[lbh]*|stset[lbh]*|steor[lbh]*)\b' /tmp/c.dis | awk '{print $3}' | sort | uniq -c | sort -rn
  echo "-- LSE total: $(grep -coE '^[[:space:]]*[0-9a-f]+:[[:space:]]+[0-9a-f]{8}[[:space:]]+(cas[apl]*[bh]*|casp[apl]*|ldadd[apl]*[bh]*|ldclr[apl]*[bh]*|ldset[apl]*[bh]*|ldeor[apl]*[bh]*|swp[apl]*[bh]*|stadd[lbh]*|stclr[lbh]*|stset[lbh]*)\b' /tmp/c.dis)"
  echo "-- LL/SC ops:"
  grep -oE '\b(ld[a]?x[rp]?[bh]?|st[l]?x[rp]?[bh]?)\b' /tmp/c.dis | sort | uniq -c | sort -rn
  echo "-- outline atomic bl sites: $(grep -c 'bl.*__aarch64_' /tmp/c.dis)"
  grep -oE '__aarch64_[a-z0-9_]+' /tmp/c.dis | sort | uniq -c | sort -rn | head -15
  echo "-- ldapr (RCpc): $(grep -coE '\bldapr[bh]?\b' /tmp/c.dis)"
  echo "-- crc32: $(grep -coE '\bcrc32c?[bhwx]\b' /tmp/c.dis)"
  echo "-- size: $(stat -c %s "$1")"
  echo "##### END CENSUS $2"
}

save() { # $1 = dest dir
  cp $G/objects/haiku/arm64/release/system/kernel/kernel_arm64 $1/ 2>/dev/null
  find $G/objects/haiku/arm64/release/system/libroot -maxdepth 2 -name libroot.so -exec cp {} $1/ \; 2>/dev/null
  for n in smp.o thread.o VMSAv8TranslationMap.o vm.o generic_atomic.o; do
    find $G/objects/haiku/arm64/release/system -name "$n" -exec cp {} $1/ \; 2>/dev/null
  done
  ls -l $1
}

cd $G || exit 1
echo "=== PHASE B BASELINE START $(date) ==="
grep -n 'case arm64 : archFlags' ../build/jam/ArchitectureRules
jam -q -j16 kernel_arm64 libroot.so
echo "PHASE_B_EXIT=$?"
save $EV/before
census $EV/before/kernel_arm64 BEFORE-kernel
census $EV/before/libroot.so BEFORE-libroot
echo "=== PHASE B DONE $(date) ==="

echo "=== PHASE C: apply -mcpu patch ==="
cd /opt/haiku/haiku-opt || exit 1
git apply -v /opt/haiku/opt-arch.patch && echo ARCH_PATCH_OK || { echo ARCH_PATCH_FAILED; exit 1; }
grep -n 'mcpu=neoverse' build/jam/ArchitectureRules
cd $G || exit 1
rm -rf objects/haiku
echo "=== PHASE C REBUILD START $(date) ==="
jam -q -j16 kernel_arm64 libroot.so
echo "PHASE_C_EXIT=$?"
save $EV/after
census $EV/after/kernel_arm64 AFTER-kernel
census $EV/after/libroot.so AFTER-libroot
echo "=== ALL DONE $(date) ==="
touch /opt/haiku/opt-ab.done
