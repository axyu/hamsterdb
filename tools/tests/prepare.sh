#!/bin/sh

../../samples/db1
mv test.db db1.db

../../samples/env1
mv test.db env1.db

../../samples/env3
mv test.db env3.db

echo now run ../ham_bench/ham_bench ../../../hamsterdb-tests/testfiles/1/ext_020.tst
echo and copy test-ham.db to extkeys.db
echo then run ham_info/prepare.sh

